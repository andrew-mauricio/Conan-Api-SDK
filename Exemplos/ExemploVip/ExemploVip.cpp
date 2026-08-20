// ============================================================================
//  ExemploVip — the smallest third-party plugin that uses Permission
//
//  THIS is the file the community will copy. It shows the five things every
//  plugin depending on Permission has to get right:
//
//    1. DON'T LINK against Permission. Include the header, nothing more.
//       Check after building:
//           x86_64-w64-mingw32-objdump -p ExemploVip.dll | grep "DLL Name"
//       ConanPermission.dll must NOT show up there. If it does, your plugin
//       stops loading on every server that didn't install Permission.
//
//    2. DECIDE what happens when Permission isn't installed. That's the third
//       argument, and it's mandatory on purpose.
//
//    3. ASK AT THE MOMENT OF USE, NEVER AT LOAD TIME. This is the change the
//       live test forced on us, and it has its own section below.
//
//    4. DON'T query per tick for no reason. The query costs ~194 ns
//       (measured), but turning the game object into an id costs ~12 us the
//       first time for each player — resolve it when the player shows up and
//       keep the id.
//
//    5. Treat -1 as -1. "I don't know" is not "no".
//
//  ────────────────────────────────────────────────────────────────────────────
//  WHY THE QUERY MOVED OUT OF LOAD TIME (item 3)
//  ────────────────────────────────────────────────────────────────────────────
//  An earlier version of this file asked Permission from inside
//  ConanPluginCarregar() itself. It looked harmless and wasn't: ConanLoader
//  loads DLLs in whatever order FindFirstFile returns, which is NOT specified.
//  If ExemploVip.dll comes up before ConanPermission.dll — and "Exemplo" sorts
//  ahead of "Permission", so that's the LIKELY case, not the rare one — the
//  query runs while Permission doesn't yet exist in the process.
//
//  The result wasn't an error: it was the log saying "Permission NAO esta
//  instalado" on a server where it was installed and working. A silent defect,
//  the kind a server owner spends hours chasing.
//
//  And there was no player in the world at that moment either, so a sweep for
//  PlayerControllers had nothing to find: the plugin loads alongside the
//  server, long before anyone connects.
//
//  The fix isn't "retry later during load". It's recognising the question only
//  makes sense when somebody ASKS for the benefit. So this plugin now hooks
//  chat and only queries when the player types `!vip`. At that instant three
//  things are true for free, with no synchronisation at all: the server has
//  fully come up, Permission is already in the process, and the
//  PlayerController that asked is right there — it's the hook's own `c->Obj`.
//
//  Notice what went away with it: the sweep over world objects. Taking the
//  controller of whoever spoke is cheaper, simpler, and can't hand back the
//  wrong player.
//
//  ────────────────────────────────────────────────────────────────────────────
//  WHAT THIS PLUGIN CARRIES INSIDE IT: NOTHING OF OURS
//  ────────────────────────────────────────────────────────────────────────────
//  A single declaration header (ConanPluginApi.h) and a table of function
//  pointers received at load time. There's no .a to link, no .cpp of ours to
//  compile alongside, and the hook engine never enters this binary. Same
//  principle as Permission, applied to the API — and for the same reason: a C++
//  library doesn't cross a compiler boundary without quietly corrupting
//  memory.
// ============================================================================
#include "Conan/ConanPluginApi.h"      // the table; this is all that's ours here
#include "Conan/ConanPermission.h"     // header only; nothing to link

#include <windows.h>
#include <cstdio>
#include <cstring>

// The table the loader hands over. Everything this plugin knows how to do with
// the game goes through it.
static const ConanApiTabela* g_api = nullptr;

// Our chat hook's id, so we can give it back on unload.
static uint32_t g_hookChat = 0;

// ── offsets inside ChatRpcData ──────────────────────────────────────────────
//
// They came from the reflection catalogue, not from guessing. The struct is 128
// bytes and arrives BY VALUE, so it starts at the beginning of the parameter
// block.
static const uint32_t CHAT_UID     = 0x038;   // int64  CharacterUniqueID
static const uint32_t CHAT_USUARIO = 0x048;   // FString userName
static const uint32_t CHAT_TEXTO   = 0x068;   // FString Message

