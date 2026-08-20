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

| você usa | funciona? | testado por nós |
|---|---|---|
| Visual Studio 2026 (`cl` 19.51, v145) | sim | **sim** — é o que usamos para validar cada release |
| Visual Studio 2017, 2019, 2022 (v141–v143) | sim | não diretamente; a ABI C não mudou entre eles |
| mingw-w64 (GCC 13+) | sim | **sim** — cada plugin do SDK compila com ele a cada build |
| clang com alvo Windows | sim | não diretamente |

O que testamos a cada versão é o `ExemploOla` **baixado do pacote publicado**,
compilado com MinGW e com MSVC, e os dois binários subindo no mesmo servidor.
Não é declaração de intenção: se um dos dois parar de funcionar, a release não
sai.

O comando MSVC que usamos, para você copiar:

```bat
cl /nologo /std:c++17 /O2 /EHsc /LD /MT ^
   /I "caminho\do\sdk\include" ^
   MeuPlugin.cpp /Fe:MeuPlugin.dll
```

Ou não digite nada: toda pasta de exemplo traz um **`compilar.bat`** que acha o
compilador que você tem e roda. Abra o *Prompt de Comando do Desenvolvedor x64
para VS* pelo menu Iniciar, entre na pasta do exemplo e rode. Ele procura o
`cl.exe` primeiro e cai no `g++` se você tiver o MinGW-w64 no Windows.

O `compilar.sh` ao lado é a mesma compilação para Linux e WSL. É com ele que
este projeto compila, e é por isso que ele está lá — no Windows você não precisa
dele.

Os exports saem sem decoração de nome (`ConanPluginCarregar`, não
`?ConanPluginCarregar@@YA...`) por causa do `extern "C"` no header, e a DLL
resultante depende só de `KERNEL32.dll` por causa do `/MT`. Se você vir
`MSVCP140.dll` ou `VCRUNTIME140.dll` na lista de dependências, o `/MT` não
pegou — e a DLL vai falhar ao carregar num servidor sob Wine.

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
#define CONAN_API_VERSAO 6
```

Ele sobe quando **funções novas são acrescentadas** — e elas entram sempre no
**fim** da struct. Nunca removemos nem reordenamos campo, e é isso que faz um
plugin compilado hoje continuar valendo amanhã.

Declare no seu `PluginInfo.json` a versão mínima de que você precisa:

```json
{ "FullName": "Meu Plugin", "Version": "1.0.0", "MinApiVersion": 6 }
```

| situação | o que acontece |
|---|---|
| você usa só o que existe na v2, e declara `"MinApiVersion": 2` | roda em qualquer API v2 ou superior |
| você usa `MensagemNaTela` (v3) mas declara 2 | o carregador deixa carregar, e o **seu** teste de `api->tamanho` é a última defesa |
| você declara 6 e o servidor tem a v5 | o carregador **recusa antes de carregar** e diz qual versão falta |

Por isso a checagem no começo do seu `ConanPluginCarregar` não é formalidade:

```cpp
if (!api || api->tamanho < sizeof(ConanApiTabela)) return;
```

Sem ela, um plugin compilado contra uma tabela maior lê ponteiro além do fim da
struct e chama lixo, num servidor com API mais velha.

### Se o seu plugin usa offset cru, DECLARE

Esta é a diferença entre um plugin que sobrevive a um patch da Funcom e um que
passa a ler memória errada em silêncio.

```json
{
  "FullName": "Meu Plugin",
  "Version": "1.0.0",
  "MinApiVersion": 6,
  "BuildDoJogo": 24784646,
  "UsaOffsetsCrus": true
}
```

| o que você declara | o que o carregador faz quando o jogo atualiza |
|---|---|
| `UsaOffsetsCrus: true` + `BuildDoJogo` | **recusa carregar** e diz para pedir a versão nova ao autor |
| `UsaOffsetsCrus: true` sem `BuildDoJogo` | recusa — declaração que não diz nada não é checagem |
| só `BuildDoJogo` | carrega, e registra no log que a build mudou |
| nada | carrega — é o certo se você só usa a tabela |

**Por que isso é seu problema e não nosso:** a API se recusa a carregar numa
build que não conhece, de propósito. O seu plugin não tem essa porta a menos que
você a peça — e nós não temos como adivinhar se o `0x068` que você escreveu é um
offset do jogo ou uma constante sua.

O sintoma de errar aqui é o pior que existe: **nenhum erro.** O plugin carrega,
roda, e lê o campo vizinho. O dono do servidor vê comportamento estranho semanas
depois e não tem como ligar ao seu plugin.

**Como não precisar disso:** use `OffsetDoMembro(obj, "NomeDoCampo")` (v5) em vez
do número. Ele resolve pela reflexão, na build que estiver rodando.

### A versão da API e a BUILD DO JOGO

São coisas diferentes, e as duas aparecem em toda release:

```
Conan-Api 2.7.0 — build 24784646
```

- **`2.7.0`** — a versão do projeto
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
mv ExemploOla.cpp MeuPlugin.cpp
sed -i 's/ExemploOla/MeuPlugin/g' compilar.sh
./compilar.sh
```

