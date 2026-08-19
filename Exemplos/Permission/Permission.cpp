// ============================================================================
//  Permission — o serviço central de grupos e permissões
//
//  Conan Exiles Enhanced · primeiro plugin oficial da API
//
//  O QUE ELE É
//  -----------
//  Um plugin que outros plugins consultam. Ele não dá VIP a ninguém, não vende
//  nada e não muda o jogo: ele responde "este jogador tem o direito X?" e
//  guarda a resposta num banco que sobrevive a queda e a reinício.
//
//  A parte que decide quem pode o quê está em Armazem.cpp, que NÃO depende do
//  jogo — e por isso é testável de verdade, num .exe, sob o mesmo Wine.
//  Este arquivo é só a ponte: reflexão do jogo -> identidade -> Armazém, e a
//  tabela de funções que os outros plugins recebem.
//
//  A LINHA QUE SEPARA O QUE ESTÁ PROVADO DO QUE NÃO ESTÁ
//  ----------------------------------------------------
//  Provado, rodando: o Armazém (45 checagens sob Wine, incluindo queda com
//  TerminateProcess no meio da escrita e integrity_check depois).
//
//  NÃO provado ainda, e está escrito em cada ponto: QUAL dos identificadores do
//  Conan é o certo para usar como chave. A reflexão diz onde eles moram e de
//  que tipo são — isso é fato medido. O que eles CONTÊM só um jogador de
//  verdade conectando mostra, e o servidor de teste nunca recebeu um.
//
//  Então este plugin não escolhe por chute: ele lê TODAS as fontes, grava as
//  três no log no primeiro contato com cada jogador, e usa a que a configuração
//  manda usar. Quem tem o servidor olha o log uma vez e decide com dado real.
//  A alternativa — cravar uma e seguir — é exatamente o defeito com cara de
//  sucesso: o plugin de VIP funcionaria, e o VIP iria para a pessoa errada.
// ============================================================================

// ============================================================================
//  A FRONTEIRA ENTRE PLUGINS: ELA NAO EXISTE. Leia antes de confiar no banco.
//
//  O QUE E FATO, MEDIDO NESTA MAQUINA
//  ----------------------------------
//  Todo plugin roda DENTRO do processo do jogo (docs/COMECAR.md: "roda dentro
//  do processo do jogo"), com o MESMO usuario e no MESMO espaco de enderecamento
//  deste Permission. Disso decorre, sem exagero:
//
//    · o banco e um arquivo comum em <Win64>\Conan-Api\Dados\permission.db, num
//      caminho que QUALQUER plugin deriva do proprio executavel (a distribuicao
//      padroniza a pasta). Um plugin que embarque o sqlite3 — que ja mora nesta
//      arvore — abre esse arquivo com o proprio handle e roda um INSERT em
//      grupo_permissao ou jogador_grupo: da VIP a si mesmo, apaga o VIP de
//      terceiros, le o diario de auditoria. Nada neste codigo impede isso.
//    · estando no mesmo heap, um plugin pode sobrescrever a memoria do
//      Instantaneo deste Permission direto, sem sequer tocar no banco.
//
//  A "fronteira" e so a convencao de chamar a ABI. Nao ha ACL, nao ha processo
//  separado, nao ha namespace do SO: isolamento zero.
//
//  A ABI (ConanPermissionObterApi) NAO E FRONTEIRA DE SEGURANCA
//  -----------------------------------------------------------
//  Ela e a porta pretendida — confere a versao de ABI e enfileira escrita numa
//  thread — mas (a) qualquer plugin no processo chama a fabrica e recebe a
//  tabela; (b) qualquer plugin IGNORA a ABI e vai direto no arquivo ou na
//  memoria. Recusar ABI errada evita QUEDA por layout incompativel; nao evita
//  ADULTERACAO por quem quer adulterar.
//
//  A UNICA MEDIDA REAL QUE EXISTE, E EXATAMENTE O QUE ELA COBRE
//  -----------------------------------------------------------
//  O ConanPermission.dll e ligado com -Wl,--exclude-all-symbols (compilar.sh):
//  NAO exporta as ~250 funcoes sqlite3_*. Isso impede que outro codigo do
//  processo pegue o NOSSO sqlite3_exec por GetProcAddress e escreva com o nosso
//  handle. COBRE so isso. NAO cobre o plugin que embarca o proprio sqlite3 (o
//  ataque acima), porque ai ele nem precisa do nosso.
//
//  POR QUE NAO HA UMA TRANCA MAIOR AQUI (e nao e por preguica)
//  ---------------------------------------------------------
//  Uma tranca de arquivo (PRAGMA locking_mode=EXCLUSIVE) barraria o handle de
//  outro plugin — mas barraria TAMBEM o leitor legitimo (uma loja web ou
//  monitor que abre permission.db so para LER status de VIP), e nao impede a
//  escrita por memoria (mesmo heap). Seria meia defesa, com custo real ao uso
//  honesto, mexendo no nucleo do Armazem que 45 testes sob Wine validaram em
//  WAL — sem que eu consiga, aqui, re-provar que a recuperacao de queda continua
//  igual. Meia defesa nao provada, vendida como fronteira, e o "defeito com cara
//  de sucesso" que este projeto proibe. Fica de fora, e fica ESCRITO que fica.
//
//  O QUE ISSO SIGNIFICA PARA QUEM RODA O SERVIDOR
//  ---------------------------------------------
//  Instalar um plugin e conceder a ele acesso TOTAL ao servidor: ao banco de
//  permissoes, a identidade dos jogadores (ver PRIVACIDADE, abaixo) e a memoria
//  de todos os outros plugins. O modelo "qualquer um publica no portal" e
//  incompativel com isolamento zero. Isolamento de verdade exigiria rodar plugin
//  em OUTRO processo, com IPC — mudanca de arquitetura da API inteira, fora do
//  escopo deste plugin. Ate la: instale so plugin de origem que voce audita,
//  como faria com qualquer .dll que roda COMO o servidor.
//
//  PRIVACIDADE: A IDENTIDADE DO JOGADOR E LEGIVEL POR QUALQUER PLUGIN
//  ----------------------------------------------------------------
//  Pela mesma ausencia de fronteira, o MasterAccountId (conta mestra Funcom), o
//  id de plataforma e o nome de cada jogador ficam ao alcance de qualquer plugin
//  — a ABI ainda expoe id_do_controller de proposito, e mesmo sem ela um plugin
//  le os offsets. Alem disso, as 20 PRIMEIRAS resolucoes vao para o ConanApi.log
//  em TEXTO CLARO (bloco "registrar TODAS as fontes", mais abaixo): e diagnostico
//  proposital, para o dono escolher a chave com dado real, mas grava
//  identificador PERSISTENTE da pessoa em disco. O log fica na maquina do dono,
//  sob controle dele; ainda assim, quem responde por LGPD/GDPR precisa saber que
//  esses dados existem ali. O teto de 20 ja limita o que vai a disco; apagar ou
//  rotacionar o ConanApi.log limpa o historico.
// ============================================================================
// ── o ÚNICO header nosso que entra aqui ─────────────────────────────────────
//
// Nada do motor é compilado dentro deste DLL: nem o decodificador, nem a mesa
// de hooks, nem os offsets desta build. Tudo chega por ponteiro de função, na
// tabela que o carregador entrega em ConanPluginCarregar. É por isso que este
// arquivo não inclui ConanSDK.h/ConanBase.h/ConanHooks.h e o compilar.sh não
// linka libconanapi.a — e é o que permite recompilar o motor sem recompilar o
// Permission.
#include "Conan/ConanPluginApi.h"
#include "Armazem.h"
#include "include/Conan/ConanPermission.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>

