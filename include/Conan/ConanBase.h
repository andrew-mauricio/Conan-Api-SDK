// ============================================================================
//  ConanBase.h — foundation of the Conan Exiles Enhanced plugin API
//
//  Conan Exiles Enhanced · Unreal Engine 5.6.1 (++exiles+release)
//
//  The only hand-written header. Everything else (ConanSDK.h, thousands of
//  classes) is generated from the server's live reflection data.
//
//  A NOTE ON IDENTIFIER NAMES
//  --------------------------
//  Identifiers in the ABI are Portuguese (ConanPluginCarregar, LerTextoDoJogo,
//  CONAN_CANCELAR). They are part of the published ABI, and renaming them would
//  break every plugin already compiled against it. All documentation is in
//  English; a glossary is in Docs/DEVELOPERS.md.
//
//  THE IDEA
//  --------
//  Funcom publishes neither an SDK nor PDBs for the dedicated server. But
//  Unreal loads its own reflection metadata into memory in order to function,
//  and that metadata is present in any running UE process. Reading it makes it
//  possible to reconstruct the catalogue of classes, members and functions
//  without depending on anyone.
//
//  MEMBER ACCESS
//  -------------
//  A member is not a struct field — it is read at a measured offset, through
//  FieldRef. Reproducing the game's struct field by field looks more elegant,
//  but a single padding mistake misaligns everything below it SILENTLY, and the
//  defect only surfaces as memory corruption at runtime. With FieldRef, a wrong
//  offset gets ONE field wrong — the error stays isolated and visible.
//
//  FUNCTION CALLS
//  --------------
//  By NAME, never by address. An address baked into a plugin is a time bomb: on
//  update day the plugin calls the wrong place and the server dies with no
//  readable error. A name costs one lookup on the first call (then cached) and
//  survives updates.
// ============================================================================
#pragma once

// THE TABLE. Until v2.4.0 this header did not know about it: the runtime spoke
// through internal helpers and the plugin through `api->`, two separate paths.
// v6 joins them, because that is what lets ConanSDK.h (thousands of classes
// with real signatures) work without linking a static library of ours — which
// was the actual reason it never shipped in the package the README advertised.
#include "ConanPluginApi.h"

// <atomic> because of the offset cache in the generated accessors: a
// `static int32_t` read and written by two threads is a formal data race, even
// when both write the same value — and ProcessEvent arrives from 34 threads on
// this build.
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>
#include <utility>

// ── opening a file without a warning on either compiler ─────────────────────
//
// MSVC marks `fopen` as unsafe (C4996) and warns on every include. The easy way
// out would be defining _CRT_SECURE_NO_WARNINGS, but that switches off ALL CRT
// security warnings in the project of whoever compiles a plugin — a decision
// that is not ours to make for them. And a warning that always appears is a
// warning nobody reads: before long the developer ignores the ones that matter
// too.
#include <cstdio>
#if defined(_MSC_VER)
  #define CONAN_FOPEN(fh, caminho, modo)  do { \
        FILE* _f = nullptr; \
        (fh) = (fopen_s(&_f, (caminho), (modo)) == 0) ? _f : nullptr; \
      } while (0)
#else
  #define CONAN_FOPEN(fh, caminho, modo)  do { (fh) = std::fopen((caminho), (modo)); } while (0)
#endif

// alloca lives in <malloc.h> on Windows (both MSVC and MinGW), <alloca.h> elsewhere
#if defined(_WIN32)
  #include <malloc.h>
  #define CONAN_ALLOCA(n) _alloca(n)
#else
  #include <alloca.h>
  #define CONAN_ALLOCA(n) alloca(n)
#endif

// ── calling convention ──────────────────────────────────────────────────────
//
// On Windows x64 there is only ONE native convention. `__fastcall` changes
// nothing there: MinGW accepts it silently, MSVC accepts it but WARNS that it
// ignored it, and in a project the community will compile, a compiler warning
// is noise that makes people stop and wonder whether something is wrong.
// Leaving it empty tells the truth.
#define CONAN_CALL

// Declared BEFORE the namespace: if `class UClass*` only appeared inside
// ConanApi, the compiler would create ConanApi::UClass and the generated headers
// (which use ::UClass) wouldn't see the same type.
class UObject;
class UClass;
class UFunction;

namespace ConanApi
{
    // Declared BEFORE Call(), which uses them. In a template, a name that
    // doesn't depend on a template parameter is resolved at definition time;
    // declaring it later is an error, and the error points at the line of use,
    // not at the cause.
    void Log(const char* fmt, ...);

    // ── did the last call actually execute? ─────────────────────────────────
    //
    // `Call<R>` returns a value even when the function does not run — there is
    // no other way, the return type is R. But returning zero silently would be
    // the very defect the sentinel exists to kill. Whoever needs the distinction
    // asks:
    //
    //     bool v = actor->GetActorEnableCollision();
    //     if (!ConanApi::UltimaChamadaExecutou()) { /* absence, not an answer */ }
    //
    // It answers `false` on ALL paths where the returned value is an absence:
    //   · the object is null;
    //   · the function does not exist on that class (wrong name, or it
    //     disappeared in a game update) — this is the most common case, and it
    //     was the one left out: the `return` from a missing function left
    //     without touching the flag, which starts out `true`, and the plugin
    //     received zero along with confirmation that it was an answer;
    //   · an argument did not fit the parameter (the call was not made);
    //   · a typed return was requested that the function has nowhere to put;
    //   · ProcessEvent filtered the call (the 0xCD sentinel, further down).
    //
    // It is per thread: the server has 34+ threads, and a global here would let
    // one thread read another's result.
    bool UltimaChamadaExecutou();
    void MarcarExecucao(bool ok);

    // ── what reflection needs to know about a function to be able to call it ─
    //
    // Working that out costs walking the class hierarchy and the property chain.
    // Done ONCE per (class, name) and cached: a call in the game loop can't pay
    // for a lookup.
    // How many parameters fit in the record. Whoever fills it derives the
    // ceiling from `sizeof(Offset)/sizeof(Offset[0])`, so this number rules on
    // its own.
    constexpr int MAX_PARMS = 24;

    struct FuncInfo
    {
        void*    Function;          // a UFunction
        uint16_t ParmsSize;         // size of the parameter block
        uint8_t  NumParms;          // how many parameters, return included
        uint8_t  NumEntrada;        // how many are INPUTS (excluding the return)
        uint16_t Offset[MAX_PARMS]; // each parameter's offset inside the block
        uint16_t OffsetRetorno;     // 0xFFFF when the function returns nothing

        // ── SIZE of each parameter (reflection's ElementSize) ───────────────
        //
        // WHY THESE TWO FIELDS HAD TO EXIST
        // ----------------------------------
        // The function record only carried the OFFSET. With that, `Empacotar`
        // wrote sizeof(T) of the plugin's argument at the parameter's offset and
        // stopped there — never asking how many bytes the parameter actually is.
        // The defect that opens is told in full in `EspacoNoBloco()`, just
        // below.
        //
        // 0 = whoever filled this record did NOT measure the size. That is not
        // an error and disables no guard: validation falls back to the limit
        // derived from offsets, which is conservative and never rejects a
        // correct call. With the measured size the same guard becomes exact and
        // also starts catching the inverse case — an argument SMALLER than the
        // slot, which corrupts nothing and delivers a wrong number (a 4-byte
        // `float` in an 8-byte DoubleProperty becomes a denormal: 3.5f read as a
        // double gives 5.336073e-315). There are 1,088 DoubleProperty input
        // parameters on this build, and ExemploOla documents that case because
        // it has been through it.
        //
        // They sit at the END of the struct on purpose: a new field in the
        // middle would shift Offset[] and OffsetRetorno for anything compiled
        // earlier. At the end of the struct, a mismatch moves no existing field.
        uint16_t Tamanho[MAX_PARMS];
        uint16_t TamanhoRetorno;
    };

    void*            ModuleBase();
    UClass*          FindClass(const char* nome);
    const FuncInfo*  ResolveFunction(void* obj, const char* nome);
    void             InvokeRaw(void* obj, void* function, void* parms);

    // ── finding live objects ────────────────────────────────────────────────
    //
    // Without this the headers are just a map: the plugin knows how to read any
    // member of any class and has nowhere to get its first pointer. This is
    // where a plugin starts — grab the GameMode, the list of PlayerControllers,
    // the Actors of a type — and navigates the rest from there.
    //
    // Walking the global object array costs: there are 1.5 million objects on a
    // busy server. Call it at startup or occasionally, never every tick.
    int      NumObjects();
    UObject* GetObjectByIndex(int i);

    // the first object of the class (or of a subclass, with incluirFilhas)
    UObject* FindObject(const char* nomeClasse, bool incluirFilhas = true);

    // all of them; returns how many fit into `saida`
    int      FindObjects(const char* nomeClasse, UObject** saida, int max,
                         bool incluirFilhas = true);

    // the class's default object (CDO); exists even with no instance in the world
    UObject* GetDefaultObject(const char* nomeClasse);

    // ── low-level tools ─────────────────────────────────────────────────────
    // Exposed because a DISCOVERY plugin needs them: decoding an FName the API
    // doesn't model yet, or checking whether a pointer is safe before following
    // it. An ordinary plugin never has to touch these.
    std::string NomeDeFName(int32_t indice);

    // Finds the FName (index + Number) of a function with this name, on any
    // class. Use it to VALIDATE a name before registering a hook: a misspelled
    // name produces a hook that never fires, which is the quietest defect in
    // this territory.
    bool AcharFNameDeFuncao(const char* nome, int32_t* indice, int32_t* numero);

    // ── the C version of Call<>, for the table the plugin receives ──────────
    //
    // Same as Call<>, with arguments in an array instead of a template: the
    // plugin is C and doesn't instantiate templates of ours. It checks each
    // argument's size against the real parameter and REFUSES on mismatch.
    bool ChamarPorTabela(void* obj, const char* nome,
                         const void** args, const uint32_t* tams, int nargs,
                         void* retorno, uint32_t tamRetorno);

    // Same again, with the OUTPUT slots. Declared here and defined on the
    // runtime side. A plugin never calls this directly: it reaches it through
    // the table (ChamarFuncaoEx), which is what lets ConanSDK.h work without
    // linking a library of ours.
    bool ChamarPorTabelaEx(void* obj, const char* nome,
                           const void** args, const uint32_t* tams, int nargs,
                           const ConanSaida* saidas, int nsaidas,
                           void* retorno, uint32_t tamRetorno);
    bool        Legivel(const void* p, size_t n);

