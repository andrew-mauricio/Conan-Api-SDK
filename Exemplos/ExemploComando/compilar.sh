#!/bin/bash
# Compila o plugin. Nada além do compilador cruzado do MinGW é necessário —
# quem for da comunidade não precisa de Visual Studio nem do editor da Unreal.
#
# Note o que NÃO está aqui: não linka libconanapi.a e não compila nenhum .cpp
# nosso. O plugin só inclui ConanPluginApi.h (declarações) e recebe a tabela de
# funções pronta em tempo de execução. É por isso que o compilador deixou de
# importar: não há biblioteca C++ nossa atravessando a fronteira.
set -e
AQUI="$(cd "$(dirname "$0")" && pwd)"
API="$AQUI/../../api"
x86_64-w64-mingw32-g++ -std=c++17 -O2 -shared \
    -I "$API/include" \
    -o "$AQUI/ExemploComando.dll" \
    "$AQUI/ExemploComando.cpp" \
    -static-libgcc -static-libstdc++ -Wl,--enable-stdcall-fixup
echo "  ✅ $AQUI/ExemploComando.dll"
