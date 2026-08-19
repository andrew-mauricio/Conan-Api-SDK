// ============================================================================
//  ExemploComando — intercepta comando de chat, o que 90% dos plugins de
//                   servidor precisam fazer
//
//  Copie esta pasta para escrever o seu `/kit`, `/vip`, `/tp`.
//
//  Este plugin não inclui nada nosso além de `ConanPluginApi.h` — só
//  declarações. Tudo o que ele faz passa pela tabela `g_api->`, e nenhum
//  código nosso é compilado ou linkado aqui dentro.
//
//  ────────────────────────────────────────────────────────────────────────────
//  COMO O CHAT CHEGA AO SERVIDOR
//  ────────────────────────────────────────────────────────────────────────────
//  O cliente manda uma RPC: `ConanPlayerController::ServerSendChatMessage`. O
//  prefixo `Server` é a convenção da Unreal para "isto roda no servidor a pedido
//  do cliente" — é exatamente o ponto onde um comando deve ser tratado.
//
//  O único parâmetro é um struct `ChatRpcData` de 128 bytes, cujo layout veio da
//  reflexão (não de suposição):
//
//      +0x000  uint64   Timestamp
//      +0x008  struct   UserId  (UniqueNetIdRepl)
//      +0x038  int64    CharacterUniqueID     <- o "uid" que aparece no log
//      +0x040  int64    targetUniqueId
//      +0x048  FString  userName
//      +0x058  FString  Channel
//      +0x068  FString  Message               <- o texto digitado
//      +0x078  bool     generated
//
//  CANCELAR O HOOK ENGOLE A MENSAGEM. É isso que faz um comando ser um comando:
//  o jogador digita `!kit`, o plugin age, e o texto não aparece no chat de todo
//  mundo. Devolver `CONAN_CONTINUAR` deixa a mensagem passar normalmente.
//
//  ────────────────────────────────────────────────────────────────────────────
//  POR QUE ESTE PLUGIN NÃO RESPONDE AO JOGADOR (ainda)
//  ────────────────────────────────────────────────────────────────────────────
//  `ConanCheatManager::BroadcastMessage(FString)` existe e manda texto. Mas
//  chamá-la exige CONSTRUIR uma FString — e FString é um ponteiro para memória
//  que o jogo gerencia. Passar um buffer nosso significa que o jogo pode tentar
//  liberar memória que não é dele, ou guardar um ponteiro que vai morrer quando
//  a nossa função retornar.
//
//  Isso é resolvível, e não com um palpite: precisa saber como o jogo aloca
//  FString (FMemory::Malloc pelo alocador dele, não pelo nosso `new`). Até estar
//  medido, este exemplo REGISTRA no log em vez de responder no chat — porque um
//  plugin que corrompe o heap do servidor é pior que um plugin que não fala.
//  É a mesma razão pela qual a tabela só oferece `LerTextoDoJogo`: ler texto do
//  jogo é seguro, devolver texto para o jogo ainda não é.
// ============================================================================
#include "Conan/ConanPluginApi.h"
#include <windows.h>
#include <cstring>
#include <cstdint>

// A tabela que o carregador nos entrega. Tudo passa por aqui; não existe
// nenhuma implementação nossa dentro deste binário.
static const ConanApiTabela* g_api = nullptr;

// offsets dentro de ChatRpcData, medidos na reflexão
static const uint32_t CHAT_UID     = 0x038;
static const uint32_t CHAT_USUARIO = 0x048;
static const uint32_t CHAT_CANAL   = 0x058;
static const uint32_t CHAT_TEXTO   = 0x068;

static int g_comandos = 0;
static int g_mensagens = 0;

// ── ler FString do bloco de parâmetros ──────────────────────────────────────
//
// Antes este plugin decodificava a FString na mão. FString é
// {void* Data; int32 Num; int32 Max} e no Windows o caractere é UTF-16, e `Num`
// INCLUI o terminador — errar isso corta o último caractere ou lê um lixo a
// mais. Esse detalhe agora é problema da API: `LerTextoDoJogo(base, offset, ...)`
// confere se a memória está legível antes de tocar nela e devolve char*. Um
// erro de layout se conserta num lugar só, sem recompilar plugin nenhum.
static bool LerTexto(const void* base, uint32_t off, char* saida, int tam)
{
    saida[0] = 0;
    return g_api->LerTextoDoJogo(base, off, saida, tam) != 0;
}