Saiu `MeuPlugin.dll`. Para instalar, copie a **pasta** `MeuPlugin/` (com a DLL
dentro) para `Conan-Api/Plugins/` do servidor e reinicie.

**Quer o plugin no seu próprio projeto, fora da pasta do SDK?** Pode — só diga
onde está o `include`:

```bash
CONAN_SDK_INCLUDE=/caminho/do/Conan-Api-SDK-v2.5.0/include ./compilar.sh
```

O `compilar.sh` sobe a árvore de diretórios procurando o header sozinho, então
enquanto o plugin estiver **dentro** do SDK (em qualquer profundidade) ele acha.
Movido para fora, ele não adivinha: para e diz esta linha, em vez de deixar o
compilador reclamar de um `#include` que parece errado e não é. O nome da pasta e o
nome da DLL devem bater — é assim que o carregador escolhe qual carregar.

## A estrutura de um plugin

Cada plugin é **uma pasta**, com tudo dentro:

```
Conan-Api/Plugins/MeuPlugin/
   MeuPlugin.dll        <- o carregador procura este nome primeiro
   PluginInfo.json      <- opcional; se existir, o log mostra nome e versão
   config.json          <- opcional, e SEU: CaminhoConfig("MeuPlugin")
   meubanco.db          <- CaminhoDados("MeuPlugin", "meubanco.db")
```

O usuário instala arrastando a pasta. Desinstala apagando a pasta.

### Só a DLL é obrigatória

| arquivo | obrigatório? | quem lê |
|---|---|---|
| `MeuPlugin.dll` | **sim** — a única coisa que o carregador precisa | o carregador |
| `PluginInfo.json` | não | o carregador, **se** existir |
| `config.json` | não | **o seu plugin**; a API nunca abre |

Um plugin com nada além da DLL carrega e funciona. O `Cartografo` e o
`GravadorDeEventos` deste projeto rodam assim:

```
  [ok] Cartografo   [sem PluginInfo.json]
```

O `config.json` não tem formato imposto por nós. A API só devolve o **caminho**
com `CaminhoConfig("MeuPlugin")` — o arquivo pode ser JSON, pode ter outro nome,
pode não existir.

**Sem `PluginInfo.json` você perde quatro coisas:**

- o nome e a versão do plugin no log (aparece só o nome da pasta)
- `MinApiVersion` — nada recusa o seu plugin numa API velha demais
- `Dependencies` — nada garante que o Permission suba antes de você
- `BuildDoJogo` / `UsaOffsetsCrus` — o plugin carrega depois de uma atualização
  do jogo mesmo quando não deveria

Para um plugin que só você usa, nada disso importa. Para um que você **publica**,
todos importam.

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

## Responder no primeiro segundo: registre cedo

O servidor aceita jogador **antes** de o mundo terminar de montar. Nessa janela
a reflexão ainda não existe, então um plugin ativado só depois dela deixa sem
resposta quem digitou um comando cedo.

Para isso existe um segundo ponto de entrada, opcional:

```cpp
extern "C" __declspec(dllexport)
void ConanPluginRegistrar(const ConanApiTabela* api)   // ANTES do mundo
{
    ConanApi::UsarTabela(api);
    g_id = api->HookProcessEvent("ServerSendChatMessage", AoFalar, nullptr, 100);
}

extern "C" __declspec(dllexport)
void ConanPluginCarregar(const ConanApiTabela* api)    // DEPOIS do mundo
{
    ...
}
```

