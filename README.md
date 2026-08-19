<p align="center">
  <img src=".github/imagens/conan-header.jpg" alt="Conan Exiles Enhanced">
</p>

<p align="center">
  <a href="README.md"><img src=".github/imagens/bandeiras/br.png" alt="Portugues" height="13">&nbsp;<b>Portugu&ecirc;s</b></a>
  &nbsp;&nbsp;&middot;&nbsp;&nbsp;
  <a href="Docs/README.en.md"><img src=".github/imagens/bandeiras/us.png" alt="English" height="13">&nbsp;English</a>
  &nbsp;&nbsp;&middot;&nbsp;&nbsp;
  <a href="Docs/README.es.md"><img src=".github/imagens/bandeiras/es.png" alt="Espanol" height="13">&nbsp;Espa&ntilde;ol</a>
</p>

# Conan-Api SDK — escreva plugins para Conan Exiles

Você precisa de **um header** e de um compilador C++. Não tem biblioteca para
linkar, não tem código nosso para compilar junto, não tem projeto para
configurar.

```cpp
#include "Conan/ConanPluginApi.h"

static const ConanApiTabela* g_api = nullptr;

extern "C" ConanAcao AoFalar(ConanChamada* c)
{
    char texto[512];
    g_api->LerTextoDoJogo(c->Parms, 0x068, texto, sizeof(texto));

    if (texto[0] != '!') return CONAN_CONTINUAR;   // conversa normal, deixa passar

    g_api->Log("alguém digitou: %s", texto);
    return CONAN_CANCELAR;                          // engole a mensagem
}

extern "C" __declspec(dllexport)
void ConanPluginCarregar(const ConanApiTabela* api)
{
    if (!api || api->tamanho < sizeof(ConanApiTabela)) return;
    g_api = api;
    g_api->HookProcessEvent("ServerSendChatMessage", AoFalar, nullptr, 100);
}
```

Compile como DLL x64, ponha numa pasta com o nome do seu plugin dentro de
`Conan-Api/Plugins/`, reinicie o servidor. Acabou.

---

## Por que o seu compilador não importa

A maioria das APIs de plugin te obriga a usar exatamente o compilador que elas
usaram. O motivo é real: biblioteca C++ não atravessa compilador. O layout de
`std::string` e de vtable muda entre MSVC e MinGW, e muda até entre versões do
MSVC. Quando não bate, não dá erro claro — linka, roda, e corrompe memória na
primeira string que cruzar a fronteira.

Aqui isso não acontece, porque **você não linka nada nosso**. O carregador chama
o seu plugin passando uma tabela de ponteiros de função, e você chama tudo por
`api->`:

```mermaid
flowchart LR
    A[Servidor do jogo] --> B[Carregador]
    B -- "passa a tabela" --> C[Seu plugin.dll]
    C -- "api-&gt;HookProcessEvent(...)" --> D[Motor da Conan-Api]
    D --> A
    style C fill:#2d5016,color:#fff
    style D fill:#1a3a52,color:#fff
```

Tudo em C puro: `struct` de ponteiros de função, convenção `__cdecl`. Visual
Studio de qualquer versão, MinGW, clang — todos concordam sobre isso.

**Um efeito colateral que vale saber:** como o motor mora do nosso lado, quando
corrigimos um defeito nele, o seu plugin não precisa ser recompilado.

---

## Visual Studio

1. **Novo Projeto** → *Biblioteca de Vínculo Dinâmico (DLL)*
2. **C/C++ → Geral → Diretórios de Inclusão Adicionais**: aponte para `include`
3. **C/C++ → Geração de Código → Biblioteca de Runtime**: `/MT`, não `/MD`
4. **Plataforma: x64**

O `/MT` importa. Com `/MD`, sua DLL depende do runtime da Microsoft instalado na
máquina — e o servidor roda sob Wine, num contêiner onde esse runtime pode não
existir. O sintoma é `LoadLibrary` falhando com um código genérico que não
explica nada. Com `/MT`, o runtime vai dentro da sua DLL.

Não há nada para linkar: sem `.lib`, sem `.a`, sem adicionar `.cpp` nosso.

## mingw-w64