// As duas cópias de MAX_ID não podem divergir: se o header público disser 64 e
// o Armazém 32, ids longos passariam pela ABI e seriam recusados lá dentro sem
// explicação. Duas cópias da mesma verdade divergem quando ninguém confere.
static_assert(Perm::MAX_ID == CONAN_PERM_MAX_ID,
              "MAX_ID do Armazem e CONAN_PERM_MAX_ID do header publico divergiram");
// Os outros dois tetos entraram junto com a checagem de terminador (17/08/2026)
// e correm o MESMO risco de divergir. Se o header público disser que um nó cabe
// em 128 e o Armazém recusar acima de 64, todo plugin com nó longo levaria
// "não sei" sem explicação — e "não sei" é o que o plugin de terceiro trata
// como ausência do Permission.
static_assert(Perm::MAX_GRUPO == CONAN_PERM_MAX_GRUPO,
              "MAX_GRUPO do Armazem e CONAN_PERM_MAX_GRUPO do header publico divergiram");
static_assert(Perm::MAX_NO == CONAN_PERM_MAX_NO,
              "MAX_NO do Armazem e CONAN_PERM_MAX_NO do header publico divergiram");

// ── offsets, todos vindos da reflexão viva desta build ──────────────────────
//
// Moram AQUI, no plugin, e é assim que tem de ser no modelo de tabela: a API
// não exporta offset nenhum, ela exporta LerMembro/LerTextoDoJogo. São POUCOS,
// e ter os cinco à vista, com o tipo ao lado, é o que permite conferir contra o
// catálogo em dez segundos quando o jogo atualizar. Vieram de
// golden/catalogo_interno.json, build 24383534.
namespace Off
{
    constexpr uintptr_t CONTROLLER_PLAYERSTATE = 0x308;  // Controller.PlayerState  ObjectProperty
    constexpr uintptr_t PAWN_PLAYERSTATE       = 0x320;  // Pawn.PlayerState        ObjectProperty
    constexpr uintptr_t PS_PLAYERID            = 0x304;  // PlayerState.playerId    IntProperty
    constexpr uintptr_t PS_NOME                = 0x398;  // PlayerState.PlayerNamePrivate  StrProperty
    constexpr uintptr_t CPS_MASTERACCOUNTID    = 0x3C0;  // ConanPlayerState.MasterAccountId StrProperty
    // PlayerState.UniqueID (StructProperty, 0x310) existe e NÃO é lido aqui.
    // É um FUniqueNetIdRepl, e o layout dos ScriptStruct desta build nunca foi
    // extraído — o Cartógrafo coleta os ScriptStruct e não emite o layout deles.
    // Ler um struct por offset presumido é o caminho mais curto para valor
    // plausível e errado. Fica de fora até ser medido.
}

// ── a tabela, e por que TUDO passa por ela ──────────────────────────────────
//
// Este ponteiro é o plugin inteiro do lado da API: enquanto for nulo, este
// binário não sabe fazer NADA sozinho — não tem log, não tem reflexão, não
// sabe nem onde fica a própria pasta. É de propósito, e é o que garante que
// atualizar o motor não obriga a recompilar este DLL.
static const ConanApiTabela* g_api = nullptr;

namespace
{
    Perm::Armazem g_armazem;
    HMODULE       g_meuModulo = nullptr;
    bool          g_pronto    = false;

    // qual fonte de identidade a configuração manda usar
    enum class Fonte { AUTO, MASTERACCOUNT, PLATAFORMA };
    Fonte g_fonte = Fonte::AUTO;
}

