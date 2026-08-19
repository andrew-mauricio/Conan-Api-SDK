#!/bin/bash
# Compila o plugin. Nada além do compilador cruzado do MinGW é necessário —
# quem for da comunidade não precisa de Visual Studio nem do editor da Unreal.
#
# Repare no que NÃO está aqui: nenhum .a nosso, nenhum .cpp nosso. O plugin usa
# só as declarações de ConanPluginApi.h e recebe a tabela de funções em tempo de
# execução. Por isso a linha de link não menciona libconanapi.a — se mencionasse,
# o motor entraria no binário de novo e o modelo de tabela perderia a razão de
# existir.
set -e
AQUI="$(cd "$(dirname "$0")" && pwd)"
API="$AQUI/../../api"
x86_64-w64-mingw32-g++ -std=c++17 -O2 -shared \
    -I "$API/include" \
    -o "$AQUI/ExemploBlueprint.dll" \
    "$AQUI/ExemploBlueprint.cpp" \
    -static-libgcc -static-libstdc++ -Wl,--enable-stdcall-fixup
echo "  ✅ $AQUI/ExemploBlueprint.dll"
