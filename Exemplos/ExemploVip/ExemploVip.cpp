// ============================================================================
//  ExemploVip — o menor plugin de terceiro que usa o Permission
//
//  É ESTE o arquivo que a comunidade vai copiar. Ele mostra as cinco coisas
//  que todo plugin que depende do Permission precisa fazer certo:
//
//    1. NÃO LINKAR contra o Permission. Só incluir o header.
//       Confira depois de compilar:
//           x86_64-w64-mingw32-objdump -p ExemploVip.dll | grep "DLL Name"
//       ConanPermission.dll NÃO pode aparecer ali. Se aparecer, o seu plugin
//       deixa de carregar em todo servidor que não instalou o Permission.
//
//    2. DECIDIR o que fazer quando o Permission não está instalado. É o
//       terceiro argumento, e ele é obrigatório de propósito.
//
//    3. CONSULTAR NA HORA DO USO, NUNCA NO CARREGAMENTO. Esta é a mudança que
//       o teste real cobrou, e ela tem seção própria logo abaixo.
//
//    4. NÃO consultar por tick sem necessidade. A consulta custa ~194 ns
//       (medido), mas traduzir o objeto do jogo em id custa ~12 µs na primeira
//       vez de cada jogador — resolva quando o jogador aparece, guarde o id.
//
//    5. Tratar -1 como -1. "Não sei" não é "não".
//
//  ────────────────────────────────────────────────────────────────────────────
//  POR QUE A CONSULTA SAIU DO CARREGAMENTO (item 3)
//  ────────────────────────────────────────────────────────────────────────────
//  A versão anterior deste arquivo perguntava ao Permission dentro do próprio
//  ConanPluginCarregar(). Parecia inofensivo e não era: o ConanLoader carrega
//  as DLLs na ordem que o FindFirstFile devolve, que NÃO é especificada. Se o
//  ExemploVip.dll subir antes do ConanPermission.dll — e "Exemplo" vem antes de
//  "Permission" em ordem alfabética, então isso é o caso PROVÁVEL, não o raro —
//  a consulta acontece com o Permission ainda inexistente no processo.
//
//  O resultado não era um erro: era o log dizendo "Permission NAO esta
//  instalado" num servidor onde ele estava instalado e funcionando. Defeito
//  silencioso, do tipo que o dono do servidor investiga por horas.
//
//  E não havia jogador nenhum no mundo naquele instante, então a varredura de
//  PlayerControllers também não tinha o que encontrar: o plugin carregava junto
//  com o servidor, muito antes de alguém conectar.
//
//  A correção não é "tentar de novo mais tarde no carregamento". É reconhecer
//  que a pergunta só faz sentido quando alguém PEDE o benefício. Por isso este
//  plugin agora engancha o chat e só consulta quando o jogador digita `!vip`.
//  Nesse instante três coisas são verdade de graça, sem nenhuma sincronização:
//  o servidor já subiu inteiro, o Permission já está no processo, e o
//  PlayerController que pediu está na mão — é o `c->Obj` do próprio hook.
//
//  Repare no que sumiu junto: a varredura de objetos do mundo. Pegar o
//  controller de quem falou é mais barato, mais simples e não pode devolver o
//  jogador errado.
//
//  ────────────────────────────────────────────────────────────────────────────
//  O QUE ESTE PLUGIN LEVA DENTRO DELE: NADA NOSSO
//  ────────────────────────────────────────────────────────────────────────────
//  Um único header de declarações (ConanPluginApi.h) e uma tabela de ponteiros
//  de função recebida no carregamento. Não há .a para linkar, não há .cpp nosso
//  para compilar junto, e o motor de hooks não entra neste binário. O mesmo
//  princípio do Permission, aplicado à API — e pela mesma razão: biblioteca C++
//  não atravessa fronteira de compilador sem corromper memória em silêncio.
// ============================================================================
#include "Conan/ConanPluginApi.h"      // a tabela; é só isto que é nosso aqui
#include "Conan/ConanPermission.h"     // só o header; nada para linkar

#include <windows.h>
#include <cstring>

// A tabela que o carregador entrega. Tudo o que este plugin sabe fazer com o
// jogo passa por aqui.
static const ConanApiTabela* g_api = nullptr;

// O id do nosso hook de chat, para devolvê-lo no descarregamento.
static uint32_t g_hookChat = 0;

