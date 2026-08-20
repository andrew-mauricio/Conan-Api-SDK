// ============================================================================
//  ExemploComando — intercepting a chat command, which is what 90% of server
//                   plugins need to do
//
//  Copy this folder to write your own `!kit`, `!vip`, `!tp`.
//
//  This plugin includes nothing of ours beyond `ConanPluginApi.h` — pure
//  declarations. Everything it does goes through the `g_api->` table, and none
//  of our code is compiled or linked in here.
//
//  ────────────────────────────────────────────────────────────────────────────
//  HOW CHAT REACHES THE SERVER
//  ────────────────────────────────────────────────────────────────────────────
//  The client sends an RPC: `ConanPlayerController::ServerSendChatMessage`. The
//  `Server` prefix is Unreal's convention for "this runs on the server at the
//  client's request" — exactly where a command should be handled.
//
//  Its only parameter is a 128-byte `ChatRpcData` struct, whose layout came
//  from reflection (not from guessing):
//
//      +0x000  uint64   Timestamp
//      +0x008  struct   UserId  (UniqueNetIdRepl)
//      +0x038  int64    CharacterUniqueID     <- the "uid" you see in the log
//      +0x040  int64    targetUniqueId
//      +0x048  FString  userName
//      +0x058  FString  Channel
//      +0x068  FString  Message               <- the text that was typed
//      +0x078  bool     generated
//
//  CANCELLING IN THE HOOK SWALLOWS THE MESSAGE. That's what makes a command a
//  command: the player types `!kit`, the plugin acts, and the text never shows
//  up in everyone's chat. Returning `CONAN_CONTINUAR` lets the message through
//  as normal.
//
//  ────────────────────────────────────────────────────────────────────────────
//  WHY THIS EXAMPLE LOGS INSTEAD OF ANSWERING IN CHAT
//  ────────────────────────────────────────────────────────────────────────────
//  Answering the player is possible, and Conan Shop does it in production. What
//  it takes is an FString the GAME allocated: FString is a pointer into memory
//  the game manages, so handing it a buffer of ours means the game may try to
//  free memory that isn't its own, or hold a pointer that dies when our
//  function returns.
//
//  The API solves that with `ConanApi::Texto` / `ConanApi::TextoRico`, which
//  build the value through the game's own allocator — see ConanBase.h. This
//  example deliberately stays at the smallest thing that works: it logs, so
//  that the one idea it teaches (intercept and swallow) isn't buried under a
//  second one. For the answering side, read Conan Shop's `Falar()`.
// ============================================================================
#include "Conan/ConanPluginApi.h"
#include <windows.h>
#include <cstring>
#include <cstdint>

// The table the loader hands us. Everything goes through it; there's no
// implementation of ours inside this binary.
static const ConanApiTabela* g_api = nullptr;

// offsets inside ChatRpcData, measured from reflection
static const uint32_t CHAT_UID     = 0x038;
static const uint32_t CHAT_USUARIO = 0x048;
static const uint32_t CHAT_CANAL   = 0x058;
static const uint32_t CHAT_TEXTO   = 0x068;

static int g_comandos = 0;
static int g_mensagens = 0;

// ── reading an FString out of the parameter block ────────────────────────────
//
// This plugin used to decode the FString by hand. FString is
// {void* Data; int32 Num; int32 Max}, on Windows the character is UTF-16, and
// `Num` INCLUDES the terminator — get that wrong and you cut the last
// character or read one junk char too many. That detail is now the API's
// problem: `LerTextoDoJogo(base, offset, ...)` checks the memory is readable
// before touching it and hands back a char*. A layout mistake gets fixed in one
// place, with no plugin recompiled.
static bool LerTexto(const void* base, uint32_t off, char* saida, int tam)
{
    saida[0] = 0;
    return g_api->LerTextoDoJogo(base, off, saida, tam) != 0;
}