// O callback é `extern "C"` e recebe `ConanChamada*`: a tabela é C puro, e é
// isso que faz este plugin compilar em qualquer compilador.
extern "C" ConanAcao AoFalar(ConanChamada* c)
{
    // O parâmetro 0 é o struct ChatRpcData, por VALOR — então ele mora no início
    // do bloco de parâmetros, e não é um ponteiro a seguir.
    if (!c || !c->Parms || c->ParmsSize < 0x80) return CONAN_CONTINUAR;
    const void* chat = c->Parms;

    char texto[512] = {0}, usuario[128] = {0}, canal[64] = {0};
    LerTexto(chat, CHAT_TEXTO,   texto,   sizeof(texto));
    LerTexto(chat, CHAT_USUARIO, usuario, sizeof(usuario));
    LerTexto(chat, CHAT_CANAL,   canal,   sizeof(canal));

    // O uid é um int64 cru, não texto: aqui ainda vale perguntar à API se a
    // memória está legível antes de ler. Objeto do jogo pode estar na janela do
    // coletor de lixo, e ler ponteiro morto derruba o servidor inteiro.
    int64_t uid = 0;
    const uint8_t* pUid = static_cast<const uint8_t*>(chat) + CHAT_UID;
    if (g_api->Legivel(pUid, 8))
        uid = *reinterpret_cast<const int64_t*>(pUid);

    ++g_mensagens;
    g_api->Log("[chat] %s (uid %lld) no canal \"%s\": \"%s\"",
               usuario[0] ? usuario : "?", (long long)uid,
               canal[0] ? canal : "?", texto);

    // ── é comando? ──────────────────────────────────────────────────────────
    //
    // POR QUE `!` E NÃO `/` — e como isso foi descoberto
    // --------------------------------------------------
    // A primeira versão usava `/`. No teste real, `/apiteste` DESAPARECEU do chat
    // do jogador — o que parecia sucesso — mas NÃO chegou a este hook: o log
    // registra a mensagem antes de decidir cancelar, e a linha não saiu.
    //
    // Ou seja: o CLIENTE filtrou o comando desconhecido e nunca o enviou ao
    // servidor. O texto sumiu antes de sair da máquina do jogador.
    //
    // Isso é o pior tipo de resultado ambíguo: o sintoma visível ("sumiu do
    // chat") era o mesmo nos dois casos, e sem o log do servidor não havia como
    // distinguir "meu hook cancelou" de "nunca chegou". Foi o log que decidiu.
    //
    // Por isso o prefixo é `!`. O cliente trata `!alguma-coisa` como texto comum
    // e envia; `/alguma-coisa` ele tenta resolver localmente primeiro.
    const char PREFIXO = '!';
    if (texto[0] != PREFIXO) return CONAN_CONTINUAR;   // conversa normal: passa

    ++g_comandos;

    if (std::strncmp(texto, "!apiteste", 9) == 0)
    {
        // Aqui entraria a ação do comando. Como demonstração, só registra.
        g_api->Log("[chat] >>> COMANDO !apiteste de %s. Este texto NAO vai aparecer "
                   "no chat: o hook cancelou a mensagem, que e' o que faz um comando "
                   "ser um comando.", usuario);
        g_api->Log("[chat]     comandos ate agora: %d de %d mensagens",
                   g_comandos, g_mensagens);
        return CONAN_CANCELAR;                        // engole a mensagem
    }

    // Comando desconhecido: deixa passar. Engolir todo `!` faria o plugin
    // sequestrar o que outros plugins tratam — e o jogador ficaria sem entender
    // por que o jogo parou de responder.
    g_api->Log("[chat]     \"%s\" nao e' meu comando; deixei passar", texto);
    return CONAN_CONTINUAR;
}

extern "C" __declspec(dllexport)
void ConanPluginCarregar(const ConanApiTabela* api)
{
    // Confira a tabela ANTES de usar qualquer campo. Um plugin compilado contra
    // uma tabela MAIOR, rodando numa API mais velha, leria ponteiro além do fim
    // da struct e chamaria lixo. Sair calado é melhor que derrubar o servidor —
    // e nem dá para avisar por Log, porque Log é campo da tabela suspeita.
    if (!api || api->tamanho < sizeof(ConanApiTabela)) return;
    g_api = api;

    g_api->Log("");
    g_api->Log("=== ExemploComando ===");
    if (!g_api->Pronta()) { g_api->Log("  reflexao indisponivel"); return; }

    // Assinatura nova: (nome, antes, depois, prioridade). Sem callback de
    // "depois" — a decisão de engolir a mensagem é tomada ANTES de o jogo
    // executar, senão o texto já saiu para todo mundo. Prioridade 100 é o meio
    // da tabela: quem precisar decidir antes deste pode pedir número menor.
    const uint32_t id = g_api->HookProcessEvent("ServerSendChatMessage",
                                                AoFalar, nullptr, 100);
    if (id)
    {
        g_api->Log("  hook %u em ConanPlayerController::ServerSendChatMessage", id);
        g_api->Log("  fale no chat para ver aparecer aqui.");
        g_api->Log("  digite !apiteste para ver um comando ser ENGOLIDO (nao aparece no chat).");
        g_api->Log("  o prefixo e' ! e nao /: o cliente do jogo filtra / desconhecido e nao envia.");
    }
    else
        g_api->Log("  x nao registrei o hook — ver o motivo acima");
}
