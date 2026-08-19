#!/bin/bash
# Compila o plugin. Nada além do compilador cruzado do MinGW é necessário —
# quem for da comunidade não precisa de Visual Studio nem do editor da Unreal.
#
# NÃO HÁ NADA NOSSO PARA LINKAR. Até 17/08/2026 esta linha juntava
# `$API/lib/libconanapi.a` e arrastava o motor inteiro para dentro do DLL do
# plugin. No modelo de tabela o único arquivo nosso que entra é o header
# `Conan/ConanPluginApi.h`, que é só declaração: por isso aqui só existe `-I`.
# Se algum dia esta linha voltar a citar um `.a` ou um `.cpp` nosso, o modelo
# foi quebrado — a promessa de "seu compilador não importa" morre junto.
set -e
AQUI="$(cd "$(dirname "$0")" && pwd)"
API="$AQUI/../../api"
x86_64-w64-mingw32-g++ -std=c++17 -O2 -shared \
    -I "$API/include" \
    -o "$AQUI/ExemploOla.dll" \
    "$AQUI/ExemploOla.cpp" \
    -static-libgcc -static-libstdc++ -Wl,--enable-stdcall-fixup
echo "  ✅ $AQUI/ExemploOla.dll"