// The callback is `extern "C"` and takes a `ConanChamada*`: the table is plain
// C, and that's what makes this plugin build under any compiler.
extern "C" ConanAcao AoFalar(ConanChamada* c)
{
    // Parameter 0 is the ChatRpcData struct BY VALUE — so it sits at the start
    // of the parameter block, and isn't a pointer to follow.
    if (!c || !c->Parms || c->ParmsSize < 0x80) return CONAN_CONTINUAR;
    const void* chat = c->Parms;

    char texto[512] = {0}, usuario[128] = {0}, canal[64] = {0};
    LerTexto(chat, CHAT_TEXTO,   texto,   sizeof(texto));
    LerTexto(chat, CHAT_USUARIO, usuario, sizeof(usuario));
    LerTexto(chat, CHAT_CANAL,   canal,   sizeof(canal));

    // The uid is a raw int64, not text: it's still worth asking the API whether
    // the memory is readable before reading. A game object can be inside the
    // garbage collector's window, and reading a dead pointer takes the whole
    // server down.
    int64_t uid = 0;
    const uint8_t* pUid = static_cast<const uint8_t*>(chat) + CHAT_UID;
    if (g_api->Legivel(pUid, 8))
        uid = *reinterpret_cast<const int64_t*>(pUid);

    ++g_mensagens;
    g_api->Log("[chat] %s (uid %lld) no canal \"%s\": \"%s\"",
               usuario[0] ? usuario : "?", (long long)uid,
               canal[0] ? canal : "?", texto);

    // ── is it a command? ────────────────────────────────────────────────────
    //
    // WHY `!` AND NOT `/` — and how we found out
    // ------------------------------------------
    // The first version used `/`. In the live test, `/apiteste` DISAPPEARED
    // from the player's chat — which looked like success — but it never reached
    // this hook: the log records the message before deciding to cancel, and no
    // line came out.
    //
    // Meaning: the CLIENT filtered the unknown command and never sent it to the
    // server. The text died before leaving the player's machine.
    //
    // That's the worst kind of ambiguous result: the visible symptom ("it
    // vanished from chat") was the same either way, and without the server log
    // there was no telling "my hook cancelled it" from "it never arrived". The
    // log is what settled it.
    //
    // Hence the `!` prefix. The client treats `!something` as ordinary text and
    // sends it; `/something` it tries to resolve locally first.
    const char PREFIXO = '!';
    if (texto[0] != PREFIXO) return CONAN_CONTINUAR;   // ordinary talk: let it by

    ++g_comandos;

    if (std::strncmp(texto, "!apiteste", 9) == 0)
    {
        // The command's action would go here. As a demo, it just logs.
        g_api->Log("[chat] >>> COMANDO !apiteste de %s. Este texto NAO vai aparecer "
                   "no chat: o hook cancelou a mensagem, que e' o que faz um comando "
                   "ser um comando.", usuario);
        g_api->Log("[chat]     comandos ate agora: %d de %d mensagens",
                   g_comandos, g_mensagens);
        return CONAN_CANCELAR;                        // swallow the message
    }

    // Unknown command: let it through. Swallowing every `!` would have this
    // plugin hijack what other plugins handle — and the player would be left
    // wondering why the game stopped answering.
    g_api->Log("[chat]     \"%s\" nao e' meu comando; deixei passar", texto);
    return CONAN_CONTINUAR;
}

extern "C" __declspec(dllexport)
void ConanPluginCarregar(const ConanApiTabela* api)
{
    // Check the table BEFORE using any field. A plugin compiled against a
    // BIGGER table, running on an older API, would read a pointer past the end
    // of the struct and call junk. Leaving quietly beats taking the server down
    // — and you can't even warn through Log, because Log is a field of the very
    // table you don't trust.
    if (!api || api->tamanho < sizeof(ConanApiTabela)) return;
    g_api = api;

    g_api->Log("");
    g_api->Log("=== ExemploComando ===");
    if (!g_api->Pronta()) { g_api->Log("  reflexao indisponivel"); return; }

    // Signature: (name, before, after, priority). No "after" callback — the
    // decision to swallow the message is taken BEFORE the game runs, otherwise
    // the text has already gone out to everyone. Priority 100 is mid-table:
    // anyone needing to decide ahead of this one can ask for a lower number.
    const uint32_t id = g_api->HookProcessEvent("ServerSendChatMessage",
                                                AoFalar, nullptr, 100);
    if (id)
    {
        g_api->Log("  hook %u em ConanPlayerController::ServerSendChatMessage", id);
        g_api->Log("  fale no chat para ver aparecer aqui.");
        g_api->Log("  digite !apiteste para ver um comando ser ENGOLIDO (nao aparece no chat).");
        g_api->Log("  o prefixo e' ! e nao /: o cliente do jogo filtra / desconhecido e nao envia.");
    }
    else
        g_api->Log("  x nao registrei o hook — ver o motivo acima");
}