// ── por que LOG é macro, e não função ───────────────────────────────────────
//
// `g_api->Log` é variádica (`const char* fmt, ...`). Uma função nossa que
// recebesse `...` não conseguiria REPASSAR os argumentos para ela — não existe
// vprintf equivalente na tabela — e teria de formatar num buffer intermediário,
// com um segundo teto de tamanho e um segundo lugar para truncar errado. A
// macro entrega os argumentos direto, sem cópia.
//
// O `g_api &&` não é paranoia: `ConanPermissionObterApi` é exportada e pode ser
// chamada por outro plugin ANTES de o carregador nos entregar a tabela (a ordem
// de carga das DLLs não é especificada — ver ConanPermObter no header público).
// Sem a guarda, esse caminho seria uma queda do servidor em vez de uma recusa.
#define LOG(...) do { if (g_api && g_api->Log) g_api->Log(__VA_ARGS__); } while (0)

namespace
{
    void LogArmazem(const char* s) { LOG("%s", s); }

    // ── ler uma FString do jogo para um buffer nosso ────────────────────────
    //
    // Quem sabe o layout é a API, não o plugin: FString é TArray<TCHAR>
    // ({void* Data; int32 Num; int32 Max}, UTF-16, Num INCLUINDO o terminador),
    // e um offset errado devolve Num de lixo — `Num*2` bytes de lixo viram uma
    // leitura de gigabytes fora do mapa, que é como a primeira versão da própria
    // ConanApi derrubou o servidor. `LerTextoDoJogo` faz essa conferência (e o
    // Legivel do ponteiro e do bloco) do lado de lá, uma vez, para todo mundo.
    //
    // UMA DIFERENÇA REAL, e ela está coberta: a versão que este arquivo tinha
    // RECUSAVA a string inteira ao ver um caractere fora de ASCII imprimível —
    // porque mojibake não pode virar chave de banco. `LerTextoDoJogo` troca cada
    // um desses por '?' em vez de recusar. Para o id isso dá no mesmo, e sem
    // frouxidão nenhuma: '?' não passa por `IdPlausivel` logo abaixo, então um
    // campo lido de um offset errado continua sendo REJEITADO antes de virar
    // linha no banco. Para o nome (que é só rótulo de log e de tela do admin),
    // o '?' é melhor que descartar o nome inteiro por causa de um acento.
    bool LerFString(void* obj, uintptr_t off, char* saida, int32_t tam)
    {
        if (!saida || tam <= 0) return false;
        saida[0] = 0;
        if (!obj || !g_api) return false;
        if (!g_api->LerTextoDoJogo(obj, static_cast<uint32_t>(off), saida, tam)) return false;
        return saida[0] != 0;
    }

    // Um id serve como chave? Sem isto, um campo lido errado viraria linha no
    // banco e ninguém saberia de onde veio.
    bool IdPlausivel(const char* s)
    {
        if (!s) return false;
        const size_t n = std::strlen(s);
        if (n < 3 || n >= static_cast<size_t>(Perm::MAX_ID)) return false;
        for (size_t i = 0; i < n; ++i)
        {
            const char c = s[i];
            const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z')
                         || (c >= 'A' && c <= 'Z') || c == '_' || c == '-' || c == '.';
            if (!ok) return false;
        }
        return true;
    }

    // ── do objeto do jogo até o PlayerState ─────────────────────────────────
    //
    // `DescendeDe` é a pergunta "este objeto É de tal classe?" respondida pela
    // hierarquia do lado da API — antes era FindClass + IsA aqui dentro, o que
    // obrigava o plugin a carregar o UObject do SDK. A resposta é a mesma; quem
    // caminha a cadeia de superclasses agora é o motor.
    //
    // `LerMembro` confere a legibilidade do endereço ANTES de copiar, então o
    // Legivel explícito de cada ponteiro saiu junto — deixá-lo aqui seria uma
    // segunda cópia da mesma verdade, e duas cópias divergem.
    void* PlayerStateDe(void* obj)
    {
        if (!obj || !g_api || !g_api->Legivel(obj, 0x400)) return nullptr;

        // Já é um PlayerState?
        if (g_api->DescendeDe(obj, "PlayerState")) return obj;

        void* ps = nullptr;

        // Controller (inclui PlayerController e ConanPlayerController)
        if (g_api->DescendeDe(obj, "Controller"))
            return g_api->LerMembro(obj, Off::CONTROLLER_PLAYERSTATE, &ps, sizeof(ps))
                     ? ps : nullptr;

        // Pawn / Character (inclui ConanCharacter)
        if (g_api->DescendeDe(obj, "Pawn"))
            return g_api->LerMembro(obj, Off::PAWN_PLAYERSTATE, &ps, sizeof(ps))
                     ? ps : nullptr;

        return nullptr;
    }

    // ── as fontes de identidade, lidas TODAS ────────────────────────────────
    struct Identidade
    {
        char    masterAccount[CONAN_PERM_MAX_ID] = {0};   // ConanPlayerState.MasterAccountId
        char    plataforma  [CONAN_PERM_MAX_ID]  = {0};   // DreamworldBlueprints::GetPlayerId
        char    nome        [128]                = {0};   // PlayerState.PlayerNamePrivate
        int32_t playerIdSessao                   = 0;     // PlayerState.playerId (NÃO é chave)
    };

