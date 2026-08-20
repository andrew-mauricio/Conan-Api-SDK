#!/bin/bash
# ============================================================================
#  Builds ConanPermission.dll
#
#  Only needs mingw-w64. No Visual Studio, no Unreal editor, no vcpkg — the
#  same promise as the rest of the API.
#
#  sqlite3.o is compiled ONCE and reused (takes ~75 s; it's 261,454 lines).
#  Delete build/sqlite3.o to force a rebuild.
# ============================================================================
set -e
AQUI="$(cd "$(dirname "$0")" && pwd)"
API="$AQUI/../../api"
# Anything generic lives in plugins/comum and is NOT copied here: MySqlCliente
# (the MySQL protocol written in-house) and the SQLite amalgamation also serve
# ConanShop. Two copies of the same truth drift apart — and they'd drift in a
# database client of all places, where the bug is silent.
COMUM="$AQUI/../comum"
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
  x86_64-w64-mingw32-gcc -c "$COMUM/terceiros/sqlite3/sqlite3.c" \
      -o "$OBJ/sqlite3.o" "${FLAGS_SQLITE[@]}"
fi

echo "== ConanPermission.dll =="
# -Wl,--no-insert-timestamp: REPRODUCIBLE build. Without it MinGW stamps the
# build time into the PE header, and two builds of the SAME source come out with
# different hashes — which makes it impossible to prove the published DLL came
# from the published source. Added on 2026-08-20, after the ConanShop package
# shipped with a binary whose hash differed from the one tested and running.
#
# -Wl,--exclude-all-symbols: the DLL exports ONLY what is marked
# __declspec(dllexport).
#
# Without this line MinGW auto-exports everything, and this DLL would start
# exporting the ~250 sqlite3_* functions that sit statically inside it. Two real
# consequences, both silent:
#   1. another plugin that also embeds SQLite would export the SAME names, and
#      Windows would resolve to whichever loaded first — one plugin using the
#      other's SQLite, with somebody else's pragmas and configuration;
#   2. any code in the process could grab sqlite3_exec through GetProcAddress
#      and write to our database.
# The exported surface has to be exactly the ABI: two functions.
#
# AND NOTHING OF OURS IS LINKED HERE. Until 2026-08-17 this recipe ended with
# "$API/lib/libconanapi.a", and the whole engine (4,398 lines: decoder, hook
# table, this build's offsets) went INSIDE this DLL. The table model ended that:
# the plugin includes only ConanPluginApi.h — 246 lines of declarations, no
# implementation — and gets the pointers handed to it in ConanPluginCarregar.
#
# Put libconanapi.a back on this line and you put a SECOND copy of the engine in
# the process, carrying the offsets from the day THIS DLL was built. It compiles,
# links, loads — and the day the game updates, one copy knows the new game and
# the other doesn't, each answering for half the plugins. The -I on $API/include
# stays, and it's there for the header only.
#
# -lws2_32 came in on 2026-08-18, with MySqlCliente. It's Winsock, and it's the
# ONLY new library: the MySQL client speaks the protocol straight on the socket,
# with SHA-1, SHA-256 and RSA implemented inside the .cpp itself, precisely so
# the server owner doesn't have to install any DLL. See MySqlCliente.h.
#
# And it goes in ALWAYS, even for people who'll only use sqlite: a .dll that
# links MySQL "when it needs to" would be two different .dlls with the same
# name, and supporting somebody else's server would start with figuring out
# which of the two the owner downloaded.
x86_64-w64-mingw32-g++ -std=c++17 -O2 -Wall -Wextra -shared \
    -I "$API/include" -I "$AQUI/include" -I "$AQUI" -I "$COMUM" \
    -o "$OBJ/ConanPermission.dll" \
    "$AQUI/Permission.cpp" "$AQUI/Armazem.cpp" "$AQUI/Comandos.cpp" \
    "$AQUI/Banco.cpp" "$AQUI/BancoSqlite.cpp" "$AQUI/BancoMysql.cpp" \
    "$COMUM/MySqlCliente.cpp" "$OBJ/sqlite3.o" \
    -static-libgcc -static-libstdc++ -static -lws2_32 \
    -Wl,--exclude-all-symbols \
    -Wl,--no-insert-timestamp

cp "$OBJ/ConanPermission.dll" "$AQUI/ConanPermission.dll"

echo
echo "== a guarda de exportacao sabe enxergar? (controle positivo) =="
# ── WHY THIS STEP EXISTS (real bug, 2026-08-18) ─────────────────────────────
#
# The check below ("no sqlite3_* exported") kept coming back [ok]. On 2026-08-18
# it was calibrated for the first time, rebuilding the SAME DLL **without**
# -Wl,--exclude-all-symbols to watch the guard fail it. It did NOT fail it: the
# count stayed 0.
#
# The reason: MinGW only auto-exports everything when there is NO
# __declspec(dllexport) anywhere in the link. Permission.cpp has two, so
# auto-export was already off — and the guard had been passing a DLL that never
# had a way to fail. Green and blind, which is the worst state there is: it
# looks like protection and isn't.
#
# The fix isn't deleting the guard (--exclude-all-symbols is still the net for
# the day someone swaps the dllexports for a .def). The fix is proving, in the
# SAME experiment, that it can produce non-zero. A decoy with a `sqlite3_*` and
# a `Perm::` REALLY exported has to be caught by both patterns. If it isn't, the
# "0" further down is worth nothing and the script stops with 2 — DIDN'T CHECK
# isn't approval.
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
# Pattern with NO \t and NO \s on purpose: this script goes into the hands of
# the community, and `\t` in an extended expression is NOT interpreted by GNU
# grep (ugrep does). A pattern that only matches on the author's grep is a test
# that passes silently on the wrong machine. It already bit us in the first
# version.
x86_64-w64-mingw32-objdump -p "$AQUI/ConanPermission.dll" \
  | sed -n '/Ordinal\/Name Pointer. Table/,+6p' || true
N=$(x86_64-w64-mingw32-objdump -p "$AQUI/ConanPermission.dll" | grep -c "sqlite3_" || true)
if [ "$N" != "0" ]; then
  echo "  [XXX] FALHA: o DLL esta exportando $N simbolo(s) sqlite3_*."
  exit 1
fi
echo "  [ok] nenhum simbolo sqlite3_* exportado"

# Same rule for the MySQL client, for the same reason and with one thing worse:
# the Perm::MySqlCliente class carries SHA-1, SHA-256 and RSA written here. If
# another plugin embeds a copy of it, Windows would resolve the names to
# whichever loaded first — one plugin using the other's crypto, with somebody
# else's timeouts and log. The exported surface has to be exactly the ABI.
N=$(x86_64-w64-mingw32-objdump -p "$AQUI/ConanPermission.dll" | grep -c "MySqlCliente\|Perm@@\|_ZN4Perm" || true)
if [ "$N" != "0" ]; then
  echo "  [XXX] FALHA: o DLL esta exportando $N simbolo(s) internos de Perm::."
  exit 1
fi
echo "  [ok] nenhum simbolo interno de Perm:: exportado"

# The positive proof too: the factory HAS to be exported. Without this check, a
# misused --exclude-all-symbols would give a DLL that loads, exports nothing,
# and makes EVERY third-party plugin degrade to "I don't know" — with no error
# at all.
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
