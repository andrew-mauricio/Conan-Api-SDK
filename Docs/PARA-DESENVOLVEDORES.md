# Escrever um plugin para a Conan-Api

## O resumo, para quem tem pressa

Você precisa de **um header** e de um compilador C++. Só isso.

```cpp
#include "Conan/ConanPluginApi.h"

static const ConanApiTabela* g_api = nullptr;

extern "C" ConanAcao AoFalar(ConanChamada* c)
{
    char texto[512];
    g_api->LerTextoDoJogo(c->Parms, 0x068, texto, sizeof(texto));
    if (texto[0] != '!') return CONAN_CONTINUAR;      // conversa normal passa

    g_api->Log("comando: %s", texto);
    return CONAN_CANCELAR;                            // engole a mensagem
}

extern "C" __declspec(dllexport)
void ConanPluginCarregar(const ConanApiTabela* api)
{
    if (!api || api->tamanho < sizeof(ConanApiTabela)) return;
    g_api = api;
    g_api->Log("meu plugin subiu");
    g_api->HookProcessEvent("ServerSendChatMessage", AoFalar, nullptr, 100);
}
```

Compile como DLL, ponha numa pasta com o nome do seu plugin dentro de
`Conan-Api/Plugins/`, reinicie o servidor. Pronto.

---

## Não há biblioteca para linkar. Nem fonte nosso para compilar.

Isto é diferente da maioria das APIs de plugin, e é de propósito.

O seu plugin **recebe** a API: o carregador chama `ConanPluginCarregar` passando
um ponteiro para uma tabela de funções. Você chama tudo por `api->`. Nenhuma
linha do nosso motor entra no seu binário.

**O que isso te dá:**

| | |
|---|---|
| **seu compilador não importa** | é C puro. Visual Studio de qualquer versão, MinGW, clang |
| **sem inferno de ABI** | não existe `std::string` nossa cruzando a fronteira para corromper heap |
| **atualização não te obriga a recompilar** | campos novos entram no fim da tabela e `versao` sobe |
| **menos código seu para dar errado** | o decodificador de instruções e a mesa de hooks ficam do nosso lado |

**Sempre confira o cabeçalho** antes de usar a tabela:

```cpp
if (!api || api->tamanho < sizeof(ConanApiTabela)) return;
```

Um plugin compilado contra uma tabela maior, rodando numa API mais velha, leria
ponteiro além do fim da struct e chamaria lixo. Nós nunca removemos nem
reordenamos campo — só acrescentamos no fim — mas a checagem é sua rede.

---

## Visual Studio

1. **Novo Projeto** → *Biblioteca de Vínculo Dinâmico (DLL)*
2. **Propriedades → C/C++ → Geral → Diretórios de Inclusão Adicionais**:
   aponte para a pasta `include` do SDK que você baixou
3. **Propriedades → C/C++ → Geração de Código → Biblioteca de Runtime**: `/MT`
   (Multi-thread), **não** `/MD`
4. **Plataforma: x64.** O servidor é 64 bits; uma DLL 32 bits não carrega e o
   erro não diz por quê.

**Por que `/MT` e não `/MD`:** `/MD` faz a sua DLL depender do runtime da
Microsoft instalado na máquina. O servidor roda sob Wine, num contêiner onde
esse runtime pode não existir — e o sintoma é `LoadLibrary` falhando com um
código genérico. Com `/MT` o runtime vai dentro da sua DLL.

Não há nada para linkar. Sem `.lib`, sem `.a`, sem adicionar `.cpp` nosso ao
projeto.

## mingw-w64 (Linux, macOS ou Windows)

```bash
x86_64-w64-mingw32-g++ -std=c++17 -O2 -shared \
    -I caminho/para/o-sdk/include \
    -o MeuPlugin.dll MeuPlugin.cpp \
    -static-libgcc -static-libstdc++
```

`-static-libgcc -static-libstdc++` pelo mesmo motivo do `/MT`: nada de depender
de DLL do compilador que não vai estar no servidor.

---

## Do zero ao plugin rodando

```bash
cp -r Exemplos/ExemploOla MeuPlugin
cd MeuPlugin
# renomeie o .cpp e ajuste as duas linhas do compilar.sh que citam ExemploOla
./compilar.sh
```

