// ExemploVigia — a welcome message, a player count and chat commands.
//
// What it does, and why these choices:
//   · welcomes whoever joins        (the K2_PostLogin event, from EVENTS.md)
//   · answers !online in chat       (ServerSendChatMessage, offset 0x068)
//   · answers !vigia only for those with permission  (Permission, degrading)
//
// Written by following Docs/DEVELOPERS.md line by line, to find what trips up
// somebody arriving who knows nothing yet.

#include "Conan/ConanPluginApi.h"
#include "Conan/ConanPermission.h"

#include <cstdio>
#include <cstring>

static const ConanApiTabela* g_api = nullptr;

// How many joined since the server came up. A deliberately simple counter: the
// point is to exercise the path, not to be clever.
static int g_entraram = 0;

// ── how many players are online right now ───────────────────────────────────
//
// The docs say FindObjects finds live objects by class. That's how you count
// people online without keeping state of your own (which goes out of sync on
// the first client crash).
static int ContarOnline()
{
    void* achados[128];
    const int n = g_api->FindObjects("ConanPlayerController", achados, 128, /*incluirFilhas=*/1);
    return n < 0 ? 0 : n;
}

// ── somebody spoke in chat ──────────────────────────────────────────────────
extern "C" ConanAcao AoFalar(ConanChamada* c)
{
    char texto[512];
    // 0x068 = Message inside ChatRpcData. The guide warns that a raw offset is
    // the one thing a game update can move — noted.
    if (g_api->LerTextoDoJogo(c->Parms, 0x068, texto, sizeof(texto)) <= 0)
        return CONAN_CONTINUAR;

    if (texto[0] != '!')
        return CONAN_CONTINUAR;              // ordinary talk carries on

    // To answer you need the NAME of whoever spoke: MensagemParaJogador
    // addresses by name, not by controller. userName sits at 0x048.
    char quem[128];
    if (g_api->LerTextoDoJogo(c->Parms, 0x048, quem, sizeof(quem)) <= 0)
        return CONAN_CONTINUAR;

    if (std::strcmp(texto, "!online") == 0)
    {
        char msg[128];
        std::snprintf(msg, sizeof(msg), "Online agora: %d — entraram %d desde o boot.",
                      ContarOnline(), g_entraram);
        g_api->MensagemParaJogador(quem, msg);
        g_api->Log("[Vigia] !online respondido a %s (%d online)", quem, ContarOnline());
        return CONAN_CANCELAR;               // swallow it: a command, not talk
    }

    if (std::strcmp(texto, "!vigia") == 0)
    {
        // The docs insist: query at USE time, never at load, and pick the
        // absent-value by the cost of being wrong. This only shows statistics,
        // so denying for 30 s during a MySQL outage hurts nobody.
        char id[64];
        int libera = 0;
        if (ConanPermIdDoController(c->Obj, id, sizeof(id)) > 0)
            libera = (ConanPermTem(id, "vigia.ver", /*se_ausente=*/0) == 1);

        if (libera)
        {
            char msg[160];
            std::snprintf(msg, sizeof(msg), "Vigia: %d online, %d entraram, hooks ativos.",
                          ContarOnline(), g_entraram);
            g_api->MensagemParaJogador(quem, msg);
        }
        else
        {
            g_api->MensagemParaJogador(quem, "Vigia: voce nao tem permissao (vigia.ver).");
        }
        return CONAN_CANCELAR;
    }

    return CONAN_CONTINUAR;
}

// ── somebody joined the server ──────────────────────────────────────────────
extern "C" ConanAcao AoEntrar(ConanChamada* c)
{
    ++g_entraram;

    // NewPlayer is the first parameter (EVENTS.md says so). Without it there's
    // nobody to send the message to.
    void* controller = nullptr;
    if (g_api->LerParm(c, 0, &controller, sizeof(controller)) > 0 && controller)
    {
        // Here we have the controller (not the name), so the route is the screen.
        g_api->MensagemNaTela(controller,
            "Bem-vindo! Digite !online para ver quem esta jogando.", 8.0f);
    }

    g_api->Log("[Vigia] jogador entrou (%d desde o boot, %d online)",
               g_entraram, ContarOnline());
    return CONAN_CONTINUAR;                  // never cancel a login
}

extern "C" __declspec(dllexport)
void ConanPluginCarregar(const ConanApiTabela* api)
{
    // The safety net the guide insists on: a plugin compiled against a bigger
    // table, running on an older API, would read a pointer past the end of the
    // struct.
    if (!api || api->tamanho < sizeof(ConanApiTabela))
        return;

    g_api = api;
    g_api->Log("[Vigia] subiu — tabela v%d, %d bytes", (int)api->versao, (int)api->tamanho);

    // WATCH the direction: HookProcessEvent returns the hook's ID, and ZERO is
    // the failure. I wrote `!= 0` first, out of C's "0 = ok" reflex, and the
    // warning would have fired exactly when it worked. The guide shows the call
    // without using the return value, so anyone who decides to check has to
    // guess.
    if (g_api->HookProcessEvent("ServerSendChatMessage", AoFalar, nullptr, 100) == 0)
        g_api->Log("[Vigia] AVISO: nao consegui hookar o chat");

    if (g_api->HookProcessEvent("K2_PostLogin", AoEntrar, nullptr, 100) == 0)
        g_api->Log("[Vigia] AVISO: nao consegui hookar a entrada de jogador");

    g_api->Log("[Vigia] pronto: !online e !vigia");
}

extern "C" __declspec(dllexport)
void ConanPluginDescarregar(void)
{
    if (g_api)
        g_api->Log("[Vigia] saindo — %d entraram nesta sessao", g_entraram);
}
