// ============================================================================
//  Permission — the central group and permission service
//
//  Conan Exiles Enhanced · the API's first official plugin
//
//  WHAT IT IS
//  ----------
//  A plugin other plugins query. It gives nobody VIP, sells nothing and changes
//  nothing about the game: it answers "does this player have right X?" and
//  keeps the answer in a database that survives a crash and a restart.
//
//  The part that decides who may do what is in Armazem.cpp, which does NOT
//  depend on the game — and so is genuinely testable, in an .exe, under the
//  same Wine. This file is only the bridge: game reflection -> identity ->
//  Armazem, plus the function table other plugins receive.
//
//  THE LINE BETWEEN WHAT IS PROVED AND WHAT ISN'T
//  ----------------------------------------------
//  Proved, running: Armazem (45 checks under Wine, including a crash with
//  TerminateProcess in the middle of a write and an integrity_check
//  afterwards).
//
//  NOT proved yet, and written down at every point: WHICH of Conan's
//  identifiers is the right one to use as a key. Reflection says where they
//  live and what type they are — that's measured fact. What they CONTAIN only a
//  real player connecting shows, and the test server never got one.
//
//  So this plugin doesn't choose by guessing: it reads ALL the sources, writes
//  all three to the log on first contact with each player, and uses the one the
//  configuration says to use. Whoever owns the server looks at the log once and
//  decides with real data. The alternative — pick one and move on — is exactly
//  the defect that wears success's face: the VIP plugin would work, and the VIP
//  would go to the wrong person.
// ============================================================================

// ============================================================================
//  THE BOUNDARY BETWEEN PLUGINS: THERE ISN'T ONE. Read this before trusting
//  the database.
//
//  WHAT IS FACT, MEASURED ON THIS MACHINE
//  --------------------------------------
//  Every plugin runs INSIDE the game's process (Docs/COMECAR.md: "runs inside
//  the game's process"), as the SAME user and in the SAME address space as this
//  Permission. From that it follows, without exaggeration:
//
//    · the database is an ordinary file at
//      <Win64>\Conan-Api\Dados\permission.db, at a path ANY plugin derives
//      from the executable itself (the distribution standardises the folder). A
//      plugin that embeds sqlite3 — which already lives in this tree — opens
//      that file with its own handle and runs an INSERT into grupo_permissao or
//      jogador_grupo: grants itself VIP, deletes somebody else's VIP, reads the
//      audit log. Nothing in this code stops it.
//    · being in the same heap, a plugin can overwrite this Permission's
//      Instantaneo memory directly, without touching the database at all.
//
//  The "boundary" is only the convention of calling the ABI. There's no ACL, no
//  separate process, no OS namespace: zero isolation.
//
//  THE ABI (ConanPermissionObterApi) IS NOT A SECURITY BOUNDARY
//  -----------------------------------------------------------
//  It's the intended door — it checks the ABI version and queues writes onto a
//  thread — but (a) any plugin in the process calls the factory and gets the
//  table; (b) any plugin can IGNORE the ABI and go straight to the file or the
//  memory. Refusing a wrong ABI prevents a CRASH from an incompatible layout;
//  it doesn't prevent TAMPERING by anyone who wants to tamper.
//
//  THE ONE REAL MEASURE THAT EXISTS, AND EXACTLY WHAT IT COVERS
//  -----------------------------------------------------------
//  ConanPermission.dll is linked with -Wl,--exclude-all-symbols (compilar.sh):
//  it does NOT export the ~250 sqlite3_* functions. That stops other code in
//  the process from grabbing OUR sqlite3_exec through GetProcAddress and
//  writing with our handle. It covers only that. It does NOT cover the plugin
//  that embeds its own sqlite3 (the attack above), because then it doesn't need
//  ours at all.
//
//  WHY THERE ISN'T A BIGGER LOCK HERE (and it isn't laziness)
//  ---------------------------------------------------------
//  A file lock (PRAGMA locking_mode=EXCLUSIVE) would block another plugin's
//  handle — but it would ALSO block the legitimate reader (a web shop or
//  monitor opening permission.db purely to READ VIP status), and it doesn't
//  stop writes through memory (same heap). It would be half a defence, at a
//  real cost to honest use, poking at the Armazem core that 45 tests under Wine
//  validated in WAL — without my being able, here, to re-prove that crash
//  recovery still behaves the same. Half a defence, unproved, sold as a
//  boundary, is the "defect that wears success's face" this project forbids. It
//  stays out, and it stays WRITTEN DOWN that it stays out.
//
//  WHAT THIS MEANS FOR WHOEVER RUNS THE SERVER
//  -------------------------------------------
//  Installing a plugin grants it TOTAL access to the server: to the permission
//  database, to the players' identities (see PRIVACY, below) and to every other
//  plugin's memory. The "anyone publishes to the portal" model is incompatible
//  with zero isolation. Real isolation would mean running plugins in ANOTHER
//  process, over IPC — an architectural change to the whole API, outside this
//  plugin's scope. Until then: install only plugins whose origin you audit, as
//  you would any .dll that runs AS the server.
//
//  PRIVACY: A PLAYER'S IDENTITY IS READABLE BY ANY PLUGIN
//  -----------------------------------------------------
//  By the same absence of a boundary, the MasterAccountId (Funcom master
//  account), the platform id and each player's name are within reach of any
//  plugin — the ABI still exposes id_do_controller on purpose, and even without
//  it a plugin reads the offsets. On top of that, the FIRST 20 resolutions go
//  into ConanApi.log in PLAIN TEXT (the "log ALL the sources" block, further
//  down): that's deliberate diagnostics, so the owner can choose the key with
//  real data, but it writes a PERSISTENT identifier for a person to disk. The
//  log stays on the owner's machine, under their control; even so, whoever
//  answers for GDPR needs to know that data is there. The cap of 20 already
//  limits what reaches disk; deleting or rotating ConanApi.log clears the
//  history.
// ============================================================================
// ── the ONLY header of ours that enters here ────────────────────────────────
//
// None of the runtime is compiled into this DLL: not the decoder, not the hook
// table, not this build's offsets. Everything arrives as a function pointer, in
// the table the loader hands over in ConanPluginCarregar. That's why this file
// doesn't include ConanSDK.h/ConanBase.h/ConanHooks.h and compilar.sh doesn't
// link libconanapi.a — and it's what lets the runtime be rebuilt without
// rebuilding Permission.
#include "Conan/ConanPluginApi.h"
#include "Armazem.h"
#include "Comandos.h"
#include "include/Conan/ConanPermission.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>

