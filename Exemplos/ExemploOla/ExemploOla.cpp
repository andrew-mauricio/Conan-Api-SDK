// ============================================================================
//  ExemploOla — the smallest possible plugin that still PROVES the API works
//
//  This is not a "hello world". A hello world prints a line and proves nothing:
//  it would print exactly the same if every offset in this API were wrong. This
//  plugin exercises the paths the table offers and PRINTS WHAT IT READ, so that
//  anyone can check it against what the server shows:
//
//     1. reflection up + measured cost -> a number in the log, not an estimate
//     2. find a class by name          -> must find Conan's, not just engine ones
//     3. read a member by offset       -> the value must be plausible, not junk
//     4. hierarchy by name             -> DescendeDe must match what's expected
//     5. call a function by name       -> must answer what we already know
//     6. bitfield vs function          -> two sources that don't talk to each other
//
//  Copy this folder to start your own.
//
//  ────────────────────────────────────────────────────────────────────────────
//  THE MODEL CHANGED: A FUNCTION TABLE, NOTHING OF OURS INSIDE YOUR BINARY
//  ────────────────────────────────────────────────────────────────────────────
//  This example used to include `Conan/ConanSDK.h` and compile thousands of
//  lines of OUR runtime into its DLL. Now the only file of ours that takes part
//  is `Conan/ConanPluginApi.h`, which is pure declaration, and the plugin links
//  no library of ours at all. Everything is called through `g_api->`.
//
//  Why that matters TO WHOEVER COPIES THIS EXAMPLE (the long reasoning is in
//  the header of ConanPluginApi.h): your compiler stops mattering, because the
//  boundary became plain C; and a fix in our runtime doesn't force you to
//  rebuild, because it no longer lives inside your binary.
//
//  BUILD
//     ./compilar.sh          (produces ExemploOla.dll)
//  INSTALL
//     copy it to  <server>/ConanSandbox/Binaries/Win64/Conan-Api/Plugins/
//
//  THE FOLDER HAS A HYPHEN: `Conan-Api`, never run together.
//  This line used to say the folder WITHOUT the hyphen, and the loader builds
//  `<exe folder>\Conan-Api` + `\Plugins\*.dll`, with the API deriving
//  Config/Dados/Logs from the SAME place. Anyone following the instruction
//  copied their DLL into a folder the loader NEVER scans: the plugin didn't
//  load and there wasn't a line of error pointing at the cause. The log only
//  said "plugin folder empty or missing", naming the OTHER path, which the
//  owner didn't know existed. A silent failure on the community's first contact
//  with the API. The script plugins/conferir-caminhos.sh exists so it doesn't
//  come back.
// ============================================================================
#include "Conan/ConanPluginApi.h"
#include <windows.h>
#include <stdint.h>

// The table arrives once, in ConanPluginCarregar, and stays valid for the whole
// life of the process: it's a static object on the API's side, never
// reallocated. Keeping the pointer in a global is the usual pattern — and it's
// the ONLY state this plugin has.
static const ConanApiTabela* g_api = nullptr;

// ── shortcuts for ChamarFuncao ──────────────────────────────────────────────
//
// `ChamarFuncao` is deliberately raw: an array of pointers to the values and an
// array of SIZES. There's no `Call<T>` template here, and that's on purpose —
// templates are C++, and C++ doesn't cross the boundary between your compiler
// and ours (see ConanPluginApi.h). The price is writing the size by hand; the
// change you get back is that the API checks that size against what reflection
// says about the parameter and REFUSES the call when it doesn't match, instead
// of quietly corrupting the block. These two functions exist only to keep the
// rest of the file readable.
static int ChamarII(void* obj, const char* nome, int32_t a, int32_t b, int32_t* saida)
{
    const void*    args[2] = { &a, &b };
    const uint32_t tams[2] = { 4, 4 };          // IntProperty: 4 bytes each
    return g_api->ChamarFuncao(obj, nome, args, tams, 2, saida, sizeof(*saida));
}

static int ChamarDD(void* obj, const char* nome, double a, double b, double* saida)
{
    const void*    args[2] = { &a, &b };
    const uint32_t tams[2] = { 8, 8 };          // DoubleProperty: 8 bytes each
    return g_api->ChamarFuncao(obj, nome, args, tams, 2, saida, sizeof(*saida));
}

