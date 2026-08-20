// ============================================================================
//  ConanPermission.h — the Permission plugin's public interface
//
//  Conan Exiles Enhanced · C++ plugin API
//
//  THIS IS THE ONLY FILE A THIRD-PARTY PLUGIN NEEDS.
//  There is no library to link. No .lib, no .a, no import library. Copy this
//  header, include it, use it. That is not laziness — it is the central design
//  decision, and the three reasons are below.
//
//  A NOTE ON IDENTIFIER NAMES
//  --------------------------
//  The identifiers here are Portuguese (ConanPermTem, id_do_controller,
//  se_ausente). They are part of the published ABI and renaming them would
//  break every plugin already compiled against it. Rough guide: Tem = Has,
//  Grupos = Groups, se_ausente = if_absent, conceder = grant, revogar = revoke.
//
//  ----------------------------------------------------------------------------
//  WHY YOU DO NOT LINK AGAINST PERMISSION
//  ----------------------------------------------------------------------------
//
//  1. A PLUGIN THAT LINKS WILL NOT LOAD WITHOUT ITS TARGET.
//     An import library writes a static dependency into the PE. If the server
//     owner did not install Permission, Windows refuses LoadLibrary on the
//     third-party plugin with "module not found" and the loader records a
//     failure. A VIP plugin has to DEGRADE (answer "I do not know"), not cease
//     to exist. With runtime resolution, Permission's absence is a return
//     value, not a load error.
//
//  2. std::string AND std::vector DO NOT CROSS A DLL BOUNDARY.
//     MSVC and MinGW have incompatible layouts for std::string (different SSO
//     sizes), std::vector and std::shared_ptr; and each allocates on ITS OWN
//     heap. Passing a std::string from the plugin to a Permission built with
//     the other compiler produces no link error: it produces silent memory
//     corruption, or a free() on the wrong heap — which is the WORSE defect,
//     the one that looks like success. It even breaks between two MSVC versions
//     with different _ITERATOR_DEBUG_LEVEL. So: C types only here. const char*
//     goes in, the caller's char* comes out, fixed-width integers for the rest.
//     Whoever allocates, frees.
//
//  3. UPDATING PERMISSION MUST NOT BREAK WHAT IS ALREADY COMPILED.
//     The function table below is a POD struct with `tamanho` and `abi` at the
//     start. A new field goes ONLY AT THE END; nothing is reordered or removed,
//     ever. A plugin compiled today keeps calling the same offsets tomorrow,
//     and a new plugin discovers through `tamanho` whether the older Permission
//     that is installed has the field it wants to use. Size check plus ABI
//     check: both, because either one alone lies.
//
//  ----------------------------------------------------------------------------
//  HOW TO USE IT
//  ----------------------------------------------------------------------------
//
//      #include "Conan/ConanPermission.h"
//
//      // inside your plugin, with the PlayerController's UObject* in hand:
//      char id[CONAN_PERM_MAX_ID];
//      if (ConanPermIdDoController(pc, id, sizeof(id)) > 0)
//      {
//          // 3rd argument: what to answer if Permission is NOT installed.
//          // You choose — and you are required to choose.
//          if (ConanPermTem(id, "myplugin.kit.daily", /*if_absent=*/0) == 1)
//              GiveKit(pc);
//      }
//
//  THREE ANSWERS, NEVER TWO. Every query returns:
//      1  = allowed
//      0  = denied
//     -1  = I DO NOT KNOW (Permission absent, still starting, or empty id)
//
//  A plugin that treats "I do not know" as "denied" on its own is being wrong
//  on purpose; one that treats it as "allowed" opens the server up. That is why
//  the convenience functions require the fallback value explicitly in the
//  argument: there is no silent default.
// ============================================================================
#ifndef CONAN_PERMISSION_H
#define CONAN_PERMISSION_H

#include <stdint.h>
#include <string.h>