    // "Can I WRITE here?" — a different question from Legivel(), and the
    // difference crashes servers: PAGE_READONLY passes Legivel and fails the
    // memcpy.
    bool        Gravavel(void* p, size_t n);

    // Talking to the player. Builds the FString with THE GAME's memory (the game
    // allocates, we overwrite), which is what prevents the crash when
    // ProcessEvent destroys the parameter block.
    bool        MensagemParaTodos(const char* texto);
    bool        MensagemParaJogador(const char* nomeDoJogador, const char* texto);
    // On the player's SCREEN (not in chat). `playerController` is theirs.
    bool        MensagemNaTela(void* playerController, const char* texto, float segundos);

    // ── where a plugin keeps its own things ─────────────────────────────────
    //
    // Every plugin needs somewhere for configuration and data, and "the current
    // directory" won't do: the game process's cwd isn't guaranteed and can
    // change. These paths derive from the EXECUTABLE's location, which is fixed.
    //
    // There's a single tree, next to the executable:
    //
    //     <Win64>/Conan-Api/Plugins/     the DLLs
    //     <Win64>/Conan-Api/Config/      one file per plugin
    //     <Win64>/Conan-Api/Dados/       databases and state
    //     <Win64>/Conan-Api/Logs/        logs
    //
    // The folder is created if missing. A plugin that fails for want of a folder
    // is a plugin that fails on half the installations.
    const char* CaminhoRaiz();                       // <Win64>/Conan-Api

    // ── paths: ONE FOLDER PER PLUGIN ────────────────────────────────────────
    //
    // Each plugin lives in Conan-Api\\Plugins\\<YourPlugin>\\ and keeps
    // everything there:
    //
    //     CaminhoConfig("YourPlugin")               -> .../Plugins/YourPlugin/config.json
    //     CaminhoDados("YourPlugin", "db.db")       -> .../Plugins/YourPlugin/db.db
    //
    // The name you pass is the FOLDER's. If it doesn't exist, it falls back to
    // the old layout (global Config\ and Dados\) instead of returning a path
    // that isn't there, so an older installation keeps working.
    const char* CaminhoConfig(const char* plugin);
    const char* CaminhoDados(const char* plugin, const char* arquivo);
    const char* CaminhoDados(const char* arquivo);   // old layout: .../Dados/<file>

    // ── plugin registration ─────────────────────────────────────────────────
    // The loader calls this; the plugin needn't know how it was loaded.
    // (`Log` is declared at the top of this namespace, before `Call` uses it.)
    bool     Pronta();          // did the anchors check out? only then is it safe to use

    // ── how much may be written starting at an offset in the block ──────────
    //
    // THE DEFECT THIS FIXES
    // ---------------------
    // `Empacotar` used to copy sizeof(T) from the plugin's argument into the
    // parameter's offset without looking at the parameter's size — and the
    // comment right above it PROMISED the opposite ("never corrupt the server's
    // stack"). The buffer has exactly ParmsSize bytes, from an alloca.
    //
    // `ActorComponent::SetComponentTickInterval` has parmssize=4 and a single
    // 4-byte FloatProperty at offset 0. The most natural line in C++ — a literal
    // without the `f` suffix:
    //
    //     comp->Call<void>("SetComponentTickInterval", 0.5);
    //
    // wrote 8 bytes into an alloca of 4. Reproduced before the fix, under
    // AddressSanitizer (native g++, the same header):
    //
    //     ERROR: AddressSanitizer: dynamic-stack-buffer-overflow
    //     WRITE of size 8 ... ConanApi::Empacotar<double> ... ConanBase.h:193
    //
    // There are 1,468 functions on this build with parmssize=4 and a single
    // input parameter — every one of them is that trigger. If the call comes
    // from inside a hook callback, the corrupted stack is the game thread's.
    //
    // THE LIMIT, AND WHY IT DOES NOT WAIT FOR ANYONE TO MEASURE ANYTHING
    // -------------------------------------------------------------------
    // Parameters sit side by side in a single block. The ceiling for one
    // starting at `off` is the next offset ABOVE it — another parameter's, or
    // the return value's — and, failing both, the end of the block. The
    // function record already knows this today.
    //
    // Writing past the END OF THE BLOCK corrupts the caller's stack. Writing
    // past the end of the PARAMETER corrupts the neighbouring parameter: the
    // function runs, with an argument nobody asked for, and the symptom appears
    // far from the cause.
    //
    // CALIBRATION (against the catalogue: the 42,767 input parameters of the
    // 22,913 functions with inputs on this build, comparing the derived limit
    // against the ElementSize reflection reports):
    //     limit == real size ................................. 35,184 (82.27%)
    //     limit  > real size (alignment slack) ................ 7,583 (17.73%)
    //     limit  < real size ....................................... 0
    // The zero on that last line is what decides it: the limit NEVER falls below
    // the real size, so this guard rejects no correct call. In the 17.73% with
    // slack it is conservative — what closes that gap is `Tamanho[]`, once
    // measured. And it does reject the defect: among the 10,877 input parameters
    // whose limit is under 8 bytes, a `double` literal does not get through.
    inline uint32_t EspacoNoBloco(const FuncInfo* fi, uint32_t off)
    {
        uint32_t fim = fi->ParmsSize;
        const int n = (fi->NumEntrada < MAX_PARMS) ? int(fi->NumEntrada) : MAX_PARMS;
        for (int j = 0; j < n; ++j)
        {
            const uint32_t o = fi->Offset[j];
            if (o > off && o < fim) fim = o;
        }
        if (fi->OffsetRetorno != 0xFFFF)
        {
            const uint32_t r = fi->OffsetRetorno;
            if (r > off && r < fim) fim = r;
        }
        return (fim > off) ? (fim - off) : 0u;
    }

    // ── TEXT AS AN ARGUMENT: the game allocates, we fill ────────────────────
    //
    // Builds an FString with the GAME's memory. 16 bytes: {wchar_t* Data; int32
    // Num; int32 Max}. A plugin can never build this on its own — see the long
    // note at the end of this file for what happens when it tries.
    bool CriarTextoDoJogo(const char* texto, void* destino16Bytes);

    // ── TEXT AS AN ARGUMENT ─────────────────────────────────────────────────
    //
    // What the plugin writes:    actor->SetName(ConanApi::Texto("Andrew"));
    // What reaches the game:     an FString the GAME allocated.
    //
    // The lifetime is that of the call expression, which is enough: the game
    // copies or consumes during ProcessEvent, and destroys the block on return.
    // FText: same principle, one step further.
    bool CriarTextoRicoDoJogo(const char* texto, void* destino16Bytes);

    // ── RICH TEXT (FText) AS AN ARGUMENT ────────────────────────────────────
    //
    //     pc->ClientShowMessageBox(ConanApi::TextoRico("Titulo"),
    //                              ConanApi::TextoRico("Mensagem"));
    //
    // FText is what Conan's INTERFACE functions ask for: notification,
    // message box, label. Without this, 838 parameters were left untyped.
    struct TextoRico
    {
        unsigned char bruto[16];
        bool valido;
        explicit TextoRico(const char* s) : bruto{}, valido(false)
        { valido = CriarTextoRicoDoJogo(s, bruto); }
    };

    struct Texto
    {
        unsigned char bruto[16];
        bool valido;
        explicit Texto(const char* s) : bruto{}, valido(false)
        { valido = CriarTextoDoJogo(s, bruto); }
    };

    // ── FName AS AN ARGUMENT ────────────────────────────────────────────────
    //
    //     character->SpawnTemplateItem(10001, ConanApi::Nome("shop"), 10, ...)
    //
    // WHY THIS HAD TO EXIST
    // ---------------------
    // An FName is a reference to an entry in the process's name pool — two
    // int32s, and neither can be invented: they are the index of the text
    // INSIDE that run's pool. Building {0,0} sends "None"; building an arbitrary
    // number sends whatever name sits at that position, which exists, is valid,
    // and is NOT what was asked for. The game accepts it and does something
    // else, without an error.
    //
    // Until 2026-08-20 the SDK had no such bridge, and the consequence was
    // large: every function taking an FName parameter was out of a plugin's
    // reach — including SpawnTemplateItem, which is how you hand an item to a
    // player, and AddItemTemplate, which is how you put an item into an
    // inventory. A shop was impossible to write, and the reason showed up
    // nowhere.
    //
    // WHY THROUGH THE GAME'S FUNCTION, RATHER THAN READING THE POOL
    // -------------------------------------------------------------
    // Scanning the name pool for the text would also yield the index, at the
    // cost of walking hundreds of thousands of entries with arithmetic over the
    // pool's internal layout — a layout Epic has already changed between Unreal
    // versions. Conv_StringToName is the same bridge Blueprint uses, has a
    // public signature, and does what FName genuinely requires: if the name does
    // not exist in the pool yet, it CREATES it. A pool search would answer "not
    // found" for a new name — which is precisely the case of a plugin inventing
    // a context of its own.
    //
    // The constructor is defined further down, once `Call` exists.
    struct Nome
    {
        unsigned char bruto[8];   // sizeof(FName): ComparisonIndex + Number
        bool valido;
        explicit Nome(const char* s);
    };

    // ── TEXT AS OUTPUT: ForaTexto ───────────────────────────────────────────
    //
    // WHY IT'S A SEPARATE TYPE FROM Fora<T>
    // --------------------------------------
    // Fora<T> copies sizeof(T) bytes from the slot into the destination. That's
    // right for int, float, FVector, and WRONG for FString: copying those 16 bytes would
    // hand the plugin a pointer into the GAME's memory, which ProcessEvent
    // destroys on return. The plugin would be left holding a dangling pointer
    // and reading freed memory, which is worse than not having the output.
    //
    // What this type does: it DECODES. It reads the FString from the slot while
    // it is still valid, converts UTF-16 into the plugin's buffer, and nothing
    // belonging to the game crosses the boundary. 238 functions on this build
    // came out without a signature because of this.
    //
    //     char name[128];
    //     obj->GetStringAttribute(..., ConanApi::ParaForaTexto(name, sizeof(name)));
    //
    // The decoder itself is `TextoDeSlot`, declared a few lines below, and
    // implemented on the runtime side where validated reads of game memory live.