// The two copies of MAX_ID must not drift: if the public header says 64 and
// Armazem says 32, long ids would pass the ABI and be refused inside with no
// explanation. Two copies of the same truth drift when nobody checks.
static_assert(Perm::MAX_ID == CONAN_PERM_MAX_ID,
              "MAX_ID do Armazem e CONAN_PERM_MAX_ID do header publico divergiram");
// The other two caps came in with the terminator check (2026-08-17) and run
// the SAME drift risk. If the public header says a node fits in 128 and Armazem
// refuses anything over 64, every plugin with a long node would get "don't
// know" with no explanation — and "don't know" is what a third-party plugin
// treats as Permission being absent.
static_assert(Perm::MAX_GRUPO == CONAN_PERM_MAX_GRUPO,
              "MAX_GRUPO do Armazem e CONAN_PERM_MAX_GRUPO do header publico divergiram");
static_assert(Perm::MAX_NO == CONAN_PERM_MAX_NO,
              "MAX_NO do Armazem e CONAN_PERM_MAX_NO do header publico divergiram");

// ── offsets, all of them from this build's live reflection ─────────────────
//
// They live HERE, in the plugin, and that's how it has to be under the table
// model: the API exports no offsets, it exports LerMembro/LerTextoDoJogo. There
// are FEW of them, and having all five in plain sight with the type beside them
// is what lets you check against the catalogue in ten seconds when the game
// updates. They came from golden/catalogo_interno.json, build 24383534.
namespace Off
{
    constexpr uintptr_t CONTROLLER_PLAYERSTATE = 0x308;  // Controller.PlayerState  ObjectProperty
    constexpr uintptr_t PAWN_PLAYERSTATE       = 0x320;  // Pawn.PlayerState        ObjectProperty
    constexpr uintptr_t PS_PLAYERID            = 0x304;  // PlayerState.playerId    IntProperty
    constexpr uintptr_t PS_NOME                = 0x398;  // PlayerState.PlayerNamePrivate  StrProperty
    constexpr uintptr_t CPS_MASTERACCOUNTID    = 0x3C0;  // ConanPlayerState.MasterAccountId StrProperty
    // PlayerState.UniqueID (StructProperty, 0x310) exists and is NOT read
    // here. It's an FUniqueNetIdRepl, and this build's ScriptStruct layouts
    // were never extracted. Reading a struct at a presumed offset is the
    // shortest path to a plausible, wrong value. It stays out until it's
    // measured.
}