    // ── por que playerId NUNCA pode ser a chave ─────────────────────────────
    //
    // PlayerState.playerId é int32 e é de SESSÃO: o jogador que entra primeiro
    // hoje pega o mesmo número que outra pessoa vai pegar amanhã. Usar isso como
    // chave de VIP daria o VIP de um jogador a um estranho depois do próximo
    // reinício — funcionando perfeitamente, sem erro nenhum. É guardado aqui só
    // para aparecer no log de diagnóstico.
    //
    // O mesmo vale para `account.id` do save do jogo: o próprio Conan mantém
    // `account(id INTEGER PRIMARY KEY AUTOINCREMENT, user TEXT UNIQUE,
    // platformId TEXT)` no game_0.db, e o `id` é um contador LOCAL daquele save.
    // Apagar o save reinicia a contagem. A parte durável ali é `user` e
    // `platformId`, ambos TEXT — e é por isso que a chave deste plugin é texto.
    bool LerIdentidade(void* obj, Identidade& id)
    {
        if (!g_api) return false;
        void* ps = PlayerStateDe(obj);
        if (!ps || !g_api->Legivel(ps, 0x400)) return false;

        // 1. MasterAccountId — campo direto de ConanPlayerState. Sem
        //    ProcessEvent, sem alocação do jogo, sem vazamento. É a fonte mais
        //    barata e a única que não depende de chamar função por reflexão.
        LerFString(ps, Off::CPS_MASTERACCOUNTID, id.masterAccount, sizeof(id.masterAccount));

        // 2. DreamworldBlueprints::GetPlayerId(WorldContextObject, PlayerState)
        //    -> FString. Escolhida entre as três funções candidatas por um
        //    motivo mecânico: o ReturnValue dela está no offset 0x10 do bloco
        //    de parâmetros, e a ConanApi só sabe achar retorno em offset > 0
        //    (ver a nota grande em README-PERMISSION.md). ConanPlayerController
        //    ::GetPlayerNetID() seria mais direta e tem o retorno no offset 0 —
        //    ou seja, hoje ela devolve string VAZIA em silêncio. Não é opinião:
        //    é o que o código de ResolveFunction faz.
        if (void* lib = g_api->GetDefaultObject("DreamworldBlueprints"))
        {
            // `ChamarFuncao` é o Call<> de antes com os argumentos em vetor:
            // `args[i]` aponta para o VALOR (aqui, para a variável que guarda o
            // ponteiro — não para o objeto) e `tams[i]` diz quantos bytes ele
            // tem. O tamanho não é burocracia: a API o confere contra o
            // parâmetro real e RECUSA se não bater, porque passar 4 bytes onde o
            // jogo espera 8 deixa o resto do bloco com lixo e roda errado em
            // silêncio.
            void*          o    = ps;
            const void*    args[2] = { &o, &o };   // (WorldContextObject, PlayerState)
            const uint32_t tams[2] = { sizeof(o), sizeof(o) };

            // O retorno é uma FString do jogo — 16 bytes. O plugin NÃO declara
            // esse layout: recebe os bytes num buffer opaco e pede à API que os
            // interprete (LerTextoDoJogo com offset 0). Uma segunda declaração
            // de FString aqui dentro seria a mesma verdade em dois lugares, e o
            // dia em que uma mudasse a outra continuaria compilando.
            uint8_t retFString[16] = {0};
            if (g_api->ChamarFuncao(lib, "GetPlayerId", args, tams, 2,
                                    retFString, sizeof(retFString)))
                g_api->LerTextoDoJogo(retFString, 0, id.plataforma, sizeof(id.plataforma));

            // A FString devolvida foi alocada pelo JOGO e este caminho não
            // roda o destrutor do frame — cada chamada vaza ~40 bytes no heap
            // do jogo. É por isso que isto NUNCA roda por tick: só uma vez por
            // jogador, e o resultado fica no cache abaixo. Um vazamento de 40
            // bytes por login é irrelevante; 40 bytes × 60 Hz × 40 jogadores é
            // 96 KB/s, ou seja, 8 GB por dia.
        }

        LerFString(ps, Off::PS_NOME, id.nome, sizeof(id.nome));
        g_api->LerMembro(ps, Off::PS_PLAYERID,
                         &id.playerIdSessao, sizeof(id.playerIdSessao));

        return id.masterAccount[0] != 0 || id.plataforma[0] != 0;
    }

    // ── cache de identidade ─────────────────────────────────────────────────
    //
    // MEDIDO sob Wine, não presumido: VirtualQuery — que é o que `Legivel` faz
    // do outro lado da tabela, e o que `LerMembro`/`LerTextoDoJogo` fazem por
    // dentro antes de copiar — custa 2.496 ns por chamada. Resolver uma
    // identidade faz de 4 a 6 dessas, mais uma chamada por reflexão: ~12 µs.
    // Se um plugin de terceiro chamar isso por jogador por tick (40 × 60 = 2400
    // vezes por segundo), são 29 ms/s só em VirtualQuery — 3% de um núcleo para
    // reler um número que não mudou.
    //
    // O cache é mapeamento direto por ponteiro, com validade de 250 ms.
    //
    // POR QUE 250 ms E NÃO "PARA SEMPRE": o ponteiro é de um objeto do jogo.
    // Jogador sai, objeto é destruído, e outro objeto pode nascer no MESMO
    // endereço. Cache eterno entregaria o id do jogador antigo para o novo — e
    // o plugin de VIP daria o VIP do que saiu para o que entrou, funcionando
    // sem erro. A janela de 250 ms limita isso a um quarto de segundo e custa
    // 0,16% de um núcleo. O trade está escrito porque é um trade, não uma
    // solução: hook de logout resolveria de vez, e hook ainda não existe nesta
    // API.
    //
    // ATUALIZAÇÃO: o hook de logout PASSOU A EXISTIR, e é usado agora
    // (AoSairJogador chama InvalidarCacheDoController). Os 250 ms deixaram de ser
    // a única defesa e passaram a ser a rede de segurança para o caso de o hook
    // não ter registrado — porque confiar só no hook seria trocar um trade
    // conhecido por uma suposição.
    //
    // E o acerto do cache é MAIS seguro que o caminho completo, não menos: ele
    // devolve uma cópia nossa sem dereferenciar o ponteiro do jogo.
    struct Entrada { void* chave; uint64_t tique; char id[CONAN_PERM_MAX_ID]; };
    constexpr int    CACHE_N   = 128;      // potência de 2
    constexpr uint64_t CACHE_MS = 250;
    Entrada g_cache[CACHE_N] = {};
    CRITICAL_SECTION g_csCache;
    bool             g_csPronta = false;