`HookProcessEvent` chamado no `Registrar` **enfileira** e devolve um id válido
na hora. A API arma o hook no instante em que o mundo monta — antes de qualquer
plugin ser ativado.

**O que você NÃO pode fazer no `Registrar`:** tocar em objeto do jogo. Não há
mundo, não há reflexão, e `FindClass`/`FindObjects` devolvem nada. Se você
precisa do mundo, o lugar é o `Carregar`.

**Você não precisa exportar o `Registrar`.** Sem ele o plugin funciona
exatamente como antes — os hooks entram quando o `Carregar` roda. O `Registrar`
só encurta a janela.

---

## Do chat até o jogador: o caminho que todo plugin precisa

É o pulo que falta em quase toda API, e sem ele as 9.247 classes não servem para
nada: **alguém digitou algo — quem foi, e onde ele está?**

```cpp
// O auxiliar que se repete em todo plugin: resolve o offset PELO NOME,
// lê o ponteiro, e recusa o que não for legível.
static void* MembroPonteiro(void* obj, const char* nome)
{
    if (!obj) return nullptr;
    const int32_t off = g_api->OffsetDoMembro(obj, nome);
    if (off < 0) return nullptr;

    void* p = nullptr;
    if (g_api->LerMembro(obj, uint32_t(off), &p, sizeof(p)) <= 0) return nullptr;
    if (!p || !g_api->Legivel(p, 8)) return nullptr;
    return p;
}

extern "C" ConanAcao AoFalar(ConanChamada* c)
{
    void* controller = c->Obj;              // 1. QUEM falou

    char nome[128] = "";                    // 2. o NOME está no PlayerState
    if (void* ps = MembroPonteiro(controller, "PlayerState"))
    {
        const int32_t off = g_api->OffsetDoMembro(ps, "PlayerNamePrivate");
        if (off >= 0) g_api->LerTextoDoJogo(ps, uint32_t(off), nome, sizeof(nome));
    }

    void* corpo = MembroPonteiro(controller, "Character");   // 3. o PERSONAGEM
    if (!corpo) corpo = MembroPonteiro(controller, "Pawn");

    struct { double X, Y, Z; } pos{};       // 4. a POSIÇÃO, pela função do jogo
    g_api->ChamarFuncao(corpo, "K2_GetActorLocation", nullptr, nullptr, 0,
                        &pos, sizeof(pos));

    g_api->MensagemParaJogador(nome, "achei você");
    return CONAN_CANCELAR;
}
```

**Nenhum offset gravado.** Rodando neste servidor, `OffsetDoMembro` devolveu
`PlayerState → 0x308`, `Character → 0x350`, `Pawn → 0x340`. Escrever esses
números no seu código funciona hoje e lê o campo vizinho depois do próximo patch.

A posição vem da **função** e não do campo de propósito: `RelativeLocation` é
replicado, e ler o campo cru entrega o valor de antes da última replicação.

### Sem hook nenhum

Quando o começo não é o jogador falando — tarefa agendada, comando de admin:

```cpp
void* pcs[64];
int n = g_api->FindObjects("ConanPlayerController", pcs, 64, /*incluirFilhas=*/1);
```

Daí em diante é o mesmo caminho.

**Não guarde esses ponteiros entre chamadas.** O coletor de lixo do jogo destrói
objetos e reaproveita endereços; `Legivel` continuaria dizendo que sim, porque a
página segue mapeada, e você agiria sobre outra coisa.

Exemplo completo em `Exemplos/ExemploJogador`.

---

## O que o seu hook recebe

O guia usa `c->Parms` nos exemplos, mas a struct tem mais — e `c->Obj` é o que
falta na maioria dos plugins:

```c
typedef struct ConanChamada {
    void*    Obj;         // o UObject que recebeu a chamada  <- quem, no jogo
    void*    Func;        // a UFunction
    void*    Parms;       // bloco de parâmetros (pode ser NULL)
    uint32_t ParmsSize;
    int32_t  NomeIndice;  // FName.ComparisonIndex — comparação O(1)
    int32_t  NomeNumero;
} ConanChamada;
```

Num hook de `ServerSendChatMessage`, `c->Obj` é o `ConanPlayerController` de
quem falou. É ele que você passa para `MensagemNaTela` e para o `Permission`.