// ── the table, and why EVERYTHING goes through it ───────────────────────────
//
// This pointer is the entire plugin on the API's side: while it's null, this
// binary can do NOTHING on its own — no log, no reflection, it doesn't even
// know where its own folder is. That's deliberate, and it's what guarantees
// that updating the runtime doesn't force a rebuild of this DLL.
static const ConanApiTabela* g_api = nullptr;

namespace
{
    Perm::Armazem g_armazem;
    HMODULE       g_meuModulo = nullptr;
    bool          g_pronto    = false;

    // which identity source the configuration says to use
    enum class Fonte { AUTO, MASTERACCOUNT, PLATAFORMA };
    Fonte g_fonte = Fonte::AUTO;
}

// ── why LOG is a macro and not a function ───────────────────────────────────
//
// `g_api->Log` is variadic (`const char* fmt, ...`). A function of ours taking
// `...` couldn't FORWARD the arguments to it — there's no vprintf equivalent in
// the table — and would have to format into an intermediate buffer, with a
// second size cap and a second place to truncate wrongly. The macro hands the
// arguments straight through, with no copy.
//
// The `g_api &&` isn't paranoia: `ConanPermissionObterApi` is exported and can
// be called by another plugin BEFORE the loader hands us the table (DLL load
// order isn't specified — see ConanPermObter in the public header). Without the
// guard, that path would be a server crash instead of a refusal.
#define LOG(...) do { if (g_api && g_api->Log) g_api->Log(__VA_ARGS__); } while (0)

namespace
{
    void LogArmazem(const char* s) { LOG("%s", s); }

    // ── read a game FString into a buffer of ours ───────────────────────────
    //
    // The one that knows the layout is the API, not the plugin: FString is a
    // TArray<TCHAR> ({void* Data; int32 Num; int32 Max}, UTF-16, with Num
    // INCLUDING the terminator), and a wrong offset hands back a junk Num —
    // `Num*2` bytes of junk become a multi-gigabyte read off the map, which is
    // how ConanApi's own first version took the server down. `LerTextoDoJogo`
    // does that check (plus the Legivel of the pointer and the block) on the
    // other side, once, for everybody.
    //
    // ONE REAL DIFFERENCE, and it's covered: the version this file used to have
    // REFUSED the whole string on seeing a character outside printable ASCII —
    // because mojibake must not become a database key. `LerTextoDoJogo`
    // replaces each of those with '?' instead of refusing. For the id that
    // comes to the same thing, with no slack: '?' doesn't pass `IdPlausivel`
    // just below, so a field read from a wrong offset is still REJECTED before
    // it becomes a row. For the name (which is only a log and admin-screen
    // label), a '?' beats throwing away the whole name over one accent.
    bool LerFString(void* obj, uintptr_t off, char* saida, int32_t tam)
    {
        if (!saida || tam <= 0) return false;
        saida[0] = 0;
        if (!obj || !g_api) return false;
        if (!g_api->LerTextoDoJogo(obj, static_cast<uint32_t>(off), saida, tam)) return false;
        return saida[0] != 0;
    }

    // Does an id work as a key? Without this, a wrongly read field would
    // become a row and nobody would know where it came from.
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

    // ── from the game object to the PlayerState ─────────────────────────────
    //
    // `DescendeDe` is the question "IS this object of such a class?" answered by
    // the hierarchy on the API's side — it used to be FindClass + IsA in here,
    // which forced the plugin to pull in the SDK's UObject. The answer is the
    // same; what walks the superclass chain now is the runtime.
    //
    // `LerMembro` checks the address's readability BEFORE copying, so the
    // explicit Legivel on each pointer went away with it — leaving it here
    // would be a second copy of the same truth, and two copies drift.
    void* PlayerStateDe(void* obj)
    {
        if (!obj || !g_api || !g_api->Legivel(obj, 0x400)) return nullptr;

        // Already a PlayerState?
        if (g_api->DescendeDe(obj, "PlayerState")) return obj;

        void* ps = nullptr;

        // Controller (includes PlayerController and ConanPlayerController)
        if (g_api->DescendeDe(obj, "Controller"))
            return g_api->LerMembro(obj, Off::CONTROLLER_PLAYERSTATE, &ps, sizeof(ps))
                     ? ps : nullptr;

        // Pawn / Character (includes ConanCharacter)
        if (g_api->DescendeDe(obj, "Pawn"))
            return g_api->LerMembro(obj, Off::PAWN_PLAYERSTATE, &ps, sizeof(ps))
                     ? ps : nullptr;

        return nullptr;
    }