// ── the benefit, at the moment it's asked for ───────────────────────────────
//
// `controller` is the ConanPlayerController of whoever typed — the owner of the
// intercepted call. It's exactly what Permission expects in
// ConanPermIdDoController().
static void AtenderPedidoDeVip(void* controller, const char* usuario)
{
    // Graceful degradation, which is the point of the example. You do NOT abort
    // when Permission is missing: the plugin stays loaded, keeps answering, and
    // simply grants nothing. A plugin that refuses to exist without Permission
    // forces everyone to install it — and the promise was the opposite.
    //
    // This question happens HERE, not at load time, on purpose: now "missing"
    // really means missing. Up there it meant "hasn't come up yet", which is a
    // completely different answer wearing the same face.
    if (!ConanPermDisponivel())
    {
        g_api->Log("[vip] %s pediu VIP, mas o ConanPermission.dll nao esta "
                   "instalado. Nao concedo nada. Instale-o em "
                   "Conan-Api/Plugins/ para ligar os beneficios.", usuario);
        return;
    }

    char id[CONAN_PERM_MAX_ID];
    const int32_t len = ConanPermIdDoController(controller, id, sizeof(id));
    if (len <= 0)
    {
        // 0 = the player has no identity yet (still joining).
        // -1 = an error, or Permission went away between the line above and
        //      this one.
        // In BOTH cases the right answer is to neither grant nor deny: saying
        // the player isn't VIP would be the silent defect — whoever paid
        // wouldn't get their kit and nobody would see an error.
        g_api->Log("[vip] %s: sem id ainda (%d) — pode pedir de novo em "
                   "alguns segundos", usuario, (int)len);
        return;
    }

    // se_ausente = 0: with no Permission installed, nobody is VIP. That's the
    // conservative choice and it IS a choice — a "restricted area" plugin might
    // want 1 here (no access control, let everyone in), and the header makes
    // you say which of the two you mean.
    const int32_t vip   = ConanPermTem(id, "vip.kit.diario", /*se_ausente=*/0);
    const int32_t tele  = ConanPermTem(id, "vip.teleporte",  /*se_ausente=*/0);
    const int64_t vence = ConanPermExpiraEm(id, "vip");

    char grupos[256];
    ConanPermGrupos(id, grupos, sizeof(grupos));
    for (char* p = grupos; *p; ++p) if (*p == '\n') *p = ' ';   // one line in the log

    g_api->Log("[vip] id=%s  kit.diario=%d  teleporte=%d  vence=%lld  grupos=[%s]",
               id, (int)vip, (int)tele, (long long)vence, grupos);

    // A real plugin would hand out the kit here; this one logs and TELLS the
    // player.
    //
    // ── IT ANSWERS NOW, AND THAT CHANGED ON 2026-08-19 ──────────────────────
    //
    // It used to be true that handing text to the game crashed the server,
    // because the FString was OURS and ProcessEvent destroyed it with the
    // GAME's allocator. So this plugin did all the work — chat hook,
    // identification, database lookup, decision — and then said nothing. From
    // the player's side, indistinguishable from a broken plugin.
    //
    // What changed: table v3 brought MensagemNaTela, and it asks the GAME
    // ITSELF for the FText (KismetTextLibrary::Conv_StringToText). No memory of
    // ours crosses the boundary, invariant I-2 still stands, and the channel is
    // PROVED on a real player's screen — 2026-08-19, "rodada 3/8" visible in
    // Andrew's client.
    //
    // A plugin that decides correctly and tells nobody hasn't solved the server
    // owner's problem: it just moved it into a log file.
    char aviso[256];
    if (vip == 1)
    {
        g_api->Log("[vip] -> %s TEM direito ao kit diario. (aqui entraria a "
                   "entrega do kit)", usuario);
        std::snprintf(aviso, sizeof(aviso),
                      "VIP confirmado: voce tem direito ao kit diario.");
    }
    else
    {
        g_api->Log("[vip] -> %s nao tem vip.kit.diario", usuario);
        std::snprintf(aviso, sizeof(aviso),
                      "Voce ainda nao tem VIP. Fale com o dono do servidor.");
    }

    // If the answer doesn't go out, SAY SO. Silence here would bring back
    // exactly the defect this block exists to fix.
    if (controller)
    {
        const int r = g_api->MensagemNaTela(controller, aviso, 8.0f);
        if (!r) g_api->Log("[vip] AVISO: nao consegui responder na tela de %s "
                           "(a decisao acima valeu, mas ele nao foi informado)",
                           usuario);
    }
    else
    {
        g_api->Log("[vip] AVISO: sem controller nesta chamada; %s nao foi "
                   "informado da decisao.", usuario);
    }
}

