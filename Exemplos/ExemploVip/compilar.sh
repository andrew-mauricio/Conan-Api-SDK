#!/bin/bash
# Um plugin de terceiro que usa o Permission. Repare no que NÃO está aqui:
#
#   · nenhuma referência a ConanPermission.dll, nenhuma .lib, nenhum -l;
#   · nenhum libconanapi.a, nenhum .cpp nosso.
#
# Só dois caminhos de header. No modelo de tabela o plugin não linka NADA
# nosso: ele recebe ponteiros de função no carregamento. Se um dia voltar um
# `.a` para esta linha de comando, é sinal de que alguém incluiu ConanSDK.h por
# engano — e aí o motor inteiro entra no binário do plugin de novo.
set -e
AQUI="$(cd "$(dirname "$0")" && pwd)"
API="$AQUI/../../api"
PERM="$AQUI/../Permission/include"

# ── o plugin não pode incluir nada nosso além da tabela ─────────────────────
# Barato de conferir e caro de descobrir tarde: incluir o SDK compila, linka e
# só falha em produção, quando o layout de C++ do compilador do terceiro não
# bate com o nosso.
if grep -nE '#include[[:space:]]*"Conan/(ConanSDK|ConanBase|ConanHooks|ConanStructs|ConanAtomico|ConanGuarda|ConanDecodificador)\.h"' "$AQUI/ExemploVip.cpp"; then
  echo "  [XXX] FALHA: o plugin incluiu header interno nosso. So ConanPluginApi.h."
  exit 1
fi

x86_64-w64-mingw32-g++ -std=c++17 -O2 -Wall -Wextra -shared \
    -I "$API/include" -I "$PERM" \
    -o "$AQUI/ExemploVip.dll" \
    "$AQUI/ExemploVip.cpp" \
    -static-libgcc -static-libstdc++ -static \
    -Wl,--exclude-all-symbols

echo "== o plugin depende de que DLLs? =="
x86_64-w64-mingw32-objdump -p "$AQUI/ExemploVip.dll" | grep "DLL Name:"
if x86_64-w64-mingw32-objdump -p "$AQUI/ExemploVip.dll" | grep -qi "ConanPermission"; then
  echo "  [XXX] FALHA: ficou uma dependencia estatica do ConanPermission.dll."
  echo "        Este plugin nao carregaria em servidor sem o Permission instalado."
  exit 1
fi
echo "  [ok] nenhuma dependencia do ConanPermission.dll — degrada em vez de nao carregar"

echo "== exporta o contrato? =="
x86_64-w64-mingw32-objdump -p "$AQUI/ExemploVip.dll" | grep -E "ConanPlugin(Carregar|Descarregar)" || {
  echo "  [XXX] FALHA: ConanPluginCarregar nao esta exportada — a DLL carrega e nada acontece."
  exit 1
}
echo "  ✅ $AQUI/ExemploVip.dll"