    // ── the build check that finishes after the world is up ─────────────────
    //
    // Redoes the part of the build check that was left pending at startup.
    // Called by the loader AFTER the world comes up: at startup the native
    // UFunctions don't exist yet, so that invariant would read "4 of 6" forever.
    bool ReconferirBuild();

    // Arms the hooks that were REQUESTED before reflection existed. Called once,
    // the instant the world comes up. This is what lets a command answer in the
    // first second instead of after the loader activates the plugins.
    void DrenarPendentes();

    // ── A MEMBER BY NAME, RESOLVED AT RUNTIME ───────────────────────────────
    //
    // Returns the member's offset on this build, or -1 if not found. Walks up
    // the hierarchy and caches per (class, name).
    //
    // WHY THIS MATTERS MORE THAN IT LOOKS: with name resolution, the surface a
    // game update breaks stops being 36,210 hardcoded offsets and becomes a
    // handful of anchor pointers. Everything else is derived from those.
    //
    // -1 is NEVER confused with offset 0, which is legitimate.
    int32_t OffsetDoMembro(void* objeto, const char* nome,
                           uint32_t* tamanho = nullptr, uint64_t* flags = nullptr);

    // 1 replicated · 0 not · -1 don't know. All three states matter: -1 is NOT
    // "safe to write".
    int  EhReplicadoPorNome(void* objeto, const char* nome);
    int  EhReplicadoPorOffset(void* objeto, uint32_t offset);
    bool NomeDoMembroNoOffset(void* objeto, uint32_t offset, char* saida, int tam);

    int TextoDeSlot(const void* slot16Bytes, char* saida, int tam);

    // FText from the slot -> char*, going through the game's Conv_TextToString.
    // An FText doesn't hold characters; it holds a counted reference.
    int TextoRicoDeSlot(const void* slot16Bytes, char* saida, int tam);

    // FText output. Kept separate from ForaTexto because decoding goes through
    // one more call into the game, and confusing the two would produce empty
    // text without saying why.
    struct ForaTextoRico
    {
        char* destino;
        int   tam;
        ForaTextoRico(char* d, int t) : destino(d), tam(t) { if (d && t > 0) d[0] = 0; }
    };
    inline ForaTextoRico ParaForaTextoRico(char* d, int t) { return ForaTextoRico(d, t); }

    struct ForaTexto
    {
        char* destino;
        int   tam;
        ForaTexto(char* d, int t) : destino(d), tam(t) { if (d && t > 0) d[0] = 0; }
    };
    inline ForaTexto ParaForaTexto(char* d, int t) { return ForaTexto(d, t); }

    // ── INPUT AND OUTPUT AT ONCE: EntreSai<T> ───────────────────────────────
    //
    // WHY THIS HAD TO EXIST
    // ---------------------
    // 4,777 parameters on this build carry both CPF_OutParm AND
    // CPF_ReferenceParm: Unreal's `UPARAM(ref)`. They arrive by reference
    // ALREADY FILLED IN by the caller, and the function may write back.
    //
    // Until 2026-08-19 they were classified as PURE output, and the consequence
    // was silent: in BoxOverlapActors, the type filter and the list of actors to
    // ignore are two of them. Treated as output, the plugin had no way to pass
    // them, the search ran with no filter at all, and the result looked
    // legitimate. No error, no log.
    //
    // EntreSai<T> writes the plugin's value into the slot BEFORE the call and
    // copies it back AFTER. It's the only way to honour both sides.
    template<typename T>
    struct EntreSai
    {
        T* valor;
        explicit EntreSai(T& v) : valor(&v) {}
    };
    template<typename T> inline EntreSai<T> ParaEntreSai(T& v) { return EntreSai<T>(v); }

    // ── A LIST AS OUTPUT: ForaLista<T> ──────────────────────────────────────
    //
    // WHY THIS IS YET ANOTHER TYPE, AND NOT Fora<T>
    // ----------------------------------------------
    // A TArray in the parameter block is an FScriptArray: {void* Data; int Num;
    // int Max}. Copying those 16 bytes would hand the plugin the GAME's pointer,
    // and ProcessEvent frees that buffer on return. The plugin would be reading
    // freed memory, which is worse than not having the output at all.
    //
    // Here the ELEMENTS are COPIED into the plugin's buffer while the array is
    // still valid. Nothing belonging to the game crosses the boundary, and the
    // lifetime becomes the plugin's.
    //
    // 1,653 output parameters and 541 return values were left without a
    // signature because of this. Functions like BoxOverlapActors, which returns
    // TArray<AActor*> and is exactly what an area or event plugin needs.
    //
    //     AActor* found[32]; int n = 0;
    //     lib->BoxOverlapActors(..., ConanApi::ParaForaLista(found, 32, n));
    template<typename T>
    struct ForaLista
    {
        T*   destino;
        int  capacidade;
        int* quantos;      // receives how many actually fitted
        ForaLista(T* d, int cap, int& n) : destino(d), capacidade(cap), quantos(&n)
        { n = 0; }
    };
    template<typename T>
    inline ForaLista<T> ParaForaLista(T* d, int cap, int& n)
    { return ForaLista<T>(d, cap, n); }

    // ── OUTPUT PARAMETER: Fora<T> ───────────────────────────────────────────
    //
    // WHY THIS EXISTS
    // ---------------
    // 7,985 functions on this build came out of ConanSDK.h as generic templates
    // — no signature, no types, no parameter names — for ONE reason only: they
    // had an OUTPUT parameter. Measured on 2026-08-19, and it was the LARGEST
    // group, bigger than all the type problems combined.
    //
    // "Output" in Unreal is neither a pointer nor a reference in the block: it
    // is an ORDINARY slot of the parameter block that the function WRITES
    // instead of reading. The caller has to read that slot after the function
    // runs. The old Call built the block, called, and discarded the whole block
    // — the output value was written and thrown away.
    //
    // Fora<T> marks "this argument is an output": Empacotar records the
    // destination address and the slot offset, and Call copies it back AFTER
    // the invocation.
    //
    // WHY A TYPE, AND NOT JUST PASSING T&
    // ------------------------------------
    // Because T& is indistinguishable from T at packing time, and guessing
    // would be the worst path: copying back an INPUT parameter would overwrite
    // the plugin's variable with garbage from the block. The type makes the
    // intent explicit, and the generated header emits it only where reflection
    // says CPF_OutParm.
    template<typename T>
    struct Fora
    {
        T* destino;
        explicit Fora(T& d) : destino(&d) {}
    };

    // ── THE RETURN THAT DOESN'T FIT IN A RETURN ─────────────────────────────
    //
    // A function returning FString/FText/TArray can't have that as a C++ return
    // value: they're 16 bytes with the game's pointer inside, which dies when
    // ProcessEvent destroys the block. Returning them would hand back a dangling
    // pointer, and the symptom would surface far from here.
    //
    // That's 1,125 functions on this build (365 FString + 219 FText + 541
    // TArray). All of them came out generic because of it.
    //
    // The way out is the same as for output parameters: decode inside the window
    // where the data is valid. These wrappers mark "this destination is the
    // RETURN", and the runtime translates that to the return offset.
    struct RetornoTexto
    {
        char* destino; int tam;
        RetornoTexto(char* d, int t) : destino(d), tam(t) { if (d && t > 0) d[0] = 0; }
    };
    inline RetornoTexto ParaRetornoTexto(char* d, int t) { return RetornoTexto(d, t); }

    struct RetornoTextoRico
    {
        char* destino; int tam;
        RetornoTextoRico(char* d, int t) : destino(d), tam(t) { if (d && t > 0) d[0] = 0; }
    };
    inline RetornoTextoRico ParaRetornoTextoRico(char* d, int t)
    { return RetornoTextoRico(d, t); }

    template<typename T>
    struct RetornoLista
    {
        T* destino; int capacidade; int* quantos;
        RetornoLista(T* d, int cap, int& n) : destino(d), capacidade(cap), quantos(&n)
        { n = 0; }
    };
    template<typename T>
    inline RetornoLista<T> ParaRetornoLista(T* d, int cap, int& n)
    { return RetornoLista<T>(d, cap, n); }

    // ── UPARAM(ref) OF TEXT: goes in AND comes back in the same buffer ──────
    //
    // 1,399 functions on this build — the largest group of generics once the
    // reasons became precise. They used to hide under the label "has N OUTPUT
    // parameters", which said THAT there was output, not WHY it couldn't be
    // expressed.
    //
    // Both halves already existed separately:
    //   `Texto`     builds an FString with the GAME's memory from a char*
    //   `ForaTexto` decodes an FString slot back into a char*
    //
    // A UPARAM(ref) FString is exactly those two, on the SAME slot: write
    // before, read after. The wrapper below just joins the pair. There's no new
    // mechanism, and therefore no new ownership risk.
    struct EntreSaiTexto
    {
        Texto entrada;      // the game's FString, already built
        char* destino;
        int   tam;
        EntreSaiTexto(char* buf, int t)
            : entrada(buf ? buf : ""), destino(buf), tam(t) {}
    };
    inline EntreSaiTexto ParaEntreSaiTexto(char* buf, int tam)
    { return EntreSaiTexto(buf, tam); }

    // The same pair, for FText: TextoRico builds through the game's
    // Conv_StringToText, and the way back goes through Conv_TextToString. 193
    // functions.
    struct EntreSaiTextoRico
    {
        TextoRico entrada;
        char*     destino;
        int       tam;
        EntreSaiTextoRico(char* buf, int t)
            : entrada(buf ? buf : ""), destino(buf), tam(t) {}
    };
    inline EntreSaiTextoRico ParaEntreSaiTextoRico(char* buf, int tam)
    { return EntreSaiTextoRico(buf, tam); }
    template<typename T> inline Fora<T> ParaFora(T& d) { return Fora<T>(d); }