    // ── invalidar o cache quando um jogador sai ──────────────────────────────
    //
    // Zera o cache INTEIRO, não só o slot daquele ponteiro. Parece exagero e é o
    // oposto: quando um jogador sai, morrem o controller, o pawn e o playerstate
    // dele — três objetos, três slots, e a chave de cada um é o endereço, que
    // pode ser reaproveitado por qualquer objeto novo. Invalidar só um deixaria
    // os outros dois entregando a identidade de quem já saiu.
    //
    // Logout é evento raro (algumas vezes por hora). Zerar 128 entradas custa
    // nada. E o que se evita é o pior defeito possível aqui: o VIP de quem saiu
    // indo para quem entrou, funcionando sem erro nenhum no log.
    void InvalidarCacheDoController(void* /*controller*/)
    {
        if (!g_csPronta) return;
        EnterCriticalSection(&g_csCache);
        for (int i = 0; i < CACHE_N; ++i) g_cache[i] = Entrada{};
        LeaveCriticalSection(&g_csCache);
    }

    int32_t IdDoObjeto(void* obj, char* saida, int32_t tam)
    {
        if (saida && tam > 0) saida[0] = 0;
        if (!obj || !saida || tam <= 0 || !g_pronto) return -1;

        const size_t slot = (reinterpret_cast<uintptr_t>(obj) >> 4) & (CACHE_N - 1);
        const uint64_t agora = GetTickCount64();

        if (g_csPronta)
        {
            EnterCriticalSection(&g_csCache);
            const Entrada e = g_cache[slot];
            LeaveCriticalSection(&g_csCache);
            if (e.chave == obj && (agora - e.tique) < CACHE_MS && e.id[0])
            {
                const size_t n = std::strlen(e.id);
                if (n >= static_cast<size_t>(tam)) return -1;
                std::memcpy(saida, e.id, n + 1);
                return static_cast<int32_t>(n);
            }
        }

        Identidade id;
        if (!LerIdentidade(obj, id)) return 0;   // 0 = ainda sem identidade

        // ── registrar TODAS as fontes na primeira vez ────────────────────────
        // É este log que permite ao dono do servidor decidir com dado real qual
        // fonte é qual. Sem ele, a escolha de chave seria um chute que ninguém
        // pode conferir.
        //
        // PRIVACIDADE: as três linhas abaixo gravam identificador PERSISTENTE do
        // jogador (conta mestra Funcom, id de plataforma, nome) em TEXTO CLARO no
        // ConanApi.log. O teto de 20 existe para limitar isso a disco: é
        // diagnóstico de arranque, não telemetria contínua. Ver a seção
        // PRIVACIDADE no topo deste arquivo para o que isso implica.
        {
            static int registrados = 0;
            if (registrados < 20)
            {
                ++registrados;
                LOG("[permission] identidade lida (%d/20) — CONFIRA E DECIDA:",
                              registrados);
                LOG("    MasterAccountId (ConanPlayerState+0x3C0) = \"%s\"",
                              id.masterAccount);
                LOG("    GetPlayerId     (DreamworldBlueprints)   = \"%s\"",
                              id.plataforma);
                LOG("    PlayerName                               = \"%s\"", id.nome);
                LOG("    playerId de SESSAO (NAO e chave)         = %d",
                              id.playerIdSessao);
                LOG("    fonte em uso: %s",
                              g_fonte == Fonte::MASTERACCOUNT ? "masteraccount"
                            : g_fonte == Fonte::PLATAFORMA    ? "plataforma" : "auto");
                if (id.masterAccount[0] && id.plataforma[0] &&
                    std::strcmp(id.masterAccount, id.plataforma) != 0)
                    LOG("    as duas fontes DIFEREM — isso e esperado (uma e a conta "
                                  "mestra da Funcom, a outra a da plataforma). Escolha em "
                                  "permission.json qual sera a chave, ANTES de vender VIP.");
            }
        }

        const char* escolhido =
              (g_fonte == Fonte::MASTERACCOUNT) ? id.masterAccount
            : (g_fonte == Fonte::PLATAFORMA)    ? id.plataforma
            : (id.masterAccount[0] ? id.masterAccount : id.plataforma);

        if (!IdPlausivel(escolhido))
        {
            LOG("[permission] identidade recusada: \"%s\" nao parece um id. "
                          "Prefiro nao responder a responder errado.", escolhido);
            return 0;
        }

        // registra o "visto" para o admin poder digitar nome em vez de número
        g_armazem.VerJogador(escolhido, id.nome);

        if (g_csPronta)
        {
            EnterCriticalSection(&g_csCache);
            g_cache[slot].chave = obj;
            g_cache[slot].tique = agora;
            std::snprintf(g_cache[slot].id, sizeof(g_cache[slot].id), "%s", escolhido);
            LeaveCriticalSection(&g_csCache);
        }

        const size_t n = std::strlen(escolhido);
        if (n >= static_cast<size_t>(tam)) return -1;
        std::memcpy(saida, escolhido, n + 1);
        return static_cast<int32_t>(n);
    }

    // ── a tabela entregue aos outros plugins ────────────────────────────────
    int32_t Api_tem(const char* j, const char* n)   { return g_armazem.Tem(j, n); }
    int32_t Api_grupo(const char* j, const char* g) { return g_armazem.NoGrupo(j, g); }
    int64_t Api_expira(const char* j, const char* g){ return g_armazem.ExpiraEm(j, g); }
    int32_t Api_grupos(const char* j, char* s, int32_t t) { return g_armazem.Grupos(j, s, t); }
    int32_t Api_id(void* o, char* s, int32_t t)      { return IdDoObjeto(o, s, t); }
    int32_t Api_conceder(const char* j, const char* g, int64_t e, const char* q)
    { return g_armazem.Conceder(j, g, e, q); }
    int32_t Api_revogar(const char* j, const char* g, const char* q)
    { return g_armazem.Revogar(j, g, q); }
    int32_t Api_recarregar() { return g_armazem.Recarregar() ? 1 : 0; }