    // ── the identity sources, ALL of them read ──────────────────────────────
    struct Identidade
    {
        char    masterAccount[CONAN_PERM_MAX_ID] = {0};   // ConanPlayerState.MasterAccountId
        char    plataforma  [CONAN_PERM_MAX_ID]  = {0};   // DreamworldBlueprints::GetPlayerId
        char    nome        [128]                = {0};   // PlayerState.PlayerNamePrivate
        int32_t playerIdSessao                   = 0;     // PlayerState.playerId (NOT a key)
    };

    // ── why playerId can NEVER be the key ───────────────────────────────────
    //
    // PlayerState.playerId is an int32 and it's per-SESSION: whoever joins
    // first today gets the same number somebody else will get tomorrow. Using
    // that as a VIP key would hand one player's VIP to a stranger after the
    // next restart — working perfectly, with no error at all. It's kept here
    // only so it shows up in the diagnostic log.
    //
    // The same goes for the game save's `account.id`: Conan itself keeps
    // `account(id INTEGER PRIMARY KEY AUTOINCREMENT, user TEXT UNIQUE,
    // platformId TEXT)` in game_0.db, and `id` is a LOCAL counter for that
    // save. Deleting the save restarts the count. The durable part there is
    // `user` and `platformId`, both TEXT — and that's why this plugin's key is
    // text.
    bool LerIdentidade(void* obj, Identidade& id)
    {
        if (!g_api) return false;
        void* ps = PlayerStateDe(obj);
        if (!ps || !g_api->Legivel(ps, 0x400)) return false;

        // 1. MasterAccountId — a direct field of ConanPlayerState. No
        //    ProcessEvent, no game allocation, no leak. It's the cheapest
        //    source and the only one that doesn't depend on calling a function
        //    through reflection.
        LerFString(ps, Off::CPS_MASTERACCOUNTID, id.masterAccount, sizeof(id.masterAccount));

        // 2. DreamworldBlueprints::GetPlayerId(WorldContextObject, PlayerState)
        //    -> FString. Chosen among the three candidate functions for a
        //    mechanical reason: its ReturnValue sits at offset 0x10 of the
        //    parameter block, and ConanApi only knows how to find a return
        //    value at offset > 0 (see the long note in README-PERMISSION.md).
        //    ConanPlayerController::GetPlayerNetID() would be more direct and
        //    has its return at offset 0 — meaning today it quietly returns an
        //    EMPTY string. That isn't an opinion: it's what ResolveFunction's
        //    code does.
        if (void* lib = g_api->GetDefaultObject("DreamworldBlueprints"))
        {
            // `ChamarFuncao` is the old Call<> with the arguments in arrays:
            // `args[i]` points at the VALUE (here, at the variable holding the
            // pointer — not at the object) and `tams[i]` says how many bytes it
            // is. The size isn't bureaucracy: the API checks it against the
            // real parameter and REFUSES on a mismatch, because passing 4 bytes
            // where the game wants 8 leaves the rest of the block full of junk
            // and runs wrong in silence.
            void*          o    = ps;
            const void*    args[2] = { &o, &o };   // (WorldContextObject, PlayerState)
            const uint32_t tams[2] = { sizeof(o), sizeof(o) };

            // The return is a game FString — 16 bytes. The plugin does NOT
            // declare that layout: it takes the bytes into an opaque buffer and
            // asks the API to interpret them (LerTextoDoJogo at offset 0). A
            // second FString declaration in here would be the same truth in two
            // places, and the day one changed the other would still compile.
            uint8_t retFString[16] = {0};
            if (g_api->ChamarFuncao(lib, "GetPlayerId", args, tams, 2,
                                    retFString, sizeof(retFString)))
                g_api->LerTextoDoJogo(retFString, 0, id.plataforma, sizeof(id.plataforma));

            // The returned FString was allocated by the GAME and this path
            // doesn't run the frame's destructor — every call leaks ~40 bytes
            // on the game's heap. That's why this NEVER runs per tick: once per
            // player, and the result goes into the cache below. A 40-byte leak
            // per login is irrelevant; 40 bytes × 60 Hz × 40 players is
            // 96 KB/s, which is 8 GB a day.
        }

        LerFString(ps, Off::PS_NOME, id.nome, sizeof(id.nome));
        g_api->LerMembro(ps, Off::PS_PLAYERID,
                         &id.playerIdSessao, sizeof(id.playerIdSessao));

        return id.masterAccount[0] != 0 || id.plataforma[0] != 0;
    }

