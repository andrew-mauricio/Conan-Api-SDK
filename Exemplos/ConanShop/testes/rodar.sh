#!/bin/bash
# Runs the ConanShop suite UNDER WINE — the real environment.
#
# WHY UNDER WINE, AND NOT ON NATIVE LINUX
# ---------------------------------------
# The plugin runs inside Conan's Windows server, which on this VPS runs under
# Wine. Testing the logic on native Linux would prove it works somewhere it
# isn't going to run — and the differences that matter (SQLite opening a file,
# how threads get scheduled in the race) are exactly the ones that change
# between the two.
#
# WHAT EACH ONE PROVES
#   controle_positivo  that teste_pontos KNOWS HOW TO FAIL (see below)
#   teste_pontos       the wallet: nobody spends what they don't have, or twice
#   teste_config       a broken config does NOT replace a good one
#   teste_texto        !shop doesn't swallow !shopreload; the owner's format
#                      isn't printf
#
# THE POSITIVE CONTROL IS NOT DECORATION. In the first version teste_pontos
# passed and the positive control FAILED: with the test's own mutex wrapped
# around it, even the broken implementation passed. The instrument was blind and
# the "PASSED" measured nothing. Until the positive control passes, ignore the
# rest.
set -e
AQUI="$(cd "$(dirname "$0")" && pwd)"
PLUG="$AQUI/.."
OBJ="$PLUG/build"
mkdir -p "$OBJ"

COMUM=""
for c in "$PLUG/../comum" "$PLUG/../../plugins/comum" "$PLUG/comum"; do
    [ -f "$c/MySqlCliente.h" ] && { COMUM="$c"; break; }
done
[ -z "$COMUM" ] && { echo "  x nao achei plugins/comum/ (MySqlCliente.h)"; exit 1; }

[ -f "$OBJ/sqlite3.o" ] || {
  echo "== sqlite3.c (uma vez, ~75 s) =="
  x86_64-w64-mingw32-gcc -c "$COMUM/terceiros/sqlite3/sqlite3.c" -o "$OBJ/sqlite3.o" \
    -O2 -DSQLITE_THREADSAFE=1 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_DEFAULT_MEMSTATUS=0 \
    -DSQLITE_OMIT_DEPRECATED -DSQLITE_DQS=0 -DSQLITE_ENABLE_JSON1 \
    -DSQLITE_MAX_EXPR_DEPTH=0 -DSQLITE_DEFAULT_FOREIGN_KEYS=1
}

echo "== compilando a bateria =="
x86_64-w64-mingw32-g++ -std=c++17 -O2 -Wall -Wextra -I "$PLUG" -I "$COMUM" \
  -o "$OBJ/teste_pontos.exe" \
  "$AQUI/teste_pontos.cpp" "$PLUG/Pontos.cpp" "$COMUM/MySqlCliente.cpp" "$OBJ/sqlite3.o" \
  -static-libgcc -static-libstdc++ -static -lws2_32

x86_64-w64-mingw32-g++ -std=c++17 -O2 -Wall -Wextra -I "$COMUM" \
  -o "$OBJ/controle_positivo.exe" \
  "$AQUI/controle_positivo.cpp" "$OBJ/sqlite3.o" \
  -static-libgcc -static-libstdc++ -static

x86_64-w64-mingw32-g++ -std=c++17 -O2 -Wall -Wextra -I "$PLUG" -I "$COMUM" \
  -o "$OBJ/teste_config.exe" \
  "$AQUI/teste_config.cpp" "$PLUG/Config.cpp" "$OBJ/sqlite3.o" \
  -static-libgcc -static-libstdc++ -static

x86_64-w64-mingw32-g++ -std=c++17 -O2 -Wall -Wextra -I "$PLUG" \
  -o "$OBJ/teste_texto.exe" "$AQUI/teste_texto.cpp" \
  -static-libgcc -static-libstdc++ -static

# ── where to run ────────────────────────────────────────────────────────────
#
# It takes a local wine; if there isn't one, it tries the server container's,
# which is where this VPS's Wine lives. With neither, it STOPS with exit code 2
# — rather than "skipping the tests" and returning success, which is the most
# common way a suite lies. Code 2 means I DIDN'T CHECK, and that isn't a pass.
RODAR=""
if command -v wine >/dev/null 2>&1; then
    RODAR="local"
elif command -v docker >/dev/null 2>&1 && docker ps --format '{{.Names}}' 2>/dev/null | grep -q '^conan_Server$'; then
    RODAR="container"
