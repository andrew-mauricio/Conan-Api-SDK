# Mapa de eventos — o que hookar para reagir ao jogo

*Tradução. O documento principal é o [EVENTS.md](EVENTS.md), em inglês.*

Este é o documento que faltava. A API sabe interceptar função **por nome**, mas
existem 36.392 nomes e nenhuma pista de qual corresponde a qual evento. Procurar
"chat" no catálogo devolve 281 resultados, quase todos irrelevantes.

Aqui estão os que importam, com **assinatura completa** — nome do parâmetro, tipo
e offset, tirados da reflexão do servidor.

---

## Os eventos, e o que cada um entrega

| evento | função | parâmetros |
|---|---|---|
| **chat / comando** | `ConanPlayerController::ServerSendChatMessage` | `chatData` → `ChatRpcData` (128 B) |
| **jogador entrou** | `BaseGameMode_C::K2_PostLogin` | `NewPlayer` (PlayerController) |
| **jogador saiu** | `BaseGameMode_C::K2_OnLogout` | `ExitingController` |
| **jogador morreu** | `BasePlayerChar_C::KillPlayerCharacter` | `HitLocation`, `KillerName`, `CauseOfDeath` |
| **jogador renasceu** | `BasePlayerChar_C::OnRespawn` | — |
| **coletou recurso** | `BasePlayerChar_C::SignalXPHarvest` | `ResourceType` (FName), `ResourceNumber` (int), `IsResourcePickup` (bool) |
| **morte (combate)** | `BaseBPCombat_C::OnDeath` | `HitInformation` (struct), `OriginalActorLifespan` (double) |
| **avisar a todos** | `ConanCheatManager::BroadcastMessage` | `Message` (FString) |
| **pegou item de lore** | `BasePlayerChar_C::PickUpLoreItem` | 2 parâmetros |
| **NPC morreu** | `BaseNPC_C::OnDeath` · `BaseHumanoidNPC_C::OnDeath` | `HitInformation`, lifespan |
| **matar personagem** | `BaseBPCombat_C::KillCharacter` | 5 parâmetros |

---

## Chat e comandos — o mais usado

O cliente manda uma RPC. O prefixo `Server` é a convenção da Unreal para "roda no
servidor a pedido do cliente", e é exatamente onde um comando deve ser tratado.

### O layout de `ChatRpcData` (128 bytes)

```
+0x000  uint64   Timestamp
+0x008  struct   UserId               (UniqueNetIdRepl)
+0x038  int64    CharacterUniqueID    <- o "uid" que aparece no log do jogo
+0x040  int64    targetUniqueId
+0x048  FString  userName             <- "Jogador"
+0x058  FString  Channel              <- "Global"
+0x068  FString  Message              <- "!kit", "oi"
+0x078  bool     generated
```

### O prefixo de comando é `!`, e isso foi medido

**No Conan, `/` não serve.** O cliente do jogo intercepta `/comando` localmente e
**não envia ao servidor** — nenhum plugin, em nenhuma API, consegue ver. Medido no
servidor real, com o hook registrando toda mensagem antes de decidir nada:

```
digitado        chegou ao hook?   o jogo processou?
"oi"            SIM               SIM
"!apiteste"     SIM               SIM
"/apiteste"     NAO               NAO      <- sumiu na maquina do jogador
```

O `/apiteste` **desapareceu do chat do jogador**, o que parecia sucesso do hook —
e não era. O sintoma visível é idêntico nos dois casos ("sumiu"), e só o log do
servidor distingue "meu hook cancelou" de "nunca chegou".

Quem vem de outras APIs de servidor de sobrevivência espera `/`, porque nesses
jogos o cliente envia. Aqui a escolha não é de estilo: é `!` ou não funciona. Use
`!` ou `.` — qualquer coisa que o cliente trate como texto comum.

### Cancelar ENGOLE a mensagem

É isso que faz um comando ser um comando: o jogador digita `!kit`, o plugin age, e
o texto não aparece no chat de ninguém.