    // Where to copy back, and how much. Filled by Empacotar, consumed by Call.
    // MAX_PARMS entries are enough: there can't be more outputs than
    // parameters.
    struct SaidasPendentes
    {
        void*    destino[MAX_PARMS];
        uint32_t offset[MAX_PARMS];
        uint32_t tam[MAX_PARMS];
        // tam == TAM_TEXTO marks "decode this slot's FString instead of copying
        // bytes". It shares the same array on purpose: one more list to walk is
        // one more list to forget to walk.
        int      capTexto[MAX_PARMS];
        // For ForaLista: where to write the count, and each element's size.
        int*     contagem[MAX_PARMS];
        uint32_t tamElem[MAX_PARMS];
        int      n;
    };
    static const uint32_t TAM_TEXTO      = 0xFFFFFFFFu;
    static const uint32_t TAM_TEXTO_RICO = 0xFFFFFFFEu;
    static const uint32_t TAM_LISTA      = 0xFFFFFFFDu;

    inline bool Empacotar(const FuncInfo*, const char*, uint8_t*, int) { return true; }

    template<typename T, typename... R>
    inline bool Empacotar(const FuncInfo* fi, const char* nome, uint8_t* buf, int i,
                          T&& v, R&&... resto)
    {
        bool ok = true;
        // An argument beyond what the function accepts is ignored rather than
        // written past the block: a community plugin with the wrong signature
        // has to fail quietly, never corrupt the server's stack.
        if (i < int(fi->NumEntrada) && i < MAX_PARMS)
        {
            using U = typename std::decay<T>::type;
            const uint32_t off    = fi->Offset[i];
            const uint32_t limite = EspacoNoBloco(fi, off);
            const uint32_t medido = fi->Tamanho[i];      // 0 = not measured

            // Two rejections, and neither trusts the other:
            //  · doesn't FIT   — would write past the parameter (or the block);
            //  · doesn't MATCH — fits, but the measured size says it's another
            //                    type.
            const bool cabe = (sizeof(U) <= limite);
            const bool bate = (medido == 0) || (sizeof(U) == medido);
            if (!cabe || !bate)
            {
                Log("Call(\"%s\"): argumento %d tem %u bytes e o parametro %d "
                    "aceita %u (offset %u, bloco de %u; tamanho %s). A chamada "
                    "NAO foi feita. Causa mais comum: literal sem sufixo — 0.5 e' "
                    "double de 8 bytes, 0.5f e' float de 4. Use o tipo da "
                    "reflexao, que e' o que o ConanSDK.h ja usa.",
                    nome ? nome : "(sem nome)", i, unsigned(sizeof(U)), i,
                    unsigned(medido ? medido : limite), unsigned(off),
                    unsigned(fi->ParmsSize),
                    medido ? "medido na reflexao" : "deduzido do offset seguinte");
                ok = false;
            }
            else
            {
                U tmp = v;
                std::memcpy(buf + off, &tmp, sizeof(U));
            }
        }
        // The rest is ALWAYS checked, even after a rejection: whoever got two
        // arguments wrong has to see both in the log, not discover the second
        // after fixing the first.
        const bool restoOk = Empacotar(fi, nome, buf, i + 1, std::forward<R>(resto)...);
        return ok && restoOk;
    }

    // ── PACKING WITH OUTPUTS ────────────────────────────────────────────────
    //
    // Mirrors the Empacotar above, with one difference: when the argument is a
    // Fora<T>, the slot receives NO value (the function is what writes into it)
    // and the plugin's destination is recorded for the copy back.
    //
    // Duplicating the recursion instead of adding a parameter to the original
    // Empacotar is deliberate: that one is the hot path, used by every plugin
    // already compiled, and touching it for the sake of a new feature would
    // trade a gain for a risk in code that already works.
    inline bool EmpacotarS(const FuncInfo*, const char*, uint8_t*, int,
                           SaidasPendentes*) { return true; }

    // FText in the slot: the 16 bytes the GAME allocated go straight in. The
    // size is checked against reflection like any other argument — if the
    // parameter isn't an FText, the call is refused instead of writing 16 bytes
    // into a 4-byte slot.
    template<typename... R>
    inline bool Empacotar(const FuncInfo* fi, const char* nome, uint8_t* buf,
                          int i, const TextoRico& t, R&&... resto)
    {
        bool ok = true;
        if (i < int(fi->NumEntrada) && i < MAX_PARMS)
        {
            const uint32_t off    = fi->Offset[i];
            const uint32_t limite = EspacoNoBloco(fi, off);
            const uint32_t medido = fi->Tamanho[i];
            if (!t.valido)
            {
                Log("Call(\"%s\"): nao consegui pedir o FText ao jogo para o "
                    "argumento %d. A chamada NAO foi feita.",
                    nome ? nome : "(sem nome)", i);
                ok = false;
            }
            else if (sizeof(t.bruto) > limite || (medido != 0 && medido != sizeof(t.bruto)))
            {
                Log("Call(\"%s\"): argumento %d e' texto rico (16 bytes de FText) "
                    "e o parametro aceita %u. A chamada NAO foi feita.",
                    nome ? nome : "(sem nome)", i, unsigned(medido ? medido : limite));
                ok = false;
            }
            else
                std::memcpy(buf + off, t.bruto, sizeof(t.bruto));
        }
        const bool r = Empacotar(fi, nome, buf, i + 1, std::forward<R>(resto)...);
        return ok && r;
    }

    // The same, for FString.
    template<typename... R>
    inline bool Empacotar(const FuncInfo* fi, const char* nome, uint8_t* buf,
                          int i, const Texto& t, R&&... resto)
    {
        bool ok = true;
        if (i < int(fi->NumEntrada) && i < MAX_PARMS)
        {
            const uint32_t off    = fi->Offset[i];
            const uint32_t limite = EspacoNoBloco(fi, off);
            const uint32_t medido = fi->Tamanho[i];
            if (!t.valido)
            {
                Log("Call(\"%s\"): nao consegui pedir a FString ao jogo para o "
                    "argumento %d. A chamada NAO foi feita.",
                    nome ? nome : "(sem nome)", i);
                ok = false;
            }
            else if (sizeof(t.bruto) > limite || (medido != 0 && medido != sizeof(t.bruto)))
            {
                Log("Call(\"%s\"): argumento %d e' texto (16 bytes de FString) e o "
                    "parametro aceita %u. A chamada NAO foi feita.",
                    nome ? nome : "(sem nome)", i,
                    unsigned(medido ? medido : limite));
                ok = false;
            }
            else
                std::memcpy(buf + off, t.bruto, sizeof(t.bruto));
        }
        const bool r = Empacotar(fi, nome, buf, i + 1, std::forward<R>(resto)...);
        return ok && r;
    }

    template<typename T, typename... R>
    inline bool EmpacotarS(const FuncInfo* fi, const char* nome, uint8_t* buf,
                           int i, SaidasPendentes* sp, EntreSai<T> v, R&&... resto)
    {
        // Writes it as INPUT (reusing Empacotar, which already checks size)...
        bool ok = Empacotar(fi, nome, buf, i, *v.valor);
        // ...and records it to be copied back as OUTPUT.
        if (ok && i < int(fi->NumEntrada) && i < MAX_PARMS && sp->n < MAX_PARMS)
        {
            sp->destino[sp->n]  = v.valor;
            sp->offset[sp->n]   = fi->Offset[i];
            sp->tam[sp->n]      = uint32_t(sizeof(T));
            sp->contagem[sp->n] = nullptr;
            ++sp->n;
        }
        const bool r = EmpacotarS(fi, nome, buf, i + 1, sp, std::forward<R>(resto)...);
        return ok && r;
    }

    template<typename T, typename... R>
    inline bool EmpacotarS(const FuncInfo* fi, const char* nome, uint8_t* buf,
                           int i, SaidasPendentes* sp, ForaLista<T> v, R&&... resto)
    {
        bool ok = true;
        if (i < int(fi->NumEntrada) && i < MAX_PARMS)
        {
            const uint32_t off    = fi->Offset[i];
            const uint32_t limite = EspacoNoBloco(fi, off);
            const uint32_t medido = fi->Tamanho[i];
            // An FScriptArray is 16 bytes. If the slot isn't 16, it's not a
            // TArray.
            if (limite < 16 || (medido != 0 && medido != 16))
            {
                Log("Call(\"%s\"): parametro de saida %d nao e' TArray (aceita %u "
                    "bytes; TArray tem 16). A chamada NAO foi feita.",
                    nome ? nome : "(sem nome)", i, unsigned(medido ? medido : limite));
                ok = false;
            }
            else if (!v.destino || v.capacidade <= 0 || !v.quantos)
            {
                Log("Call(\"%s\"): saida de lista %d sem buffer de destino.",
                    nome ? nome : "(sem nome)", i);
                ok = false;
            }
            else if (sp->n < MAX_PARMS)
            {
                sp->destino[sp->n]  = v.destino;
                sp->offset[sp->n]   = off;
                sp->tam[sp->n]      = TAM_LISTA;
                sp->capTexto[sp->n] = v.capacidade;
                sp->contagem[sp->n] = v.quantos;
                sp->tamElem[sp->n]  = uint32_t(sizeof(T));
                ++sp->n;
            }
        }
        const bool r = EmpacotarS(fi, nome, buf, i + 1, sp, std::forward<R>(resto)...);
        return ok && r;
    }

    template<typename... R>
    inline bool EmpacotarS(const FuncInfo* fi, const char* nome, uint8_t* buf,
                           int i, SaidasPendentes* sp, ForaTextoRico v, R&&... resto)
    {
        bool ok = true;
        if (i < int(fi->NumEntrada) && i < MAX_PARMS)
        {
            const uint32_t off    = fi->Offset[i];
            const uint32_t limite = EspacoNoBloco(fi, off);
            const uint32_t medido = fi->Tamanho[i];
            if (limite < 16 || (medido != 0 && medido != 16))
            {
                Log("Call(\"%s\"): parametro de saida %d nao e' FText (aceita %u "
                    "bytes; FText tem 16). A chamada NAO foi feita.",
                    nome ? nome : "(sem nome)", i, unsigned(medido ? medido : limite));
                ok = false;
            }
            else if (!v.destino || v.tam <= 0)
            {
                Log("Call(\"%s\"): saida de texto rico %d sem buffer.",
                    nome ? nome : "(sem nome)", i);
                ok = false;
            }
            else if (sp->n < MAX_PARMS)
            {
                sp->destino[sp->n]  = v.destino;
                sp->offset[sp->n]   = off;
                sp->tam[sp->n]      = TAM_TEXTO_RICO;
                sp->capTexto[sp->n] = v.tam;
                ++sp->n;
            }
        }
        const bool r = EmpacotarS(fi, nome, buf, i + 1, sp, std::forward<R>(resto)...);
        return ok && r;
    }

