#!/bin/bash
# Compila o plugin. Nada alem do compilador cruzado do MinGW e' necessario —
# quem for da comunidade nao precisa de Visual Studio nem do editor da Unreal.
#
# NAO HA NADA NOSSO PARA LINKAR. Ate 17/08/2026 esta linha juntava
# `libconanapi.a` e arrastava o motor inteiro para dentro do DLL do plugin. No
# modelo de tabela o unico arquivo nosso que entra e' o header
# `Conan/ConanPluginApi.h`, que e' so declaracao: por isso aqui so existe `-I`.
#
# OS INCLUDES SAO PROCURADOS, NUNCA ASSUMIDOS. Este mesmo arquivo roda em dois
# lugares de formato diferente: a arvore de desenvolvimento (plugins/X/) e o SDK
# que a comunidade baixa (Exemplos/X/). Ate a v2.3.0 ele trazia so' o caminho da
# arvore, entao o PRIMEIRO comando do guia morria com "ConanPluginApi.h: No such
# file" para todo mundo que baixou — e aqui dentro passava.
#
# O ConanPermission.h merece busca propria porque mora em lugar DIFERENTE nos
# dois: no SDK vai junto dos outros headers; na arvore fica com o plugin que o
# publica (plugins/Permission/include). Reescrevi este script uma vez esquecendo
# disso e quebrei o ExemploVip na arvore, sem quebrar no pacote.
set -e
AQUI="$(cd "$(dirname "$0")" && pwd)"

INC=""
for c in "$AQUI/../../include" "$AQUI/../../api/include" "$AQUI/../include" "$CONAN_SDK_INCLUDE"; do
    [ -n "$c" ] && [ -f "$c/Conan/ConanPluginApi.h" ] && { INC="$c"; break; }
done
if [ -z "$INC" ]; then
    echo "  x nao achei Conan/ConanPluginApi.h. Procurei a partir de: $AQUI"
    echo "    Aponte com:  CONAN_SDK_INCLUDE=/caminho/do/sdk/include ./compilar.sh"
    exit 1
fi

# Segundo -I so' quando o header do Permission NAO esta junto do principal.
PERM=""
if [ ! -f "$INC/Conan/ConanPermission.h" ]; then
    for c in "$AQUI/../Permission/include" "$AQUI/../../plugins/Permission/include" \
             "$AQUI/../../Exemplos/Permission/include"; do
        [ -f "$c/Conan/ConanPermission.h" ] && { PERM="-I $c"; break; }
    done
fi

x86_64-w64-mingw32-g++ -std=c++17 -O2 -shared \
    -I "$INC" $PERM \
    -o "$AQUI/ExemploComando.dll" \
    "$AQUI/ExemploComando.cpp" \
    -static-libgcc -static-libstdc++ -Wl,--enable-stdcall-fixup
echo "  ✅ $AQUI/ExemploComando.dll"