// ── offsets dentro de ChatRpcData ───────────────────────────────────────────
//
// Vieram do catálogo da reflexão, não de suposição. O struct tem 128 bytes e
// chega POR VALOR, então ele começa no início do bloco de parâmetros.
static const uint32_t CHAT_UID     = 0x038;   // int64  CharacterUniqueID
static const uint32_t CHAT_USUARIO = 0x048;   // FString userName
static const uint32_t CHAT_TEXTO   = 0x068;   // FString Message

// ── o benefício, no momento em que ele é pedido ─────────────────────────────
//
// `controller` é o ConanPlayerController de quem digitou — o dono da chamada
// interceptada. É exatamente o que o Permission espera em id_do_controller().
static void AtenderPedidoDeVip(void* controller, const char* usuario)
{
    // A degradação, que é o ponto do exemplo. NÃO se aborta quando o Permission
    // está ausente: o plugin continua carregado, continua respondendo, e apenas
    // não concede nada. Um plugin que se recusa a existir sem o Permission
    // obriga todo mundo a instalá-lo — e a promessa era a oposta.
    //
    // Esta pergunta acontece AQUI, e não no carregamento, de propósito: agora
    // "ausente" quer mesmo dizer ausente. Lá em cima queria dizer "ainda não
    // subiu", que é uma resposta completamente diferente com a mesma cara.
    if (!ConanPermDisponivel())
    {
        g_api->Log("[vip] %s pediu VIP, mas o ConanPermission.dll nao esta "
                   "instalado. Nao concedo nada. Instale-o em "
                   "Conan-Api/Plugins/ para ligar os beneficios.", usuario);
        return;
    }

    char id[CONAN_PERM_MAX_ID];
    const int32_t len = ConanPermIdDoController(controller, id, sizeof(id));
    if (len <= 0)
    {
        // 0 = o jogador ainda não tem identidade (está entrando).
        // -1 = erro, ou o Permission saiu do ar entre a linha acima e esta.
        // Nos DOIS casos a resposta certa é não conceder e não negar: dizer
        // que o jogador não é VIP seria o defeito silencioso — quem pagou não
        // receberia o kit e ninguém veria erro nenhum.
        g_api->Log("[vip] %s: sem id ainda (%d) — pode pedir de novo em "
                   "alguns segundos", usuario, (int)len);
        return;
    }

    // se_ausente = 0: sem Permission instalado, ninguém é VIP. É a escolha
    // conservadora e é UMA ESCOLHA — um plugin de "área restrita" poderia
    // querer 1 aqui (sem controle de acesso, libera), e o header obriga a
    // dizer qual dos dois você quer.
    const int32_t vip   = ConanPermTem(id, "vip.kit.diario", /*se_ausente=*/0);
    const int32_t tele  = ConanPermTem(id, "vip.teleporte",  /*se_ausente=*/0);
    const int64_t vence = ConanPermExpiraEm(id, "vip");

    char grupos[256];
    ConanPermGrupos(id, grupos, sizeof(grupos));
    for (char* p = grupos; *p; ++p) if (*p == '\n') *p = ' ';   // uma linha no log

    g_api->Log("[vip] id=%s  kit.diario=%d  teleporte=%d  vence=%lld  grupos=[%s]",
               id, (int)vip, (int)tele, (long long)vence, grupos);

    // Um plugin real daria o kit aqui. Este só registra, porque o objetivo é
    // provar o caminho — e porque MANDAR TEXTO DE VOLTA AO JOGO DERRUBA O
    // SERVIDOR nesta build: construir uma FString nossa e entregá-la ao jogo
    // faz o ProcessEvent destruir o bloco de parâmetros com o alocador DELE
    // sobre memória NOSSA. Está medido, o processo morre na chamada. Enquanto
    // não houver ferramenta na tabela para isso, a resposta ao jogador é o log
    // do dono do servidor.
    if (vip == 1)
        g_api->Log("[vip] -> %s TEM direito ao kit diario. (aqui entraria a "
                   "entrega do kit)", usuario);
    else
        g_api->Log("[vip] -> %s nao tem vip.kit.diario", usuario);
}