**Não guarde `c->Obj` entre chamadas.** O coletor de lixo do jogo pode destruir
o objeto e reaproveitar o endereço; `Legivel` continuaria dizendo 1, porque a
página segue mapeada, e você agiria sobre outra coisa. Pegue-o de novo em cada
hook.

## Contar quem está online

```cpp
void* achados[128];
int n = g_api->FindObjects("ConanPlayerController", achados, 128, /*incluirFilhas=*/1);
```

O último parâmetro não é decoração: sem `incluirFilhas`, subclasses ficam de
fora e a contagem sai menor do que a realidade. E `FindObject` (singular) é
outra pergunta — devolve **o primeiro**, então com dois jogadores no servidor
você enxergaria um só.

## O retorno de HookProcessEvent: zero é FALHA

```cpp
uint32_t id = g_api->HookProcessEvent("ServerSendChatMessage", AoFalar, nullptr, 100);
if (id == 0)
    g_api->Log("nao consegui hookar o chat");   // o motivo já está no log
```

Ele devolve o **id do hook**, não um código de erro. `0` significa que falhou —
o contrário do reflexo `0 == ok` que a maioria de nós tem em C. Guarde o id se
pretende chamar `RemoverHook`.

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

## O jogo inteiro, com assinatura de verdade

Além da tabela, o pacote traz o **`ConanSDK.h`**: **9.247 classes** do Conan com
os membros e as funções que a reflexão do jogo declara. Não é lista de nomes:
**89% das 38.340 funções têm assinatura completa** — tipo e nome de cada
parâmetro, conferidos contra o próprio servidor rodando, com o mundo cheio.

```cpp
#include "Conan/ConanSDK.h"

void ConanPluginCarregar(const ConanApiTabela* api)
{
    ConanApi::UsarTabela(api);          // <- obrigatória, veja abaixo
    ...
    cm->TeleportPlayer(1000.0f, 2000.0f, 300.0f);
    FVector onde = ator->K2_GetActorLocation();
    cm->CheatSpawnItem(TemplateId, quantidade);
}
```

**`ConanApi::UsarTabela(api)` é obrigatória.** O header não tem de onde tirar a
tabela sozinho — ela chega no seu `ConanPluginCarregar`. Sem essa linha, toda
chamada do SDK vira nada, silenciosamente; por isso a primeira delas avisa no
`stderr` em vez de deixar você caçar o motivo.

Parâmetro de **saída** vira referência, e a API copia o valor de volta:

```cpp
FHitResult batida{};
ator->K2_SetActorLocation(destino, false, batida, true);
```

Texto de saída vira `char*` e capacidade — **decodificado**, não o ponteiro do
jogo (que morre quando a chamada retorna):

```cpp
char nome[128];
alguem->GetDisplayName(nome, sizeof(nome));
```

Lista de saída vira ponteiro, capacidade e contagem, com os **elementos**
copiados:

```cpp
AActor* achados[32]; int quantos = 0;
lib->AlgumaBuscaDeAtores(..., achados, 32, quantos);
```

### `FName` de entrada: use `ConanApi::Nome("...")`

Um `FName` **não é uma string**: são dois inteiros apontando para uma entrada do
pool de nomes daquele processo. Você não tem como inventar esses números — e
inventar é pior do que errar, porque `{0,0}` é o nome `None` e qualquer outro
par é *algum* nome válido, só que não o seu. O jogo aceita e faz outra coisa,
sem um erro sequer.

```cpp
// entregar um item ao personagem: o Context é um FName
personagem->SpawnTemplateItem(10001, ConanApi::Nome("minhaloja"),
                              100, 1.0f, 0.0f, true);

// pôr um item num inventário
inventario->AddItemTemplate(10001, -1, ConanApi::Nome("minhaloja"),
                            100, false, 1.0f, 0.0f);
```

`ConanApi::Nome` pede ao próprio jogo (`Conv_StringToName`), que **cria** o nome
no pool se ele ainda não existir — o que importa quando você inventa um contexto
seu, que nenhuma parte do jogo usaria. O resultado é memorizado, então chamar
dentro de um laço não custa.

