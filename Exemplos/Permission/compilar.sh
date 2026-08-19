#!/bin/bash
# ============================================================================
#  Compila o ConanPermission.dll
#
#  Só precisa de mingw-w64. Sem Visual Studio, sem editor da Unreal, sem
#  vcpkg — a mesma promessa do resto da API.
#
#  O sqlite3.o é compilado UMA vez e reaproveitado (leva ~75 s; são 261.454
#  linhas). Apague build/sqlite3.o para forçar a recompilação.
# ============================================================================
set -e
AQUI="$(cd "$(dirname "$0")" && pwd)"
API="$AQUI/../../api"
OBJ="$AQUI/build"
mkdir -p "$OBJ"

FLAGS_SQLITE=(
  -O2
  -DSQLITE_THREADSAFE=1
  -DSQLITE_OMIT_LOAD_EXTENSION
  -DSQLITE_DEFAULT_MEMSTATUS=0
  -DSQLITE_OMIT_DEPRECATED
  -DSQLITE_DQS=0
  -DSQLITE_ENABLE_JSON1
  -DSQLITE_MAX_EXPR_DEPTH=0
  -DSQLITE_DEFAULT_FOREIGN_KEYS=1
)

if [ ! -f "$OBJ/sqlite3.o" ]; then
  echo "== sqlite3.c (uma vez, ~75 s) =="
  x86_64-w64-mingw32-gcc -c "$AQUI/terceiros/sqlite3/sqlite3.c" \
      -o "$OBJ/sqlite3.o" "${FLAGS_SQLITE[@]}"
fi

echo "== ConanPermission.dll =="
# -Wl,--exclude-all-symbols: o DLL exporta SÓ o que tem __declspec(dllexport).
#
# Sem esta linha o MinGW auto-exporta tudo, e este DLL passaria a exportar as
# ~250 funções sqlite3_* que estão estaticamente dentro dele. Duas consequências
# reais, as duas silenciosas:
#   1. outro plugin que também embarque SQLite exportaria os MESMOS nomes, e o
#      Windows resolveria para o primeiro que carregou — um plugin usando o
#      SQLite do outro, com pragmas e configuração de outra pessoa;
#   2. qualquer código no processo poderia pegar sqlite3_exec por GetProcAddress
#      e escrever no nosso banco.
# A superfície exportada tem de ser exatamente a ABI: duas funções.
#
# E NADA NOSSO É LINKADO AQUI. Até 17/08/2026 esta receita terminava com
# "$API/lib/libconanapi.a", e o motor inteiro (4.398 linhas: decodificador, mesa
# de hooks, os offsets desta build) ia para DENTRO deste DLL. No modelo de tabela
# isso acabou: o plugin inclui só o ConanPluginApi.h — 246 linhas de declaração,
# nenhuma implementação — e recebe os ponteiros prontos em ConanPluginCarregar.
#
# Quem devolver a libconanapi.a a esta linha põe uma SEGUNDA cópia do motor no
# processo, com os offsets do dia em que ESTE DLL foi compilado. Compila, linka,
# carrega — e no dia em que o jogo atualizar uma cópia sabe do jogo novo e a
# outra não, cada uma respondendo por metade dos plugins. O -I do $API/include
# fica, e é só pelo header.
#
# -lws2_32 entrou em 18/08/2026, com o MySqlCliente. Ele é o Winsock, e é a
# ÚNICA biblioteca nova: o cliente MySQL fala o protocolo direto no socket, com
# SHA-1, SHA-256 e RSA implementados dentro do próprio .cpp, justamente para o
# dono do servidor não ter de instalar DLL nenhuma. Ver MySqlCliente.h.
#
# E ele entra SEMPRE, mesmo em quem só vai usar sqlite: um .dll que só linka o
# MySQL "quando precisa" seriam dois .dll diferentes com o mesmo nome, e o
# suporte a um servidor de terceiro começaria por descobrir qual dos dois o
# dono baixou.
x86_64-w64-mingw32-g++ -std=c++17 -O2 -Wall -Wextra -shared \
    -I "$API/include" -I "$AQUI/include" -I "$AQUI" \
    -o "$OBJ/ConanPermission.dll" \
    "$AQUI/Permission.cpp" "$AQUI/Armazem.cpp" \
    "$AQUI/Banco.cpp" "$AQUI/BancoSqlite.cpp" "$AQUI/BancoMysql.cpp" \
    "$AQUI/MySqlCliente.cpp" "$OBJ/sqlite3.o" \
    -static-libgcc -static-libstdc++ -static -lws2_32 \
    -Wl,--exclude-all-symbols

cp "$OBJ/ConanPermission.dll" "$AQUI/ConanPermission.dll"

echo
echo "== a guarda de exportacao sabe enxergar? (controle positivo) =="
# ── POR QUE ESTE PASSO EXISTE (defeito real, 18/08/2026) ────────────────────
#
# A checagem abaixo ("nenhum sqlite3_* exportado") vivia dando [ok]. Em 18/08
# ela foi calibrada pela primeira vez, recompilando o MESMO DLL **sem** o
# -Wl,--exclude-all-symbols para ver a guarda reprovar. Ela NAO reprovou:
# continuou 0.
#
# O motivo: o MinGW so auto-exporta tudo quando NAO ha nenhum
# __declspec(dllexport) na ligacao. Como Permission.cpp tem dois, a
# auto-exportacao ja estava desligada — e a guarda vinha aprovando um DLL que
# nunca teve como falhar. Verde e cega, que e o pior estado possivel: parece
# protecao e nao e.
#
# O conserto nao e apagar a guarda (o --exclude-all-symbols continua sendo a
# rede para o dia em que alguem trocar os dllexport por um .def). O conserto e
# provar, no MESMO experimento, que ela sabe produzir nao-zero. Uma isca com um
# `sqlite3_*` e um `Perm::` REALMENTE exportados tem de ser pega pelos dois
# padroes. Se ela nao for, o "0" la embaixo nao vale nada e o script para com
# 2 — NAO VERIFICOU nao e aprovacao.
ISCA="$OBJ/isca_da_guarda"
cat > "$ISCA.cpp" <<'ISCAEOF'
namespace Perm { class MySqlCliente { public: __declspec(dllexport) int Isca(); };
                 int MySqlCliente::Isca() { return 1; } }