// The object's name into a buffer of ours. `NomeDoObjeto` returns that same
// buffer, so it drops straight into Log. std::string doesn't cross the C
// boundary — which is why the plugin supplies the memory and knows its size.
static const char* Nome(void* obj, char* buf, int tam)
{
    return g_api->NomeDoObjeto(obj, buf, tam);
}

static void Passo1_Reflexao()
{
    // ── the cost is MEASURED, not estimated ─────────────────────────────────
    //
    // An adversarial review measured name decoding at 7.66 us per object,
    // because every name fired three VirtualQuery calls (2,551 ns each) under
    // Wine. At 300,000 objects a full sweep went past 2 SECONDS — and a plugin
    // resolving player identity inside the game loop would hang the server,
    // with "it's slow" as the symptom, which points at nothing.
    //
    // A number in the log on every boot: comments age, measurements don't. What
    // the measurement shows is the CACHE: the 1st resolution pays for the
    // sweep, the 2nd doesn't. If the 2nd call ever costs what the 1st did, the
    // cache is dead — and it shows up here before it shows up as "slow
    // server".
    DWORD t0 = GetTickCount();
    void* cdo = g_api->GetDefaultObject("KismetMathLibrary");
    const DWORD d1 = GetTickCount() - t0;

    t0 = GetTickCount();
    cdo = g_api->GetDefaultObject("KismetMathLibrary");        // 2nd time: cache
    const DWORD d2 = GetTickCount() - t0;

    g_api->Log("[1] GetDefaultObject: 1a chamada %lu ms · 2a (cache) %lu ms  %s",
               d1, d2, cdo ? "" : "(NAO ACHOU)");

    // ── what this step can no longer measure, and why ───────────────────────
    //
    // The previous version printed `NumObjects()` (how many live objects the
    // GUObjectArray holds) and timed `FindObjects("Actor", ...)`. Neither of
    // those exists in the table: it exposes no SWEEP of the object array, only
    // a lookup of ONE object by name (`FindObject`). There's no other table
    // function to stand in for it — so this example stopped measuring, and says
    // so, rather than printing a similar-looking number that measures something
    // else. A wrong number wearing the face of a right one is worse than no
    // number.
    g_api->Log("    (contagem de objetos e varredura por classe nao existem na"
               " tabela — este passo mede so o custo da resolucao por nome)");
}

static void Passo2_Classes()
{
    // Finding an ENGINE class proves next to nothing — every UE API finds
    // Actor. What proves something is finding CONAN's classes: they only exist
    // if the catalogue came from this game's reflection.
    const char* alvos[] = { "ConanPlayerController", "ConanCharacter",
                            "ConanGameMode", "Actor", "PlayerController" };
    for (const char* n : alvos)
    {
        void* c = g_api->FindClass(n);
        g_api->Log("[2] FindClass(\"%s\") -> %s", n, c ? "achou" : "NAO ACHOU");
    }
}

static void Passo3_Membros()
{
    // Read a member off a REAL object. If the offset were wrong, junk would
    // come out — and junk is visible: a negative count, a huge number, an
    // absurd pointer.
    void* o = g_api->FindObject("ConanPlayerController");
    if (!o) { g_api->Log("[3] nenhum ConanPlayerController no mundo (servidor vazio?)"); return; }

    char buf[256];
    g_api->Log("[3] %s", Nome(o, buf, sizeof(buf)));

    // The offset comes from the reflection catalogue (golden/,
    // AConanPlayerController: CachedFollowerCount is an IntProperty at +0x930),
    // NEVER from a guess. `LerMembro` still puts the address through a
    // readability test before touching it: a game object can be inside the
    // garbage collector's window, and reading a dead pointer takes the whole
    // server down.
    int32_t seguidores = 0;
    if (g_api->LerMembro(o, 0x930, &seguidores, sizeof(seguidores)))
        g_api->Log("    CachedFollowerCount (+0x930) = %d", seguidores);
    else
        g_api->Log("    CachedFollowerCount (+0x930): ILEGIVEL — objeto saiu de baixo de nos");
}