Saiu `MeuPlugin.dll`. Para instalar, copie a **pasta** `MeuPlugin/` (com a DLL
dentro) para `Conan-Api/Plugins/` do servidor e reinicie. O nome da pasta e o
nome da DLL devem bater — é assim que o carregador escolhe qual carregar.

## A estrutura de um plugin

Cada plugin é **uma pasta**, com tudo dentro:

```
Conan-Api/Plugins/MeuPlugin/
   MeuPlugin.dll        <- o carregador procura este nome primeiro
   config.json          <- CaminhoConfig("MeuPlugin")
   meubanco.db          <- CaminhoDados("MeuPlugin", "meubanco.db")
```

O usuário instala arrastando a pasta. Desinstala apagando a pasta.

**Se houver mais de uma `.dll` na pasta**, o carregador usa a que tem o nome da
pasta. Havendo só uma, usa essa. Havendo duas e nenhuma com o nome da pasta, ele
**recusa** e diz no log qual renomear — escolher "a primeira" seria uma decisão
invisível que muda com a ordem do sistema de arquivos.

**Guarde tudo pela API**, nunca por caminho relativo:

```cpp
const char* banco = g_api->CaminhoDados("MeuPlugin", "meubanco.db");
```

Caminho relativo é resolvido a partir do diretório de trabalho do **servidor**,
não da sua pasta. Um plugin já gravou 9 MB por boot no lugar errado assim.

---

## Usar o Permission (VIP, grupos, permissões)

O `Permission` é o plugin padrão do pacote. Ele guarda quem tem o quê, e outros
plugins consultam por uma ABI C — **sem linkar nada**, por `GetProcAddress`:

```cpp
#include "Conan/ConanPermission.h"      // header-only, ~100 linhas

char id[64];
if (ConanPermIdDoController(controller, id, sizeof(id)) > 0)
    if (ConanPermTem(id, "meuplugin.kit.diario", /*se_ausente=*/0) == 1)
        DarKit(controller);
```

**Se o Permission não estiver instalado**, as funções devolvem o valor
`se_ausente` que você passou, e o seu plugin continua funcionando. Ele degrada,
não quebra.

**Consulte no momento do USO, não no carregamento.** Perguntar dentro de
`ConanPluginCarregar` pode acontecer antes de o Permission ter subido, e você
conclui, de boa-fé, que ninguém o instalou. (Aconteceu aqui: o `ExemploVip`
anunciava "Permission não está instalado" num servidor onde ele estava.)

---

## O contrato

```cpp
extern "C" __declspec(dllexport)
void ConanPluginCarregar(const ConanApiTabela* api);   // obrigatória

extern "C" __declspec(dllexport)
void ConanPluginDescarregar(void);                     // opcional
```

**Não faça trabalho no `DllMain`.** Ali o carregador do Windows segura uma trava
global, e chamar quase qualquer coisa trava o processo inteiro. Faça tudo no
`ConanPluginCarregar`.

---

## O que a API NÃO deixa você fazer, e por quê

**Mandar texto para o jogo.** Você pode ler texto que já é do jogo
(`LerTextoDoJogo`) e alterar o que passa por um hook. Mas montar uma `FString`
sua e passá-la ao jogo **derruba o servidor**: `ProcessEvent` destrói o bloco de
parâmetros ao retornar e chama o alocador *do jogo* sobre um ponteiro da sua
pilha. Isso foi testado, não é suposição. Para falar com o dono do servidor, use
`Log`.

**Hookar qualquer endereço.** `HookFuncao` **recusa** cerca de 32% das funções,
com o motivo em `TextoRecusa`. Recusa não é falha: é a API se negando a fazer
algo que executaria meia instrução um dia, corrompendo memória horas depois sem
erro legível.

---

## Antes de publicar

- [ ] compila em x64, com `/MT` (MSVC) ou `-static-*` (MinGW)
- [ ] a pasta tem o nome do plugin, e a DLL também
- [ ] tudo que ele grava passa por `CaminhoDados("SeuPlugin", ...)`
- [ ] confere `api->tamanho` antes de usar a tabela
- [ ] `DllMain` não faz nada
- [ ] se usa o Permission, consulta no uso e degrada quando ele falta
- [ ] rodou num servidor de verdade — 200 no `curl` não prova tela, e teste
      unitário não prova o caminho real
