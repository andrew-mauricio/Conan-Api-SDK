// ============================================================================
//  ExemploBlueprint — intercepts EVERY Blueprint function execution
//
//  It closes the gap the ProcessEvent hook left behind: a Blueprint-to-
//  Blueprint call inside the same graph goes through `UObject::CallFunction`
//  and escapes the event funnel.
//
//  The way out wasn't finding `CallFunction` — it was finding where the two
//  paths MEET. `UObject::ProcessInternal` is the bytecode interpreter, and
//  every Blueprint function goes through it, wherever it came from.
//
//  THE COST, MEASURED — and neither the guess nor the first correction got it
//
//  I wrote that it would fire "far more than an event hook": a guess. I
//  measured the first 30 s and "corrected" it to the opposite (4 against
//  1,537): also wrong, because the server was still coming up. The real
//  series:
//
//      boot        4 · 46 · 10,787 per 30 s     <- populating the world
//      steady      ~190,000 per 30 s            <- ~6,300 executions/s
//      sample      15,401,413 executions in 47 min
//      ratio       0.43 Blueprint executions per event (47 min sample)
//
//  SAME ORDER OF MAGNITUDE as the funnel, roughly half of it. The first number
//  a measurement gives you isn't the number — waiting for steady state is part
//  of measuring.
//
//  Even so, that's 6,300 calls a second going through the callback. Here it
//  does the minimum: bumps a relaxed counter and passes the call on. If yours
//  does more, measure the effect on frame time before leaving it enabled.
//
//  ─────────────────────────────────────────────────────────────────────────
//  TABLE MODEL: nothing of ours enters this binary
//  ─────────────────────────────────────────────────────────────────────────
//  Only `ConanPluginApi.h` — declarations, no implementation. No library to
//  link. That matters HERE more than in any other example: this plugin's detour
//  sits on the game's hottest path, and under the old model it carried a copy
//  of our runtime inside it. Now what runs 6,300 times a second is only what's
//  in this file.
// ============================================================================
#include "Conan/ConanPluginApi.h"
#include <windows.h>
#include <atomic>

// The table arrives in ConanPluginCarregar and stays valid for the life of the
// process. We keep the pointer; it isn't copied, it belongs to the API.
static const ConanApiTabela* g_api = nullptr;

static std::atomic<uint64_t> g_execs{0};
using FnInterno = void (*)(void*, void*, void*);
static FnInterno g_original = nullptr;

// The detour has to be cheap: this is the hottest path in the whole game. No
// `g_api->` in here — an indirect call through the table 6,300 times a second
// is a cost that doesn't need to exist. The counter is local to the plugin.
extern "C" void MeuDetour(void* a, void* b, void* c)
{
    g_execs.fetch_add(1, std::memory_order_relaxed);
    // Without calling the original, the server stops executing Blueprint — in
    // other words, stops working. The header says so, and it means it.
    if (g_original) g_original(a, b, c);
}

// Runs on the game's thread, scheduled by the API. It's the only place that
// talks to the table in steady state.
static void Relatar(void*)
{
    static uint64_t antes = 0;
    const uint64_t agora = g_execs.load(std::memory_order_relaxed);
    uint64_t funil = 0, desp = 0;
    g_api->EstatisticaHooks(&funil, &desp);
    g_api->Log("[bp] execucoes de Blueprint: %llu (+%llu em 30s) | funil de eventos: %llu",
               (unsigned long long)agora, (unsigned long long)(agora - antes),
               (unsigned long long)funil);
    // The executions-per-event ratio is what compares against the funnel.
    if (funil > 0)
        g_api->Log("[bp]    %.2f execucoes de Blueprint por evento (regime medido: ~0,43)",
                   double(agora) / double(funil));
    antes = agora;
}

extern "C" __declspec(dllexport)
void ConanPluginCarregar(const ConanApiTabela* api)
{
    // Without this, a plugin compiled against a BIGGER table running on an
    // older API would read a pointer past the end of the struct and call junk.
    // The damage here would be worse than in any other plugin: what calls the
    // junk is the hot-path detour, and the server goes down on the first
    // Blueprint it executes.
    if (!api || api->tamanho < sizeof(ConanApiTabela)) return;
    g_api = api;

    g_api->Log("");
    g_api->Log("=== ExemploBlueprint ===");
    if (!g_api->Pronta()) { g_api->Log("  reflexao indisponivel"); return; }

    // ── PASS THE GLOBAL ITSELF, NOT A LOCAL VARIABLE ────────────────────────
    //
    // The previous version did this, and it's WRONG:
    //
    //     void* orig = nullptr;
    //     HookExecucaoDeBlueprint(&MeuDetour, &orig);   // patch already live
    //     g_original = orig;                            // only now does the
    //                                                   // detour see it
    //
    // The runtime fills the pointer in BEFORE writing the patch's 5 bytes (see
    // ConanHooks.cpp: `*original = tramp;` comes before EscreverAtomico5) — so
    // it does its part right. But what filled the GLOBAL the detour reads was
    // the plugin's next line, AFTER the function returned. Between the patch
    // and that line there's a window where MeuDetour runs with
    // g_original == nullptr, and the `if (g_original)` below — which is the
    // right instinct — drops the call SILENTLY. Every dropped Blueprint
    // execution is game code that simply didn't run.
    //
    // Passing the address of the global itself, the runtime writes into it
    // before the patch exists. The window has nowhere left to happen.
    //
    // THIS MATTERS MORE HERE THAN IN ANY OTHER EXAMPLE: this file is what a
    // developer copies when they go to hook something. The wrong pattern would
    // multiply across every plugin in the community.
    const ConanRecusa r = g_api->HookExecucaoDeBlueprint(
        reinterpret_cast<void*>(&MeuDetour),
        reinterpret_cast<void**>(&g_original));
    if (r != CONAN_OK)
    {
        g_api->Log("  ✗ nao hookei: %s", g_api->TextoRecusa(r));
        return;
    }
    void* const orig = reinterpret_cast<void*>(g_original);
    g_api->Log("  ✅ hook instalado em ProcessInternal. trampolim=%p", orig);
    g_api->AgendarNaThreadDoJogo(Relatar, 30, nullptr, 1);
    g_api->Log("  relatorio a cada 30 s. Se o numero SUBIR, o hook esta pegando");
    g_api->Log("  execucao de Blueprint — inclusive a que ProcessEvent nao ve.");
}