```bash
x86_64-w64-mingw32-g++ -std=c++17 -O2 -shared \
    -I caminho/para/include \
    -o MeuPlugin.dll MeuPlugin.cpp \
    -static-libgcc -static-libstdc++
```

Os `-static-*` pelo mesmo motivo do `/MT`.

---

## Comece copiando um exemplo

```bash
cp -r Exemplos/ExemploOla MeuPlugin
cd MeuPlugin
# renomeie o .cpp e ajuste as duas linhas do compilar.sh que citam ExemploOla
./compilar.sh
```

| exemplo | o que ele ensina |
|---|---|
| **ExemploOla** | o menor plugin que ainda prova algo: achar objeto, chamar função, escrever no log |
| **ExemploComando** | interceptar `!comando` no chat e engolir a mensagem |
| **ExemploVip** | consultar o Permission e continuar funcionando quando ele não está instalado |
| **ExemploAgendado** | rodar código de tempos em tempos, na thread certa |
| **ExemploBlueprint** | interceptar execução de Blueprint — o que o hook por nome não vê. **Leia o aviso no topo do arquivo antes de copiar**: a versão anterior dele deixava uma janela em que o detour rodava sem o ponteiro original, e nessa janela toda execução de Blueprint era descartada em silêncio — como o login do Conan é feito de Blueprint, ninguém entrava no servidor |
| **Permission** | o plugin completo: banco, configuração, e uma ABI que outros consomem |

---

## A estrutura de um plugin

```
Conan-Api/Plugins/MeuPlugin/
   MeuPlugin.dll        <- o carregador procura este nome primeiro
   PluginInfo.json      <- nome, versão, o que você exige (opcional, mas faça)
   config.json          <- a sua configuração
   meubanco.db          <- o que você gravar nasce aqui
```

O `PluginInfo.json` é o cartão de identidade:

```json
{
  "FullName":      "Meu Plugin",
  "Description":   "O que ele faz, em uma linha",
  "Version":       "1.0.0",
  "MinApiVersion": 2,
  "Dependencies":  ["Permission"]
}
```

`MinApiVersion` faz o carregador **recusar** seu plugin numa API velha demais,
em vez de deixá-lo rodar e ler lixo. A tabela desta versão é a **v4** — declare
o menor número de que você realmente precisa, não o mais alto: quem pede v4 sem
usar nada da v4 se recusa a rodar em servidor que ainda está na v3, sem motivo.

Campo novo entra sempre no **fim** da tabela, e nada é removido nem reordenado.
Isso está exercitado: um plugin compilado contra a v3 (328 bytes de tabela) foi
carregado sobre a API v4 (344 bytes) num servidor de verdade, chamou uma função
e o servidor seguiu de pé. Seu plugin publicado continua funcionando quando a
tabela cresce. `Dependencies` garante que o Permission
suba antes de você — sem isso, você perguntaria a ele antes de ele existir, e
concluiria que não está instalado. (Isso aconteceu de verdade aqui.)

**Guarde tudo pela API**, nunca por caminho relativo:

```cpp
const char* banco = g_api->CaminhoDados("MeuPlugin", "meubanco.db");
```

Caminho relativo é resolvido a partir do diretório do **servidor**, não da sua
pasta. Um plugin nosso já gravou 9 MB por boot no lugar errado assim.

---

## Usar o Permission

Sem linkar nada — é `GetProcAddress` por baixo, num header pequeno:

```cpp
#include "Conan/ConanPermission.h"

char id[64];
if (ConanPermIdDoController(controller, id, sizeof(id)) > 0)
    if (ConanPermTem(id, "meuplugin.kit.diario", /*se_ausente=*/0) == 1)
        DarKit(controller);
```

Se o Permission não estiver instalado, as funções devolvem o valor `se_ausente`
que você passou e o seu plugin continua rodando. **Pergunte no momento do uso,
não no carregamento** — carregar é cedo demais.

---

## O que a API não deixa você fazer, e por quê

**Recusar hook.** `HookFuncao` recusa cerca de 32% dos endereços, com o motivo
em `TextoRecusa`. Isso não é falha: é a API se negando a instalar um desvio que
um dia executaria meia instrução, corrompendo memória horas depois, num lugar
sem relação com a causa.