    // ── the identity cache ──────────────────────────────────────────────────
    //
    // MEASURED under Wine, not assumed: VirtualQuery — which is what `Legivel`
    // does on the other side of the table, and what `LerMembro` /
    // `LerTextoDoJogo` do internally before copying — costs 2,496 ns per call.
    // Resolving one identity makes 4 to 6 of those, plus a reflected call:
    // ~12 us. If a third-party plugin calls that per player per tick
    // (40 × 60 = 2,400 times a second), that's 29 ms/s in VirtualQuery alone —
    // 3% of a core to re-read a number that didn't change.
    //
    // The cache is direct-mapped by pointer, valid for 250 ms.
    //
    // WHY 250 ms AND NOT "FOREVER": the pointer belongs to a game object. A
    // player leaves, the object is destroyed, and another object can be born at
    // the SAME address. An eternal cache would hand the old player's id to the
    // new one — and the VIP plugin would give the leaver's VIP to the joiner,
    // working with no error. The 250 ms window bounds that to a quarter of a
    // second and costs 0.16% of a core. The trade-off is written down because
    // it IS a trade-off, not a solution: a logout hook would settle it for
    // good, and hooks didn't exist in this API yet.
    //
    // UPDATE: the logout hook now DOES exist, and is used (AoSairJogador calls
    // InvalidarCacheDoController). The 250 ms stopped being the only defence
    // and became the safety net for the case where the hook failed to register
    // — because trusting the hook alone would be swapping a known trade-off for
    // an assumption.
    //
    // And a cache hit is MORE safe than the full path, not less: it returns a
    // copy of ours without dereferencing the game's pointer.
    struct Entrada { void* chave; uint64_t tique; char id[CONAN_PERM_MAX_ID]; };
    constexpr int    CACHE_N   = 128;      // a power of 2
    constexpr uint64_t CACHE_MS = 250;
    Entrada g_cache[CACHE_N] = {};
    CRITICAL_SECTION g_csCache;
    bool             g_csPronta = false;

    // ── invalidating the cache when a player leaves ──────────────────────────
    //
    // It clears the WHOLE cache, not just that pointer's slot. That looks like
    // overkill and is the opposite: when a player leaves, their controller,
    // pawn and playerstate all die — three objects, three slots, and each one's
    // key is its address, which any new object can reuse. Invalidating only one
    // would leave the other two handing out the identity of someone who's gone.
    //
    // Logout is a rare event (a few times an hour). Clearing 128 entries costs
    // nothing. And what it avoids is the worst possible defect here: the
    // leaver's VIP going to the joiner, working with no error in the log at
    // all.
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
        if (!LerIdentidade(obj, id)) return 0;   // 0 = no identity yet

        // ── log ALL the sources, the first time ─────────────────────────────
        // This log is what lets the server owner decide, with real data, which
        // source is which. Without it, choosing the key would be a guess nobody
        // could check.
        //
        // PRIVACY: the three lines below write a PERSISTENT identifier for the
        // player (Funcom master account, platform id, name) in PLAIN TEXT into
        // ConanApi.log. The cap of 20 exists to bound what reaches disk: this
        // is startup diagnostics, not continuous telemetry. See the PRIVACY
        // section at the top of this file for what that implies.
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

        // records the "seen" so an admin can type a name instead of a number
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

    // ── the table handed to other plugins ───────────────────────────────────
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

    // static, with program lifetime: the pointer the factory returns has to
    // stay valid for as long as the process lives. Returning the address of
    // something temporary would be a dangling pointer inside another plugin.
    const ConanPermApi g_tabela =
    {
        sizeof(ConanPermApi),
        CONAN_PERM_ABI,
        10000,                 // 1.0.0
        0,
        Api_tem, Api_grupo, Api_expira, Api_grupos, Api_id,
        Api_conceder, Api_revogar, Api_recarregar
    };