> Isto entrou em 20/08/2026 e destravou o que estava fora de alcance: sem essa
> ponte, **nenhuma** função com parâmetro `FName` era chamável — inclusive as
> duas acima, que são *como se entrega um item a um jogador*. Uma loja era
> impossível de escrever, e o motivo não aparecia em lugar nenhum.

**Você não linka nada.** O SDK inteiro roteia pela tabela — é por isso que ele
funciona igual em MSVC, MinGW e clang. Se algum dia o seu projeto pedir uma
`libconanapi.a`, algo está errado: não existe biblioteca nossa para você linkar.

Os 15% restantes saem como template genérico, de propósito: são tipos que
**carregam posse de memória do jogo** (`TArray<FString>`, `TMap`, delegates
multicast). Passá-los por valor duplicaria ponteiros, e alguém liberaria duas
vezes. Preferimos template sem tipo a assinatura que corrompe.

---

## Falar com o jogador

Isto existe desde a **v3** da tabela, e são três rotas diferentes:

```cpp
g_api->MensagemParaTodos("O servidor reinicia em 5 minutos.");
g_api->MensagemParaJogador("NomeDoJogador", "Kit entregue. Volte em 24h.");
g_api->MensagemNaTela(playerController, "Bem-vindo!", 8.0f);
```

**Repare em quem cada uma endereça** — é o engano mais fácil de cometer:

| função | recebe | de onde vem |
|---|---|---|
| `MensagemParaTodos` | nada | — |
| `MensagemParaJogador` | **o nome**, `const char*` | `userName`, offset `0x048` do chat |
| `MensagemNaTela` | **o controller**, `void*` | `c->Obj` no hook, ou `LerParm` no login |

Passar o controller para `MensagemParaJogador` não compila (o tipo salva você).
Mas num hook de chat você tem o `ChatRpcData`, e é de lá que sai o nome.

`MensagemParaJogador` devolve **0 se o jogador não estiver conectado** — trate
isso, porque ele pode ter saído entre o comando e a resposta.

### O que continua proibido

**Montar uma `FString` sua e passá-la ao jogo.** Isso **derruba o servidor**:
`ProcessEvent` destrói o bloco de parâmetros ao retornar e chama o alocador *do
jogo* sobre um ponteiro da sua pilha. Foi medido aqui, não é suposição — e é
justamente por isso que as funções acima existem: elas pedem ao jogo que aloque.
Se precisar passar texto a uma função do jogo por conta própria, use
`CriarTextoDoJogo` (v4), que devolve 16 bytes que o jogo possui.

**Hookar qualquer endereço.** `HookFuncao` **recusa** cerca de 32% das funções,
com o motivo em `TextoRecusa`. Recusa não é falha: é a API se negando a fazer
algo que executaria meia instrução um dia, corrompendo memória horas depois sem
erro legível.

---

## Instalar sem derrubar o servidor

O dono de servidor não precisa reiniciar para instalar o seu plugin:

```
1. copiar a pasta MeuPlugin/ para Conan-Api/Plugins/
2. criar o arquivo vazio Conan-Api/CARREGAR-NOVOS
```

Em até 3 segundos o carregador atende, roda as mesmas conferências do
carregamento normal e chama o seu `ConanPluginCarregar`. O log diz o resultado:

```
[novos] [ok] MeuPlugin carregado SEM reiniciar o servidor.
```

**Trocar a versão de um plugin já carregado ainda exige reiniciar.** Isso não é
limitação temporária: o seu plugin, depois de carregado, tem hooks armados e
possivelmente tarefas agendadas apontando para dentro da sua DLL. Descarregá-la
com qualquer um deles vivo faria o jogo saltar para memória desmapeada mais
tarde, longe da causa. Preferimos pedir um reinício a entregar isso.

**O que isso muda para você:** o seu `ConanPluginCarregar` pode rodar com o
mundo **já cheio** — jogadores conectados, objetos vivos, outros plugins
funcionando. Se ele assume que está no arranque (que a lista de jogadores está
vazia, que nada foi inicializado ainda), vai se surpreender. Escreva-o para
funcionar nos dois momentos.

---

## Quando não funciona: onde olhar

Dois arquivos respondem quase tudo, e ficam em `Conan-Api/Logs/`:

