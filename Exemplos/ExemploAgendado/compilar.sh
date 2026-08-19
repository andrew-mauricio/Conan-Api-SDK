#!/bin/bash
# Compila o plugin. Nada além do compilador cruzado do MinGW é necessário —
# quem for da comunidade não precisa de Visual Studio nem do editor da Unreal.
#
# No modelo de tabela não há nada nosso para linkar nem para compilar junto:
# só o header de declarações entra, pelo -I. Se um dia voltar a aparecer uma
# biblioteca nossa (.a) ou um .cpp nosso na linha abaixo, o modelo foi quebrado.
set -e
AQUI="$(cd "$(dirname "$0")" && pwd)"
API="$AQUI/../../api"
x86_64-w64-mingw32-g++ -std=c++17 -O2 -shared \
    -I "$API/include" \
    -o "$AQUI/ExemploAgendado.dll" \
    "$AQUI/ExemploAgendado.cpp" \
    -static-libgcc -static-libstdc++ -Wl,--enable-stdcall-fixup
echo "  ✅ $AQUI/ExemploAgendado.dll"