// ── o hook de chat ──────────────────────────────────────────────────────────
//
// extern "C" e recebendo ConanChamada*: a assinatura é ABI de C pura, que é o
// que permite o motor estar num binário e este callback em outro.
extern "C" ConanAcao AoFalar(ConanChamada* c)
{
    if (!c || !c->Parms || c->ParmsSize < 0x80) return CONAN_CONTINUAR;

    char texto[512];
    texto[0] = 0;
    // A API lê a FString do jogo para char* por nós — inclusive conferindo se a
    // memória está legível antes de tocar nela. Fazer isso à mão foi durante um
    // tempo o jeito, e ler um ponteiro morto dentro da janela do coletor de
    // lixo derruba o servidor inteiro.
    if (!g_api->LerTextoDoJogo(c->Parms, CHAT_TEXTO, texto, sizeof(texto)))
        return CONAN_CONTINUAR;

    // ── O PREFIXO É `!`, NÃO `/` ────────────────────────────────────────────
    //
    // Medido no servidor real: `"oi"`, `"!apiteste"` e `"teste/barra"` chegam
    // ao hook; `"/apiteste"` NUNCA chega. O cliente do Conan tenta resolver
    // `/comando` localmente e não envia nada ao servidor — nenhum plugin
    // consegue ver. O sintoma na tela é idêntico ao de um hook que cancelou
    // ("sumiu do chat"), e só o log do servidor separa os dois casos.
    if (texto[0] != '!') return CONAN_CONTINUAR;

    // `!vip` exato, não `!vipqualquercoisa`: engolir o que não é nosso faria
    // este plugin sequestrar o comando de outro plugin, e o jogador ficaria
    // sem entender por que o servidor parou de responder.
    if (std::strncmp(texto, "!vip", 4) != 0) return CONAN_CONTINUAR;
    if (texto[4] != 0 && texto[4] != ' ')   return CONAN_CONTINUAR;

    char usuario[128];
    usuario[0] = 0;
    if (!g_api->LerTextoDoJogo(c->Parms, CHAT_USUARIO, usuario, sizeof(usuario)))
        std::strcpy(usuario, "?");
    if (!usuario[0]) std::strcpy(usuario, "?");

    int64_t uid = 0;
    g_api->LerMembro(c->Parms, CHAT_UID, &uid, sizeof(uid));

    g_api->Log("[vip] !vip pedido por %s (uid %lld)", usuario, (long long)uid);

    // `c->Obj` é o ConanPlayerController que recebeu a RPC — quem falou. Não é
    // preciso procurá-lo no mundo, e não há como pegar o jogador errado.
    AtenderPedidoDeVip(c->Obj, usuario);

    // Cancelar engole a mensagem: é isso que faz um comando ser um comando. O
    // `!vip` não aparece no chat de todo mundo.
    return CONAN_CANCELAR;
}

extern "C" __declspec(dllexport)
void ConanPluginCarregar(const ConanApiTabela* api)
{
    // Sem isto, um plugin compilado contra uma tabela MAIOR rodando numa API
    // mais velha leria ponteiro além do fim da struct e chamaria lixo. Não dá
    // erro: dá um salto para um endereço qualquer, na thread do jogo.
    if (!api || api->tamanho < sizeof(ConanApiTabela)) return;
    g_api = api;

    g_api->Log("");
    g_api->Log("=== ExemploVip — plugin de terceiro usando o Permission ===");

    if (!g_api->Pronta()) { g_api->Log("[vip] reflexao indisponivel; abortado"); return; }

    // Repare no que NÃO acontece aqui: nenhuma pergunta ao Permission. Neste
    // instante ele pode nem estar carregado ainda, e perguntar agora só
    // produziria uma resposta errada com cara de certa. Ver a seção "POR QUE A
    // CONSULTA SAIU DO CARREGAMENTO" no topo do arquivo.
    g_hookChat = g_api->HookProcessEvent("ServerSendChatMessage", AoFalar, nullptr, 100);
    if (!g_hookChat)
    {
        // 0 = falhou, e o motivo já foi para o log pela própria API.
        g_api->Log("[vip] nao consegui enganchar o chat; o comando !vip nao vai "
                   "funcionar");
        return;
    }

    g_api->Log("[vip] pronto. Digite !vip no chat do jogo para pedir o beneficio.");
    g_api->Log("=== fim ===");
}

extern "C" __declspec(dllexport)
void ConanPluginDescarregar(void)
{
    // Devolver o hook. A API só deixa remover hook NOSSO — ela guarda qual
    // módulo registrou cada um.
    //
    // Honestidade sobre o estado de hoje: o ConanLoader NÃO chama esta função
    // (ele não descarrega plugin nenhum; o processo do servidor morre inteiro).
    // Ela fica aqui porque o contrato no ConanPluginApi.h a prevê, custa nada, e
    // no dia em que o carregador passar a descarregar, este plugin já está
    // certo. O que ela NÃO é: garantia de limpeza. Não guarde nada aqui que
    // precise mesmo acontecer.
    if (g_api && g_hookChat) { g_api->RemoverHook(g_hookChat); g_hookChat = 0; }
}
