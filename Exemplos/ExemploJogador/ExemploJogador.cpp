// ExemploJogador — do "alguem falou" ate' o personagem daquele jogador.
//
// POR QUE ESTE EXEMPLO EXISTE
// ---------------------------
// O SDK ensinava a chamar funcao, a hookar, a montar texto e a consultar
// permissao. Nao ensinava o pulo que TODO plugin util precisa dar:
//
//     "alguem digitou !kit"  ->  QUEM foi?  ->  onde ele esta?  ->  age
//
// Sem isso o dev tem 9.247 classes e nenhum caminho de entrada. Este arquivo e'
// esse caminho, e ele cabe em vinte linhas.
//
// TUDO POR NOME, NENHUM OFFSET GRAVADO
// -------------------------------------
// `OffsetDoMembro` resolve pela reflexao da build que estiver rodando. Gravar
// 0x308 no binario funciona hoje e le' o campo vizinho depois do proximo patch
// da Funcom — sem erro, sem log, so' com dado errado. O custo de resolver por
// nome e' uma busca na primeira chamada; o custo de gravar o numero e' um
// plugin que mente em silencio.
#include "Conan/ConanPluginApi.h"

#include <cstdio>
#include <cstring>

static const ConanApiTabela* g_api = nullptr;

// ── ler um ponteiro de um membro, pelo NOME ────────────────────────────────
//
// O padrao que se repete em todo plugin: resolve o offset uma vez, le' o
// ponteiro, confere que ele e' legivel antes de devolver.
static void* MembroPonteiro(void* obj, const char* nome)
{
    if (!obj) return nullptr;
    const int32_t off = g_api->OffsetDoMembro(obj, nome);
    if (off < 0) return nullptr;                 // nao existe nesta build

    void* p = nullptr;
    if (g_api->LerMembro(obj, uint32_t(off), &p, sizeof(p)) <= 0) return nullptr;
    // `Legivel` diz que a memoria esta MAPEADA, nao que o objeto esta vivo.
    // Serve para recusar ponteiro obviamente invalido antes de tocar nele.
    if (!p || !g_api->Legivel(p, 8)) return nullptr;
    return p;
}

// ── quem falou, e onde ele esta ────────────────────────────────────────────
extern "C" ConanAcao AoFalar(ConanChamada* c)
{
    char texto[256];
    if (g_api->LerTextoDoJogo(c->Parms, 0x068, texto, sizeof(texto)) <= 0)
        return CONAN_CONTINUAR;
    if (std::strcmp(texto, "!ondeestou") != 0)
        return CONAN_CONTINUAR;

    // 1. QUEM. Num hook, `c->Obj` e' o objeto que recebeu a chamada — aqui, o
    //    ConanPlayerController de quem digitou.
    void* controller = c->Obj;

    // 2. O NOME. Fica no PlayerState, nao no controller.
    char nome[128] = "(desconhecido)";
    if (void* ps = MembroPonteiro(controller, "PlayerState"))
    {
        const int32_t off = g_api->OffsetDoMembro(ps, "PlayerNamePrivate");
        if (off >= 0) g_api->LerTextoDoJogo(ps, uint32_t(off), nome, sizeof(nome));
    }

    // 3. O PERSONAGEM. `Character` e' o pawn ja' tipado; `Pawn` serve quando a
    //    classe nao for de personagem. Tentar os dois cobre os dois casos.
    void* corpo = MembroPonteiro(controller, "Character");
    if (!corpo) corpo = MembroPonteiro(controller, "Pawn");

    // 4. A POSICAO. Pela funcao do jogo, nao pelo campo: `RelativeLocation` e'
    //    replicado, e ler o campo cru pega o valor de antes da ultima
    //    replicacao. A funcao percorre o caminho que o proprio jogo usa.
    char msg[192];
    if (corpo)
    {
        struct { double X, Y, Z; } pos{};
        if (g_api->ChamarFuncao(corpo, "K2_GetActorLocation",
                                nullptr, nullptr, 0, &pos, sizeof(pos)) == 1 &&
            g_api->UltimaChamadaExecutou())
        {
            std::snprintf(msg, sizeof(msg), "%s, voce esta em %.0f / %.0f / %.0f",
                          nome, pos.X, pos.Y, pos.Z);
        }
        else
        {
            std::snprintf(msg, sizeof(msg),
                          "%s, achei o seu personagem mas a posicao nao respondeu.", nome);
        }
    }
    else
    {
        std::snprintf(msg, sizeof(msg),
                      "%s, voce esta conectado mas sem personagem no mundo.", nome);
    }

    g_api->MensagemParaJogador(nome, msg);
    g_api->Log("[Jogador] !ondeestou -> %s", msg);
    return CONAN_CANCELAR;
}

// ── e sem hook nenhum: varrer quem esta online ─────────────────────────────
//
// O outro caminho de entrada. Serve para tarefa agendada, comando de admin, ou
// qualquer coisa que nao comece com o jogador falando.
static void ListarOnline()
{
    void* pcs[64];
    const int n = g_api->FindObjects("ConanPlayerController", pcs, 64,
                                     /*incluirFilhas=*/1);
    g_api->Log("[Jogador] %d controller(s) no mundo:", n < 0 ? 0 : n);
    for (int i = 0; i < n; ++i)
    {
        char nome[128] = "(sem nome)";
        if (void* ps = MembroPonteiro(pcs[i], "PlayerState"))
        {
            const int32_t off = g_api->OffsetDoMembro(ps, "PlayerNamePrivate");
            if (off >= 0) g_api->LerTextoDoJogo(ps, uint32_t(off), nome, sizeof(nome));
        }
        void* corpo = MembroPonteiro(pcs[i], "Character");
        g_api->Log("   [%d] %-24s personagem: %s", i, nome, corpo ? "sim" : "nao");
    }
}

extern "C" __declspec(dllexport)
void ConanPluginCarregar(const ConanApiTabela* api)
{
    if (!api || api->tamanho < sizeof(ConanApiTabela)) return;
    g_api = api;

    g_api->Log("=====================================================");
    g_api->Log(" ExemploJogador — do chat ate' o personagem");
    g_api->Log("=====================================================");

    // Os offsets que este plugin usa, resolvidos AGORA para o log mostrar que
    // vieram da reflexao e nao de constante gravada.
    if (void* cdo = g_api->GetDefaultObject("ConanPlayerController"))
    {
        g_api->Log("   PlayerState  -> offset 0x%X (resolvido por nome)",
                   g_api->OffsetDoMembro(cdo, "PlayerState"));
        g_api->Log("   Character    -> offset 0x%X",
                   g_api->OffsetDoMembro(cdo, "Character"));
        g_api->Log("   Pawn         -> offset 0x%X",
                   g_api->OffsetDoMembro(cdo, "Pawn"));
    }

    ListarOnline();

    if (g_api->HookProcessEvent("ServerSendChatMessage", AoFalar, nullptr, 100) == 0)
        g_api->Log("   AVISO: nao consegui hookar o chat");
    else
        g_api->Log("   pronto: digite !ondeestou no jogo");
    g_api->Log("=====================================================");
}

extern "C" __declspec(dllexport)
void ConanPluginDescarregar(void) {}
