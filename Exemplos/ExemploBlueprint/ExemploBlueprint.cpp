// ============================================================================
//  ExemploBlueprint — intercepta TODA execução de função de Blueprint
//
//  Fecha o buraco que o hook de ProcessEvent deixava: chamada
//  Blueprint→Blueprint dentro do mesmo grafo vai por `UObject::CallFunction` e
//  escapa do funil de eventos.
//
//  A saída não foi achar `CallFunction` — foi achar onde os dois caminhos se
//  ENCONTRAM. `UObject::ProcessInternal` é o interpretador de bytecode, e toda
//  função de Blueprint passa por ele, venha de onde vier.
//
//  O CUSTO, MEDIDO — e nem o palpite nem a primeira correção acertaram
//
//  Escrevi que dispararia "muito mais que um hook de evento": palpite. Medi os
//  primeiros 30 s e "corrigi" para o contrário (4 contra 1.537): também errado,
//  porque o servidor ainda estava subindo. A série real:
//
//      boot        4 · 46 · 10.787 por 30 s      <- povoando o mundo
//      regime      ~190.000 por 30 s             <- ~6.300 execuções/s
//      amostra     15.401.413 execuções em 47 min
//      proporção   0,43 execução de Blueprint por evento (47 min de amostra)
//
//  MESMA ORDEM DE GRANDEZA que o funil, cerca de metade. O primeiro número que
//  sai de uma medição não é o número — esperar o regime é parte de medir.
//
//  Mesmo assim são 6.300 chamadas por segundo passando pelo callback. Aqui ele
//  faz o mínimo: incrementa um contador relaxed e repassa. Se o seu fizer mais
//  que isso, meça o efeito no frame time antes de deixar ligado.
//
//  ─────────────────────────────────────────────────────────────────────────
//  MODELO DE TABELA: nada nosso entra neste binário
//  ─────────────────────────────────────────────────────────────────────────
//  Só `ConanPluginApi.h` — declarações, sem implementação. Nenhuma biblioteca
//  para linkar. Isso importa AQUI mais do que em qualquer outro exemplo: o
//  detour deste plugin fica no caminho mais quente do jogo, e no modelo antigo
//  ele carregava uma cópia do nosso motor dentro dele. Agora o que roda 6.300
//  vezes por segundo é só o que está neste arquivo.
// ============================================================================
#include "Conan/ConanPluginApi.h"
#include <windows.h>
#include <atomic>

// A tabela chega em ConanPluginCarregar e vale por toda a vida do processo.
// Guardamos o ponteiro; ele não é copiado, é da API.
static const ConanApiTabela* g_api = nullptr;

static std::atomic<uint64_t> g_execs{0};
using FnInterno = void (*)(void*, void*, void*);
static FnInterno g_original = nullptr;

// O detour tem de ser barato: este é o caminho mais quente de todo o jogo.
// Nada de `g_api->` aqui dentro — chamada indireta pela tabela 6.300 vezes por
// segundo é custo que não precisa existir. O contador é local ao plugin.
extern "C" void MeuDetour(void* a, void* b, void* c)
{
    g_execs.fetch_add(1, std::memory_order_relaxed);
    // Sem chamar o original, o servidor para de executar Blueprint — ou seja,
    // para de funcionar. O header diz isso, e é literal.
    if (g_original) g_original(a, b, c);
}

// Roda na thread do jogo, agendada pela API. É o único lugar que fala com a
// tabela em regime.
static void Relatar(void*)
{
    static uint64_t antes = 0;
    const uint64_t agora = g_execs.load(std::memory_order_relaxed);
    uint64_t funil = 0, desp = 0;
    g_api->EstatisticaHooks(&funil, &desp);
    g_api->Log("[bp] execucoes de Blueprint: %llu (+%llu em 30s) | funil de eventos: %llu",
               (unsigned long long)agora, (unsigned long long)(agora - antes),
               (unsigned long long)funil);
    // A proporção em execuções-por-evento é a que compara com o funil.
    if (funil > 0)
        g_api->Log("[bp]    %.2f execucoes de Blueprint por evento (regime medido: ~0,43)",
                   double(agora) / double(funil));
    antes = agora;
}

extern "C" __declspec(dllexport)
void ConanPluginCarregar(const ConanApiTabela* api)
{
    // Sem isto, um plugin compilado contra uma tabela MAIOR rodando numa API
    // mais velha leria ponteiro além do fim da struct e chamaria lixo. Aqui o
    // dano seria pior que em outro plugin: quem chama lixo é o detour do
    // caminho quente, e o servidor cai no primeiro Blueprint que executar.
    if (!api || api->tamanho < sizeof(ConanApiTabela)) return;
    g_api = api;

    g_api->Log("");
    g_api->Log("=== ExemploBlueprint ===");
    if (!g_api->Pronta()) { g_api->Log("  reflexao indisponivel"); return; }

    // ── PASSAR A PRÓPRIA GLOBAL, E NÃO UMA VARIÁVEL LOCAL ───────────────────
    //
    // A versão anterior fazia assim, e está ERRADO:
    //
    //     void* orig = nullptr;
    //     HookExecucaoDeBlueprint(&MeuDetour, &orig);   // patch já ativo aqui
    //     g_original = orig;                            // só agora o detour vê
    //
    // O motor preenche o ponteiro ANTES de escrever os 5 bytes do patch (ver
    // ConanHooks.cpp: `*original = tramp;` vem antes de EscreverAtomico5) — ou
    // seja, ele faz a parte dele certo. Mas quem preenchia a GLOBAL que o detour
    // lê era a linha seguinte do plugin, DEPOIS de a função voltar. Entre o
    // patch e essa linha existe uma janela em que MeuDetour roda com
    // g_original == nullptr, e o `if (g_original)` abaixo — que é a atitude
    // certa — descarta a chamada em SILÊNCIO. Cada execução de Blueprint
    // descartada é código do jogo que simplesmente não rodou.
    //
    // Passando o endereço da própria global, o motor escreve nela antes do
    // patch existir. A janela deixa de ter onde acontecer.
    //
    // ISTO IMPORTA MAIS AQUI DO QUE EM QUALQUER OUTRO EXEMPLO: este arquivo é o
    // que o dev copia quando vai hookar alguma coisa. O padrão errado se
    // multiplicaria por toda plugin da comunidade.
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