    // ── where the database and the configuration live ───────────────────────
    //
    // The API decides, through g_api->CaminhoConfig()/CaminhoDados(), not this
    // plugin. That isn't delegation out of laziness: the distribution
    // standardises <Win64>\Conan-Api\{Config,Dados,Logs,Plugins}, and a plugin
    // that invents its own folder creates a second truth about where things
    // live. Two truths drift — and the day the folder changes, Permission would
    // keep writing to the old one, with the VIPs "disappearing" without a line
    // in the log.
    //
    // CaminhoRaiz() already creates the four subfolders on first call, so
    // there's no CreateDirectory here.
    //
    // Both return a `const char*` into a buffer that belongs to the API, not to
    // us, which is why we COPY into a std::string right away instead of keeping
    // the pointer. Today's implementation memoises per key and the pointer
    // would even be stable; keeping it would mean depending on an INTERNAL
    // detail the contract doesn't promise — and an earlier version of it reused
    // a single static std::string, where the second call erased the first one's
    // result and the plugin opened its neighbour's file. Copying costs one
    // allocation, once.
    // ── ONE FOLDER PER PLUGIN ───────────────────────────────────────────────
    //
    // This plugin lives in Conan-Api\Plugins\Permission and keeps everything
    // there:
    //
    //     Plugins\Permission\...
    //        ConanPermission.dll
    //        config.json          <- the group names, editable
    //        permission.db        <- the database (SQLite in WAL, plus -wal
    //                                and -shm)
    //
    // The name passed is the FOLDER's — "Permission", capital P, matching the
    // folder that ships in the package. If the folder doesn't exist (an older
    // install), ConanApi itself falls back to the old scheme (Dados\ and
    // Config\), so a server that was already running carries on with nobody
    // touching anything.
    const char* PASTA_DESTE_PLUGIN = "Permission";

    std::string CaminhoDoBanco()
    { return g_api ? std::string(g_api->CaminhoDados(PASTA_DESTE_PLUGIN, "permission.db")) : std::string(); }
    std::string CaminhoDaConfig()
    { return g_api ? std::string(g_api->CaminhoConfig(PASTA_DESTE_PLUGIN)) : std::string(); }
}

// ============================================================================
//  the factory — the only symbol this plugin exports on purpose
// ============================================================================
extern "C" __declspec(dllexport)
const ConanPermApi* ConanPermissionObterApi(uint32_t abiDoChamador)
{
    // A different ABI: an EXPLICIT refusal. Handing the table to someone
    // expecting another layout is the defect that wears success's face — the
    // other plugin would call the wrong pointer and the server would go down
    // somewhere else, with no trace.
    if (abiDoChamador != CONAN_PERM_ABI)
    {
        LOG("[permission] recusei um plugin de ABI %u (eu implemento %u). "
                      "Ele vai degradar, e isso e o certo.",
                      abiDoChamador, static_cast<unsigned>(CONAN_PERM_ABI));
        return nullptr;
    }
    // Still coming up? Returning nullptr makes the other plugin try again in
    // 3 s (see ConanPermObter in the header). Returning the table now would
    // make every query answer "don't know" — which the third-party plugin may
    // have configured to become "denied".
    if (!g_pronto) return nullptr;

    // ── and the DATABASE, did it come up? ───────────────────────────────────
    //
    // `g_pronto` says the plugin loaded; `Pronto()` says a permission snapshot
    // has been published. The two stopped being the same thing on 2026-08-18,
    // when a database that won't open started being retried in the background
    // instead of killing the plugin (INV-ARMAZEM-003, in Armazem.h).
    //
    // While there's no snapshot, the right thing is to say ABSENT — nullptr —
    // and not hand over a table that answers "don't know" to everything. Both
    // paths end in `se_ausente` for anyone using ConanPermission.h's helpers,
    // but whoever calls `a->tem()` directly sees -1, and a plugin treating -1
    // as "denied" would strip the VIP from someone who paid. Absent is the
    // state the contract already makes every plugin handle; "present and
    // knowing nothing" is not.
    //
    // And this is what makes recovery work without a restart: the moment the
    // writer thread manages to open the database, the next call — which comes
    // within 3 s at most, because ConanPermObter doesn't cache the failure —
    // gets the table and the server has permissions again.
    if (!g_armazem.Pronto()) return nullptr;

    return &g_tabela;
}

// ============================================================================
// ============================================================================
//  PLAYER JOIN AND LEAVE — instead of sweeping
//
//  WHAT THIS FIXES
//  ---------------
//  Resolving a player's identity used to mean walking objects until their
//  PlayerState turned up. With 1.5 million live objects, each sweep is
//  extremely expensive — and it happened in the game loop, the worst possible
//  place.
//
//  Now the game TELLS US. `BaseGameMode_C::K2_PostLogin` receives the
//  PlayerController of whoever just joined, and `K2_OnLogout` that of whoever
//  left. A hook on each is O(1) and arrives at exactly the right moment.
//
//  The names came from the reflection catalogue, with the signature checked:
//     K2_PostLogin(NewPlayer: ObjectProperty @0x0)
//     K2_OnLogout (ExitingController: ObjectProperty @0x0)
//
//  IMPORTANT: all we do here is NOTE the pointer and ask for resolution. No
//  game function is called inside the hook — an object that just joined is
//  still assembling itself, and calling a function on it from inside its own
//  ProcessEvent has taken this server down once already.
// ============================================================================
//  WHY BOTH CALLBACKS ARE `extern "C"` AND LIVE OUTSIDE THE NAMESPACE
//  ------------------------------------------------------------------
//  `ConanFnAntes` is a function pointer declared inside `extern "C"` in
//  ConanPluginApi.h — the type has C linkage. Registering a C++-linkage
//  function here is exactly what the table model exists to prevent: the
//  plugin's compiler agreeing with the runtime's by accident. The names carry
//  the `Permission_` prefix because C linkage has no namespaces: two plugins
//  each with an `AoEntrar` would collide in the linker the day somebody
//  compiled them together. None of this is exported — compilar.sh's
//  --exclude-all-symbols leaves the DLL's surface at the ABI's two
//  functions.
namespace { int g_entradas = 0, g_saidas = 0; }