#if !defined(_WIN32)
#  error "Permission only exists on Conan's Windows dedicated server (which runs under Wine on Linux)."
#endif
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

// ── ABI version ─────────────────────────────────────────────────────────────
//
// Increments ONLY when an existing field changes meaning — which should never
// happen. Appending a field at the end does NOT bump the ABI: that is what
// `tamanho` is for. Keeping the two separate is what allows adding a function
// without invalidating an already-compiled plugin.
#define CONAN_PERM_ABI 1

// Buffer sizes. Fixed on purpose: a fixed-size buffer owned by the caller is
// the only way to return text across a DLL boundary without agreeing on an
// allocator.
#define CONAN_PERM_MAX_ID     64    // platform id: 17 digits today, ample slack
#define CONAN_PERM_MAX_GRUPO  64    // group name
#define CONAN_PERM_MAX_NO    128    // permission node, e.g. "vip.kit.diario"

// file names, for anyone who wants to look the module up themselves
#define CONAN_PERM_DLL       "ConanPermission.dll"
#define CONAN_PERM_FABRICA   "ConanPermissionObterApi"

// A header the community includes can't spit out warnings. The convenience
// wrappers below are `static` in a header: anyone using two of them and
// ignoring the other eight would get eight -Wunused-function from GCC. The
// attribute silences that without hiding a single real warning.
#if defined(__GNUC__) || defined(__clang__)
#  define CONAN_PERM_TALVEZ __attribute__((unused))
#else
#  define CONAN_PERM_TALVEZ
#endif

