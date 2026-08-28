// ============================================================================
//  ConanPluginApi.h — THE TABLE. This is what a plugin receives, and all it
//  receives.
//
//  ┌──────────────────────────────────────────────────────────────────────┐
//  │  This file is SELF-CONTAINED. Include nothing else of ours.          │
//  │  There is no library to link and no .cpp of ours to compile.         │
//  └──────────────────────────────────────────────────────────────────────┘
//
//  ────────────────────────────────────────────────────────────────────────────
//  A NOTE ON IDENTIFIER NAMES
//  ────────────────────────────────────────────────────────────────────────────
//  The identifiers in this table are Portuguese (ConanPluginCarregar,
//  LerTextoDoJogo, CONAN_CANCELAR). They are part of the published ABI, and
//  renaming them would break every plugin already compiled against it.
//
//  All documentation is in English, and every field below is commented. A
//  glossary is in Docs/DEVELOPERS.md. Rough guide: "Ler" = read, "Escrever" =
//  write, "Chamar" = call, "Carregar" = load, "Nome" = name, "Tabela" = table,
//  "Caminho" = path, "Jogo" = game, "Tarefa" = task.
//
//  ────────────────────────────────────────────────────────────────────────────
//  HOW IT WORKS
//  ────────────────────────────────────────────────────────────────────────────
//  The loader enters the dedicated server process, brings reflection up, and
//  calls your plugin passing a pointer to a table of functions:
//
//      static const ConanApiTabela* g_api = nullptr;
//
//      extern "C" ConanAcao AoFalar(ConanChamada* c)
//      {
//          return CONAN_CONTINUAR;
//      }
//
//      extern "C" __declspec(dllexport)
//      void ConanPluginCarregar(const ConanApiTabela* api)
//      {
//          if (!api || api->tamanho < sizeof(ConanApiTabela)) return;
//          g_api = api;
//          g_api->Log("my plugin is up");
//          g_api->HookProcessEvent("ServerSendChatMessage", AoFalar, nullptr, 100);
//      }
//
//  This example COMPILES as written — it is extracted from this comment and
//  compiled on every package build. An earlier version called HookProcessEvent
//  with two arguments where it takes four: anyone copying from here, which is
//  the first place a developer looks, hit a compile error on their first try.
//
//  You call everything through `api->`. There is no implementation of ours
//  inside your binary.
//
//  ────────────────────────────────────────────────────────────────────────────
//  WHY THIS WAY, AND NOT BY COMPILING SOURCE ALONGSIDE
//  ────────────────────────────────────────────────────────────────────────────
//  Three reasons, and all three matter to YOU, not just to us:
//
//  1. YOUR COMPILER NO LONGER MATTERS. This is plain C: a `struct` of function
//     pointers with `__cdecl` convention. Any Visual Studio version, MinGW,
//     clang — they all agree on that. Previously an SDK had to ship source,
//     because a C++ library does not cross compilers: the layout of
//     `std::string` and of vtables differs between MSVC and MinGW, and even
//     between MSVC versions. It linked, it ran, and it corrupted memory with no
//     readable error. That problem does not exist here.
//
//  2. UPDATING THE API DOES NOT FORCE YOU TO RECOMPILE. As long as new fields
//     are appended **at the end** of the table and `versao` increments, a plugin
//     compiled today keeps working tomorrow. If the hook engine changes
//     internally, you never find out — and that is how it should be.
//
//  3. THE ENGINE STAYS ON ONE SIDE. The instruction decoder, the hook table,
//     this build's offsets — none of it enters your binary. Less of your code
//     to go wrong, and we can fix an engine defect without asking 40 plugins to
//     be rebuilt.
//
//  ────────────────────────────────────────────────────────────────────────────
//  COMPATIBILITY: ALWAYS CHECK `versao` AND `tamanho`
//  ────────────────────────────────────────────────────────────────────────────
//  A plugin compiled against a LARGER table, running on an older API, would
//  read a pointer past the end of the struct — and call garbage. Hence:
//
//      if (!api || api->tamanho < sizeof(ConanApiTabela)) {
//          // API older than this plugin. Use only what exists, or bail out.
//      }
//
//  We never remove or reorder a field. We only append.
// ============================================================================
#ifndef CONAN_PLUGIN_API_H
#define CONAN_PLUGIN_API_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// The version increments when fields are APPENDED. Never when something moves
// — that does not happen.
#define CONAN_API_VERSAO 8