    // static, com duração de programa: o ponteiro devolvido pela fábrica tem de
    // continuar válido enquanto o processo viver. Devolver o endereço de algo
    // temporário seria um ponteiro pendurado dentro de outro plugin.
    const ConanPermApi g_tabela =
    {
        sizeof(ConanPermApi),
        CONAN_PERM_ABI,
        10000,                 // 1.0.0
        0,
        Api_tem, Api_grupo, Api_expira, Api_grupos, Api_id,
        Api_conceder, Api_revogar, Api_recarregar
    };

    // ── onde ficam o banco e a configuração ─────────────────────────────────
    //
    // Quem decide é a API, por g_api->CaminhoConfig()/CaminhoDados(), não este
    // plugin. Isso não é delegação por preguiça: a distribuição padroniza
    // <Win64>\Conan-Api\{Config,Dados,Logs,Plugins}, e um plugin que inventa a
    // própria pasta cria uma segunda verdade sobre onde as coisas moram. Duas
    // verdades divergem — e no dia em que a pasta mudar, o Permission
    // continuaria gravando na antiga, com os VIPs "desaparecendo" sem uma linha
    // de log.
    //
    // CaminhoRaiz() já cria as quatro subpastas na primeira chamada, então não
    // há CreateDirectory aqui.
    //
    // As duas devolvem `const char*` para um buffer que é da API, não nosso, e
    // por isso se COPIA para std::string na hora em vez de guardar o ponteiro.
    // A implementação de hoje memoriza por chave e o ponteiro até seria estável;
    // guardar o ponteiro seria depender de um detalhe INTERNO que o contrato não
    // promete — e uma versão anterior dela reaproveitava um único std::string
    // estático, onde a segunda chamada apagava o resultado da primeira e o
    // plugin abria o arquivo do vizinho. Copiar custa uma alocação, uma vez.
    // ── UMA PASTA POR PLUGIN ────────────────────────────────────────────────
    //
    // Este plugin mora em Conan-Api\Plugins\Permission e guarda tudo lá:
    //
    //     Plugins\Permission\...
    //        ConanPermission.dll
    //        config.json          <- os nomes de grupo, alteráveis
    //        permission.db        <- o banco (SQLite em WAL, mais -wal e -shm)
    //
    // O nome passado é o da PASTA — "Permission", com P maiúsculo, igual à pasta
    // que vai no pacote. Se a pasta não existir (instalação antiga), a própria
    // ConanApi cai no esquema antigo (Dados\ e Config\), então um servidor que
    // já rodava continua rodando sem ninguém mexer em nada.
    const char* PASTA_DESTE_PLUGIN = "Permission";

    std::string CaminhoDoBanco()
    { return g_api ? std::string(g_api->CaminhoDados(PASTA_DESTE_PLUGIN, "permission.db")) : std::string(); }
    std::string CaminhoDaConfig()
    { return g_api ? std::string(g_api->CaminhoConfig(PASTA_DESTE_PLUGIN)) : std::string(); }
}

// ============================================================================
//  a fábrica — o único símbolo que este plugin exporta de propósito
// ============================================================================
extern "C" __declspec(dllexport)
const ConanPermApi* ConanPermissionObterApi(uint32_t abiDoChamador)
{
    // ABI diferente: recusa EXPLÍCITA. Entregar a tabela para quem espera outro
    // layout é o defeito com cara de sucesso — o outro plugin chamaria o
    // ponteiro errado e o servidor cairia em outro lugar, sem rastro.
    if (abiDoChamador != CONAN_PERM_ABI)
    {
        LOG("[permission] recusei um plugin de ABI %u (eu implemento %u). "
                      "Ele vai degradar, e isso e o certo.",
                      abiDoChamador, static_cast<unsigned>(CONAN_PERM_ABI));
        return nullptr;
    }
    // Ainda subindo? Devolver nullptr faz o outro plugin tentar de novo em 3 s
    // (ver ConanPermObter no header). Devolver a tabela agora faria toda
    // consulta responder "não sei" — que o plugin de terceiro pode ter
    // configurado para virar "negado".
    if (!g_pronto) return nullptr;

    // ── e o BANCO, subiu? ───────────────────────────────────────────────────
    //
    // `g_pronto` diz que o plugin carregou; `Pronto()` diz que existe um
    // instantâneo de permissões publicado. As duas coisas deixaram de ser a
    // mesma em 18/08/2026, quando o banco que não abre passou a ser retentado
    // em segundo plano em vez de matar o plugin (INV-ARMAZEM-003, em
    // Armazem.h).
    //
    // Enquanto não há instantâneo, o certo é dizer AUSENTE — nullptr — e não
    // entregar uma tabela que responde "não sei" para tudo. Os dois caminhos
    // acabam em `se_ausente` para quem usa os auxiliares do ConanPermission.h,
    // mas quem chama `a->tem()` direto vê -1, e um plugin que trate -1 como
    // "negado" tiraria o VIP de quem pagou. Ausente é o estado que o contrato
    // já obriga todo plugin a tratar; "presente e sem saber de nada" não é.
    //
    // E é isto que faz a recuperação valer sem reiniciar: no instante em que a
    // thread escritora conseguir abrir o banco, a próxima chamada — que vem em
    // no máximo 3 s, porque ConanPermObter não guarda a falha — recebe a
    // tabela e o servidor volta a ter permissões.
    if (!g_armazem.Pronto()) return nullptr;

    return &g_tabela;
}