static void Passo4_Hierarquia()
{
    void* o = g_api->FindObject("ConanGameMode");
    if (!o) { g_api->Log("[4] ConanGameMode nao encontrado"); return; }

    char buf[256];
    g_api->Log("[4] GameMode: %s", Nome(o, buf, sizeof(buf)));

    // `DescendeDe` walks the superclass chain by name. The positive/negative
    // pair is what gives the test its value: an implementation answering "yes"
    // to everything would pass the first line and fail the second. One
    // answering "no" to everything fails the first.
    g_api->Log("    DescendeDe(Actor)                 = %s",
               g_api->DescendeDe(o, "Actor") ? "sim" : "nao");
    g_api->Log("    DescendeDe(ConanPlayerController) = %s   %s",
               g_api->DescendeDe(o, "ConanPlayerController") ? "sim" : "nao",
               g_api->DescendeDe(o, "ConanPlayerController")
                 ? "<<< ERRADO: GameMode nao e PlayerController"
                 : "<<< CORRETO — o negativo separa de um 'sim' automatico");
}

static void Passo5_ChamarFuncao()
{
    // ── THE PROOF THAT ProcessEvent WORKS ───────────────────────────────────
    //
    // Steps 1 to 4 read. This one WRITES parameters, calls the game's code and
    // checks the result — and it's the only one that can't lie.
    //
    // The functions chosen come from the Blueprint maths library: pure, no side
    // effects at all, and with answers everybody knows by heart. If the results
    // match, ALL of this is right at the same time:
    //
    //    · the UFunction was found by walking the hierarchy by name;
    //    · both parameters were written at the offsets reflection gave;
    //    · ProcessEvent really was at vtable index 79;
    //    · the return value was read at the right offset of the parameter block;
    //    · and the crossing plugin -> table -> runtime scrambled nothing.
    //
    // Get any one of those wrong and the result isn't 12 — it's junk, or zero,
    // or the server goes down. There's no way to pass by accident.
    void* mat = g_api->GetDefaultObject("KismetMathLibrary");
    if (!mat) { g_api->Log("[5] KismetMathLibrary nao encontrada"); return; }

    // ── FIRST: a test that is NOT commutative ───────────────────────────────
    //
    // This closes a real hole in the earlier proof. `Add_IntInt(7,5)`,
    // `Multiply_IntInt(6,7)` and `FMax(3.5, 9.25)` are all COMMUTATIVE: they'd
    // give the same result with the parameters swapped. They prove the call
    // happens and the return value is read — but they do NOT prove that A went
    // to A's offset and B to B's.
    //
    // Subtraction doesn't forgive: flip the order and 10-3=7 becomes 3-10=-7. A
    // flipped sign is a one-bit discriminator, impossible to mistake. And under
    // the table model it covers more ground than it used to: the order now also
    // depends on `args[]` arriving on the other side of the C boundary in the
    // same order.
    int32_t sub = 0;
    ChamarII(mat, "Subtract_IntInt", 10, 3, &sub);
    g_api->Log("[5] Subtract_IntInt(10, 3) = %d   %s", sub,
        sub == 7  ? "<<< CORRETO — a ORDEM dos parametros esta certa"
      : sub == -7 ? "<<< ERRADO: parametros INVERTIDOS (deu 3-10)"
                  : "<<< ERRADO (esperado 7)");

    // Division likewise, and with a remainder: 100/7 = 14 (integer). Flipped
    // it would give 0.
    int32_t div = 0;
    ChamarII(mat, "Divide_IntInt", 100, 7, &div);
    g_api->Log("[5] Divide_IntInt(100, 7) = %d   %s", div,
        div == 14 ? "<<< CORRETO" : div == 0 ? "<<< ERRADO: invertido" : "<<< ERRADO (esperado 14)");

    int32_t r = 0;
    ChamarII(mat, "Add_IntInt", 7, 5, &r);
    g_api->Log("[5] Add_IntInt(7, 5) = %d   %s", r,
        r == 12 ? "<<< CORRETO — chamada por reflexao FUNCIONA"
                : "<<< ERRADO (esperado 12)");

    int32_t m = 0;
    ChamarII(mat, "Multiply_IntInt", 6, 7, &m);
    g_api->Log("[5] Multiply_IntInt(6, 7) = %d   %s", m,
        m == 42 ? "<<< CORRETO" : "<<< ERRADO (esperado 42)");

    // negatives and zero catch sign and offset errors that a small positive hides
    int32_t n = 0;
    ChamarII(mat, "Add_IntInt", -100, 37, &n);
    g_api->Log("[5] Add_IntInt(-100, 37) = %d   %s", n,
        n == -63 ? "<<< CORRETO" : "<<< ERRADO (esperado -63)");

    // ── THE ARGUMENT'S SIZE MUST BE REFLECTION'S SIZE ───────────────────────
    //
    // This proof CERTIFIED A WRONG CALL AS CORRECT until 2026-08-17. Under the
    // old model the line read:
    //
    //     float fm = mat->Call<float>("FMax", 3.5f, 9.25f);   // WRONG
    //
    // and the server log printed `FMax(3.5, 9.25) = 9.2500  <<< CORRETO`.
    //
    // THIS build's reflection (golden/funcoes.json, KismetMathLibrary::FMax)
    // says the function is DoubleProperty throughout, parmssize 24:
    //     A  off=0 size=8 · B  off=8 size=8 · ReturnValue off=16 size=8
    //
    // The template copied `sizeof(T)` bytes to the parameter's offset. With
    // `3.5f` that's 4 bytes into an 8-byte slot: the high half stays zeroed and
    // the game receives a DENORMAL. Checked byte by byte, same arithmetic:
    //     3.5f  in a double slot -> 5.336073e-315
    //     9.25f in a double slot -> 5.394356e-315
    //
    // The 9.25 came out in the log by ARITHMETIC ACCIDENT: with the high half
    // zeroed, the order of those denormals mirrors the order of the bit
    // patterns of POSITIVE floats. FMax picked the right operand for the wrong
    // reason, and the return value was read back out of the low 4 bytes. The
    // three cases the old proof chose — 3.5/9.25, 2.0/1.0, 100.0/0.5 — were all
    // positive: they all passed, by luck, and the 0xCD sentinel catches none of
    // it (the function DID execute; it just got different numbers).
    //
    // Under the table model the size stopped being deduced from the C++ type
    // and became DECLARED (`tams[]`), with the API checking it against
    // reflection — see the guard test just below. But declaring it is still on
    // you: `tams` = whatever reflection says, always. Here, 8 and 8.
    double fm = 0.0;
    ChamarDD(mat, "FMax", 3.5, 9.25, &fm);
    g_api->Log("[5] FMax(3.5, 9.25) = %.6f  %s", fm,
        (fm > 9.2499 && fm < 9.2501) ? "<<< CORRETO" : "<<< ERRADO (esperado 9.25)");

    // ── the case that SEPARATES — without it this proof approves the wrong ──
    //
    // Same job as Subtract_IntInt above: a one-bit discriminator. With 8 bytes
    // it gives 3.5; with 4 bytes written into an 8-byte slot it would give
    // -1.0. There's no way for the two paths to agree here.
    double fneg = 0.0;
    ChamarDD(mat, "FMax", 3.5, -1.0, &fneg);
    g_api->Log("[5] FMax(3.5, -1.0) = %.6f  %s", fneg,
        (fneg > 3.4999  && fneg < 3.5001)  ? "<<< CORRETO — o tamanho declarado casa com a reflexao (8 bytes)"
      : (fneg > -1.0001 && fneg < -0.9999) ? "<<< ERRADO: veio -1.0 — argumento de 4 bytes num slot de 8"
                                           : "<<< ERRADO (esperado 3.5)");

    // Negative on both sides: catches the same defect by another route, and
    // catches order inversion too. Right = -2.5; with 4 bytes in an 8-byte slot
    // it came out -7.25; with A and B swapped it would also give -7.25 — which
    // is why the one above, the one that separates the two causes, comes
    // first.
    double fneg2 = 0.0;
    ChamarDD(mat, "FMax", -2.5, -7.25, &fneg2);
    g_api->Log("[5] FMax(-2.5, -7.25) = %.6f  %s", fneg2,
        (fneg2 > -2.5001 && fneg2 < -2.4999) ? "<<< CORRETO"
                                             : "<<< ERRADO (esperado -2.5)");

    // ── and now the GUARD, which is what the new model added ────────────────
    //
    // The call below is the 2026-08-17 defect written on purpose: 4 bytes where
    // reflection asks for 8. Under the old model it ran and lied. Here it has
    // to be REFUSED, with the reason in the log and the return value untouched
    // — and that's what this line proves. A plugin copying this example
    // inherits the protection, not the defect.
    //
    // It's safe: nothing executes when the call is refused. The "RECUSADO" line
    // about to appear in the log JUST BELOW is expected.
    g_api->Log("[5] proposital: FMax com tams=4 (a proxima linha RECUSADO e esperada)");
    const float  fa = 3.5f, fb = -1.0f;
    const void*    args[2] = { &fa, &fb };
    const uint32_t tams[2] = { 4, 4 };          // WRONG on purpose
    double lixo = 12345.0;
    const int passou = g_api->ChamarFuncao(mat, "FMax", args, tams, 2, &lixo, sizeof(lixo));
    g_api->Log("    ChamarFuncao devolveu %d, retorno %s   %s",
        passou, (lixo == 12345.0) ? "intocado" : "SOBRESCRITO",
        (!passou && lixo == 12345.0)
          ? "<<< CORRETO — a API recusou o tamanho errado em vez de corromper"
          : "<<< ERRADO: a chamada de tamanho errado NAO foi barrada");
}