// ── the function table ──────────────────────────────────────────────────────
//
// Pure POD: no virtual methods, no inheritance, no C++ types.
// A C++ vtable would also "work", and it's a trap: the order a compiler assigns
// to virtual slots isn't guaranteed by the standard, and MSVC and MinGW diverge
// under multiple inheritance. A function pointer in a struct is defined by the
// platform ABI, not by the compiler.
typedef struct ConanPermApi
{
    // ── header: ALWAYS the first four, ALWAYS in this order ─────────────────
    uint32_t tamanho;        // sizeof(ConanPermApi) as PERMISSION compiled it
    uint32_t abi;            // the CONAN_PERM_ABI it implements
    uint32_t versao;         // 10203 == 1.2.3; for logging and diagnostics only
    uint32_t reservado;      // 0 — keeps the pointers 8-byte aligned

    // ── what EVERY string in this ABI has to satisfy ───────────────────────
    //
    // They're `const char*` with no length parameter. That's a C ABI, and it's
    // the price of not passing std::string across a DLL boundary (see item 2
    // above). So the contract has to be written down:
    //
    //   · `jogador` is '\0'-terminated within CONAN_PERM_MAX_ID bytes;
    //   · `grupo`   is '\0'-terminated within CONAN_PERM_MAX_GRUPO bytes;
    //   · `no`      is '\0'-terminated within CONAN_PERM_MAX_NO bytes;
    //   · `quem`    is '\0'-terminated within 256 bytes.
    //
    // Permission CHECKS this and refuses (-1 / 0) anything that doesn't comply.
    // Until 2026-08-17 it only checked for null and empty, then handed the
    // string to code that walks to the '\0': the hash function, the node
    // comparison, the std::string built for the write queue. A string with no
    // terminator took that loop off the end of the mapping, on the game thread,
    // OUTSIDE the loader's SEH guard: it crashed the whole server, not just the
    // plugin. It didn't take a malicious plugin, just a badly built buffer.
    //
    // WHAT THE CHECK **DOESN'T** COVER, said here rather than hidden:
    //   · a pointer already invalid at its first byte (reading a `const char*`
    //     means dereferencing it — there's no possible defence);
    //   · an unterminated string starting less than N bytes from the end of a
    //     mapped region: the bounded read still crosses the edge.
    // Closing both would take a VirtualQuery per call (2,551 ns measured under
    // Wine, against 145 ns for the entire query — 17x, in the game loop) or
    // length-carrying variants in the ABI. Neither was done, so it's written
    // down instead of promised.

    // ── queries (this is what runs in the game loop) ────────────────────────
    //
    // Answers from an in-memory snapshot built outside the loop. NONE of these
    // functions opens a file, touches SQLite, allocates memory, or takes a lock
    // a writer might be holding. Cost: one hash and, at worst, a few wildcard
    // comparisons.
    //
    // They return 1 allowed · 0 denied · -1 don't know.
    int32_t (*tem)      (const char* jogador, const char* no);
    int32_t (*no_grupo) (const char* jogador, const char* grupo);

    // When the VIP expires, in seconds since 1970 (UTC).
    //   > 0  expires at that time
    //     0  belongs, and never expires
    //    -1  doesn't belong / don't know  (use no_grupo() to tell them apart)
    int64_t (*expira_em)(const char* jogador, const char* grupo);

    // The player's groups, one per line, '\0'-terminated. Returns the size the
    // output WOULD need (possibly larger than `tam`, meaning it truncated), or
    // negative on error. The snprintf convention, which everyone already knows.
    int32_t (*grupos)   (const char* jogador, char* saida, int32_t tam);

    // ── identity ────────────────────────────────────────────────────────────
    //
    // Translates a UObject* of ConanPlayerController (or ConanPlayerState, or
    // ConanCharacter) into the key Permission uses.
    //
    // This is in the ABI ON PURPOSE: no third-party plugin should reimplement
    // identity extraction. The path has traps (a reflection function that
    // returns an FString and leaks the buffer if called in a loop; a field that
    // only exists after login completes) and Permission pays that price once,
    // with a cache. If Funcom changes the field tomorrow, it changes HERE and
    // every plugin stays correct.
    //
    // Returns the id's length (>0), 0 if the object has no identity yet (player
    // still connecting), or negative on error.
    int32_t (*id_do_controller)(void* objetoDoJogo, char* saida, int32_t tam);

    // ── writes (NEVER in the game loop) ─────────────────────────────────────
    //
    // Queues for the writer thread and returns immediately. `expira_em` is in
    // seconds since 1970, or 0 for never. `quem` goes into the audit log: who
    // gave VIP to whom, and when.
    // Returns 1 accepted · 0 refused (group doesn't exist, invalid id) · -1
    // don't know.
    int32_t (*conceder)(const char* jogador, const char* grupo,
                        int64_t expira_em, const char* quem);
    int32_t (*revogar) (const char* jogador, const char* grupo, const char* quem);

    // Re-reads permission.json and the database. Returns 1 if it did, 0 if it
    // failed.
    int32_t (*recarregar)(void);

    // ── ADD NEW FIELDS HERE, AT THE END, AND ONLY HERE ──────────────────────
    // Whoever adds one: bump `versao`, do NOT bump `abi`, and `tamanho` grows
    // on its own. An older plugin never notices; a newer one checks with
    // ConanPermTemCampo() before calling.
} ConanPermApi;

// The factory exported by ConanPermission.dll. It takes the ABI the CALLER
// knows; returns NULL if it can't serve that (a future ABI, or the plugin
// hasn't finished starting). Passing the caller's ABI, rather than only reading
// the callee's, lets Permission refuse explicitly instead of handing over a
// table the other side will misread.
typedef const ConanPermApi* (*ConanPermFabrica)(uint32_t abiDoChamador);