**Passar tamanho errado.** `ChamarFuncao` confere o tamanho de cada argumento
contra o parâmetro real e recusa quando não bate. Se você passar um `float` onde
o jogo espera `double`, ela para e diz. Medimos: **293 funções** desta build
corrompem a pilha por esse caminho, e o sintoma aparece longe da causa.

**Montar você mesmo uma string do jogo.** O jogo destrói o bloco de parâmetros
ao retornar e chama o alocador **dele** sobre o ponteiro que estiver lá. Se for
memória sua, o servidor cai — testado, não é teoria.

Você não precisa fazer isso: **escreva texto normal e a API monta**.

```cpp
pc->ClientHUDShowNotification("Bem-vindo ao servidor!", true, true);
```

Por baixo, a API pede a `FString` (ou o `FText`) ao **próprio jogo** e devolve o
que ele construiu. Nenhuma memória sua atravessa a fronteira, e quem aloca é
quem libera.

---

## O jogo inteiro, com assinatura de verdade

O `ConanSDK.h` traz **9.228 classes** do Conan com os membros e as funções que a
reflexão do jogo declara. Não é lista de nomes: **85% das 36.750 funções têm
assinatura completa** — tipo e nome de cada parâmetro, conferidos contra o
próprio servidor rodando.

```cpp
#include "Conan/ConanSDK.h"

cm->TeleportPlayer(1000.0f, 2000.0f, 300.0f);     // ponto de salvamento
FVector onde = ator->K2_GetActorLocation();        // onde ele está
cm->CheatSpawnItem(TemplateId, quantidade);        // item para a loja
pc->ClientHUDShowNotification("Bem-vindo!", true, true);
```

Parâmetro de **saída** vira referência, e a API copia o valor de volta:

```cpp
FHitResult batida{};
ator->K2_SetActorLocation(destino, false, batida, true);
```

Lista de saída vira ponteiro, capacidade e contagem — com os **elementos**
copiados, nunca o ponteiro do jogo (que morre quando a chamada retorna):

```cpp
AActor* achados[32]; int quantos = 0;
lib->AlgumaBuscaDeAtores(..., achados, 32, quantos);
```

Os 15% restantes saem como template genérico, e isso é deliberado: são tipos que
**carregam posse de memória do jogo** (`TArray<FString>`, `TMap`, delegates
multicast). Passá-los por valor duplicaria ponteiros, e alguém liberaria duas
vezes. Preferimos template sem tipo a assinatura que corrompe.

---

![O Exílio](.github/imagens/conan-3.jpg)

## Publicou um plugin?

Antes de publicar, confira:

- [ ] compila em **x64**, com `/MT` (MSVC) ou `-static-*` (MinGW)
- [ ] a pasta tem o nome do plugin, e a DLL também
- [ ] tudo que ele grava passa por `CaminhoDados("SeuPlugin", ...)`
- [ ] confere `api->tamanho` antes de usar a tabela
- [ ] `DllMain` não faz nada (ali o Windows segura uma trava global)
- [ ] tem `PluginInfo.json` com versão e `MinApiVersion`
- [ ] **rodou num servidor de verdade** — compilar não é funcionar

---

## Para rodar um servidor

É outro repositório: **[Conan-Api](../../../Conan-Api)** — o carregador, os
plugins prontos e como instalar.

---

## Créditos

*Conan Exiles* é da **Funcom**. As imagens são material de divulgação oficial do
Steam. Este projeto é independente e não tem vínculo com a Funcom nem com a
Inflexion Games.

<p align="center">
  <a href="README.md"><img src=".github/imagens/bandeiras/br.png" alt="Portugues" height="13">&nbsp;<b>Portugu&ecirc;s</b></a>
  &nbsp;&nbsp;&middot;&nbsp;&nbsp;
  <a href="Docs/README.en.md"><img src=".github/imagens/bandeiras/us.png" alt="English" height="13">&nbsp;English</a>
  &nbsp;&nbsp;&middot;&nbsp;&nbsp;
  <a href="Docs/README.es.md"><img src=".github/imagens/bandeiras/es.png" alt="Espanol" height="13">&nbsp;Espa&ntilde;ol</a>
</p>