| arquivo | o que ele conta |
|---|---|
| `ConanLoader.log` | quais plugins o carregador **viu**, quais **recusou** e por quê |
| `ConanApi.log` | o que os plugins **escreveram** com `Log()`, e os avisos do motor |

O carregador escreve o veredito de cada plugin com o motivo junto. Vale ler a
linha inteira antes de qualquer outra coisa:

```
  [ok] MeuPlugin  "Meu Plugin"  v1.0.0  api>=6
  [x]  Outro — exige API versao 7; esta instalacao e' a 6.
  [x]  Terceiro — abriu, mas NAO exporta ConanPluginCarregar().
```

### Os enganos que mais aparecem

**A DLL não abre.** Quase sempre é arquitetura (compilou x86 em vez de x64) ou o
runtime da Microsoft faltando — `/MD` em vez de `/MT`. O log diz o código de erro
do Windows; `193` é "não é uma aplicação Win32 válida", que na prática significa
32 bits.

**Abriu, mas nada acontece.** Verifique se o nome da pasta e o da DLL batem, e
se você exportou `ConanPluginCarregar`. No MSVC, sem `extern "C"` o nome sai
decorado e o carregador não acha:

```bat
dumpbin /exports MeuPlugin.dll | findstr ConanPlugin
```

Deve aparecer `ConanPluginCarregar`, não `?ConanPluginCarregar@@YAXPEBU...`.

**Carregou, mas as chamadas do `ConanSDK.h` não fazem nada.** Faltou
`ConanApi::UsarTabela(api)`. O aviso sai no `stderr` do servidor, que sob Wine
cai no log do jogo, não no nosso.

**A função responde `false` e você não sabe se rodou.** Use
`UltimaChamadaExecutou()`: o jogo filtra chamadas em CDO, template de Blueprint
e ator não inicializado, e nesses casos o retorno vem do bloco zerado. Sem esse
sinal, "a função disse não" e "a função não rodou" viram o mesmo `false`.

**Escreveu num campo e o cliente não vê.** O campo é replicado. Pergunte antes:

```cpp
const int32_t off = api->OffsetDoMembro(obj, "Campo");
if (api->EhReplicado(obj, off) == 1) { /* chame a função do jogo, não escreva */ }
```

São 1.222 dos 36.210 membros desta build. `EscreverMembro` avisa uma vez por
campo, com o nome.

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

---

## Glossário: os nomes da ABI

Os identificadores da tabela estão em português porque fazem parte da ABI
publicada, e renomeá-los quebraria todo plugin já compilado. O que cada um faz:

| nome na ABI | o que é |
|---|---|
| `ConanPluginCarregar` | entrada do plugin, chamada depois que o mundo subiu |
| `ConanPluginRegistrar` | entrada opcional e mais cedo, antes do mundo |
| `ConanPluginDescarregar` | chamada no descarregamento |
| `ConanApiTabela` | a tabela de funções |
| `ConanChamada` | o contexto da chamada interceptada |
| `ConanAcao` / `CONAN_CONTINUAR` / `CONAN_CANCELAR` | veredito do hook: seguir ou cancelar |
| `Log` | escreve no `ConanApi.log` |
| `Legivel` | este ponteiro está legível? |
| `LerMembro` / `EscreverMembro` | lê / escreve um campo por offset |
| `LerBit` / `EscreverBit` | lê / escreve um bitfield |
| `OffsetDoMembro` | resolve o offset de um campo **pelo nome** |
| `EhReplicado` | este campo é replicado? |
| `ChamarFuncao` / `ChamarFuncaoEx` | chama uma função do jogo pelo nome |
| `HookProcessEvent` / `RemoverHook` | instala / remove um hook |
| `AgendarNaThreadDoJogo` | agenda trabalho na thread do jogo |
| `LerTextoDoJogo` / `CriarTextoDoJogo` | lê / aloca uma string da engine |
| `MensagemParaTodos` / `MensagemParaJogador` / `MensagemNaTela` | mensagens |
| `CaminhoConfig` / `CaminhoDados` / `CaminhoRaiz` | caminhos que são do seu plugin |
| `UltimaChamadaExecutou` | a última chamada realmente executou? |
| `NumObjects` / `GetObjectByIndex` / `FindObject` / `FindObjects` | busca de objeto |
| `GetDefaultObject` / `DescendeDe` / `NomeDoObjeto` | auxiliares de classe |