    template<typename... R>
    inline bool EmpacotarS(const FuncInfo* fi, const char* nome, uint8_t* buf,
                           int i, SaidasPendentes* sp, ForaTexto v, R&&... resto)
    {
        bool ok = true;
        if (i < int(fi->NumEntrada) && i < MAX_PARMS)
        {
            const uint32_t off    = fi->Offset[i];
            const uint32_t limite = EspacoNoBloco(fi, off);
            const uint32_t medido = fi->Tamanho[i];
            // An FString is 16 bytes. If the parameter isn't 16, it's not an
            // FString, and decoding from there would read a pointer that isn't
            // there.
            if (limite < 16 || (medido != 0 && medido != 16))
            {
                Log("Call(\"%s\"): parametro de saida %d nao e' FString (aceita "
                    "%u bytes; FString tem 16). A chamada NAO foi feita.",
                    nome ? nome : "(sem nome)", i,
                    unsigned(medido ? medido : limite));
                ok = false;
            }
            else if (!v.destino || v.tam <= 0)
            {
                Log("Call(\"%s\"): saida de texto %d sem buffer de destino.",
                    nome ? nome : "(sem nome)", i);
                ok = false;
            }
            else if (sp->n < MAX_PARMS)
            {
                sp->destino[sp->n]  = v.destino;
                sp->offset[sp->n]   = off;
                sp->tam[sp->n]      = TAM_TEXTO;
                sp->capTexto[sp->n] = v.tam;
                ++sp->n;
            }
        }
        const bool r = EmpacotarS(fi, nome, buf, i + 1, sp, std::forward<R>(resto)...);
        return ok && r;
    }

    template<typename T, typename... R>
    inline bool EmpacotarS(const FuncInfo* fi, const char* nome, uint8_t* buf,
                           int i, SaidasPendentes* sp, Fora<T> v, R&&... resto)
    {
        bool ok = true;
        if (i < int(fi->NumEntrada) && i < MAX_PARMS)
        {
            const uint32_t off    = fi->Offset[i];
            const uint32_t limite = EspacoNoBloco(fi, off);
            const uint32_t medido = fi->Tamanho[i];

            // The SAME double check as the input, for the same reason: if the
            // size doesn't match, copying back would write over the plugin's
            // stack. A wrong output corrupts THE CALLER's variable, and the
            // symptom appears far from here.
            if (sizeof(T) > limite || (medido != 0 && sizeof(T) != medido))
            {
                Log("Call(\"%s\"): parametro de SAIDA %d espera %u bytes e o "
                    "destino tem %u. A chamada NAO foi feita — copiar de volta "
                    "com tamanho errado corromperia a memoria do plugin.",
                    nome ? nome : "(sem nome)", i,
                    unsigned(medido ? medido : limite), unsigned(sizeof(T)));
                ok = false;
            }
            else if (sp->n < MAX_PARMS)
            {
                sp->destino[sp->n] = v.destino;
                sp->offset[sp->n]  = off;
                sp->tam[sp->n]     = uint32_t(sizeof(T));
                ++sp->n;
            }
        }
        const bool r = EmpacotarS(fi, nome, buf, i + 1, sp, std::forward<R>(resto)...);
        return ok && r;
    }

    template<typename T, typename... R>
    inline bool EmpacotarS(const FuncInfo* fi, const char* nome, uint8_t* buf,
                           int i, SaidasPendentes* sp, T&& v, R&&... resto)
    {
        const bool ok = Empacotar(fi, nome, buf, i, std::forward<T>(v));
        const bool r  = EmpacotarS(fi, nome, buf, i + 1, sp, std::forward<R>(resto)...);
        return ok && r;
    }

    // ── THE CALL WITH OUTPUTS ───────────────────────────────────────────────
    //
    // Identical to Call, plus the step that was missing: after invoking, the
    // slots marked as Fora<T> are copied into the plugin's destination.
    //
    // The copy happens ONLY if the function actually executed. If ProcessEvent
    // filtered the call (CDO, Blueprint template, uninitialised Actor), the
    // block still holds whatever was there, and copying from it would hand back
    // garbage wearing the face of an answer, which is the defect the sentinel
    // exists to prevent.

// ═══════════════════════════════════════════════════════════════════════════
//  TWO PATHS FOR THE SAME CALL, AND WHY THERE HAVE TO BE TWO
//
//  RUNTIME (CONAN_MOTOR defined): invokes directly, with the function record in
//  hand. It holds the map of the function (each slot's offset, the measured
//  sizes, the return offset) and that is why it is the side that validates.
//
//  PLUGIN (the default): has neither, and CANNOT have them. If it did, it would
//  need to link our static library, and the table model dies along with the
//  promise that your compiler doesn't matter. Here the calls become the table's
//  ChamarFuncao / ChamarFuncaoEx: the plugin hands over pointers and sizes, the
//  runtime checks them against the real function record and refuses what
//  doesn't match.
//
//  THIS IS WHAT UNLOCKS ConanSDK.h. Until v2.4.0 the generated header emitted
//  `ConanApi::Call<>`, which only existed on the runtime side, so the SDK the
//  README advertised couldn't ship in the package. With the plugin path, it
//  ships.
// ═══════════════════════════════════════════════════════════════════════════
#ifdef CONAN_MOTOR

    template<typename R = void, typename... A>
    inline R CallSaida(void* obj, const char* nome, A&&... args)
    {
        const FuncInfo* fi = obj ? ResolveFunction(obj, nome) : nullptr;
        if (!fi)
        {
            MarcarExecucao(false);
            Log("CallSaida(\"%s\"): %s. Nada foi chamado.",
                nome ? nome : "(sem nome)",
                obj ? "funcao inexistente nesta classe nem nas maes"
                    : "o objeto e nulo");
            return R();
        }
        const uint32_t n = fi->ParmsSize ? fi->ParmsSize : 1;
        uint8_t* buf = static_cast<uint8_t*>(CONAN_ALLOCA(n));
        std::memset(buf, 0, n);

        SaidasPendentes sp{}; sp.n = 0;
        if (!EmpacotarS(fi, nome, buf, 0, &sp, std::forward<A>(args)...))
        {
            MarcarExecucao(false);
            return R();
        }

        MarcarExecucao(false);
        InvokeRaw(obj, fi->Function, buf);
        MarcarExecucao(true);

        for (int k = 0; k < sp.n; ++k)
        {
            if (!sp.destino[k]) continue;
            if (sp.tam[k] == TAM_LISTA)
            {
                // COPIES THE ELEMENTS, never the FScriptArray's 16 bytes: the
                // pointer inside it belongs to the game and dies on return.
                if (sp.offset[k] + 16 <= n)
                {
                    const uint8_t* a = buf + sp.offset[k];
                    void* dados; int num;
                    std::memcpy(&dados, a, sizeof(dados));
                    std::memcpy(&num, a + 8, sizeof(num));
                    int cabe = (num < sp.capTexto[k]) ? num : sp.capTexto[k];
                    if (cabe < 0) cabe = 0;
                    if (dados && cabe > 0 &&
                        Legivel(dados, size_t(cabe) * sp.tamElem[k]))
                        std::memcpy(sp.destino[k], dados, size_t(cabe) * sp.tamElem[k]);
                    else cabe = 0;
                    if (sp.contagem[k]) *sp.contagem[k] = cabe;
                }
                continue;
            }
            if (sp.tam[k] == TAM_TEXTO_RICO)
            {
                if (sp.offset[k] + 16 <= n)
                    TextoRicoDeSlot(buf + sp.offset[k],
                                    static_cast<char*>(sp.destino[k]), sp.capTexto[k]);
                continue;
            }
            if (sp.tam[k] == TAM_TEXTO)
            {
                // FString: DECODE it while it's still valid. Copying the 16
                // bytes would hand the plugin a pointer to memory that
                // ProcessEvent destroys on return.
                if (sp.offset[k] + 16 <= n)
                    TextoDeSlot(buf + sp.offset[k],
                                static_cast<char*>(sp.destino[k]), sp.capTexto[k]);
                continue;
            }
            if (sp.offset[k] + sp.tam[k] <= n)
                std::memcpy(sp.destino[k], buf + sp.offset[k], sp.tam[k]);
        }

        if (fi->OffsetRetorno != 0xFFFF)
        {
            using Rt = typename std::conditional<std::is_void<R>::value, char, R>::type;
            if (fi->OffsetRetorno + sizeof(Rt) <= n)
            {
                Rt r{};
                std::memcpy(&r, buf + fi->OffsetRetorno, sizeof(Rt));
                return static_cast<R>(r);
            }
        }
        return R();
    }