```cpp
static Acao AoFalar(Chamada& c)
{
    if (!c.Parms || c.ParmsSize < 0x80) return Acao::Continuar;

    char texto[512];
    LerTexto(c.Parms, 0x068, texto, sizeof(texto));    // Message

    if (std::strncmp(texto, "!kit", 4) == 0)
    {
        DarKit(...);
        return Acao::Cancelar;      // não aparece no chat
    }
    return Acao::Continuar;         // conversa normal passa
}
ConanApi::HookProcessEvent("ServerSendChatMessage", AoFalar);
```

`LerTexto` acima é um utilitário do **próprio plugin** — lê uma `FString` num
offset do bloco de parâmetros —, não uma função da API. O `ExemploComando` traz uma
implementação pronta; copie de lá.

**Não engula todo prefixo.** O jogo tem os comandos dele, e um plugin que sequestra
tudo deixa o jogador sem entender por que o jogo parou de responder. Trate só o seu
prefixo e devolva `Continuar` no resto.

### O autor do comando VÊ o próprio comando. Isso é normal.

Medido: o jogador digitou `!apiteste` duas vezes e **viu as duas** no chat dele.
Ainda assim o cancelamento funcionou:

```
o hook viu:            2 mensagens  -> reconheceu, cancelou
o servidor processou:  0 mensagens  -> a funcao original NAO executou
```

O log do jogo registra `ChatWindow: ... said: <texto>` para toda mensagem que o
servidor processa. Não havia nenhuma. Ver a **ausência** é a prova de que o
original não rodou — o mesmo raciocínio do `999 em vez de 12`.

O cliente do Conan **exibe a própria mensagem localmente**, otimista, antes de
saber a decisão do servidor. Então:

  · quem digitou     -> vê o comando no chat dele (eco local)
  · os outros        -> não veem nada (o servidor não retransmitiu)

Isso é o comportamento certo e ninguém adivinharia. Se você espera que o comando
desapareça da sua própria tela, vai concluir que o hook falhou — e ele não falhou.
Para dar retorno ao jogador, veja o limite de `FString` mais abaixo.

Exemplo completo: `Exemplos/ExemploComando/`.

### Responder ao jogador

Dá para responder. Monte o texto com o alocador do próprio jogo e chame uma
função de interface no controller:

```cpp
ConanApi::Call<void>(controller, "ClientHUDShowNotification",
                     ConanApi::TextoRico("Você tem 250 pontos"),
                     bool(true), bool(false));
```

`ConanApi::TextoRico` e `ConanApi::Texto` entregam ao jogo uma `FText`/`FString`
que o **jogo** alocou — é esse o truque; a seção mais abaixo explica por quê. O
`Conan Shop` usa isso em produção todo dia.

---

## Identidade do jogador

Esta é a parte em que é fácil errar de um jeito que só aparece meses depois,
quando alguém perde o VIP que pagou.

**A chave é `ConanPlayerState.MasterAccountId`** — `StrProperty` no offset `0x3C0`.
Confirmada com jogador real, e o teste que a elegeu foi discriminante porque os
candidatos errados **mudaram** entre duas sessões (os identificadores abaixo são
exemplos anonimizados — a medição é real; só os valores foram trocados):

```
                                   sessão 1                            sessão 2
MasterAccountId  (+0x3C0)   "A-EXEMPLO01"                       "A-EXEMPLO01"   ← estável
(campo em +0x3F0)           "74315DA541274454139F5FBF0E15EC12"  "E0E5DFE3..."   ← muda
(token em PC +0x12E8)       "vwwLJ1ST.Ze43jjs..."               "bexfQXPIHJq~..." ← muda
SavedNetworkAddress (+0x350) "203.0.113.24"                    (o IP; muda de rede)
PlayerNamePrivate  (+0x398)  "Jogador#0000"                       (o jogador pode trocar)
```

O `+0x3F0` também parecia sólido — 32 dígitos hexadecimais, cara de GUID. Se
alguém o tivesse escolhido, o VIP de todo mundo se perderia a cada relogin, e o
sintoma ("às vezes o VIP some") é dos piores de diagnosticar.

`MasterAccountId` aparece em **quatro** lugares ao mesmo tempo — no personagem
(`BasePlayerChar_C +0xAD8`), no `PlayerState` (`+0x3C0`) e duas vezes no
`PlayerController` (`+0xBC8`, `+0x1580`). Valor repetido em objetos diferentes é
identidade canônica, não cache local.