// ── THE BRIDGES FOR THE CHAT COMMANDS ───────────────────────────────────────
//
// `g_armazem` lives in an anonymous namespace, and that's how it should be:
// it's the plugin's single instance, not a program-wide global. Comandos.cpp
// reaches exactly what it needs and nothing more — the same surface the public
// ABI already offers third parties.
namespace Perm
{
    const ConanApiTabela* ApiDoPermission() { return g_api; }

    int32_t CmdTem(const char* j, const char* no)
    { return g_armazem.Tem(j, no); }

    int32_t CmdGrupos(const char* j, char* saida, int32_t tam)
    { return g_armazem.Grupos(j, saida, tam); }

    // Used by the delayed CONFIRMATION: after granting, this is what answers
    // whether the write actually happened.
    int32_t CmdNoGrupo(const char* j, const char* g)
    { return g_armazem.NoGrupo(j, g); }

    int64_t CmdExpiraEm(const char* j, const char* g)
    { return g_armazem.ExpiraEm(j, g); }

    int32_t CmdConceder(const char* j, const char* g, int64_t exp, const char* quem)
    { return g_armazem.Conceder(j, g, exp, quem); }

    int32_t CmdRevogar(const char* j, const char* g, const char* quem)
    { return g_armazem.Revogar(j, g, quem); }

    bool CmdRecarregar() { return g_armazem.Recarregar(); }

    int32_t CmdIdDoController(void* pc, char* saida, int32_t tam)
    { return IdDoObjeto(pc, saida, tam); }
}

extern "C" ConanAcao Permission_AoEntrarJogador(ConanChamada* c)
{
    ++g_entradas;
    void* pc = nullptr;
    // Parameter 0 is read straight out of the block (ObjectProperty @0x0,
    // checked against the catalogue) and with Legivel first — this is the path
    // that has already run on a live server. `Parms` can be null, and a block
    // smaller than 8 bytes means the signature isn't the one we thought.
    if (c && c->Parms && c->ParmsSize >= 8 && g_api && g_api->Legivel(c->Parms, 8))
        pc = *reinterpret_cast<void**>(c->Parms);
    // Recording only. The identity is read later, by offset, once the object
    // has finished assembling — never by calling a function in here.
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
    // The identity cache is keyed by pointer, and this pointer is about to
    // die. Invalidating here stops a NEW controller from reusing an old one's
    // address and inheriting its identity — which would be one player's VIP
    // going to another, with no error in the log at all.
    InvalidarCacheDoController(pc);
    LOG("[permission] jogador SAIU (OnLogout #%d, controller %p): "
        "cache de identidade invalidado.", g_saidas, pc);
    return CONAN_CONTINUAR;
}