    template<typename R = void, typename... A>
    inline R Call(void* obj, const char* nome, A&&... args)
    {
        const FuncInfo* fi = obj ? ResolveFunction(obj, nome) : nullptr;
        if (!fi)
        {
            // A missing function returns a zeroed value rather than stack
            // garbage. Silence is bad, but typed garbage is worse: it becomes a
            // bug somewhere else.
            //
            // AND THE SENTINEL HAS TO BE SET HERE
            // ------------------------------------
            // This `return` used to leave without touching the sentinel, and the
            // flag is thread_local initialised to `true`. Result:
            // `UltimaChamadaExecutou()` answered TRUE after a call that never
            // existed — and the hole sat in the most common non-execution case
            // (a misspelled name, a function that vanished in a game update, a
            // null object), not in the rare case of ProcessEvent filtering.
            //
            // It's the same scenario described elsewhere in this project: a
            // permission plugin asking `IsAdmin` by a name that changed treats
            // every administrator as an ordinary player — and the sentinel,
            // which exists precisely to contradict that, was confirming the
            // `false` as a legitimate answer.
            MarcarExecucao(false);
            Log("Call(\"%s\"): %s. Nada foi chamado — o valor devolvido e zero por "
                "ausencia, nao por resultado, e UltimaChamadaExecutou() responde "
                "false.",
                nome ? nome : "(sem nome)",
                obj ? "esta funcao nao existe nesta classe nem nas maes dela"
                    : "o objeto e nulo");
            return R();
        }
        const uint32_t n = fi->ParmsSize ? fi->ParmsSize : 1;
        uint8_t* buf = static_cast<uint8_t*>(CONAN_ALLOCA(n));
        std::memset(buf, 0, n);
        if (!Empacotar(fi, nome, buf, 0, std::forward<A>(args)...))
        {
            // An argument didn't fit its parameter. `Empacotar` already logged
            // which one and why; here the call simply doesn't happen.
            MarcarExecucao(false);
            return R();
        }

        // ── SENTINEL IN THE RETURN SLOT — and the defect it reveals ──────────
        //
        // Not every call executes. `AActor::ProcessEvent` FILTERS: an object
        // that is a Blueprint template, a CDO, or an Actor not yet initialised
        // dispatches no function at all. It simply does not run.
        //
        // Without a sentinel, the parameter block stays zeroed and the return
        // comes out `false` / `0` / null pointer — indistinguishable from a
        // legitimate result. That is how this surfaced: in a test over 400
        // Actors, 399 agreed with a direct bitfield read and ONE disagreed —
        // `FS_AnchorField_GenericEx_C AnchorField_GEN_VARIABLE_...`, a template
        // object. The bit said `true`, the function said `false`, and the
        // function had never run.
        //
        // 0xCD is the historical pattern for "uninitialised memory". If it
        // survives the call, nobody wrote there — the function did not execute,
        // and that goes to the log instead of becoming a plausible value.
        // `sizeof(R)` does not exist for R = void, so the size comes from an
        // alias that swaps void for char. Without it the compiler warns on EVERY
        // call with no return — and a warning that always appears is a warning
        // nobody reads.
        //
        // THE RETURN SLOT'S SPACE COMES FROM THE SAME CALCULATION AS ARGUMENTS
        // ---------------------------------------------------------------------
        // The only ceiling used to be the end of the block (`OffsetRetorno +
        // sizeof(R) <= ParmsSize`). But the return value is not always the last
        // parameter: on this build there are 1,572 of the 17,436 functions with
        // a return where an input parameter sits at a HIGHER offset than the
        // return. In those, an oversized R made the `memset(0xCD)` run over an
        // argument that was ALREADY packed — the function received deliberate
        // garbage, which is exactly what the comment below says must not happen.
        // `EspacoNoBloco` stops at the neighbour, not at the end of the block.
        using RSeguro = typename std::conditional<std::is_void<R>::value, char, R>::type;
        bool temRetorno = false;
        if (fi->OffsetRetorno != 0xFFFF)
        {
            const uint32_t limiteRet = EspacoNoBloco(fi, fi->OffsetRetorno);
            const uint32_t medidoRet = fi->TamanhoRetorno;   // 0 = not measured
            temRetorno = (sizeof(RSeguro) <= limiteRet) &&
                         (medidoRet == 0 || sizeof(RSeguro) == medidoRet);
        }
        if constexpr (!std::is_void<R>::value)
        {
            // Only the RETURN region gets the sentinel. Marking the input
            // parameters would make the game's function read garbage on
            // purpose.
            if (temRetorno) std::memset(buf + fi->OffsetRetorno, 0xCD, sizeof(R));
        }
        MarcarExecucao(true);

        InvokeRaw(obj, fi->Function, buf);

        if constexpr (!std::is_void<R>::value)
        {
            R r{};
            if (!temRetorno)
            {
                // A typed R was asked of a function with nowhere to put it:
                // either it returns nothing, or it returns a different size. The
                // zero leaving here is absence, not an answer, and the sentinel
                // has to say so. This was one more path returning `false`/`0`
                // with `UltimaChamadaExecutou()` answering true.
                MarcarExecucao(false);
                Log("Call(\"%s\"): pediram retorno de %u bytes e a funcao nao tem "
                    "onde devolver isso (offset do retorno %d, bloco de %u). O "
                    "valor devolvido e zero por ausencia, nao por resultado. "
                    "Confira o tipo do retorno na reflexao — o ConanSDK.h ja traz "
                    "a assinatura certa.",
                    nome ? nome : "(sem nome)", unsigned(sizeof(R)),
                    (fi->OffsetRetorno == 0xFFFF) ? -1 : int(fi->OffsetRetorno),
                    unsigned(fi->ParmsSize));
                return r;
            }

            uint8_t sentinela[sizeof(R)];
            std::memset(sentinela, 0xCD, sizeof(R));
            if (std::memcmp(buf + fi->OffsetRetorno, sentinela, sizeof(R)) == 0)
            {
                MarcarExecucao(false);
                // Nobody wrote into the return slot. Fail LOUDLY: the caller
                // finds out, instead of receiving zero and taking it for an
                // answer.
                Log("Call(\"%s\"): a funcao NAO executou (objeto template/CDO ou "
                    "Actor nao inicializado — ProcessEvent filtrou). "
                    "O valor devolvido e zero por falta de resposta, nao por "
                    "resultado.", nome);
                return r;
            }
            std::memcpy(&r, buf + fi->OffsetRetorno, sizeof(R));
            return r;
        }
    }

#else   // ── PLUGIN: everything through the table, linking nothing ─────────

    // ── WHERE THE TABLE COMES FROM, ON THE PLUGIN SIDE ─────────────────────
    //
    // On the runtime side, `TabelaDoPlugin()` lives in its own translation unit.
    // A plugin doesn't have that .cpp, and shouldn't: it's precisely what it
    // does NOT link. What it has is the pointer the loader hands to
    // ConanPluginCarregar.
    //
    // So here the function becomes inline over a variable the plugin itself
    // fills, with ONE line:
    //
    //     void ConanPluginCarregar(const ConanApiTabela* api) {
    //         ConanApi::UsarTabela(api);      <- this one
    //         ...
    //     }
    //
    // Without it, every `Call` from ConanSDK.h becomes a silent no-op. That's
    // why `Call` warns in the log the first time it's called with no table: the
    // worst possible outcome here is "nothing happened and nobody said so".
    inline const ConanApiTabela*& TabelaMutavel()
    {
        static const ConanApiTabela* t = nullptr;
        return t;
    }
    inline const ConanApiTabela* TabelaDoPlugin() { return TabelaMutavel(); }
    inline void UsarTabela(const ConanApiTabela* t) { TabelaMutavel() = t; }

    // ── THE RUNTIME FUNCTIONS THE HEADER USES, ROUTED THROUGH THE TABLE ────
    //
    // `Texto`, `TextoRico` and the slot readers are DECLARED further up and used
    // by structs defined before this point. On the runtime side they come from
    // the static library. Inside a plugin there is no library, and the
    // definition has to be this: a detour through the table.
    //
    // Without it, including ConanSDK.h in a plugin links up to the first `Texto`
    // and dies with "undefined reference to ConanApi::CriarTextoDoJogo", which
    // is exactly what happened while writing the proof.
    inline bool CriarTextoDoJogo(const char* texto, void* destino16Bytes)
    {
        const ConanApiTabela* t = TabelaDoPlugin();
        return t && t->CriarTextoDoJogo && t->CriarTextoDoJogo(texto, destino16Bytes) != 0;
    }
    inline bool CriarTextoRicoDoJogo(const char* texto, void* destino16Bytes)
    {
        const ConanApiTabela* t = TabelaDoPlugin();
        return t && t->CriarTextoRicoDoJogo &&
               t->CriarTextoRicoDoJogo(texto, destino16Bytes) != 0;
    }
    // These two are only called on the runtime path. In a plugin, the slot is
    // decoded by the runtime, on the far side of the table — but the definitions
    // have to exist for the header to compile as a whole.
    inline int TextoDeSlot(const void*, char* saida, int tam)
    { if (saida && tam > 0) saida[0] = 0; return 0; }
    inline int TextoRicoDeSlot(const void*, char* saida, int tam)
    { if (saida && tam > 0) saida[0] = 0; return 0; }

    // A plugin's `Log` goes to the same file, through the table.
    inline void Log(const char* fmt, ...)
    {
        const ConanApiTabela* t = TabelaDoPlugin();
        if (!t || !t->Log) return;
        // No vsnprintf here: the table takes the format, and pre-expanded
        // arguments would be a different ABI. An unformatted line is enough for
        // the handful of warnings this header emits.
        t->Log("%s", fmt);
    }

    // Called when the plugin forgot UsarTabela(). There's nowhere to write (Log
    // also comes from the table), so what's left is the process's standard
    // error, which on the server lands in the game's log. Better that than the
    // whole plugin doing nothing, silently.
    inline void AvisarSemTabela(const char* nome)
    {
        static bool avisou = false;
        if (avisou) return;
        avisou = true;
        std::fprintf(stderr,
            "[Conan] Call(\"%s\") sem tabela: o plugin nao chamou "
            "ConanApi::UsarTabela(api) no ConanPluginCarregar. "
            "Nenhuma chamada do ConanSDK.h vai funcionar.\n",
            nome ? nome : "(sem nome)");
    }

    // Each argument is classified at compile time: ordinary input, text (which
    // already arrives as 16 bytes from the game), or one of the output
    // descriptors. The ORDER is reflection's: the argument's index is the
    // parameter's index, and reordering here would write each value into its
    // neighbour's slot.
    //
    // THE POSITIONS ARE REFLECTION'S, AND THAT ISN'T A DETAIL
    // --------------------------------------------------------
    // The runtime's function record indexes ALL parameters, inputs and outputs
    // in the same numbering. The first version of this numbered only the inputs,
    // and in a `Split(in, in, OUT, OUT, in, in)` the fifth argument (1 byte) was
    // checked against the third one's slot (16 bytes). The runtime refused —
    // correctly, and that refusal is the only reason the defect surfaced instead
    // of writing 1 byte into the middle of an FString.
    //
    // So `ent[i]` is parameter i, and an output slot goes in as `nullptr`: an
    // explicit hole, which the runtime skips.
    struct ColetaArgs
    {
        const void*  ent[MAX_PARMS];
        uint32_t     tam[MAX_PARMS];
        int          nent = 0;        // = how many parameters we have seen
        ConanSaida   sai[MAX_PARMS];
        int          nsai = 0;
        int          indice = 0;      // position in the parameter list