// ── runtime discovery ───────────────────────────────────────────────────────
//
// Static and inline: each plugin gets its own copy, nothing is shared, nothing
// has to be linked.
CONAN_PERM_TALVEZ static const ConanPermApi* ConanPermObter(void)
{
    static const ConanPermApi* g_api  = 0;
    static DWORD               g_prox = 0;   // when to try again

    if (g_api) return g_api;   // success is cached forever

    // ── WHY FAILURE IS **NOT** CACHED FOREVER ───────────────────────────────
    //
    // This is a lesson the API's own initialisation already paid for: it marked
    // "failed" and gave up permanently, and the entire load died in a real test,
    // because the first query happens before the target exists.
    //
    // Here the same defect has a concrete and guaranteed cause: the loader loads
    // DLLs in whatever order FindFirstFile returns, which is NOT specified. A
    // VIP plugin may well run its ConanPluginCarregar() BEFORE
    // ConanPermission.dll exists in the process. If absence were recorded, that
    // plugin would stay blind for the rest of the server's life, and the owner
    // would see "VIP doesn't work" with no error line anywhere.
    //
    // Retrying on every call is not an option either: a query in the game loop
    // would call GetModuleHandle 60 times a second forever. So: it retries at
    // most once every 3 seconds.
    const DWORD agora = GetTickCount();
    if (g_prox && (int32_t)(agora - g_prox) < 0) return 0;
    g_prox = agora + 3000;

    // GetModuleHandle first: if the loader already brought Permission in, that's
    // the one to use. LoadLibrary only as a second attempt, and by an absolute
    // path derived from the executable, never a relative one. The server's CWD
    // is not the binary's folder, and a relative path here would turn into a DLL
    // search-path lookup, which is exactly the vector the loader itself uses to
    // get in. You don't invite that risk indoors.
    HMODULE m = GetModuleHandleA(CONAN_PERM_DLL);
    if (!m)
    {
        // ── the official folder is Conan-Api; older ones stay for compatibility
        //
        // The loader scans `<Win64>\Conan-Api\Plugins\*.dll`, one folder per
        // plugin, so Permission lives in Plugins\Permission\. That's the first
        // attempt and the one that will always apply.
        //
        // The two that follow are installations from earlier phases: a loose DLL
        // in Plugins\, and before that a folder spelled without the hyphen.
        // Trying all three costs two LoadLibrary calls that fail; not trying
        // would cost a server where VIP simply doesn't work, with no error to
        // investigate.
        //
        // These are the ONLY legitimate mentions of the old folders anywhere in
        // plugins/: this is a compatibility search, not an installation
        // instruction. That's why they carry the marker below, which
        // plugins/conferir-caminhos.sh uses so it won't confuse them with the
        // real defect (files telling an owner to INSTALL into the wrong folder,
        // and the plugin never loading, silently).
        static const char* const pastas[] = { "\\Conan-Api\\Plugins\\Permission\\",
                                              "\\Conan-Api\\Plugins\\",          /* PASTA-ANTIGA-DE-PROPOSITO */
                                              "\\ConanApi\\Plugins\\" };         /* PASTA-ANTIGA-DE-PROPOSITO */
        char raiz[MAX_PATH];
        DWORD n = GetModuleFileNameA(0, raiz, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) return 0;
        {
            char* barra = strrchr(raiz, '\\');
            if (!barra) return 0;
            *barra = 0;
        }
        {
            int i;
            for (i = 0; i < 3 && !m; ++i)
            {
                char caminho[MAX_PATH];
                if (strlen(raiz) + strlen(pastas[i]) + sizeof(CONAN_PERM_DLL) > MAX_PATH)
                    continue;
                strcpy(caminho, raiz);
                strcat(caminho, pastas[i]);
                strcat(caminho, CONAN_PERM_DLL);
                m = LoadLibraryA(caminho);
            }
        }
        if (!m) return 0;
    }

    {
        ConanPermFabrica f = (ConanPermFabrica)(void*)
                             GetProcAddress(m, CONAN_PERM_FABRICA);
        const ConanPermApi* a = f ? f(CONAN_PERM_ABI) : 0;

        // Check what came back before trusting it. A Permission with a
        // different ABI, or with a struct smaller than the header, is treated as
        // absent. Better blind and aware than calling a pointer that isn't
        // there. A `tamanho` smaller than the header's four uint32s is junk.
        if (!a || a->abi != CONAN_PERM_ABI || a->tamanho < 16) return 0;
        g_api = a;
    }
    return g_api;
}