static void Passo6_BitfieldERetornoZero()
{
    // ── TWO FIXES PROVED AT ONCE, BY SOURCES THAT DON'T TALK TO EACH OTHER ──
    //
    // 1. BITFIELD: `bActorEnableCollision` is bit 0x02 of byte 0x6D (109) on
    //    Actor. The SAME byte houses `bActorIsBeingDestroyed` (0x04) and
    //    `bAsyncPhysicsTickEnabled` (0x80). Reading the whole byte as a bool
    //    answered "true" whenever any neighbour was set — which is why the
    //    table exposes `LerBit(obj, offset, mask)` and not just LerMembro.
    //
    // 2. RETURN AT OFFSET 0: `GetActorEnableCollision()` takes no arguments, so
    //    its return value lives at offset 0 of the parameter block. The
    //    previous version required offset > 0 to recognise a return value, and
    //    handed back `false` for EVERY argument-less function — IsAdmin()
    //    included. With no error whatsoever.
    //
    // The test: for every Actor we can reach, the bit and the function have to
    // agree. Two independent routes to the same fact — a direct memory read on
    // one side, the game's own code executing on the other.
    //
    // ── WHAT THIS STEP LOST IN THE MIGRATION, AND WHY IT ISN'T DISGUISED ────
    //
    // The previous version swept 400 Actors with `FindObjects("Actor", ...)`
    // and demanded VARIATION across the set — if they all held the same value,
    // two wrong implementations that always answer "false" would pass too. And
    // it used `UltimaChamadaExecutou()` to tell "the function answered false"
    // apart from "the call was FILTERED by ProcessEvent" (a Blueprint template,
    // a CDO, an uninitialised Actor) — an absent result is not a result.
    //
    // NEITHER of those exists in the table. With no sweep, the set here is the
    // handful of Actors reachable by name; with no execution signal, a "false"
    // might be an answer or might be a filter. The honest consequence: this
    // step stopped being conclusive and became observational. It prints what it
    // saw and says it can't separate the two — rather than keeping the old
    // verdict on top of a base that shrank, which is exactly how a test turns
    // into a rubber stamp.
    static const char* const alvos[] = {
        "ConanPlayerController", "ConanCharacter", "ConanGameMode",
        "PlayerController", "WorldSettings", "GameStateBase", "Actor"
    };

    void* vistos[16];
    int nvistos = 0;
    int concorda = 0, discorda = 0, ligados = 0, desligados = 0;
    char buf[256];

    for (const char* alvo : alvos)
    {
        void* o = g_api->FindObject(alvo);
        if (!o) continue;
        // Offset 0x6D only means anything on an AActor. Without this line, a
        // non-Actor object would have us read some arbitrary byte and the
        // tally would count junk.
        if (!g_api->DescendeDe(o, "Actor")) continue;

        // FindObject under different names can return the SAME object (the
        // first ConanPlayerController is also a PlayerController). Counting
        // one Actor twice inflates the tally without adding evidence.
        bool repetido = false;
        for (int i = 0; i < nvistos; ++i) if (vistos[i] == o) { repetido = true; break; }
        if (repetido) continue;
        if (nvistos < 16) vistos[nvistos++] = o;

        const int antes = g_api->LerBit(o, 0x6D, 0x02);          // bit 0x02 of byte 109

        uint8_t ret = 0;
        const int chamou = g_api->ChamarFuncao(o, "GetActorEnableCollision",
                                               nullptr, nullptr, 0, &ret, sizeof(ret));
        if (!chamou)
        {
            g_api->Log("    %s: GetActorEnableCollision nao resolveu", Nome(o, buf, sizeof(buf)));
            continue;
        }
        const int fn = ret ? 1 : 0;

        antes ? ++ligados : ++desligados;
        if (antes == fn) ++concorda;
        else
        {
            ++discorda;
            // Name WHO disagrees. An aggregate statistic doesn't tell you
            // what to do with "1 in 7"; the object's name does.
            g_api->Log("    DIVERGE: %s  · bit=%s funcao=%s",
                       Nome(o, buf, sizeof(buf)), antes ? "true" : "false",
                       fn ? "true" : "false");
        }
    }

    g_api->Log("[6] %d Actors alcancaveis por nome: bActorEnableCollision"
               "(bit 0x02 do byte 0x6D) x GetActorEnableCollision(retorno @0)",
               nvistos);
    g_api->Log("    concordam: %d   divergem: %d   ·   %d ligados, %d desligados",
               concorda, discorda, ligados, desligados);

    if (nvistos == 0)
        g_api->Log("    <<< SEM DADOS: nenhum Actor alcancavel por nome (servidor vazio?)");
    else if (discorda > 0)
        g_api->Log("    <<< DIVERGENCIA: as duas fontes discordam. Pode ser defeito, pode ser"
                   " chamada filtrada pelo ProcessEvent — a tabela nao expoe o sinal que"
                   " separa os dois casos. Investigue pelo nome impresso acima.");
    else if (ligados == 0 || desligados == 0)
        g_api->Log("    <<< INCONCLUSIVO: sem variacao no conjunto, o teste nao separa"
                   " codigo certo de codigo que responde sempre a mesma coisa");
    else
        g_api->Log("    <<< CONSISTENTE — bitfield com mascara E retorno no offset 0"
                   " concordam, COM variacao no conjunto");
}

