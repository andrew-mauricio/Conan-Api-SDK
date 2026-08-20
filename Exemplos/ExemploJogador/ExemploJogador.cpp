// ExemploJogador — from "somebody spoke" to that player's character.
//
// WHY THIS EXAMPLE EXISTS
// -----------------------
// The SDK taught calling a function, hooking, building text and querying
// permissions. It didn't teach the jump EVERY useful plugin has to make:
//
//     "somebody typed !kit"  ->  WHO was it?  ->  where are they?  ->  act
//
// Without that the developer has 9,247 classes and no way in. This file is that
// way in, and it fits in twenty lines.
//
// EVERYTHING BY NAME, NO OFFSET BAKED IN
// --------------------------------------
// `OffsetDoMembro` resolves against the reflection of whichever build is
// running. Baking 0x308 into the binary works today and reads the neighbouring
// field after Funcom's next patch — no error, no log, just wrong data. The cost
// of resolving by name is one lookup on the first call; the cost of baking the
// number in is a plugin that lies quietly.
#include "Conan/ConanPluginApi.h"

#include <cstdio>
#include <cstring>

static const ConanApiTabela* g_api = nullptr;

// ── read a pointer member, BY NAME ─────────────────────────────────────────
//
// The pattern that repeats in every plugin: resolve the offset once, read the
// pointer, check it's readable before handing it back.
static void* MembroPonteiro(void* obj, const char* nome)
{
    if (!obj) return nullptr;
    const int32_t off = g_api->OffsetDoMembro(obj, nome);
    if (off < 0) return nullptr;                 // doesn't exist on this build

    void* p = nullptr;
    if (g_api->LerMembro(obj, uint32_t(off), &p, sizeof(p)) <= 0) return nullptr;
    // `Legivel` says the memory is MAPPED, not that the object is alive. It's
    // there to reject an obviously invalid pointer before touching it.
    if (!p || !g_api->Legivel(p, 8)) return nullptr;
    return p;
}

// ── who spoke, and where they are ──────────────────────────────────────────
extern "C" ConanAcao AoFalar(ConanChamada* c)
{
    char texto[256];
    if (g_api->LerTextoDoJogo(c->Parms, 0x068, texto, sizeof(texto)) <= 0)
        return CONAN_CONTINUAR;
    if (std::strcmp(texto, "!ondeestou") != 0)
        return CONAN_CONTINUAR;

    // 1. WHO. In a hook, `c->Obj` is the object that received the call — here,
    //    the ConanPlayerController of whoever typed.
    void* controller = c->Obj;

    // 2. THE NAME. It lives on the PlayerState, not the controller.
    char nome[128] = "(desconhecido)";
    if (void* ps = MembroPonteiro(controller, "PlayerState"))
    {
        const int32_t off = g_api->OffsetDoMembro(ps, "PlayerNamePrivate");
        if (off >= 0) g_api->LerTextoDoJogo(ps, uint32_t(off), nome, sizeof(nome));
    }

    // 3. THE CHARACTER. `Character` is the already-typed pawn; `Pawn` covers
    //    the case where the class isn't a character. Trying both covers both.
    void* corpo = MembroPonteiro(controller, "Character");
    if (!corpo) corpo = MembroPonteiro(controller, "Pawn");

    // 4. THE POSITION. Through the game's function, not the field:
    //    `RelativeLocation` is replicated, and reading the raw field gets the
    //    value from before the last replication. The function walks the path the
    //    game itself uses.
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

// ── and with no hook at all: sweep for who's online ────────────────────────
//
// The other way in. Useful for a scheduled task, an admin command, or anything
// that doesn't start with a player speaking.
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

    // The offsets this plugin uses, resolved NOW so the log shows they came
    // from reflection and not from a baked-in constant.
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