// Is Permission installed and up?
CONAN_PERM_TALVEZ static int ConanPermDisponivel(void) { return ConanPermObter() != 0; }

// Version of the installed plugin (10203 == 1.2.3), or 0.
CONAN_PERM_TALVEZ static uint32_t ConanPermVersao(void)
{ const ConanPermApi* a = ConanPermObter(); return a ? a->versao : 0u; }

// Is the installed Permission new enough to have the field at that offset?
// Use with offsetof() + sizeof() of the field you want to call. It exists for
// the day the table grows: a new plugin against an old Permission.
CONAN_PERM_TALVEZ static int ConanPermTemCampo(uint32_t bytesNecessarios)
{ const ConanPermApi* a = ConanPermObter(); return a && a->tamanho >= bytesNecessarios; }

// ── convenience wrappers ────────────────────────────────────────────────────
//
// All of them require an explicit `se_ausente` (if_absent). There's no overload
// without that argument, on purpose: the plugin author HAS to decide what
// happens when Permission isn't installed, and the decision ends up written at
// the call site, visible in code review, instead of hidden in a default.
CONAN_PERM_TALVEZ static int32_t ConanPermTem(const char* jogador, const char* no, int32_t se_ausente)
{
    const ConanPermApi* a = ConanPermObter();
    if (!a || !a->tem || !jogador || !no || !jogador[0]) return se_ausente;
    {
        int32_t r = a->tem(jogador, no);
        return (r == 1 || r == 0) ? r : se_ausente;
    }
}

CONAN_PERM_TALVEZ static int32_t ConanPermNoGrupo(const char* jogador, const char* grupo, int32_t se_ausente)
{
    const ConanPermApi* a = ConanPermObter();
    if (!a || !a->no_grupo || !jogador || !grupo || !jogador[0]) return se_ausente;
    {
        int32_t r = a->no_grupo(jogador, grupo);
        return (r == 1 || r == 0) ? r : se_ausente;
    }
}

CONAN_PERM_TALVEZ static int64_t ConanPermExpiraEm(const char* jogador, const char* grupo)
{
    const ConanPermApi* a = ConanPermObter();
    if (!a || !a->expira_em || !jogador || !grupo) return -1;
    return a->expira_em(jogador, grupo);
}

CONAN_PERM_TALVEZ static int32_t ConanPermGrupos(const char* jogador, char* saida, int32_t tam)
{
    const ConanPermApi* a = ConanPermObter();
    if (saida && tam > 0) saida[0] = 0;
    if (!a || !a->grupos || !jogador) return -1;
    return a->grupos(jogador, saida, tam);
}

// The shortcut most plugins will use: from the game object straight to the id.
CONAN_PERM_TALVEZ static int32_t ConanPermIdDoController(void* objetoDoJogo, char* saida, int32_t tam)
{
    const ConanPermApi* a = ConanPermObter();
    if (saida && tam > 0) saida[0] = 0;
    if (!a || !a->id_do_controller || !objetoDoJogo) return -1;
    return a->id_do_controller(objetoDoJogo, saida, tam);
}

CONAN_PERM_TALVEZ static int32_t ConanPermConceder(const char* jogador, const char* grupo,
                                 int64_t expira_em, const char* quem)
{
    const ConanPermApi* a = ConanPermObter();
    if (!a || !a->conceder || !jogador || !grupo) return -1;
    return a->conceder(jogador, grupo, expira_em, quem ? quem : "");
}

CONAN_PERM_TALVEZ static int32_t ConanPermRevogar(const char* jogador, const char* grupo, const char* quem)
{
    const ConanPermApi* a = ConanPermObter();
    if (!a || !a->revogar || !jogador || !grupo) return -1;
    return a->revogar(jogador, grupo, quem ? quem : "");
}

#ifdef __cplusplus
}   // extern "C"
#endif
#endif  // CONAN_PERMISSION_H
