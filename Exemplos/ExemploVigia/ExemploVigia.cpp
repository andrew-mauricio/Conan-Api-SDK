// ExemploVigia — boas-vindas, contagem de jogadores e comandos de chat.
// SDK e leu SO a documentacao que vem dentro dele.
//
// O que ele faz, e por que estas escolhas:
//   · da boas-vindas a quem entra          (evento K2_PostLogin, do EVENTOS.md)
//   · responde !online no chat             (ServerSendChatMessage, offset 0x068)
//   · responde !vigia so' para quem tem permissao  (Permission, degradando)
//
// Escrito seguindo o guia PARA-DESENVOLVEDORES.md linha a linha, para achar o
// que trava quem chega sem conhecer nada.

#include "Conan/ConanPluginApi.h"
#include "Conan/ConanPermission.h"

#include <cstdio>
#include <cstring>

static const ConanApiTabela* g_api = nullptr;

// Quantos entraram desde que o servidor subiu. Contador simples de proposito:
// o objetivo e' exercitar o caminho, nao ser esperto.
static int g_entraram = 0;

// ── quantos jogadores estao online agora ────────────────────────────────────
//
// A doc diz que FindObjects acha objetos vivos por classe. E' assim que se
// conta gente online sem guardar estado proprio (que desincroniza no primeiro
// crash de cliente).
static int ContarOnline()
{
    void* achados[128];
    const int n = g_api->FindObjects("ConanPlayerController", achados, 128, /*incluirFilhas=*/1);
    return n < 0 ? 0 : n;
}

// ── alguem falou no chat ────────────────────────────────────────────────────
extern "C" ConanAcao AoFalar(ConanChamada* c)
{
    char texto[512];
    // 0x068 = Message dentro de ChatRpcData. O guia avisa que offset cru e' a
    // unica coisa que a atualizacao do jogo pode mover — anotado.
    if (g_api->LerTextoDoJogo(c->Parms, 0x068, texto, sizeof(texto)) <= 0)
        return CONAN_CONTINUAR;

    if (texto[0] != '!')
        return CONAN_CONTINUAR;              // conversa normal segue o caminho

    // Para responder e' preciso o NOME de quem falou: MensagemParaJogador
    // endereca por nome, nao por controller. userName mora em 0x048.
    char quem[128];
    if (g_api->LerTextoDoJogo(c->Parms, 0x048, quem, sizeof(quem)) <= 0)
        return CONAN_CONTINUAR;

    if (std::strcmp(texto, "!online") == 0)
    {
        char msg[128];
        std::snprintf(msg, sizeof(msg), "Online agora: %d — entraram %d desde o boot.",
                      ContarOnline(), g_entraram);
        g_api->MensagemParaJogador(quem, msg);
        g_api->Log("[Vigia] !online respondido a %s (%d online)", quem, ContarOnline());
        return CONAN_CANCELAR;               // engole: e' comando, nao conversa
    }

    if (std::strcmp(texto, "!vigia") == 0)
    {
        // A doc insiste: consultar no USO, nunca no carregamento, e escolher o
        // valor de ausencia pelo custo do erro. Isto so' mostra estatistica,
        // entao negar por 30 s durante uma queda de MySQL nao machuca ninguem.
        char id[64];
        int libera = 0;
        if (ConanPermIdDoController(c->Obj, id, sizeof(id)) > 0)
            libera = (ConanPermTem(id, "vigia.ver", /*se_ausente=*/0) == 1);

        if (libera)
        {
            char msg[160];
            std::snprintf(msg, sizeof(msg), "Vigia: %d online, %d entraram, hooks ativos.",
                          ContarOnline(), g_entraram);
            g_api->MensagemParaJogador(quem, msg);
        }
        else
        {
            g_api->MensagemParaJogador(quem, "Vigia: voce nao tem permissao (vigia.ver).");
        }
        return CONAN_CANCELAR;
    }

    return CONAN_CONTINUAR;
}

// ── alguem entrou no servidor ───────────────────────────────────────────────
extern "C" ConanAcao AoEntrar(ConanChamada* c)
{
    ++g_entraram;

    // NewPlayer e' o primeiro parametro (o EVENTOS.md diz isso). Sem ele nao
    // ha para quem mandar a mensagem.
    void* controller = nullptr;
    if (g_api->LerParm(c, 0, &controller, sizeof(controller)) > 0 && controller)
    {
        // Aqui eu tenho o controller (e nao o nome), entao a rota e' a tela.
        g_api->MensagemNaTela(controller,
            "Bem-vindo! Digite !online para ver quem esta jogando.", 8.0f);
    }

    g_api->Log("[Vigia] jogador entrou (%d desde o boot, %d online)",
               g_entraram, ContarOnline());
    return CONAN_CONTINUAR;                  // nunca cancelar um login
}

extern "C" __declspec(dllexport)
void ConanPluginCarregar(const ConanApiTabela* api)
{
    // A rede que o guia manda ter: plugin compilado contra tabela maior, rodando
    // em API mais velha, leria ponteiro alem do fim da struct.
    if (!api || api->tamanho < sizeof(ConanApiTabela))
        return;

    g_api = api;
    g_api->Log("[Vigia] subiu — tabela v%d, %d bytes", (int)api->versao, (int)api->tamanho);

    // ATENCAO ao sentido: HookProcessEvent devolve o ID do hook, e ZERO e' a
    // falha. Escrevi `!= 0` de primeira, por reflexo de "0 = ok" do C, e o
    // aviso dispararia exatamente quando desse certo. O guia mostra a chamada
    // sem usar o retorno, entao quem decide conferir tem de adivinhar.
    if (g_api->HookProcessEvent("ServerSendChatMessage", AoFalar, nullptr, 100) == 0)
        g_api->Log("[Vigia] AVISO: nao consegui hookar o chat");

    if (g_api->HookProcessEvent("K2_PostLogin", AoEntrar, nullptr, 100) == 0)
        g_api->Log("[Vigia] AVISO: nao consegui hookar a entrada de jogador");

    g_api->Log("[Vigia] pronto: !online e !vigia");
}

extern "C" __declspec(dllexport)
void ConanPluginDescarregar(void)
{
    if (g_api)
        g_api->Log("[Vigia] saindo — %d entraram nesta sessao", g_entraram);
}
