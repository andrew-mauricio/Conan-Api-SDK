// ============================================================================
//  ExemploAgendado — prova que um plugin consegue rodar SOZINHO, de tempo em
//                    tempo, sem criar thread própria
//
//  Antes disto, um plugin só rodava uma vez, no carregamento. Criar thread e
//  chamar a API de lá é o caminho errado: a Unreal não é thread-safe, e o dano
//  aparece longe da causa.
//
//  A tarefa aqui roda de dentro do desvio de ProcessEvent — na mesma thread que
//  a engine usa para tudo.
//
//  A PROVA: cada disparo chama uma função do jogo e confere a resposta. Se a
//  tarefa estivesse rodando na thread errada, ou o agendador estivesse
//  disparando fora de um contexto válido, a chamada não devolveria 7.
//
//  Modelo de tabela: este arquivo não inclui nada nosso além do header de
//  declarações, e não linka biblioteca nossa nenhuma. Tudo passa por `g_api->`.
// ============================================================================
#include "Conan/ConanPluginApi.h"
#include <windows.h>

// O ponteiro da tabela é guardado em estático porque a tarefa agendada roda
// muito depois do Carregar, sem receber a tabela de volta — o contexto que o
// agendador devolve é NOSSO, e aqui não precisamos dele.
static const ConanApiTabela* g_api = nullptr;

static int   g_disparos = 0;
static DWORD g_t0 = 0;

static void Tique(void* /*contexto*/)
{
    if (!g_api) return;   // não deve acontecer; a tarefa só é agendada com a tabela em mãos

    ++g_disparos;
    const DWORD dt = GetTickCount() - g_t0;

    // Chamar função do jogo DE DENTRO da tarefa: é isto que prova a thread.
    //
    // No modelo de tabela não existe mais `obj->Call<int32_t>(...)`: aquele
    // template era código NOSSO compilado dentro do plugin. Agora os argumentos
    // vão como vetor de ponteiros + vetor de tamanhos, e a API confere cada
    // tamanho contra o parâmetro real antes de montar o bloco — passar 4 bytes
    // onde o jogo espera 8 (ou o contrário) é recusado em vez de escrever lixo.
    void* mat = g_api->GetDefaultObject("KismetMathLibrary");
    int32_t r = -1;
    if (mat)
    {
        const int32_t a = 10, b = 3;
        const void*    args[2] = { &a, &b };
        const uint32_t tams[2] = { (uint32_t)sizeof(a), (uint32_t)sizeof(b) };
        int32_t ret = 0;
        // Devolve 1 se executou. Se recusar, `r` fica -1 e o log acusa ERRADO —
        // silenciar a falha aqui destruiria justamente a prova.
        if (g_api->ChamarFuncao(mat, "Subtract_IntInt", args, tams, 2,
                                &ret, (uint32_t)sizeof(ret)))
            r = ret;
    }

    uint64_t total = 0, desp = 0;
    g_api->EstatisticaHooks(&total, &desp);

    g_api->Log("[agendado] disparo %d aos %lu.%03lus · Subtract_IntInt(10,3)=%d %s "
               "· funil ja passou %llu chamadas",
               g_disparos, dt / 1000, dt % 1000, r,
               r == 7 ? "<<< CORRETO: rodei na thread do jogo" : "<<< ERRADO",
               (unsigned long long)total);

    if (g_disparos >= 4)
        g_api->Log("[agendado] 4 disparos com resposta certa: o agendador funciona.");
}

extern "C" __declspec(dllexport)
void ConanPluginCarregar(const ConanApiTabela* api)
{
    // A tabela pode ser MENOR que a que este plugin conhece se a API for mais
    // velha que o binário. Ler campo além do fim dela devolveria lixo e nós
    // chamaríamos um endereço qualquer — por isso a conferência vem antes de
    // qualquer uso, inclusive antes do primeiro Log.
    if (!api || api->tamanho < sizeof(ConanApiTabela)) return;
    g_api = api;

    g_t0 = GetTickCount();
    g_api->Log("");
    g_api->Log("=== ExemploAgendado ===");
    if (!g_api->Pronta()) { g_api->Log("  reflexao indisponivel"); return; }

    const uint32_t id = g_api->AgendarNaThreadDoJogo(Tique, 10, nullptr, 1);
    g_api->Log("  tarefa a cada 10 s -> id %u %s", id, id ? "" : "(NAO agendou)");
}