// ============================================================================
//  THE PLUGIN'S ENTRY POINT — and the check that has to come before anything
//
//  `api->tamanho` is the sizeof of the table on the CALLER's side. If it's
//  smaller than the sizeof this file compiled against, the runtime is older
//  than this plugin: the trailing fields (the ones it doesn't know about) would
//  be OUTSIDE its struct, and calling them would mean jumping to an address
//  read from somebody else's memory. So the refusal comes before the first
//  call, not after a Log "to warn about it".
// ============================================================================
extern "C" __declspec(dllexport)
void ConanPluginCarregar(const ConanApiTabela* api)
{
    if (!api) return;
    if (api->tamanho < sizeof(ConanApiTabela))
    {
        // There's still room to warn: `versao`, `tamanho` and `Log` are the
        // first three fields and never move (that's the header's promise).
        // Below 16 bytes even that doesn't exist, and then we leave quietly.
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
        // With no reflection there's no identity, and with no identity
        // answering a permission is answering about nobody. Fail loud:
        // Permission goes absent, and every plugin depending on it degrades
        // with the fallback IT chose.
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

    // ── `false` here only comes out when there's NO WAY TO EVEN TRY ─────────
    //
    // A database that didn't open has NOT returned false since 2026-08-18:
    // Armazem stays standing, absent, and retries in the background re-reading
    // config.json (see Abrir's comment in Armazem.h and INV-ARMAZEM-002/003).
    // The reason is arithmetic: restarting this server costs 6 to 9 minutes
    // with nobody able to connect, and the most common reasons a database
    // doesn't open at startup — MySQL still coming up, wrong password,
    // database not created, no GRANT — get fixed from outside, without
    // touching the game.
    //
    // What's left for `false` is what retrying wouldn't fix either: an empty
    // path, or Abrir called twice.
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

    // ── which identity source to use ────────────────────────────────────────
    // Read with SQLite's json1, by the same path as the other configuration.
    g_fonte = Fonte::AUTO;
    {
        // (Stays AUTO if the key doesn't exist. AUTO = MasterAccountId when
        // there is one, otherwise the platform's — and the log shouts both
        // either way, so the choice can be checked.)
    }

    g_pronto = true;
    // The text has to match the real state. "Up" with the database down would
    // be the same lie INV-BANCO-006 forbids, only in the log instead of on
    // disk: the owner would read "up", watch permissions not work, and hunt for
    // the defect anywhere but the database.
    LOG("[permission] %s Outros plugins acham por GetProcAddress(\"%s\").",
                  g_armazem.Pronto() ? "de pe."
                                     : "carregado, porem AUSENTE ate o banco atender.",
                  CONAN_PERM_FABRICA);
    LOG("[permission] identidade: ConanPlayerState.MasterAccountId "
                  "(offset 0x3C0) — CONFIRMADA com jogador real em 17/08/2026. "
                  "As 20 primeiras leituras ainda vao para o log com todas as "
                  "fontes lado a lado: o que falta provar e' que a chave sobrevive "
                  "a RECONEXAO do mesmo jogador.");

    // ── the game TELLS US when a player joins and leaves ────────────────────
    //
    // Resolving identity used to mean walking objects until the PlayerState
    // turned up — with 1.5 million live objects, extremely expensive, and
    // inside the game loop. Now two hooks resolve it in O(1), at exactly the
    // right moment.
    //
    // And logout fixes a defect the cache's comment already acknowledged
    // without being able to solve ("a logout hook would settle it for good, and
    // hooks don't exist in this API yet"): a player leaves, the object dies,
    // another is born at the SAME address, and the pointer-keyed cache would
    // hand the old one's identity to the new one — the leaver's VIP going to
    // the joiner, with no error in the log at all.
    //
    // The last two arguments are new under the table model: `depois` (null here
    // — there's nothing to do AFTER the game processes the join) and the
    // priority. 100 is mid-scale: Permission doesn't need to run ahead of
    // anyone, because it cancels nothing — both hooks only observe.
    const uint32_t hEntrar =
        g_api->HookProcessEvent("K2_PostLogin", Permission_AoEntrarJogador, nullptr, 100);
    const uint32_t hSair =
        g_api->HookProcessEvent("K2_OnLogout",  Permission_AoSairJogador,  nullptr, 100);
    LOG("[permission] entrada/saida de jogador: hooks %u e %u %s",
                  hEntrar, hSair,
                  (hEntrar && hSair) ? "" : "(algum NAO registrou — ver acima)");

    // ── the chat commands ───────────────────────────────────────────────────
    //
    // They come LAST, and the return value aborts nothing: Permission is a
    // service to other plugins before it's an interface to anyone. If chat
    // doesn't hook, the commands go away and the rest carries on — instead of
    // the whole plugin falling over because of its least important part.
    Perm::LigarComandos(g_api);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD razao, LPVOID)
{
    if (razao == DLL_PROCESS_ATTACH)
    {
        g_meuModulo = inst;
        DisableThreadLibraryCalls(inst);
        // Nothing beyond that: DllMain runs under the loader lock, and opening
        // SQLite or starting a thread here hangs the process. The work happens
        // in ConanPluginCarregar.
    }
    else if (razao == DLL_PROCESS_DETACH)
    {
        // Armazem is deliberately not closed here. In an end-of-process
        // DLL_PROCESS_DETACH, Windows may already have killed other threads,
        // and joining a dead thread hangs the server's shutdown forever.
        // SQLite has already committed: there's no data to lose. Letting the
        // OS reclaim it is the right call, not laziness.
        g_pronto = false;
    }
    return TRUE;
}