        ColetaArgs()
        {
            for (int i = 0; i < int(MAX_PARMS); ++i) { ent[i] = nullptr; tam[i] = 0; }
        }
        void Entrada(const void* p, uint32_t t)
        {
            if (indice < int(MAX_PARMS)) { ent[indice] = p; tam[indice] = t; }
            if (indice + 1 > nent) nent = indice + 1;
        }
        void Buraco()   // the slot exists, but the game is what writes into it
        {
            if (indice + 1 > nent) nent = indice + 1;
        }
    };

    // ── the ordinary input ─────────────────────────────────────────────────
    template<typename T>
    inline void ColetaUm(ColetaArgs& c, const T& v)
    {
        c.Entrada(&v, uint32_t(sizeof(T)));
        ++c.indice;
    }
    // Texto is already an FString the GAME allocated: its 16 bytes are passed.
    inline void ColetaUm(ColetaArgs& c, const Texto& v)
    {
        c.Entrada(v.bruto, 16);
        ++c.indice;
    }
    inline void ColetaUm(ColetaArgs& c, const TextoRico& v)
    {
        c.Entrada(v.bruto, 16);
        ++c.indice;
    }
    // The FName already resolved by the game's pool: 8 bytes, no ownership. The
    // pool owns the text and lives for the whole process, so nothing here
    // dangles after the call returns (unlike an FString, which ProcessEvent
    // destroys).
    inline void ColetaUm(ColetaArgs& c, const Nome& v)
    {
        c.Entrada(v.bruto, 8);
        ++c.indice;
    }

    // ── the outputs ────────────────────────────────────────────────────────
    template<typename T>
    inline void ColetaUm(ColetaArgs& c, const Fora<T>& v)
    {
        if (c.nsai < int(MAX_PARMS))
        { c.sai[c.nsai] = ConanSaida{ c.indice, v.destino, CONAN_SAIDA_POD,
                                      uint32_t(sizeof(T)), 0, nullptr }; ++c.nsai; }
        c.Buraco();
        ++c.indice;
    }
    inline void ColetaUm(ColetaArgs& c, const ForaTexto& v)
    {
        if (c.nsai < int(MAX_PARMS))
        { c.sai[c.nsai] = ConanSaida{ c.indice, v.destino, CONAN_SAIDA_TEXTO,
                                      uint32_t(v.tam), 0, nullptr }; ++c.nsai; }
        c.Buraco();
        ++c.indice;
    }
    inline void ColetaUm(ColetaArgs& c, const ForaTextoRico& v)
    {
        if (c.nsai < int(MAX_PARMS))
        { c.sai[c.nsai] = ConanSaida{ c.indice, v.destino, CONAN_SAIDA_TEXTO_RICO,
                                      uint32_t(v.tam), 0, nullptr }; ++c.nsai; }
        c.Buraco();
        ++c.indice;
    }
    template<typename T>
    inline void ColetaUm(ColetaArgs& c, const ForaLista<T>& v)
    {
        if (c.nsai < int(MAX_PARMS))
        { c.sai[c.nsai] = ConanSaida{ c.indice, v.destino, CONAN_SAIDA_LISTA,
                                      uint32_t(v.capacidade), uint32_t(sizeof(T)),
                                      v.quantos }; ++c.nsai; }
        c.Buraco();
        ++c.indice;
    }
    // UPARAM(ref): writes BEFORE and reads AFTER. It goes into both lists, with
    // the same index, which is exactly what the slot does in the game's block.
    template<typename T>
    inline void ColetaUm(ColetaArgs& c, const EntreSai<T>& v)
    {
        c.Entrada(v.valor, uint32_t(sizeof(T)));
        if (c.nsai < int(MAX_PARMS))
        { c.sai[c.nsai] = ConanSaida{ c.indice, v.valor, CONAN_SAIDA_POD,
                                      uint32_t(sizeof(T)), 0, nullptr }; ++c.nsai; }
        c.Buraco();
        ++c.indice;
    }

    inline void ColetaUm(ColetaArgs& c, const EntreSaiTextoRico& v)
    {
        c.Entrada(v.entrada.bruto, 16);
        if (c.nsai < int(MAX_PARMS))
        { c.sai[c.nsai] = ConanSaida{ c.indice, v.destino, CONAN_SAIDA_TEXTO_RICO,
                                      uint32_t(v.tam), 0, nullptr }; ++c.nsai; }
        ++c.indice;
    }

    inline void ColetaUm(ColetaArgs& c, const EntreSaiTexto& v)
    {
        c.Entrada(v.entrada.bruto, 16);
        if (c.nsai < int(MAX_PARMS))
        { c.sai[c.nsai] = ConanSaida{ c.indice, v.destino, CONAN_SAIDA_TEXTO,
                                      uint32_t(v.tam), 0, nullptr }; ++c.nsai; }
        ++c.indice;
    }

    // Return value: index -1, and it does NOT consume a parameter position.
    inline void ColetaUm(ColetaArgs& c, const RetornoTexto& v)
    {
        if (c.nsai < int(MAX_PARMS))
        { c.sai[c.nsai] = ConanSaida{ -1, v.destino, CONAN_SAIDA_TEXTO,
                                      uint32_t(v.tam), 0, nullptr }; ++c.nsai; }
    }
    inline void ColetaUm(ColetaArgs& c, const RetornoTextoRico& v)
    {
        if (c.nsai < int(MAX_PARMS))
        { c.sai[c.nsai] = ConanSaida{ -1, v.destino, CONAN_SAIDA_TEXTO_RICO,
                                      uint32_t(v.tam), 0, nullptr }; ++c.nsai; }
    }
    template<typename T>
    inline void ColetaUm(ColetaArgs& c, const RetornoLista<T>& v)
    {
        if (c.nsai < int(MAX_PARMS))
        { c.sai[c.nsai] = ConanSaida{ -1, v.destino, CONAN_SAIDA_LISTA,
                                      uint32_t(v.capacidade), uint32_t(sizeof(T)),
                                      v.quantos }; ++c.nsai; }
    }

    template<typename... A>
    inline void Coleta(ColetaArgs& c, A&&... args)
    { (void)c; (void)std::initializer_list<int>{ (ColetaUm(c, args), 0)..., 0 }; }

    // ── the two calls ──────────────────────────────────────────────────────
    //
    // The table may be OLDER than this header: a plugin compiled against v6 can
    // run on a v5 API, which has no ChamarFuncaoEx. Checking `tamanho` before
    // using the pointer is what separates "can't do that" from reading past the
    // end of the struct and calling garbage.
    inline bool TabelaTem(size_t bytesAte)
    {
        const ConanApiTabela* t = TabelaDoPlugin();
        return t && t->tamanho >= bytesAte;
    }

    template<typename R = void, typename... A>
    inline R Call(void* obj, const char* nome, A&&... args)
    {
        ColetaArgs c;
        Coleta(c, args...);
        const ConanApiTabela* t = TabelaDoPlugin();
        using RSeguro = typename std::conditional<std::is_void<R>::value, char, R>::type;
        RSeguro r{};
        if (!t) { AvisarSemTabela(nome); if constexpr (!std::is_void<R>::value) return r; else return; }
        if (t->ChamarFuncao)
            t->ChamarFuncao(obj, nome, c.ent, c.tam, c.nent,
                            std::is_void<R>::value ? nullptr : &r,
                            std::is_void<R>::value ? 0u : uint32_t(sizeof(RSeguro)));
        if constexpr (!std::is_void<R>::value) return r;
        else                                    return;
    }

    template<typename R = void, typename... A>
    inline R CallSaida(void* obj, const char* nome, A&&... args)
    {
        ColetaArgs c;
        Coleta(c, args...);
        const ConanApiTabela* t = TabelaDoPlugin();
        using RSeguro = typename std::conditional<std::is_void<R>::value, char, R>::type;
        RSeguro r{};
        if (TabelaTem(offsetof(ConanApiTabela, ChamarFuncaoEx) + sizeof(void*)) &&
            t->ChamarFuncaoEx)
        {
            t->ChamarFuncaoEx(obj, nome, c.ent, c.tam, c.nent,
                              c.sai, c.nsai,
                              std::is_void<R>::value ? nullptr : &r,
                              std::is_void<R>::value ? 0u : uint32_t(sizeof(RSeguro)));
        }
        else if (t && t->Log)
        {
            // Don't invent success: without ChamarFuncaoEx the output slots
            // keep whatever value they already had, and returning as if they'd
            // been filled is the very defect this API spent the night hunting.
            t->Log("CallSaida(\"%s\"): esta API e' anterior a v6 e nao sabe "
                   "devolver parametro de saida. Nada foi chamado.", nome);
        }
        if constexpr (!std::is_void<R>::value) return r;
        else                                    return;
    }

#endif  // CONAN_MOTOR

    // ── Nome: the constructor, now that `Call` exists ───────────────────────
    //
    // It serves both paths (runtime and plugin) because `Call` exists on both.
    // One implementation, and no "#ifdef" for the two to diverge later.
    //
    // It MEMOISES because the pool doesn't forget: resolved once, that text has
    // that index until the process dies. Without this, a shop delivering items
    // would pay one ProcessEvent per delivery just to rediscover the same
    // number. The cache is small on purpose: context names are few and fixed
    // (the plugin writes them in code); when full it still works, it just stops
    // memoising, and never returns the wrong answer.
    inline Nome::Nome(const char* s) : bruto{}, valido(false)
    {
        if (!s || !*s) return;

        struct Entrada { char texto[64]; unsigned char bruto[8]; };
        static Entrada memo[64];
        static int nMemo = 0;

        for (int i = 0; i < nMemo; ++i)
        {
            if (std::strcmp(memo[i].texto, s) != 0) continue;
            std::memcpy(bruto, memo[i].bruto, 8);
            valido = true;
            return;
        }

        // The library's CDO is reached differently on each side of the
        // boundary, and this is the only place in the wrapper where that shows:
        // the runtime calls the function directly, the plugin only has the
        // table. Both lines do the SAME thing — no rule diverges here, only the
        // way in.
#ifdef CONAN_MOTOR
        void* lib = static_cast<void*>(GetDefaultObject("KismetStringLibrary"));
#else
        const ConanApiTabela* tb = TabelaDoPlugin();
        if (!tb || !tb->GetDefaultObject) return;
        void* lib = tb->GetDefaultObject("KismetStringLibrary");
#endif
        if (!lib) return;

        // Conv_StringToName(InString: FString) -> FName. The input FString is
        // built by the game (Texto), and the return is the FName's 8 bytes.
        Texto entrada(s);
        if (!entrada.valido) return;

        struct { unsigned char b[8]; } r{};
        r = Call<decltype(r)>(lib, "Conv_StringToName", entrada);

        // An FName {0,0} is "None": a legitimate answer for the string "None"
        // and a failure signal for anything else. Refusing here stops a plugin
        // from sending "None" while believing it sent "shop".
        const bool ehNone = (r.b[0]|r.b[1]|r.b[2]|r.b[3]|r.b[4]|r.b[5]|r.b[6]|r.b[7]) == 0;
        if (ehNone && std::strcmp(s, "None") != 0) return;

        std::memcpy(bruto, r.b, 8);
        valido = true;

        if (nMemo < 64 && std::strlen(s) < sizeof(memo[0].texto))
        {
            std::strcpy(memo[nMemo].texto, s);
            std::memcpy(memo[nMemo].bruto, bruto, 8);
            ++nMemo;
        }
    }

// ── THE GAME BUILD, IN ONE PLACE ────────────────────────────────────────────
//
// The number used to be written by hand in the log message. When the build
// changed from 24383534 to 24784646, the anchors were updated and the message
// kept saying the old number — the log asserting one thing while the API ran
// another.
//
// At 3am, that line is what someone uses to know which build the API was made
// for. Saying the wrong number sends them to investigate the wrong place.
#define CONAN_BUILD_DO_JOGO "24784646"
}