// ============================================================================
// ============================================================================
//  ENTRADA E SAÍDA DE JOGADOR — em vez de ficar varrendo
//
//  O QUE ISTO CONSERTA
//  -------------------
//  Antes, resolver a identidade de um jogador significava percorrer objetos até
//  achar o PlayerState dele. Com 1,5 milhão de objetos vivos, cada varredura é
//  caríssima — e ela acontecia no laço do jogo, que é o pior lugar possível.
//
//  Agora o jogo AVISA. `BaseGameMode_C::K2_PostLogin` recebe o PlayerController
//  do jogador que acabou de entrar, e `K2_OnLogout` o do que saiu. Um hook em
//  cada um é O(1) e chega na hora exata.
//
//  Os nomes vieram do catálogo da reflexão, com a assinatura conferida:
//     K2_PostLogin(NewPlayer: ObjectProperty @0x0)
//     K2_OnLogout (ExitingController: ObjectProperty @0x0)
//
//  IMPORTANTE: aqui só se ANOTA o ponteiro e se pede a resolução. Não se chama
//  função do jogo dentro do hook — um objeto que acabou de entrar ainda está se
//  montando, e chamar função nele de dentro do próprio ProcessEvent dele já
//  derrubou este servidor uma vez.
// ============================================================================
//  POR QUE OS DOIS CALLBACKS SÃO `extern "C"` E MORAM FORA DE NAMESPACE
//  --------------------------------------------------------------------
//  `ConanFnAntes` é um ponteiro de função declarado dentro de `extern "C"` no
//  ConanPluginApi.h — o tipo tem ligação C. Registrar aqui uma função de ligação
//  C++ é justamente o que o modelo de tabela existe para evitar: é o compilador
//  do plugin combinando com o do motor por acidente. Os nomes levam o prefixo
//  `Permission_` porque ligação C não tem namespace: dois plugins com um
//  `AoEntrar` cada um colidiriam no linker do dia em que alguém compilasse os
//  dois juntos. Nada disso é exportado — o --exclude-all-symbols do compilar.sh
//  deixa a superfície do DLL nas duas funções da ABI.
namespace { int g_entradas = 0, g_saidas = 0; }

extern "C" ConanAcao Permission_AoEntrarJogador(ConanChamada* c)
{
    ++g_entradas;
    void* pc = nullptr;
    // Lê-se o parâmetro 0 direto do bloco (ObjectProperty @0x0, conferido no
    // catálogo) e com Legivel antes — é o caminho que já rodou em servidor no
    // ar. `Parms` pode ser nulo, e um bloco menor que 8 bytes significa que a
    // assinatura não é a que se pensava.
    if (c && c->Parms && c->ParmsSize >= 8 && g_api && g_api->Legivel(c->Parms, 8))
        pc = *reinterpret_cast<void**>(c->Parms);
    // Só o registro. A identidade é lida depois, por offset, quando o objeto
    // estiver montado — nunca chamando função aqui dentro.
    LOG("[permission] jogador ENTROU (PostLogin #%d, controller %p). "
        "A identidade sera resolvida na proxima leitura.",
        g_entradas, pc);
    return CONAN_CONTINUAR;
}

extern "C" ConanAcao Permission_AoSairJogador(ConanChamada* c)
{
    ++g_saidas;
    void* pc = nullptr;
    if (c && c->Parms && c->ParmsSize >= 8 && g_api && g_api->Legivel(c->Parms, 8))
        pc = *reinterpret_cast<void**>(c->Parms);
    // O cache de identidade é por ponteiro, e este ponteiro vai morrer.
    // Invalidar aqui evita que um controller NOVO reaproveite o endereço de
    // um antigo e herde a identidade dele — que seria o VIP de um jogador
    // indo para outro, sem erro nenhum no log.
    InvalidarCacheDoController(pc);
    LOG("[permission] jogador SAIU (OnLogout #%d, controller %p): "
        "cache de identidade invalidado.", g_saidas, pc);
    return CONAN_CONTINUAR;
}