// ── the chat hook ───────────────────────────────────────────────────────────
//
// extern "C", taking a ConanChamada*: the signature is plain C ABI, which is
// what lets the engine live in one binary and this callback in another.
extern "C" ConanAcao AoFalar(ConanChamada* c)
{
    if (!c || !c->Parms || c->ParmsSize < 0x80) return CONAN_CONTINUAR;

    char texto[512];
    texto[0] = 0;
    // The API reads the game's FString into a char* for us — including
    // checking the memory is readable before touching it. Doing it by hand was
    // the way for a while, and reading a dead pointer inside the garbage
    // collector's window takes the whole server down.
    if (!g_api->LerTextoDoJogo(c->Parms, CHAT_TEXTO, texto, sizeof(texto)))
        return CONAN_CONTINUAR;

    // ── THE PREFIX IS `!`, NOT `/` ──────────────────────────────────────────
    //
    // Measured on the live server: `"oi"`, `"!apiteste"` and `"teste/barra"`
    // all reach the hook; `"/apiteste"` NEVER does. Conan's client tries to
    // resolve `/command` locally and sends nothing to the server — no plugin
    // gets to see it. The on-screen symptom is identical to a hook that
    // cancelled ("it vanished from chat"), and only the server log separates
    // the two.
    if (texto[0] != '!') return CONAN_CONTINUAR;

    // Exactly `!vip`, not `!vipanything`: swallowing what isn't ours would
    // have this plugin hijack another plugin's command, and the player would be
    // left wondering why the server stopped answering.
    if (std::strncmp(texto, "!vip", 4) != 0) return CONAN_CONTINUAR;
    if (texto[4] != 0 && texto[4] != ' ')   return CONAN_CONTINUAR;

    char usuario[128];
    usuario[0] = 0;
    if (!g_api->LerTextoDoJogo(c->Parms, CHAT_USUARIO, usuario, sizeof(usuario)))
        std::strcpy(usuario, "?");
    if (!usuario[0]) std::strcpy(usuario, "?");

    int64_t uid = 0;
    g_api->LerMembro(c->Parms, CHAT_UID, &uid, sizeof(uid));

    g_api->Log("[vip] !vip pedido por %s (uid %lld)", usuario, (long long)uid);

    // `c->Obj` is the ConanPlayerController that received the RPC — whoever
    // spoke. No need to hunt for it in the world, and no way to grab the wrong
    // player.
    AtenderPedidoDeVip(c->Obj, usuario);

    // Cancelling swallows the message: that's what makes a command a command.
    // The `!vip` never shows up in everyone's chat.
    return CONAN_CANCELAR;
}

extern "C" __declspec(dllexport)
void ConanPluginCarregar(const ConanApiTabela* api)
{
    // Without this, a plugin compiled against a BIGGER table running on an
    // older API would read a pointer past the end of the struct and call junk.
    // It doesn't give an error: it gives a jump to some arbitrary address, on
    // the game's thread.
    if (!api || api->tamanho < sizeof(ConanApiTabela)) return;
    g_api = api;

    g_api->Log("");
    g_api->Log("=== ExemploVip — plugin de terceiro usando o Permission ===");

    if (!g_api->Pronta()) { g_api->Log("[vip] reflexao indisponivel; abortado"); return; }

    // Notice what does NOT happen here: no question to Permission. At this
    // instant it may not even be loaded, and asking now would only produce a
    // wrong answer wearing a right one's face. See "WHY THE QUERY MOVED OUT OF
    // LOAD TIME" at the top of the file.
    g_hookChat = g_api->HookProcessEvent("ServerSendChatMessage", AoFalar, nullptr, 100);
    if (!g_hookChat)
    {
        // 0 = it failed, and the API already logged the reason itself.
        g_api->Log("[vip] nao consegui enganchar o chat; o comando !vip nao vai "
                   "funcionar");
        return;
    }

    g_api->Log("[vip] pronto. Digite !vip no chat do jogo para pedir o beneficio.");
    g_api->Log("=== fim ===");
}

extern "C" __declspec(dllexport)
void ConanPluginDescarregar(void)
{
    // Give the hook back. The API only lets you remove OUR hook — it records
    // which module registered each one.
    //
    // Honest about today's state: ConanLoader does NOT call this function (it
    // unloads no plugin at all; the server process dies whole). It's here
    // because the contract in ConanPluginApi.h provides for it, it costs
    // nothing, and the day the loader starts unloading, this plugin is already
    // right. What it is NOT: a cleanup guarantee. Don't put anything here that
    // genuinely has to happen.
    if (g_api && g_hookChat) { g_api->RemoverHook(g_hookChat); g_hookChat = 0; }
}
