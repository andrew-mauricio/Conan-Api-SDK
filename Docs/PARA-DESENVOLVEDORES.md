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

## Versões: o que precisa bater e o que não precisa

Isto costuma ser a primeira dúvida, e a resposta contraria o que a maioria das
APIs de plugin de jogo obriga.

### Seu compilador NÃO precisa ser o mesmo que o nosso

Não existe biblioteca nossa para você linkar. O que atravessa a fronteira é uma
`struct` de ponteiros de função em **C puro**, com convenção `__cdecl` — e sobre
isso todo compilador concorda.

| você usa | funciona? |
|---|---|
| Visual Studio 2017, 2019, 2022 (v141, v142, v143) | sim |
| mingw-w64 (qualquer GCC recente) | sim |
| clang com alvo Windows | sim |

Em APIs que entregam biblioteca compilada, isso não é verdade: o layout de
`std::string` e de vtable muda entre MSVC e MinGW — e muda até entre versões do
MSVC. Quando não bate, não dá erro claro. Linka, roda, e corrompe memória na
primeira string que cruzar a fronteira. Aqui esse problema não existe porque
nada nosso é compilado dentro do seu plugin.

**O que precisa bater:** x64 e `/MT` (ou `-static-*` no MinGW). Isso não é sobre
compatibilidade com a gente — é sobre o servidor rodar sob Wine, onde o runtime
da Microsoft pode não estar instalado.

### A versão da TABELA, e o que ela significa

O header traz um número:

```c
#define CONAN_API_VERSAO 3
```

Ele sobe quando **funções novas são acrescentadas** — e elas entram sempre no
**fim** da struct. Nunca removemos nem reordenamos campo, e é isso que faz um
plugin compilado hoje continuar valendo amanhã.

Declare no seu `PluginInfo.json` a versão mínima de que você precisa:

```json
{ "FullName": "Meu Plugin", "Version": "1.0.0", "MinApiVersion": 3 }
```

| situação | o que acontece |
|---|---|
| você usa só o que existe na v2, e declara `"MinApiVersion": 2` | roda em qualquer API v2 ou superior |
| você usa `MensagemNaTela` (v3) mas declara 2 | o carregador deixa carregar, e o **seu** teste de `api->tamanho` é a última defesa |
| você declara 3 e o servidor tem a v2 | o carregador **recusa antes de carregar** e diz qual versão falta |

Por isso a checagem no começo do seu `ConanPluginCarregar` não é formalidade:

```cpp
if (!api || api->tamanho < sizeof(ConanApiTabela)) return;
```

Sem ela, um plugin compilado contra uma tabela maior lê ponteiro além do fim da
struct e chama lixo, num servidor com API mais velha.

### A versão da API e a BUILD DO JOGO

São coisas diferentes, e as duas aparecem em toda release:

```
Conan-Api — build 24784646
```

- **`1.1.0`** — a versão do projeto
- **`build 24784646`** — a versão do **Conan Exiles** para a qual esta API serve

A API conhece o jogo por endereços de memória daquela build. Quando a Funcom
atualizar, esses endereços mudam de lugar e a API **se recusa a carregar**, de
propósito, dizendo isso no log.

**E o seu plugin, precisa ser recompilado quando isso acontecer?** Normalmente
**não** — você fala com a tabela, e a tabela não muda de forma. Quem é regerada
é a API.

A exceção: se o seu plugin usa **offset cru do jogo** (como o `0x068` do
`ChatRpcData` no exemplo de chat), esse número pode ter mudado, e aí você precisa
conferir. Quanto mais você usar as funções da tabela em vez de offsets diretos,
menos a atualização do jogo te afeta.

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

### O Permission pode estar em SQLite **ou em MySQL**, e você não tem como saber

O dono do servidor escolhe onde os dados moram, com uma linha no `config.json`
dele (`"Database": "sqlite"` ou `"mysql"`). Isso é decisão dele, não sua, e o
seu plugin **não deve ter opinião nem código a respeito**: a ABI é a mesma nos
dois casos, com os mesmos nomes, os mesmos tipos e a mesma semântica.

