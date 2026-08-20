#!/bin/bash
# Builds ConanShop. Nothing beyond the MinGW cross-compiler is needed — nobody
# from the community needs Visual Studio or the Unreal editor.
#
# WHAT GOES INTO THE DLL, AND WHY
#   ConanShop.cpp  Config.cpp  Pontos.cpp  Comandos.cpp   the plugin
#   MySqlCliente.cpp                        our own MySQL client
#   sqlite3.o                               the local DB AND the JSON reader
#
# SQLITE IS IN HERE TWICE ON PURPOSE: it's the local database (conanshop.db) and
# also the config.json parser (json_valid/json_extract/json_each). Hand-writing
# a JSON parser to read a file the owner edits by hand would be new code on the
# plugin's most exposed path — and SQLite is already here.
#
# MySQL goes in ALWAYS, even for people who'll only use the local database: a
# .dll that links MySQL "when it needs to" would be two files with the same
# name, and supporting somebody else's server would start with figuring out
# which of the two the owner downloaded.
set -e
AQUI="$(cd "$(dirname "$0")" && pwd)"
OBJ="$AQUI/build"
mkdir -p "$OBJ"

# ── where the headers are ───────────────────────────────────────────────────
#
# Searched for, never assumed: this same script runs in the development tree
# (plugins/ConanShop/) and in the SDK the community downloads
# (Exemplos/ConanShop/), which have different shapes.
INC=""
if [ -n "${CONAN_SDK_INCLUDE:-}" ] && [ -f "$CONAN_SDK_INCLUDE/Conan/ConanPluginApi.h" ]; then
    INC="$CONAN_SDK_INCLUDE"
else
    d="$AQUI"
    for _ in 1 2 3 4 5 6; do
        for c in "$d/include" "$d/api/include"; do
            [ -f "$c/Conan/ConanPluginApi.h" ] && { INC="$c"; break 2; }
        done
        d="$(dirname "$d")"
        [ "$d" = "/" ] && break
    done
fi
if [ -z "$INC" ]; then
    echo "  x nao achei Conan/ConanPluginApi.h. Procurei a partir de: $AQUI"
    echo "    Aponte com:  CONAN_SDK_INCLUDE=/caminho/do/sdk/include ./compilar.sh"
    exit 1
fi

# Permission's header lives in a different place in the tree and in the SDK.
PERM=""
if [ ! -f "$INC/Conan/ConanPermission.h" ]; then
    for c in "$AQUI/../Permission/include" "$AQUI/../../plugins/Permission/include" \
             "$AQUI/../../Exemplos/Permission/include"; do
        [ -f "$c/Conan/ConanPermission.h" ] && { PERM="-I $c"; break; }
    done
fi
if [ -z "$PERM" ] && [ ! -f "$INC/Conan/ConanPermission.h" ]; then
    echo "  x nao achei Conan/ConanPermission.h — o ConanShop consulta o Permission"
    echo "    para VIP e para a identidade do jogador."
    exit 1
fi

# Anything generic lives in plugins/comum and is NOT copied here.
COMUM=""
for c in "$AQUI/../comum" "$AQUI/../../plugins/comum" "$AQUI/comum"; do
    [ -f "$c/MySqlCliente.h" ] && { COMUM="$c"; break; }
done
if [ -z "$COMUM" ]; then
    echo "  x nao achei MySqlCliente.h (esperado em plugins/comum/)"
    exit 1
fi

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

echo "== ConanShop.dll =="
# -Wl,--no-insert-timestamp: REPRODUCIBLE build. MinGW stamps the build time
# into the PE header, so two builds of the SAME source came out with different
# md5s. That made it impossible to prove the published binary came from the
# published source — the only question that matters to someone downloading a DLL
# that will run inside their server. Without the flag, "check the hash" meant
# nothing.
#
# -Wl,--exclude-all-symbols: the DLL exports ONLY what is marked
# __declspec(dllexport). Without it MinGW auto-exports the ~250 sqlite3_*
# functions that sit statically inside it, and another plugin doing
# GetProcAddress would grab OUR handle.
x86_64-w64-mingw32-g++ -std=c++17 -O2 -Wall -Wextra -shared \
    -I "$INC" $PERM -I "$AQUI" -I "$COMUM" \
    -o "$OBJ/ConanShop.dll" \
    "$AQUI/ConanShop.cpp" "$AQUI/Config.cpp" "$AQUI/Pontos.cpp" "$AQUI/Comandos.cpp" \
    "$COMUM/MySqlCliente.cpp" "$OBJ/sqlite3.o" \
    -static-libgcc -static-libstdc++ -static -lws2_32 \
    -Wl,--exclude-all-symbols \
    -Wl,--no-insert-timestamp

cp "$OBJ/ConanShop.dll" "$AQUI/ConanShop.dll"

# ── the export guard, with a positive control ───────────────────────────────
#
# Counting zero only proves something if the instrument can count. The decoy
# EXPORTS on purpose what the guard looks for; if the guard can't see the decoy,
# it's blind and the real DLL's "zero" is worth nothing.
echo
echo "== a guarda de exportacao sabe enxergar? (controle positivo) =="
ISCA="$OBJ/isca_da_guarda"
cat > "$ISCA.cpp" <<'ISCAEOF'
extern "C" __declspec(dllexport) int sqlite3_teste_da_guarda() { return 1; }
ISCAEOF
if x86_64-w64-mingw32-g++ -std=c++17 -O0 -shared -o "$ISCA.dll" "$ISCA.cpp" -static 2>/dev/null; then
    IP=$(x86_64-w64-mingw32-objdump -p "$ISCA.dll" | grep -c "sqlite3_" || true)
    if [ "$IP" -eq 0 ]; then
        echo "  [ x ] a guarda NAO enxerga nem a isca. Zero no DLL real nao prova nada."
        exit 1
    fi
    echo "  [ok] a isca foi vista ($IP simbolo(s)) — a guarda enxerga."
fi

N=$(x86_64-w64-mingw32-objdump -p "$AQUI/ConanShop.dll" | grep -c "sqlite3_\|MySqlCliente" || true)
if [ "$N" -ne 0 ]; then
    echo "  [ x ] o DLL exporta $N simbolo(s) interno(s) que deveria esconder."
    exit 1
fi
echo "  [ok] nenhum simbolo interno exportado"

x86_64-w64-mingw32-objdump -p "$AQUI/ConanShop.dll" | grep -q "ConanPluginCarregar" \
  && echo "  [ok] ConanPluginCarregar exportada" \
  || { echo "  [ x ] ConanPluginCarregar NAO foi exportada — o loader nao vai achar."; exit 1; }

echo
echo "  ✅ $AQUI/ConanShop.dll"
echo
echo "  INSTALAR (a pasta tem HIFEN: Conan-Api, nunca ConanApi)"
echo "    <servidor>/ConanSandbox/Binaries/Win64/Conan-Api/Plugins/ConanShop/"
echo "      ConanShop.dll"
echo "      PluginInfo.json"
echo "      config.json        <- o banco conanshop.db nasce aqui do lado"