extern "C" __declspec(dllexport) int sqlite3_teste_da_guarda() { return 1; }
ISCAEOF
if ! x86_64-w64-mingw32-g++ -std=c++17 -O0 -shared -o "$ISCA.dll" "$ISCA.cpp" -static 2>/dev/null; then
  echo "  [ ? ] nao consegui compilar a isca — a guarda NAO foi verificada."
  rm -f "$ISCA.cpp" "$ISCA.dll"; exit 2
fi
IS=$(x86_64-w64-mingw32-objdump -p "$ISCA.dll" | grep -c "sqlite3_" || true)
IP=$(x86_64-w64-mingw32-objdump -p "$ISCA.dll" | grep -c "MySqlCliente\|Perm@@\|_ZN4Perm" || true)
rm -f "$ISCA.cpp" "$ISCA.dll"
if [ "$IS" = "0" ] || [ "$IP" = "0" ]; then
  echo "  [ ? ] a isca EXPORTA sqlite3_teste_da_guarda e Perm::MySqlCliente::Isca,"
  echo "        e os padroes acharam $IS e $IP. A guarda esta cega — o '0' do DLL"
  echo "        de verdade nao provaria nada. NAO VERIFICOU."
  exit 2
fi
echo "  [ok] a isca foi pega pelos dois padroes ($IS e $IP) — o zero abaixo tem valor"

echo
echo "== o que o DLL exporta (tem de ser SO a ABI) =="
# Padrão SEM \t e SEM \s de propósito: este script vai para a mão da
# comunidade, e `\t` em expressão estendida NÃO é interpretado pelo GNU grep
# (é pelo ugrep). Um padrão que só casa no grep de quem escreveu é um teste que
# aprova em silêncio na máquina errada. Já mordeu aqui, na primeira versão.
x86_64-w64-mingw32-objdump -p "$AQUI/ConanPermission.dll" \
  | sed -n '/Ordinal\/Name Pointer. Table/,+6p' || true
N=$(x86_64-w64-mingw32-objdump -p "$AQUI/ConanPermission.dll" | grep -c "sqlite3_" || true)
if [ "$N" != "0" ]; then
  echo "  [XXX] FALHA: o DLL esta exportando $N simbolo(s) sqlite3_*."
  exit 1
fi
echo "  [ok] nenhum simbolo sqlite3_* exportado"

# Mesma regra para o cliente MySQL, pelo mesmo motivo e com um agravante: a
# classe Perm::MySqlCliente carrega SHA-1, SHA-256 e RSA escritos aqui. Se
# outro plugin embarcar uma cópia dela, o Windows resolveria os nomes para o
# primeiro que carregou — um plugin usando a criptografia do outro, com os
# tempos-limite e o log de outra pessoa. A superfície exportada tem de ser
# exatamente a ABI.
N=$(x86_64-w64-mingw32-objdump -p "$AQUI/ConanPermission.dll" | grep -c "MySqlCliente\|Perm@@\|_ZN4Perm" || true)
if [ "$N" != "0" ]; then
  echo "  [XXX] FALHA: o DLL esta exportando $N simbolo(s) internos de Perm::."
  exit 1
fi
echo "  [ok] nenhum simbolo interno de Perm:: exportado"

# A prova positiva também: a fábrica TEM de estar exportada. Sem esta checagem,
# --exclude-all-symbols mal usado geraria um DLL que carrega, não exporta nada,
# e faz TODO plugin de terceiro degradar para "não sei" — sem erro nenhum.
if ! x86_64-w64-mingw32-objdump -p "$AQUI/ConanPermission.dll" \
     | grep -q "ConanPermissionObterApi"; then
  echo "  [XXX] FALHA: ConanPermissionObterApi NAO esta exportada."
  exit 1
fi
echo "  [ok] ConanPermissionObterApi exportada"

echo
echo "  ✅ $AQUI/ConanPermission.dll"
echo
echo "  INSTALAR (a pasta tem HIFEN: Conan-Api, nunca ConanApi)"
echo "    1) ConanPermission.dll -> <servidor>/ConanSandbox/Binaries/Win64/Conan-Api/Plugins/"
echo "    2) permission.json     -> <servidor>/ConanSandbox/Binaries/Win64/Conan-Api/Config/"
echo
echo "  Ate 17/08/2026 estas duas linhas mandavam a pasta SEM hifen, e o json"
echo "  para dentro de Plugins/ — as duas erradas, e as duas falhando em"
echo "  SILENCIO. O loader varre <Win64>\\Conan-Api\\Plugins\\*.dll"
echo "  (loader/ConanLoader.cpp) e o Armazem abre"
echo "  <Win64>\\Conan-Api\\Config\\permission.json (ConanApi::CaminhoConfig)."
echo "  Quem seguia a instrucao ficava com o plugin que nunca carrega e com o"
echo "  banco sem grupo nenhum, sem UMA linha de erro apontando a causa."
echo "  O script plugins/conferir-caminhos.sh existe para isso nao voltar."