else
    echo "  x nao ha wine local nem o container conan_Server de pe."
    echo "    Os testes NAO rodaram. Isto NAO e' aprovacao."
    exit 2
fi

executar() {   # $1 = exe, $2... = argumentos
    exe="$1"; shift
    if [ "$RODAR" = "local" ]; then
        ( cd "$OBJ" && WINEDEBUG=-all wine "./$exe" "$@" 2>/dev/null )
    else
        # A pasta ~/conan/logs esta montada dentro do container.
        ponte="/home/andrew/conan/logs"
        sudo -n install -o conanlink -g conanlink -m 755 "$OBJ/$exe" "$ponte/" 2>/dev/null \
          || cp "$OBJ/$exe" "$ponte/"
        docker exec conan_Server sh -c \
          "cd /home/conan/logs && WINEDEBUG=-all timeout 300 wine ./$exe $* 2>/dev/null"
    fi
}

FALHOU=0
NAO_CONFERI=0
set +e
# Each test wants a different argument, and passing the wrong one makes the test
# fail for a reason that isn't its own — which happened here: teste_config got a
# DATABASE name where it expected a FOLDER, and reported "nao consegui abrir
# bom.json" as if the parser were broken.
for t in controle_positivo teste_pontos teste_config teste_texto; do
    echo
    echo "══════════════ $t ══════════════"
    case "$t" in
        teste_config) executar "$t.exe" "." ;;    # pasta onde escrever os .json
        teste_texto)  executar "$t.exe"     ;;    # nao usa argumento
        *)            executar "$t.exe" "$t.db" ;;
    esac
    rc=$?
    # ── FAILED vs COULDN'T RUN ──────────────────────────────────────────────
    #
    # The previous version did `else FALHOU=1` — any non-zero code became
    # FAILED. But the tests return 1 when they fail, and everything else comes
    # from OUTSIDE them:
    #
    #   124  the `timeout 300` expired
    #   125  docker couldn't execute
    #   126  the binary isn't executable
    #   127  command not found (did wine disappear?)
    #   137  killed by a signal (OOM, kill)
    #
    # That matters because these tests run INSIDE the game server's container,
    # sharing the wineserver with Conan. With the server under load, wine
    # sometimes doesn't answer — and the suite said "FAILED", sending you to
    # look for a defect that doesn't exist.
    #
    # Measured on 2026-08-20: 1 failure in ~10 runs, with no code change between
    # them. A test that fails sometimes is worse than one that fails always: it
    # teaches you to ignore the failure.
    #
    # Now: 0 = passed · 1 = REALLY failed · anything else = I DIDN'T CHECK,
    # which is neither a pass nor a failure.
    case $rc in
        0) echo "   -> passou" ;;
        1) echo "   -> REPROVOU"; FALHOU=1 ;;
        *) echo "   -> NAO CONSEGUI RODAR (codigo $rc — timeout, docker ou wine)."
           echo "      Isto NAO e' reprovacao, e tambem NAO e' aprovacao."
           NAO_CONFERI=1 ;;
    esac
done
set -e

# ── leave no trace in the server's folder ───────────────────────────────────
#
# When it runs through the container, the bridge is `~/conan/logs`, which is a
# REAL server folder mounted inside it. The .exe, .db and .json files
# teste_config writes were being left behind — 5 MB of our rubbish in a folder
# the server owner opens to read LOGS. Whoever made them cleans them up.
if [ "$RODAR" = "container" ]; then
    ponte="/home/andrew/conan/logs"
    for t in controle_positivo teste_pontos teste_config teste_texto; do
        sudo -n rm -f "$ponte/$t.exe" "$ponte/$t.db" \
                      "$ponte/$t.db-wal" "$ponte/$t.db-shm" 2>/dev/null || true
    done
    # os .json que o teste_config escreve para exercitar o parser
    for j in bom quebrado semid mysqll iguais misto limites min0; do
        sudo -n rm -f "$ponte/$j.json" 2>/dev/null || true
    done
    sudo -n rm -f "$ponte/cp.db" 2>/dev/null || true
fi

echo
if [ "$FALHOU" != "0" ]; then
    echo "  ❌ bateria REPROVADA"
    exit 1
fi
if [ "$NAO_CONFERI" != "0" ]; then
    echo "  ⚠  NAO CONSEGUI CONFERIR tudo (algum teste nao rodou)."
    echo "     Isto NAO e' aprovacao. Tente de novo com o servidor mais calmo,"
    echo "     ou rode com wine local em vez do container."
    exit 2
fi
echo "  ✅ bateria completa APROVADA (rodada: $RODAR)"
exit 0