// ── what a hook receives ────────────────────────────────────────────────────
//
// `Parms` is the intercepted function's parameter block, in the layout
// reflection describes. `Obj` is the object that owns the call.
typedef struct ConanChamada
{
    void*       Obj;         // the UObject that received the call
    void*       Func;        // the UFunction
    void*       Parms;       // parameter block (may be NULL)
    uint32_t    ParmsSize;   // block size, in bytes
    // The function's FName, ALREADY RESOLVED. Comparing two int32s is the O(1)
    // test that lets a wildcard hook separate "new name" from noise without
    // decoding text at 6,300 calls per second. Without it, a discovery plugin
    // has nothing to index on.
    int32_t     NomeIndice;  // FName.ComparisonIndex
    int32_t     NomeNumero;  // FName.Number  (Foo_1 is Number=2)
} ConanChamada;

// What your callback returns.
typedef enum ConanAcao
{
    CONAN_CONTINUAR = 0,   // let the original function run
    CONAN_CANCELAR  = 1    // swallow the call: the game does not execute it
} ConanAcao;

typedef ConanAcao (*ConanFnAntes)(ConanChamada* c);
typedef void      (*ConanFnDepois)(ConanChamada* c);
typedef void      (*ConanFnTarefa)(void* contexto);

// ── why an address hook may be REFUSED ──────────────────────────────────────
//
// A refusal is not a bug on our side: it is the API declining to do something
// that would corrupt the server. The reason is always written to the log.
typedef enum ConanRecusa
{
    CONAN_OK = 0,
    CONAN_RECUSA_ENDERECO_INVALIDO,
    CONAN_RECUSA_BYTES_INESPERADOS,
    CONAN_RECUSA_SEM_JANELA_ATOMICA,
    CONAN_RECUSA_INSTRUCAO_DESCONHECIDA,
    CONAN_RECUSA_SALTO_CAI_NO_PATCH,
    CONAN_RECUSA_FORA_DA_PDATA,
    CONAN_RECUSA_JA_HOOKADA,
    CONAN_RECUSA_SEM_ARENA,
    CONAN_RECUSA_API_NAO_PRONTA
} ConanRecusa;

// ============================================================================
//  THE TABLE
//
//  Order NEVER changes. New fields go at the end, and `versao` increments.
// ============================================================================
typedef enum ConanTipoSaida
{
    CONAN_SAIDA_POD = 0,   // copy the slot's bytes
    CONAN_SAIDA_TEXTO,     // FString: DECODES it. Copying the 16 bytes would
                           // hand the plugin a pointer that ProcessEvent
                           // destroys on return.
    CONAN_SAIDA_TEXTO_RICO,// FText, for the same reason
    CONAN_SAIDA_LISTA      // TArray: copies the ELEMENTS, never the header
} ConanTipoSaida;

typedef struct ConanSaida
{
    int      indice;       // which parameter, in reflection order
    void*    destino;      // where to write
    uint32_t tipo;         // ConanTipoSaida
    uint32_t capacidade;   // POD: bytes · TEXTO: capacity of the char* ·
                           // LISTA: how many elements fit
    uint32_t tamElemento;  // LISTA only
    int*     contagem;     // LISTA only: receives how many fit (may be NULL)
} ConanSaida;

