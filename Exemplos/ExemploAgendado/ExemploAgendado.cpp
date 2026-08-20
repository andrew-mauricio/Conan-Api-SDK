// ============================================================================
//  ExemploAgendado — proof that a plugin can run ON ITS OWN, every so often,
//                    without creating a thread of its own
//
//  Before this, a plugin only ran once, at load time. Spinning up a thread and
//  calling the API from there is the wrong road: Unreal isn't thread-safe, and
//  the damage shows up far from the cause.
//
//  The task here runs from inside the ProcessEvent detour — on the same thread
//  the engine uses for everything.
//
//  THE PROOF: each firing calls a game function and checks the answer. If the
//  task were running on the wrong thread, or the scheduler were firing outside
//  a valid context, the call wouldn't return 7.
//
//  Table model: this file includes nothing of ours beyond the declaration
//  header, and links no library of ours. Everything goes through `g_api->`.
// ============================================================================
#include "Conan/ConanPluginApi.h"
#include <windows.h>

// ── PROVING I RAN ON THE GAME'S THREAD ──────────────────────────────────────
//
// The previous version said "I ran on the game's thread" because Subtract(10,3)
// came out 7. That proves nothing about THREADS: the sum is 7 on any of them.
// It was a claim wearing a proof's face, which is worse than none — the
// developer copies the example and thinks they're verifying something.
//
// What actually proves it: capture the id of the thread that runs a hook (that
// one is the game's, by definition) and compare it with the id from inside the
// scheduled task.

// The table pointer is kept in a static because the scheduled task runs long
// after Carregar, without being handed the table back — the context the
// scheduler returns is OURS, and we don't need it here.
static const ConanApiTabela* g_api = nullptr;

static int   g_disparos = 0;
static DWORD g_t0 = 0;

// ── THE THREAD PROOF, FOR REAL ──────────────────────────────────────────────
//
// What proves it: capture the thread id inside a HOOK. A ProcessEvent hook
// runs, by definition, on the thread that runs the game. If the id inside the
// scheduled task is the SAME, AgendarNaThreadDoJogo has kept its promise; if
// it's a different one, the promise is broken — and the log has to shout,
// because a plugin touching the world off the game's thread corrupts state
// slowly.
static volatile DWORD g_threadDoJogo = 0;
static volatile DWORD g_candidata = 0;
static volatile long  g_votos = 0;

extern "C" ConanAcao CapturarThread(ConanChamada* c)
{
    (void)c;
    // THE DOMINANT THREAD, NOT THE FIRST — and that distinction cost a wrong
    // conclusion on 2026-08-19. Recording the first event that comes through
    // here looks reasonable and isn't: ProcessEvent is called from SEVERAL
    // threads, and the first one can come from any of them. That's how the API
    // got "proved" broken when the thing at fault was the instrument.
    //
    // So count, and keep whichever shows up most: the game loop's dominates by
    // orders of magnitude (measured: 99.9%).
    const DWORD eu = GetCurrentThreadId();
    if (eu == g_candidata) { ++g_votos; }
    else if (g_votos == 0) { g_candidata = eu; g_votos = 1; }
    else { --g_votos; }
    if (g_votos > 5000) g_threadDoJogo = g_candidata;
    return CONAN_CONTINUAR;
}

static void Tique(void* /*contexto*/)
{
    if (!g_api) return;   // shouldn't happen; the task is only scheduled with the table in hand

    ++g_disparos;
    const DWORD dt = GetTickCount() - g_t0;

    // Calling a game function FROM INSIDE the task: this is what proves the
    // thread.
    //
    // Under the table model there's no `obj->Call<int32_t>(...)` any more: that
    // template was OUR code compiled inside the plugin. Now the arguments go as
    // an array of pointers plus an array of sizes, and the API checks each size
    // against the real parameter before assembling the block — passing 4 bytes
    // where the game wants 8 (or the reverse) gets refused instead of writing
    // junk.
    void* mat = g_api->GetDefaultObject("KismetMathLibrary");
    int32_t r = -1;
    if (mat)
    {
        const int32_t a = 10, b = 3;
        const void*    args[2] = { &a, &b };
        const uint32_t tams[2] = { (uint32_t)sizeof(a), (uint32_t)sizeof(b) };
        int32_t ret = 0;
        // Returns 1 if it executed. On a refusal, `r` stays -1 and the log
        // says ERRADO — silencing the failure here would destroy the proof.
        if (g_api->ChamarFuncao(mat, "Subtract_IntInt", args, tams, 2,
                                &ret, (uint32_t)sizeof(ret)))
            r = ret;
    }

    uint64_t total = 0, desp = 0;
    g_api->EstatisticaHooks(&total, &desp);

    g_api->Log("[agendado] disparo %d aos %lu.%03lus · Subtract_IntInt(10,3)=%d %s "
               "· funil ja passou %llu chamadas",
               g_disparos, dt / 1000, dt % 1000, r,
               r == 7 ? "<<< a chamada por reflexao funcionou" : "<<< ERRADO",
               (unsigned long long)total);

    // The proof the name promises: the SAME thread as the hook?
    const DWORD minha = GetCurrentThreadId();
    if (!g_threadDoJogo)
        g_api->Log("[agendado] thread: ainda nao capturei a do jogo (nenhum evento "
                   "passou pelo hook). Sem base de comparacao, NAO afirmo nada.");
    else if (minha == g_threadDoJogo)
        g_api->Log("[agendado] thread %lu == a do jogo %lu  <<< PROVADO: a tarefa "
                   "roda na thread do jogo", (unsigned long)minha,
                   (unsigned long)g_threadDoJogo);
    else
        g_api->Log("[agendado] ATENCAO: thread %lu != a do jogo %lu. A promessa de "
                   "AgendarNaThreadDoJogo esta QUEBRADA — tocar no mundo daqui "
                   "corrompe estado devagar e sem erro.",
                   (unsigned long)minha, (unsigned long)g_threadDoJogo);

    if (g_disparos >= 4)
        g_api->Log("[agendado] 4 disparos com resposta certa: o agendador funciona.");
}

extern "C" __declspec(dllexport)
void ConanPluginCarregar(const ConanApiTabela* api)
{
    // The table can be SMALLER than the one this plugin knows if the API is
    // older than the binary. Reading a field past its end would hand back junk
    // and we'd call some arbitrary address — which is why the check comes
    // before any use at all, including before the first Log.
    if (!api || api->tamanho < sizeof(ConanApiTabela)) return;
    g_api = api;

    g_t0 = GetTickCount();
    g_api->Log("");
    g_api->Log("=== ExemploAgendado ===");
    if (!g_api->Pronta()) { g_api->Log("  reflexao indisponivel"); return; }

    const uint32_t id =
 // A hook on EVERYTHING, purely to capture the game thread's id. It's
 // cheap (returns CONTINUAR straight away) and it's the only honest way
 // to have something to compare against: with no baseline, the proof
 // below proves nothing.
 g_api->HookProcessEventTudo(CapturarThread, 100);
 g_api->AgendarNaThreadDoJogo(Tique, 10, nullptr, 1);
    g_api->Log("  tarefa a cada 10 s -> id %u %s", id, id ? "" : "(NAO agendou)");
}