O plugin `Permission` já usa essa chave: `ConanPermIdDoController(pc, buf, tam)`.

### E o SteamID64?

Existe, e o log do jogo o registra:

```
LogNet: Login request: userId: STEAM:7656119XXXXXXXXXX  platform: Fls
ChatWindow: Character Jogador (uid 110, player 7656119XXXXXXXXXX) said: oi
```

Mas **não foi encontrado como texto na memória** dos objetos de jogador — só o
`MasterAccountId`. E note `platform: Fls`: a autenticação é pela Funcom Live
Services, não pela Steam. Para quem quiser o SteamID, o caminho provável é o
`UserId` (`UniqueNetIdRepl`) dentro do `ChatRpcData` ou do `PlayerState` — **não
medido**, e portanto não recomendado até estar.

---

## Como este mapa foi obtido

Duas fontes, e nenhuma delas exigiu adivinhação:

**1. O catálogo da reflexão.** Filtrando 36.392 funções pelas classes que são do
jogo (`Conan*`, `Base*`, `FunCombat*`, `DW*`) e descartando as de animação, áudio
e interface, os nomes ficam legíveis e os candidatos caem de 281 para uma dúzia.

**2. O log do próprio jogo.** Ele registra o chat com personagem, uid e player:

```
ChatWindow: Character Jogador (uid 110, player 7656119XXXXXXXXXX) said: teste
```

Isso confirmou que o processamento existe e deu os campos a procurar.

**O que NÃO funcionou, para quem tentar de novo:** procurar a string de formato
`"Character %s (uid %d, player %s) said: %s"` no binário e rastrear quem a
referencia. A string está lá, uma vez só, em `.rdata` — mas a varredura por
`lea reg,[rip+disp]` apontando para ela deu **zero** referências. Zero aqui é
hipótese, não conclusão: pode ser codificação de instrução não coberta, ou acesso
por tabela. Não vale insistir — o catálogo respondeu mais rápido.

---

## Mandar texto PARA o jogo: por que precisa da API

Montar uma `FString` apontando para buffer do plugin e passá-la a uma função por
reflexão **derruba o servidor**. Foi medido, com
`ConvertToAbsolutePath("teste-api-xyz")`: o log do plugin morre exatamente na
chamada.

A razão é estrutural. `ProcessEvent` **destrói o bloco de parâmetros** quando a
função retorna — percorre `DestructorLink` e chama o destrutor de cada
propriedade. O destrutor de `FString` chama `FMemory::Free(Data)`, o alocador do
jogo, sobre um ponteiro que veio da nossa pilha. Não é o jogo lendo errado: é o
jogo fazendo o certo com memória que nunca foi dele.

Então a string tem de ser alocada pelo alocador do jogo. É o que
`ConanApi::Texto` (`FString`) e `ConanApi::TextoRico` (`FText`) fazem — a API
acha o `GMalloc`/`FMemory::Malloc` e monta o valor de 16 bytes
`{wchar_t* Data; int32 Num; int32 Max}` com a memória do próprio jogo. O tempo
de vida é o da expressão da chamada, e isso basta: o jogo copia ou consome
durante o `ProcessEvent` e destrói o bloco ao retornar.

Mesma ideia para `FName`, um passo além: `ConanApi::Nome` passa pelo
`Conv_StringToName` do jogo. Sem ele, nenhuma função que recebe `FName` era
chamável — inclusive `SpawnTemplateItem`, que é como item chega ao jogador.

**Um plugin não consegue montar nenhum dos três sozinho.** É por isso que os
três moram na tabela.

---

## O que ainda não está medido

- **A ordem e a frequência real dos eventos.** O catálogo diz que a função existe;
  não diz quantas vezes por segundo ela é chamada, nem em que ordem. Para descobrir isso, o caminho é ligar um hook curinga
  (`api->HookProcessEventTudo`) **com o servidor já carregado** e deixá-lo expirar
  sozinho. Ligado durante o arranque, ele impede o mundo de terminar de carregar
  — medido: o servidor travou em 4,35 GB em vez dos 8,7 normais.
- **`UniqueNetIdRepl`** — o struct de identidade de rede não foi decomposto.