O que isso te obriga a fazer é **uma coisa só**, e é a que costuma faltar:

> **"Ausente" não é só "não instalado". É um estado que vai e volta enquanto o
> servidor roda.**

Com SQLite, se o Permission carregou, ele responde — o arquivo é local e não
cai. Com **MySQL**, o banco fica em outra máquina, e a rede do dono não é
problema seu nem dele: ela cai. Quando cai, o `ConanPermissionObterApi`
devolve `nullptr` — **ausente** — e os auxiliares do header devolvem o
`se_ausente` que você passou. Quando a conexão volta (o Permission tenta
sozinho, em segundo plano), a chamada seguinte volta a responder normalmente.

Ou seja: o mesmo jogador pode responder `1` numa hora, `se_ausente` na seguinte
e `1` de novo depois — **sem nada de errado com o seu plugin**.

```cpp
// CERTO: decide na hora, e o valor de ausência é uma escolha SUA e consciente
if (ConanPermTem(id, "meuplugin.kit.diario", /*se_ausente=*/0) == 1)
    DarKit(controller);

// ERRADO: guarda a resposta para sempre. Se calhar de perguntar durante uma
// queda do MySQL, este jogador fica sem VIP até o servidor reiniciar.
if (!ja_perguntei) { ehVip = ConanPermTem(id, "vip.kit", 0); ja_perguntei = true; }
```

**Escolha o `se_ausente` pelo custo do erro, não por reflexo:**

| o que a permissão libera | `se_ausente` sensato | por quê |
|---|---|---|
| um kit, um teleporte, um bônus de VIP | `0` (nega) | negar por 30 s incomoda; dar de graça a todo mundo durante uma queda, não desfaz |
| um comando destrutivo de admin (`!wipe`, `!ban`) | `0` (nega) | sempre |
| algo que só *esconde* informação cosmética | `1` (libera) | o custo de errar é zero |

Nunca trate `-1` como "negado". Quem chama `a->tem()` direto na tabela recebe
`-1` para "não sei"; os auxiliares do header já traduzem isso para o seu
`se_ausente`, e é por isso que eles existem. Tratar `-1` como `0` na mão tira o
VIP de quem pagou por ele, justamente no minuto em que o banco do dono está com
problema.

### O que você **não** pode fazer

**Não abra o `permission.db` por conta própria.** É tentador — é um SQLite ali
do lado, e nada te impede tecnicamente (não há fronteira entre plugins; veja
`_fronteira` no `config.json` do Permission). Mas:

1. **o arquivo pode não existir.** Se o dono está no MySQL, não há `.db` nenhum,
   e o seu plugin passa a funcionar só na metade dos servidores;
2. **o esquema é interno e muda sem aviso.** A ABI é o contrato; as tabelas não;
3. **você não veria o cache.** O Permission responde a partir de um instantâneo
   publicado em memória, e escreve pela linha de trabalho dele. Ler o arquivo
   por fora te dá uma foto desencontrada da que todo mundo está usando.

**Não guarde nada seu dentro do banco do Permission**, nos dois meios. Use
`g_api->CaminhoDados("SeuPlugin", "seubanco.db")` e tenha o seu.

**Não chame a ABI esperando que ela vá à rede.** Ela não vai: consulta lê o
instantâneo em memória, é barata e pode ser chamada do laço do jogo. Quem fala
com o MySQL é a linha de trabalho do Permission, nunca a sua nem a do jogo — foi
assim que se garantiu que um banco fora do ar não vira jogador desconectado. Se
algum dia uma consulta sua parecer lenta, o problema não é o banco do dono.

**Se você distribui um plugin que usa o Permission**, diga no seu LEIA-ME que
ele funciona com os dois — e teste com o Permission **ausente** pelo menos uma
vez. É o caminho que ninguém exercita e é o que roda no pior dia do dono.

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