// ── minimal engine types, only what the accessors need ─────────────────────
struct FName            { int32_t ComparisonIndex; int32_t Number; };
struct FString          { void* Data; int32_t Num; int32_t Max; };
// FText is 16 bytes, not 8: measured through reflection (TextProperty's
// ElementSize). The internal layout was not measured, so these are opaque bytes.
// Inventing fields that add up to the right size would give a plausible and
// false struct.
struct FText            { uint8_t _opaco[16]; };
struct FScriptArray     { void* Data; int32_t Num; int32_t Max; };
// FScriptMap and FScriptSet are 80 bytes each (measured), not 32. The wrong
// figure made every struct containing a TMap come out smaller than it really
// is, and the following field land in the wrong place, silently.
struct FScriptMap       { uint8_t _opaco[80]; };
struct FScriptSet       { uint8_t _opaco[80]; };
struct FWeakObjectPtr   { int32_t ObjectIndex; int32_t ObjectSerialNumber; };
struct FSoftObjectPtr   { uint8_t _opaco[40]; };   // measured: 40, not 24
struct FScriptDelegate  { FWeakObjectPtr Object; FName FunctionName; };
struct FMulticastScriptDelegate { FScriptArray Invocations; };
struct FScriptInterface { void* Object; void* Interface; };
struct FVector          { double X, Y, Z; };     // UE5 uses double by default
struct FRotator         { double Pitch, Yaw, Roll; };
struct FVector2D        { double X, Y; };

// ── PASSING TEXT TO THE GAME: why it goes through the API ───────────────────
//
// There used to be a `TextoParaOJogo` class here, which built an `FString`
// pointing at a buffer of ours. It was REMOVED because it **crashes the
// server**, and the reason is structural, not a bug to be fixed.
//
// THE TEST AND THE RESULT
// -----------------------
// `ConvertToAbsolutePath("test-api-xyz")` called through reflection, with the
// FString built that way. The plugin's log dies on exactly that line:
//
//     [prova] === 1. passar FString PARA o jogo ===   (the test's own log line,
//                                                      quoted verbatim)
//                                                   <- and the process ends
//
// WHY, AND WHY THERE IS NO SIMPLE FIX
// ------------------------------------
// `ProcessEvent` DESTROYS the parameter block when the function returns: it
// walks the properties' destructor list (`DestructorLink`) and calls each
// destructor. `FString`'s destructor calls `FMemory::Free(Data)` — THE GAME'S
// allocator — on a pointer that came from our stack.
//
// It is not the game "reading it wrong": it is the game doing the right thing
// with memory that is not its own. Any buffer of ours passed as an FString
// through reflection ends up in `FMemory::Free`.
//
// The correct path is allocating with the game's allocator, and THAT IS WHAT
// THE API NOW DOES. `ConanApi::Texto` and `ConanApi::TextoRico` (declared near
// the top of this file) build the 16-byte value with the game's own memory, so
// the destructor frees a pointer that really is the game's:
//
//     pc->Call<void>("ClientHUDShowNotification",
//                    ConanApi::TextoRico("You have 250 points"),
//                    bool(true), bool(false));
//
// Same idea for `FName`, one step further: `ConanApi::Nome` goes through the
// game's own `Conv_StringToName`. Conan Shop answers players in production
// through this path every day.
//
// WHAT A PLUGIN MUST NOT DO
// -------------------------
// Build the 16 bytes by hand and point them at its own buffer. That is the
// removed class, and it still crashes for the same reason. If you need text to
// cross into the game, it comes from `Texto`/`TextoRico`/`Nome` and from
// nowhere else.
//
// This comment stays because the mistake is easy to repeat: the class compiled,
// looked right, and the test took 90 seconds to bring the server down.

// ── reading text that came FROM the game ────────────────────────────────────
// Declared inside ConanApi, and here, after FString exists. The definition lives
// in the namespace; declaring it at global scope would give a missing-symbol
// error that only appears at link time, far from the cause.
namespace ConanApi { std::string TextoDoJogo(const FString& s); }

// ── a reference to a member, resolved by offset ─────────────────────────────
template<typename T>
struct FieldRef
{
    void*     base;
    uintptr_t offset;

    T*   ptr()  const { return reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(base) + offset); }
    T&   Get()  const { return *ptr(); }
    void Set(const T& v) const { *ptr() = v; }

    operator T&() const { return Get(); }
    T& operator=(const T& v) const { Set(v); return Get(); }
    T* operator->() const { return ptr(); }
};

// ── a reference to a BIT inside a shared byte ───────────────────────────────
//
// WHY THIS EXISTS, AND THE DEFECT IT FIXES
// -----------------------------------------
// Several Unreal booleans share the SAME byte: they're bitfields. In `Actor`,
// SEVEN bools live at offset 104, told apart only by the mask:
//
//     bNetTemporary          0x68  mask 0x01
//     bOnlyRelevantToOwner   0x68  mask 0x04
//     bAlwaysRelevant        0x68  mask 0x08
//     bReplicateMovement     0x68  mask 0x10
//     bCallPreReplication    0x68  mask 0x20
//     ...                    0x68  mask 0x40
//     bHidden                0x68  mask 0x80
//
// The previous version generated a `FieldRef<bool>` for each of them, all at
// the same offset. The result is the worst class of defect this project
// recognises: reading `bOnlyRelevantToOwner` answered `true` because `bHidden`
// was set. No error, no garbage — a plausible and wrong boolean.
//
// There were 1,908 members across 401 classes like that: Actor 33,
// PrimitiveComponent 63, CharacterMovementComponent 56, Material 87.
//
// And writing was worse: `bHidden() = true` wrote 0x01 over the whole byte and
// CLEARED the other six at once.
struct BitRef
{
    void*     base;
    uintptr_t offset;
    uint8_t   mascara;

    uint8_t* byte() const
    { return reinterpret_cast<uint8_t*>(base) + offset; }

    bool Get() const { return (*byte() & mascara) != 0; }

    void Set(bool v) const
    {
        uint8_t* p = byte();
        // Read-modify-write, preserving the neighbours. Deliberately not
        // atomic: making it atomic would suggest you can write the game's
        // bitfields from any thread, and you can't — writing into a byte the
        // game also writes needs to be a deliberate decision.
        *p = v ? uint8_t(*p | mascara) : uint8_t(*p & ~mascara);
    }

    operator bool() const { return Get(); }
    bool operator=(bool v) const { Set(v); return v; }
};

// ── the root of everything ──────────────────────────────────────────────────
class UObject
{
public:
    // offsets measured against live reflection; confirmed by a second route
    // while disassembling the vtable, which reads +0x08 ObjectFlags,
    // +0x0C InternalIndex,
    // +0x10 ClassPrivate, +0x18 NamePrivate and +0x20 Outer.
    FieldRef<UClass*> ClassPrivate() { return { this, 0x10 }; }
    FieldRef<FName>   NamePrivate()  { return { this, 0x18 }; }
    FieldRef<UObject*> OuterPrivate(){ return { this, 0x20 }; }

    std::string GetName();
    std::string GetFullName();
    bool        IsA(UClass* c);

    template<typename R = void, typename... A>
    R Call(const char* nome, A&&... a)
    { return ConanApi::Call<R>(this, nome, std::forward<A>(a)...); }

    // The counterpart was missing: functions with an OUTPUT parameter are 6,157
    // of the 36,757 on this build, and without this shortcut a developer had to
    // drop down to ConanApi::CallSaida with `this` in hand — in precisely the
    // case that's easiest to get wrong.
    template<typename R = void, typename... A>
    R CallSaida(const char* nome, A&&... a)
    { return ConanApi::CallSaida<R>(this, nome, std::forward<A>(a)...); }
};

class UField : public UObject
{
public:
    FieldRef<UObject*> Next() { return { this, 0x28 }; }
};

class UStruct : public UField
{
public:
    FieldRef<UClass*>  SuperStruct()     { return { this, 0x40 }; }  // 299/300
    FieldRef<UObject*> Children()        { return { this, 0x48 }; }  // UFunction
    FieldRef<void*>    ChildProperties() { return { this, 0x70 }; }  // FProperty
};

class UClass : public UStruct
{
public:
    // UClass's own StaticClass: useful for testing whether an object IS a class
    static UClass* StaticClass() { return ConanApi::FindClass("Class"); }
};

class UFunction : public UStruct
{
public:
    static UClass* StaticClass() { return ConanApi::FindClass("Function"); }
    FieldRef<uint32_t> FunctionFlags() { return { this, 0xB0 }; }  // FUNC_Native = 0x400
    FieldRef<uint8_t>  NumParms()      { return { this, 0xB4 }; }  // 100% vs reflection
    FieldRef<uint16_t> ParmsSize()     { return { this, 0xB6 }; }  // 96.9% vs reflection
    FieldRef<void*>    Func()          { return { this, 0xD8 }; }  // native pointer
};