typedef struct ConanApiTabela
{
    // ── header: check this before using the rest ────────────────────────────
    uint32_t versao;    // CONAN_API_VERSAO of whoever filled the table
    uint32_t tamanho;   // sizeof(ConanApiTabela) of whoever filled the table

    // ── diagnostics ─────────────────────────────────────────────────────────
    //
    // Writes to Conan-Api/Logs/ConanApi.log, timestamped and rotated. This is
    // your channel to the SERVER OWNER: what they read when something went
    // wrong, hours later.
    //
    // To talk to a PLAYER, use MensagemParaTodos / MensagemParaJogador /
    // MensagemNaTela further down. Those arrived in v3; before them a plugin
    // really was mute, and this comment used to say so.
    void (*Log)(const char* fmt, ...);

    // Is reflection up? If this returns 0, the game most likely updated and the
    // API refused to work with offsets that do not check out. Do not insist.
    int (*Pronta)(void);

    // ── memory: read this before touching anything ──────────────────────────
    //
    // Says whether [p, p+tam) is MAPPED and readable.
    //
    // ── WHAT IT DOES NOT ANSWER, AND THAT MATTERS A LOT ─────────────────────
    //
    // `Legivel` answers "this memory is mapped", NOT "this object is alive".
    // The difference crashes servers, and it is the easiest trap to fall into
    // in this API:
    //
    //   · you keep a UObject pointer between one hook and the next;
    //   · the game's garbage collector destroys the object and reuses the
    //     address;
    //   · `Legivel` still returns 1, because the page is still mapped;
    //   · you read fields of an object that became something else, and act on
    //     garbage.
    //
    // There is no cheap primitive for "is alive" — Unreal solves that with weak
    // pointers that reflection does not expose. What you can do, and what we
    // recommend:
    //
    //   DO NOT keep game object pointers between calls. Fetch them again inside
    //   each hook (`c->Obj`) or via FindObject/FindObjects at the moment of
    //   use. It is cheaper than it looks and does not have this class of defect.
    //
    // Use `Legivel` for what it is for: refusing an obviously invalid pointer
    // before touching it. It has prevented a real crash here — a read 256 KB
    // past the end of a pool was taking the whole server down.
    int (*Legivel)(const void* p, size_t tam);

    // ── reflection ──────────────────────────────────────────────────────────
    void* (*FindClass)(const char* nome);          // UClass* or NULL

    // MIND THE NAME: FindObject takes a **CLASS** name and returns the first
    // INSTANCE of it. It does NOT find an object by the object's own name.
    //
    //     FindObject("ConanPlayerController")  -> the first controller  [ok]
    //     FindObject("/Game/Items/ItemTable")  -> NULL                  [x]
    //
    // The second is what everyone tries first, and the NULL looks like it says
    // "that object does not exist" when what actually happened was a different
    // question than intended. It happened here on 2026-08-20 and cost a whole
    // round: the plugin logged "table not found" with the table loaded and
    // 9,121 rows inside it.
    //
    // To find an object BY NAME, ask for the instances and pick:
    //
    //     void* found[8192];
    //     int n = api->FindObjects("DataTable", found, 8192, 1);
    //     for (int i = 0; i < n; ++i) {
    //         char name[256];
    //         if (!api->NomeDoObjeto(found[i], name, sizeof(name))) continue;
    //         if (strncmp(name, "Default__", 9) == 0) continue;   // it is the CDO
    //         if (strcmp(name, "ItemTable") == 0) { /* found it */ }
    //     }
    //
    // And when you do not find it, LIST what exists before giving up: "not
    // found" without the list sends the owner looking in the wrong place, and
    // on a modded server the name may be different.
    void* (*FindObject)(const char* nome);         // UObject* or NULL
    void* (*GetDefaultObject)(const char* nomeClasse);  // the CDO, by name
    int   (*DescendeDe)(void* obj, const char* nomeClasse);
    const char* (*NomeDoObjeto)(void* obj, char* saida, int tam);

    // ── members by offset ───────────────────────────────────────────────────
    //
    // The offset comes from the reflection catalogue, not from guessing.
    // Prefer OffsetDoMembro(obj, "FieldName") below: it resolves by name on
    // whichever build is running, and survives a game update.
    int (*LerMembro)(void* obj, uint32_t offset, void* saida, uint32_t tam);
    int (*EscreverMembro)(void* obj, uint32_t offset, const void* valor, uint32_t tam);
    // bitfield: 7 bools fit in one byte, and reading the whole byte gives junk
    int (*LerBit)(void* obj, uint32_t offset, uint8_t mascara);
    int (*EscreverBit)(void* obj, uint32_t offset, uint8_t mascara, int valor);

    // ── calling a game function through reflection ──────────────────────────
    //
    // `args` is an array of pointers to the values, and `tams` their sizes. The
    // API checks each size against the real parameter and REFUSES on mismatch —
    // passing 4 bytes where the game expects 8 writes garbage into the rest, and
    // passing 8 where 4 fit overruns the block. Returns 1 if it executed.
    int (*ChamarFuncao)(void* obj, const char* nomeFuncao,
                        const void** args, const uint32_t* tams, int nargs,
                        void* retorno, uint32_t tamRetorno);

    // ── hooks by name: the normal path ──────────────────────────────────────
    //
    // Filters by FName index before dispatching, so a hook on
    // "ServerSendChatMessage" costs nothing while the game executes anything
    // else. Returns the id (0 = failed; the reason goes to the log).
    uint32_t (*HookProcessEvent)(const char* nomeFuncao,
                                 ConanFnAntes antes, ConanFnDepois depois,
                                 int prioridade);
    // Removes only YOUR hook: the API records which module registered each one.
    int (*RemoverHook)(uint32_t id);

    // ── hooks by address ────────────────────────────────────────────────────
    ConanRecusa (*HookFuncao)(void* endereco, void* nova, void** original);
    ConanRecusa (*HookVirtual)(void* classe, int indice, void* nova, void** original);
    // Every Blueprint execution, including what ProcessEvent does not see.
    // Measured: ~6,300/s on a live server. Your detour MUST call the original.
    ConanRecusa (*HookExecucaoDeBlueprint)(void* nova, void** original);
    const char* (*TextoRecusa)(ConanRecusa r);

    // ── running on the game thread ──────────────────────────────────────────
    //
    // Touching a game object from another thread crashes the server. If your
    // plugin has a thread of its own, schedule through here.
    // The task runs on the GAME THREAD — and that is now guaranteed, not
    // probable.
    //
    // How: the runtime counts ProcessEvent calls per thread and elects the
    // dominant one (measured on this server: 19,981 of 20,000, or 99.9%).
    // Previously the task ran on whichever thread happened through the funnel;
    // it worked almost always, by that same dominance — but "almost always" is
    // not a guarantee, and touching the world from the wrong thread corrupts
    // state slowly and without an error.
    //
    // Until the election completes (seconds), NO task runs, and the log says so
    // once. Delaying the first execution costs seconds; running it on the wrong
    // thread costs somebody else's server.
    uint32_t (*AgendarNaThreadDoJogo)(ConanFnTarefa tarefa, uint32_t segundos,
                                      void* contexto, int repetir);
    int      (*CancelarAgendamento)(uint32_t id);

    // ── paths: everything inside YOUR folder ────────────────────────────────
    //
    // Your plugin lives in Conan-Api/Plugins/<YourFolder>/ and keeps everything
    // there. Pass the name of YOUR FOLDER.
    const char* (*CaminhoConfig)(const char* suaPasta);
    const char* (*CaminhoDados)(const char* suaPasta, const char* arquivo);
    const char* (*CaminhoRaiz)(void);

    // ── read/write parameters inside a hook ─────────────────────────────────
    int (*LerParm)(ConanChamada* c, int indice, void* saida, uint32_t tam);
    int (*EscreverParm)(ConanChamada* c, int indice, const void* valor, uint32_t tam);
    int (*LerRetorno)(ConanChamada* c, void* saida, uint32_t tam);
    int (*DefinirRetorno)(ConanChamada* c, const void* valor, uint32_t tam);

    // Text that ALREADY belongs to the game (an FString in a parameter block)
    // into a char*. This is the way IN.
    //
    // The way OUT is CriarTextoDoJogo / CriarTextoRicoDoJogo (v4, further down):
    // they ask the GAME to allocate. Building the 16 bytes yourself, pointing at
    // your own buffer, still crashes the server — see the note on those two.
    int (*LerTextoDoJogo)(const void* base, uint32_t offset, char* saida, int tam);

    // ── the runtime's own diagnostics ───────────────────────────────────────
    void (*EstatisticaHooks)(uint64_t* total, uint64_t* despachadas);

    // ═══════════════════════════════════════════════════════════════════════
    //  ADDED IN VERSION 2
    //
    //  These came out of migrating our own examples onto the table and finding
    //  it was not enough. Worth recording what their absence cost, because it
    //  is the kind of gap that only shows up when someone tries to use it:
    //
    //    · without NumObjects/GetObjectByIndex, the mapper enumerates NOTHING —
    //      and the entire reflection catalogue comes from it;
    //    · without FindObjects (plural), counting players was impossible:
    //      FindObject returns the FIRST one, so with two players on the server a
    //      plugin would see one. It is not the same question under another name;
    //    · without NomeDeFName, an extractor cannot read FField/FFieldClass/
    //      UEnum, which are not UObjects and keep their name at a different
    //      offset. Using NomeDoObjeto on them would return plausible and WRONG
    //      text;
    //    · without HookProcessEventTudo, an event recorder simply does not
    //      exist: its purpose is to DISCOVER which functions the game calls, so
    //      by definition it has no name to pass to HookProcessEvent.
    // ═══════════════════════════════════════════════════════════════════════

    // ── walking the world ───────────────────────────────────────────────────
    //
    // This is where a discovery plugin starts. `NumObjects` is the total number
    // of entries in the engine's global object array; `GetObjectByIndex`
    // returns the i-th one (may be NULL: there are holes in the array, and that
    // is normal, not an error).
    int   (*NumObjects)(void);
    void* (*GetObjectByIndex)(int indice);

    // All objects of a class. Returns how many fit into `saida`.
    // `incluirFilhas` != 0 also brings subclasses — for PlayerState, that is
    // what makes the actual players show up.
    int (*FindObjects)(const char* nomeClasse, void** saida, int max,
                       int incluirFilhas);

    // ── raw FName ───────────────────────────────────────────────────────────
    //
    // The text of an FName read from memory, by index. Needed for everything
    // that is NOT a UObject: FField, FFieldClass, and UEnum's <FName,int64>
    // pairs. Returns the number of characters written.
    int (*NomeDeFName)(int32_t indice, char* saida, int tam);

    // FULL name (with class and package). `NomeDoObjeto` returns the short one,
    // which does not distinguish two instances of the same class.
    const char* (*NomeCompletoDoObjeto)(void* obj, char* saida, int tam);

    // ── did the call actually EXECUTE? ──────────────────────────────────────
    //
    // `ChamarFuncao` also returns 1 when the game FILTERED the call (Blueprint
    // template, CDO, uninitialised Actor), and then the return value comes from
    // a zeroed block. Without this signal you cannot separate "the function
    // answered false" from "the function did not run" — and both become the
    // same `false` in your plugin.
    int (*UltimaChamadaExecutou)(void);

    // ── the WILDCARD: hook on EVERY function ────────────────────────────────
    //
    // Does not filter by name: fires for everything through the funnel. Its
    // purpose is to DISCOVER what the game calls, when you do not know the name
    // yet.
    //
    // IT IS EXPENSIVE and expires on purpose: `segundos` switches it off by
    // itself. Enabled at server startup, it stopped the world from finishing
    // its load — stalled at 4.35 GB instead of 8.7 GB. Enable it with the server
    // already up, collect what you need, and let it expire.
    uint32_t (*HookProcessEventTudo)(ConanFnAntes antes, uint32_t segundos);

    // ═══════════════════════════════════════════════════════════════════════
    //  ADDED IN VERSION 3 — TALKING TO THE PLAYER
    //
    //  Until here, every plugin was MUTE: it could read chat, cancel a command
    //  and write to the log, but not answer. A shop could not say "you
    //  bought", a teleport could not say "done", a help menu was impossible.
    //
    //  The cause was structural and is measured: building an FString pointing
    //  at YOUR buffer and handing it to the game crashes the server. The game
    //  destroys the parameter block on return and calls ITS allocator on YOUR
    //  memory.
    //
    //  Solved by asking the game itself to allocate, and overwriting the
    //  contents. You do not need to know any of that — pass a `const char*`.
    // ═══════════════════════════════════════════════════════════════════════

    // Announces to ALL connected players. Returns 1 if the call reached the
    // game.
    //
    //     g_api->MensagemParaTodos("Server restarts in 5 minutes.");
    int (*MensagemParaTodos)(const char* texto);

    // Talks to ONE player, by the name they use in game. That name comes, for
    // instance, from the chat's `userName` field (see Docs/EVENTS.md).
    //
    //     g_api->MensagemParaJogador(name, "Kit delivered. Come back in 24h.");
    //
    // Returns 0 if the player is not connected — handle that, because they may
    // have left between the command and the answer.
    int (*MensagemParaJogador)(const char* nomeDoJogador, const char* texto);

    // Writes on the player's SCREEN, not in chat. `playerController` is theirs —
    // you have it in the login hook, or via FindObjects("ConanPlayerController").
    // `segundos` is how long the text stays; 0 uses the default (5 s).
    //
    //     g_api->MensagemNaTela(pc, "Welcome to the server!", 8.0f);
    //
    // This one goes through ClientMessage, which does the job with an FString
    // and is Unreal's classic route for a server to write on screen.
    //
    // Conan's own HUD functions take FText instead. Those are reachable too, and
    // are what Conan Shop uses in production — build the FText with
    // CriarTextoRicoDoJogo (v4) and call the function by name:
    //
    //     unsigned char t[16];
    //     if (api->CriarTextoRicoDoJogo("You have 250 points", t)) {
    //         const void*    a[3] = { t, &sim, &nao };
    //         const uint32_t z[3] = { 16, 1, 1 };
    //         api->ChamarFuncao(pc, "ClientHUDShowNotification", a, z, 3, 0, 0);
    //     }
    int (*MensagemNaTela)(void* playerController, const char* texto, float segundos);

    // ── v4: TEXT BACKED BY GAME MEMORY ──────────────────────────────────────
    //
    // These fill 16 bytes with an FString / FText that the GAME allocated. The
    // plugin passes those 16 bytes as an argument to any function taking an
    // FString or FText, and never needs to know the layout of either.
    //
    // WHY YOU CANNOT BUILD THIS YOURSELF
    // -----------------------------------
    // ProcessEvent destroys the parameter block on return and calls THE GAME'S
    // allocator on whatever pointer is there. If that is your plugin's memory,
    // the server crashes — measured in this project, not theory.
    //
    // They return 1 on success, 0 otherwise. Zero means the following call must
    // NOT be made: passing 16 zeroed bytes as an FString hands the game a null
    // pointer.
    //
    //     unsigned char t[16];
    //     if (api->CriarTextoDoJogo("Welcome!", t))
    //         api->ChamarFuncao(pc, "SomeFunction", ...);
    int (*CriarTextoDoJogo)(const char* texto, void* destino16Bytes);
    int (*CriarTextoRicoDoJogo)(const char* texto, void* destino16Bytes);

    // ── v5: A MEMBER BY NAME, AND WHETHER IT IS REPLICATED ──────────────────
    //
    // OffsetDoMembro returns the offset ON THIS BUILD, resolved through
    // reflection, or -1 if not found. Prefer this to baking the number into your
    // plugin: a hardcoded offset becomes a time bomb the day the game updates,
    // and the symptom is reading garbage while believing you read the game.
    //
    //     const int32_t off = api->OffsetDoMembro(actor, "RelativeLocation");
    //     if (off >= 0) api->LerMembro(actor, off, &pos, sizeof(pos));
    //
    // EhReplicado answers 1 · 0 · -1. The -1 is NOT "safe to write": it is
    // "I do not know".
    //
    // WHY YOU SHOULD ASK BEFORE WRITING
    // ----------------------------------
    // Writing straight into a field the game replicates works on the server and
    // the client never sees it. On a position field it is worse: client
    // prediction corrects it back, and the symptom is rubber-banding that nobody
    // connects to a plugin.
    //
    // When a game function exists that makes the change, call it: it walks the
    // path that already replicates. Writing the field bypasses that path.
    // 1,222 of the 36,210 members in this build are replicated — among them
    // SceneComponent::RelativeLocation.
    int32_t (*OffsetDoMembro)(void* objeto, const char* nome);
    int     (*EhReplicado)(void* objeto, uint32_t offset);
    int     (*NomeDoMembro)(void* objeto, uint32_t offset, char* saida, int tam);

    // ── v6: CALLING WITH OUTPUT PARAMETERS, STILL LINKING NOTHING ───────────
    //
    // WHY THIS EXISTS
    // ---------------
    // `ConanSDK.h` — the game's classes with real signatures — was not shipping
    // in the package while the README advertised it. The cause: it emits
    // `ConanApi::Call<>` and `ConanApi::CallSaida<>`, which called internal
    // helpers that lived in a static library. Using the SDK therefore required
    // linking our library — exactly what the table model exists to avoid, and
    // the opposite of what the front page promises ("your compiler does not
    // matter").
    //
    // `ChamarFuncao` (v1) already covered INPUT and return. What was missing is
    // what the game does in 6,157 of the signatures: writing into an OUTPUT
    // slot. Without this, a third of the SDK could not work through the table.
    //
    // The four output types (ConanSaida.tipo, at the top of this file) are not
    // fussiness — each has exactly one way of being read safely:
    //
    //   CONAN_SAIDA_POD        copy the slot's bytes;
    //   CONAN_SAIDA_TEXTO      an FString: DECODE it. Copying the 16 bytes would
    //                          hand you a pointer ProcessEvent destroys;
    //   CONAN_SAIDA_TEXTO_RICO an FText, for the same reason;
    //   CONAN_SAIDA_LISTA      a TArray: copy the ELEMENTS, never the header —
    //                          the pointer inside it belongs to the game.

    // Like ChamarFuncao, plus the output slots. Returns 1 if it executed.
    // Outputs are copied AFTER the call and BEFORE the game destroys the
    // parameter block — which is the only window in which they are valid.
    int (*ChamarFuncaoEx)(void* obj, const char* nomeFuncao,
                          const void** args, const uint32_t* tams, int nargs,
                          const ConanSaida* saidas, int nsaidas,
                          void* retorno, uint32_t tamRetorno);

    // ── v7: CALLING A NATIVE VIRTUAL BY INDEX, WITH THE CHECKS ──────────────
    //
    // WHY THIS EXISTS. Reflection publishes UFunctions. It does NOT publish
    // native virtuals, and some of what a plugin needs lives only there — the
    // getter that hands back the DataTable row behind an item, the list of
    // reward ids behind a feat. Until now the only way at those was for the
    // plugin to read the vtable and jump on its own, which is exactly how this
    // project killed a live server on 27/08/2026.
    //
    // So the dangerous thing plugins would improvise anyway is done ONCE, here,
    // with every check that makes it defensible:
    //
    //   1. `obj` readable, and not the reserved null page;
    //   2. the vtable pointer read through the guarded path, never raw;
    //   3. the whole span up to the requested slot readable — a vtable shorter
    //      than the index is refused, not walked off the end of;
    //   4. `indice` capped, so a plausible-looking huge index cannot become a
    //      wild read;
    //   5. AND THE SLOT MUST POINT AT EXECUTABLE MEMORY. This is what separates
    //      a function pointer from a number that happens to sit in a mapped
    //      page — the distinction whose absence cost the server.
    //
    // Any failure returns 0 and calls NOTHING.
    //
    // THE SHAPES SUPPORTED, and why only these. Two forms cover the native
    // getters worth reaching, and both are unambiguous in the x64 ABI:
    //
    //     R  f(this)              -> pass saida = NULL
    //     R  f(this, void* out)   -> pass saida = &your_buffer
    //
    // Anything with floats, by-value structs or more parameters is refused on
    // purpose: getting the ABI subtly wrong corrupts the game thread's stack,
    // and a primitive that is right most of the time is worse than one that
    // says no.
    //
    // `retorno` receives RAX. For a getter that answers a pointer that IS the
    // answer; for one that fills `saida`, look there.
    //
    //     void* linha = NULL;
    //     if (api->ChamarVirtual(featItem, 0x500/8, NULL, &linha) && linha)
    //         api->LerMembro(linha, 0xE0, &pacote, 8);
    //
    // WHAT IT STILL CANNOT PROVE, written here rather than implied: that the
    // slot you asked for is the function you think it is. The checks prove it
    // is *a* function, reachable and executable. WHICH one comes from
    // disassembly — and calling the wrong index calls the wrong function with a
    // perfectly straight face.
    int (*ChamarVirtual)(void* obj, uint32_t indice, void* saida, void** retorno);

    // ── v8: READING AN ENGINE SET/MAP THAT REFLECTION DOES NOT PUBLISH ──────
    //
    // v8 AND NOT v7, and the reason is the invariant itself: v7 shipped with
    // `ChamarVirtual` and nothing else. Adding a field while keeping the number
    // would mean two different tables both calling themselves v7 — and a plugin
    // built against the larger one would read a pointer past the end of the
    // struct on a server that has the smaller. The number is the promise.
    //
    // Some of what the game keeps lives in a `TSet`/`TMap` that no UPROPERTY
    // points at — a global lookup table, a field the engine holds natively. The
    // layout is not a secret (it was measured off a live specimen here), but a
    // plugin walking it by hand gets the sparse array, the free list and the
    // hash wrong in ways that read plausible garbage rather than crashing.
    //
    // So the walk happens once, here:
    //
    //     +0x00  Data        pointer to the elements
    //     +0x08  ArrayNum    how many slots exist
    //     +0x34  NumFree     free ones among them
    //     +0x38  Hash inline
    //     +0x40  HashPtr     when non-null, replaces the inline hash
    //     +0x48  HashSize
    //
    // `base` points at those 80 bytes — usually `objeto + offset`, but it may be
    // any readable address, which is what makes a global table reachable.
    // `tamElemento` is 16 for a TSet, 32 for a TMap in this engine; passing the
    // wrong one is the mistake this call cannot catch for you, so it is checked
    // against `ArrayNum` before anything is copied.
    //
    // Copies raw element bytes into `saida`, up to `maxElementos`, and writes
    // how many it got into `*fora`. It DOES NOT decode FString values inside
    // them: those hold pointers into the game's memory, and the caller has to
    // read them inside the window where they are valid — the same rule as
    // CONAN_SAIDA_TEXTO.
    //
    // Returns 1 when it read; 0 when it refused, and it refuses on: unreadable
    // base, `Data` unreadable for the size implied by ArrayNum, ArrayNum
    // negative or absurd, or element size that does not match. A refusal writes
    // 0 to `*fora` — an empty answer and a refused answer must not look alike.
    int (*LerConjunto)(const void* base, uint32_t tamElemento,
                       void* saida, int maxElementos, int* fora);

    // New fields go HERE, at the end, and `versao` increments. Never in the
    // middle.
} ConanApiTabela;

// ============================================================================
//  YOUR PLUGIN'S CONTRACT
//
//  Export this function. The loader looks it up by name; without it, your DLL
//  is loaded and nothing happens.
//
//      extern "C" __declspec(dllexport)
//      void ConanPluginCarregar(const ConanApiTabela* api);
//
//  Optional, called when the server shuts down cleanly:
//
//      extern "C" __declspec(dllexport)
//      void ConanPluginDescarregar(void);
//
//  Do NO work in DllMain: Windows holds a loader lock there, and calling almost
//  anything deadlocks the process. Do everything in Carregar.
// ============================================================================
// OUR side: whoever fills the table. A plugin never calls this — it receives
// the ready pointer in ConanPluginCarregar.
#ifdef __cplusplus
}  // extern "C" — the line below is C++ on purpose; it is internal to us
namespace ConanApi { const ConanApiTabela* TabelaDoPlugin(); }
extern "C" {
#endif

typedef void (*ConanFnCarregar)(const ConanApiTabela* api);
typedef void (*ConanFnDescarregar)(void);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif // CONAN_PLUGIN_API_H