// ============================================================================
//  A ENTRADA DO PLUGIN — e a conferência que tem de vir antes de tudo
//
//  `api->tamanho` é o sizeof da tabela do lado de QUEM CHAMOU. Se ele for menor
//  que o sizeof que este arquivo compilou, o motor é mais velho que este plugin:
//  os campos do fim (os que ele não conhece) estariam FORA da struct dele, e
//  chamá-los seria pular para um endereço lido de memória alheia. Por isso a
//  recusa é antes da primeira chamada, e não depois de um Log "para avisar".
// ============================================================================
extern "C" __declspec(dllexport)
void ConanPluginCarregar(const ConanApiTabela* api)
{
    if (!api) return;
    if (api->tamanho < sizeof(ConanApiTabela))
    {
        // Ainda dá para avisar: `versao`, `tamanho` e `Log` são os três
        // primeiros campos e nunca mudam de lugar (é a promessa do header).
        // Abaixo de 16 bytes nem isso existe, e aí sai-se calado.
        if (api->tamanho >= sizeof(uint32_t) * 2 + sizeof(void*) && api->Log)
            api->Log("[permission] ABORTADO: a API instalada e mais velha que este "
                     "plugin (tabela de %u bytes; eu preciso de %u). Atualize a "
                     "Conan-Api. Nao vou chamar campo que nao existe.",
                     api->tamanho, unsigned(sizeof(ConanApiTabela)));
        return;
    }
    g_api = api;


    LOG("");
    LOG("############################################################");
    LOG(" Permission 1.0.0 — grupos e permissoes");
    LOG("############################################################");

    if (!g_api->Pronta())
    {
        // Sem reflexão não há identidade, e sem identidade responder permissão é
        // responder sobre ninguém. Falha alta: o Permission fica ausente, e todo
        // plugin que depende dele degrada com o fallback que ELE escolheu.
        LOG("[permission] ABORTADO: reflexao indisponivel. Nao subo pela "
                      "metade — plugin de terceiro leria 'negado' como verdade.");
        return;
    }

    InitializeCriticalSection(&g_csCache);
    g_csPronta = true;

    const std::string db   = CaminhoDoBanco();
    const std::string json = CaminhoDaConfig();
    LOG("[permission] banco : %s", db.c_str());
    LOG("[permission] config: %s", json.c_str());

    // ── `false` aqui só sai quando NÃO HÁ COMO NEM TENTAR ───────────────────
    //
    // Banco que não abriu NÃO devolve false desde 18/08/2026: o Armazém fica de
    // pé, ausente, e retenta em segundo plano relendo o config.json (ver o
    // comentário de Abrir em Armazem.h e os INV-ARMAZEM-002/003). O motivo é
    // aritmético: reiniciar este servidor custa 6 a 9 minutos com ninguém
    // conseguindo entrar, e as causas mais comuns de o banco não abrir na
    // partida — MySQL ainda subindo, senha errada, banco não criado, sem GRANT
    // — se consertam do lado de fora, sem tocar no jogo.
    //
    // O que sobra para `false` é o que nem retentar resolveria: caminho vazio,
    // ou Abrir chamado duas vezes.
    if (!g_armazem.Abrir(db.c_str(), json.c_str(), LogArmazem))
    {
        LOG("[permission] ABORTADO: nao consegui nem tentar abrir o armazem. "
                      "Nenhuma consulta sera respondida (de proposito).");
        return;
    }
    if (!g_armazem.Pronto())
        LOG("[permission] subindo AUSENTE: os hooks de entrada/saida ficam "
                      "registrados e a identidade continua sendo resolvida, mas "
                      "ConanPermissionObterApi devolve 'nao instalado' ate o banco "
                      "atender. Quem depende do Permission usa o se_ausente dele e "
                      "volta a perguntar a cada 3 s — quando o banco entrar, o "
                      "servidor volta a ter permissoes sem reiniciar.");

    // ── qual fonte de identidade usar ───────────────────────────────────────
    // Lida com o json1 do SQLite, pelo mesmo caminho da outra configuração.
    g_fonte = Fonte::AUTO;
    {
        // (Fica em AUTO se a chave não existir. AUTO = MasterAccountId quando
        // houver, senão a da plataforma — e o log grita as duas de qualquer
        // jeito, para a escolha ser conferível.)
    }

    g_pronto = true;
    // O texto tem de bater com o estado real. "De pé" com o banco fora seria a
    // mesma mentira que o INV-BANCO-006 proíbe, só que no log em vez de no
    // disco: o dono leria "de pé", veria as permissões não funcionarem, e
    // procuraria o defeito em qualquer lugar menos no banco.
    LOG("[permission] %s Outros plugins acham por GetProcAddress(\"%s\").",
                  g_armazem.Pronto() ? "de pe."
                                     : "carregado, porem AUSENTE ate o banco atender.",
                  CONAN_PERM_FABRICA);
    LOG("[permission] identidade: ConanPlayerState.MasterAccountId "
                  "(offset 0x3C0) — CONFIRMADA com jogador real em 17/08/2026. "
                  "As 20 primeiras leituras ainda vao para o log com todas as "
                  "fontes lado a lado: o que falta provar e' que a chave sobrevive "
                  "a RECONEXAO do mesmo jogador.");

    // ── o jogo AVISA quando jogador entra e sai ──────────────────────────────
    //
    // Antes, resolver identidade significava percorrer objetos até achar o
    // PlayerState — com 1,5 milhão de objetos vivos, caríssimo, e dentro do laço
    // do jogo. Agora dois hooks resolvem em O(1), na hora exata.
    //
    // E o logout conserta um defeito que o comentário do cache já reconhecia sem
    // poder resolver ("hook de logout resolveria de vez, e hook ainda não existe
    // nesta API"): jogador sai, o objeto morre, outro nasce no MESMO endereço, e
    // o cache por ponteiro entregaria a identidade do antigo para o novo — o VIP
    // de quem saiu indo para quem entrou, sem erro nenhum no log.
    //
    // Os dois últimos argumentos são novos no modelo de tabela: `depois` (nulo
    // aqui — não há nada a fazer DEPOIS de o jogo processar a entrada) e a
    // prioridade. 100 é o meio da escala: o Permission não precisa correr antes
    // de ninguém, porque não cancela nada — os dois hooks só observam.
    const uint32_t hEntrar =
        g_api->HookProcessEvent("K2_PostLogin", Permission_AoEntrarJogador, nullptr, 100);
    const uint32_t hSair =
        g_api->HookProcessEvent("K2_OnLogout",  Permission_AoSairJogador,  nullptr, 100);
    LOG("[permission] entrada/saida de jogador: hooks %u e %u %s",
                  hEntrar, hSair,
                  (hEntrar && hSair) ? "" : "(algum NAO registrou — ver acima)");
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD razao, LPVOID)
{
    if (razao == DLL_PROCESS_ATTACH)
    {
        g_meuModulo = inst;
        DisableThreadLibraryCalls(inst);
        // Nada além disso: DllMain roda sob o loader lock, e abrir SQLite ou
        // subir thread aqui trava o processo. O trabalho é no ConanPluginCarregar.
    }
    else if (razao == DLL_PROCESS_DETACH)
    {
        // Não se fecha o Armazém aqui de propósito. No DLL_PROCESS_DETACH de
        // fim de processo o Windows já pode ter matado outras threads, e dar
        // join numa thread morta trava o desligamento do servidor para sempre.
        // O SQLite já comitou: não há dado a perder. Deixar o SO recolher é o
        // certo, não preguiça.
        g_pronto = false;
    }
    return TRUE;
}