// ── the contract ────────────────────────────────────────────────────────────
//
// The loader looks this function up by name and hands over the table. Without
// it, the DLL loads and nothing happens.
extern "C" __declspec(dllexport)
void ConanPluginCarregar(const ConanApiTabela* api)
{
    // ── this check is not a formality ───────────────────────────────────────
    //
    // If this plugin is compiled against a table BIGGER than the one the server
    // has (an API older than the plugin), every field past the end of the real
    // struct is somebody else's memory — and calling a "function pointer" read
    // from there takes the server down somewhere with no relation to the cause.
    // `tamanho` is the only way to know that BEFORE touching anything.
    //
    // There's no logging the reason: `Log` is precisely one of the fields you
    // can't trust here. Leaving quietly is the right move.
    if (!api || api->tamanho < sizeof(ConanApiTabela)) return;
    g_api = api;

    g_api->Log("======================================================");
    g_api->Log(" ExemploOla — API do Conan Exiles Enhanced (tabela v%u, %u bytes)",
               api->versao, api->tamanho);
    g_api->Log("======================================================");

    if (!g_api->Pronta())
    {
        g_api->Log("ABORTADO: reflexao indisponivel. O jogo atualizou? Regere a API.");
        return;
    }

    Passo1_Reflexao();
    Passo2_Classes();
    Passo3_Membros();
    Passo4_Hierarquia();
    Passo5_ChamarFuncao();
    Passo6_BitfieldERetornoZero();
    g_api->Log("== fim ==");
}

// Optional. Called when the server shuts down cleanly. This plugin registers
// no hook and no thread, so there's nothing to undo — but dropping the pointer
// makes it explicit that the table is no longer valid past this point.
extern "C" __declspec(dllexport)
void ConanPluginDescarregar(void)
{
    if (g_api) g_api->Log("ExemploOla: descarregando.");
    g_api = nullptr;
}
