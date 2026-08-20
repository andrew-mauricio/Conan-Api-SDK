// ConanShop — a points shop for Conan Exiles, in the spirit of ArkShop.
//
// WHAT IT DOES
// ------------
//   · credits points to whoever is online, over time, with a per-group (VIP) rate
//   · !shop        shows the catalogue, paginated, on the game's screen
//   · !comprar id  debits and delivers the item into the inventory
//   · !pontos      shows the balance
//   · !shopreload  re-reads config.json (only for holders of shop.admin)
//
// WHAT WAS DONE DIFFERENTLY FROM ArkShop, AND WHY
// ------------------------------------------------
// 1. THE DEBIT IS ATOMIC. ArkShop does `if (points >= price && Spend(price))`
//    and the SQL underneath is `UPDATE ... SET Points = Points - ?` with no
//    condition. Another purchase by the same player fits between the read and
//    the spend. Here the condition goes inside the UPDATE and the database
//    decides (see Pontos.h).
//
// 2. THE SALE KEY IS THE TemplateId, NOT THE BLUEPRINT. In ARK, "one blueprint
//    = one item" holds. In Conan it doesn't: Stone (10001) and hundreds of other
//    simple items share the native class /Script/ConanSandbox.GameItem. A shop
//    modelled the ARK way would deliver the wrong item — and do it successfully,
//    with no error in the log.
//
// 3. A BROKEN CONFIG DOESN'T REPLACE A WORKING ONE. See Config.h.
//
// PROVEN WITH A REAL PLAYER — 2026-08-20
// ---------------------------------------
// Delivery (SpawnTemplateItem) and the on-screen list (ClientShowMessageBox)
// were verified in game, not just against the signatures reflection declares:
// !pontos answered, !shop drew the message box, and !comprar stone delivered
// 100 stone into the player's inventory while debiting 10 points.
//
// What that same session caught, and no automated test would have: the first
// version answered NOTHING. The chat hook fired, read the command and cancelled
// the message correctly, and the reply died on the way back without a single
// line in the log. See Falar() below for the cause and the fix.
#include "Conan/ConanPluginApi.h"
#include "Conan/ConanBase.h"
#include "Conan/ConanPermission.h"

#include "Config.h"
#include "Pontos.h"
#include "Comandos.h"
#include "Texto.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

static const ConanApiTabela* g_api = nullptr;
static Shop::Config          g_cfg;
static std::string           g_caminhoConfig;

// ── read a member pointer, BY NAME ──────────────────────────────────────────
//
// The same pattern as ExemploJogador, for the same reason: hardcoding 0x308
// works today and reads the neighbouring field after Funcom's next patch, with
// no error and no log entry, just wrong data.
static void* MembroPonteiro(void* obj, const char* nome)
{
    if (!obj) return nullptr;
    const int32_t off = g_api->OffsetDoMembro(obj, nome);
    if (off < 0) return nullptr;
    void* p = nullptr;
    if (g_api->LerMembro(obj, uint32_t(off), &p, sizeof(p)) <= 0) return nullptr;
    if (!p || !g_api->Legivel(p, 8)) return nullptr;
    return p;
}

// ── who is this player ──────────────────────────────────────────────────────
//
// Identity comes from Permission, which already answered that question and
// proved it with a real player (MasterAccountId). Resolving it again here would
// be a second truth about the same thing, and the two would diverge the day the
// key changed.
struct Jogador
{
    std::string id;       // the wallet's key
    std::string nome;     // for talking to them in chat
    void*       controller = nullptr;
};

static bool Identificar(void* controller, Jogador& j)
{
    if (!controller) return false;
    j.controller = controller;

    if (void* ps = MembroPonteiro(controller, "PlayerState"))
    {
        const int32_t off = g_api->OffsetDoMembro(ps, "PlayerNamePrivate");
        if (off >= 0)
        {
            char n[128] = {0};
            if (g_api->LerTextoDoJogo(ps, uint32_t(off), n, sizeof(n)) > 0) j.nome = n;
        }
    }

    const ConanPermApi* perm = ConanPermObter();
    if (perm && perm->id_do_controller)
    {
        char id[CONAN_PERM_MAX_ID] = {0};
        if (perm->id_do_controller(controller, id, sizeof(id)) > 0 && id[0])
        {
            j.id = id;
            // PARTIAL identity: the id resolved and the name didn't. That was
            // enough for the shop to work internally and answer the player
            // nothing, because the old message path looked the person up BY
            // NAME. Today the reply goes through the controller and doesn't
            // depend on this, but it's still worth saying once, because an
            // empty name also spoils the online list and admin messages.
            if (j.nome.empty())
            {
                static bool avisou = false;
                if (!avisou)
                {
                    avisou = true;
                    g_api->Log("[shop] AVISO: li o id do jogador (%s) mas nao o NOME "
                               "(PlayerState.PlayerNamePrivate). A loja funciona; "
                               "mensagens que dependem do nome, nao.", j.id.c_str());
                }
            }
            return true;
        }
    }

    // Without Permission there's no stable identity, and a wallet keyed on the
    // player's NAME would be worse than none: the player changes their display
    // name and their points vanish, with no error and no way back. Better to
    // refuse and say so.
    return false;
}

// ── answering the player ────────────────────────────────────────────────────
//
// THE FIRST VERSION OF THIS FUNCTION WAS ONE LINE, AND FAILED SILENTLY:
//
//     if (j.nome.empty() || texto.empty()) return;
//     g_api->MensagemParaJogador(j.nome.c_str(), texto.c_str());
//
// Two things wrong, both invisible. First: if the name hadn't been read, it gave
// up without saying anything — and `Identificar` returns true with an EMPTY
// name, as long as Permission resolved the id. Second: the return value of
// `MensagemParaJogador` was ignored, so a failure there vanished too.
//
// Measured with a real player on 2026-08-20: Andrew typed `!pontos` three times,
// the hook FIRED and CANCELLED the message all three times, and nothing appeared
// on his screen. The log had no line about it, because there was none to have.
//
// And there's a design error behind both: looking the player up BY NAME when
// their controller is already in hand. `MensagemParaJogador` walks the world
// hunting for a CheatManager and calls `PlayerMessage(name, text)` — a long path
// that depends on an object which may not even exist, to deliver something to
// someone already identified.
//
// It now goes straight to the controller, trying the routes in order of
// preference. The first one this build has, wins; and the log says WHICH one
// took it, once — so the next build that changes this shows up in the log
// instead of silencing the shop.
static void Falar(const Jogador& j, const std::string& texto)
{
    if (texto.empty()) return;
    if (!j.controller)
    {
        g_api->Log("[shop] nao respondi a %s: sem controller.",
                   j.nome.empty() ? j.id.c_str() : j.nome.c_str());
        return;
    }

    // 0 = don't know yet · 1..3 = the route that worked · -1 = none did
    static int caminho = 0;

    auto tentar = [&](int qual) -> bool
    {
        switch (qual)
        {
        case 1:
            // The HUD's standard notification: it's what the game itself uses
            // to tell the player things, so it appears where they already look.
            ConanApi::Call<void>(j.controller, "ClientHUDShowNotification",
                                 ConanApi::TextoRico(texto.c_str()),
                                 bool(true),    // positive
                                 bool(false));  // bPlaySound: no sound, it's a command reply
            return g_api->UltimaChamadaExecutou() != 0;
        case 2:
            // Unreal's PlayerController system message.
            ConanApi::Call<void>(j.controller, "ClientMessage",
                                 ConanApi::Texto(texto.c_str()),
                                 ConanApi::Nome("Event"),
                                 float(8.0f));
            return g_api->UltimaChamadaExecutou() != 0;
        case 3:
            // The old route, through the CheatManager. It needs the NAME, so
            // it only applies if that was read.
            if (j.nome.empty()) return false;
            return g_api->MensagemParaJogador(j.nome.c_str(), texto.c_str()) != 0;
        }
        return false;
    };

    if (caminho > 0 && tentar(caminho)) return;

    for (int i = 1; i <= 3; ++i)
    {
        if (i == caminho) continue;          // already tried it above
        if (!tentar(i)) continue;
        if (caminho != i)
        {
            static const char* const NOMES[] = { "", "ClientHUDShowNotification",
                                                 "ClientMessage", "MensagemParaJogador" };
            g_api->Log("[shop] respondendo ao jogador por %s", NOMES[i]);
            caminho = i;
        }
        return;
    }

    // None of them worked. This HAS to show up: a shop that can't answer is a
    // shop the player thinks is broken, and without this line the server owner
    // would have nowhere to start.
    if (caminho != -1)
    {
        caminho = -1;
        g_api->Log("[shop] NAO CONSIGO responder ao jogador: nenhum dos tres "
                   "caminhos funcionou nesta build (ClientHUDShowNotification, "
                   "ClientMessage, PlayerMessage). Jogador: %s / nome=\"%s\"",
                   j.id.c_str(), j.nome.c_str());
    }
}

// Defined at the end of the file (they need `Identificar`, which needs
// `Jogador`) and used in the middle. The forward declaration avoids reordering
// the whole file just to please the compiler.
namespace Shop
{
    bool ResolverJogadorPorNome(const std::string& nome, std::string& id);
}

// ── the on-screen box ───────────────────────────────────────────────────────
//
// ClientShowMessageBox(Title: FText, Message: FText) — the box with a button
// that the game already uses for notices. The button belongs to THE GAME: it
// closes, and there's no way to turn it into "next page". That's why pagination
// is done by command.
static void MostrarNaTela(const Jogador& j, const std::string& titulo,
                          const std::string& corpo)
{
    if (!j.controller) return;
    ConanApi::Call<void>(j.controller, "ClientShowMessageBox",
                         ConanApi::TextoRico(titulo.c_str()),
                         ConanApi::TextoRico(corpo.c_str()));
}

// ── delivery ────────────────────────────────────────────────────────────────
//
// ConanCharacter::SpawnTemplateItem(TemplateId, Context: FName, quantity,
//                                   durabilityPercentage, durability,
//                                   ShowNotification) -> bool
//
// `Context` is an FName, and it's because of it that the SDK gained
// ConanApi::Nome on 2026-08-20: without that bridge this call — and therefore
// any shop at all — was impossible to write with the public SDK.
static bool Entregar(const Jogador& j, const Shop::Item& item, std::string& porque)
{
    void* corpo = MembroPonteiro(j.controller, "Character");
    if (!corpo) corpo = MembroPonteiro(j.controller, "Pawn");
    if (!corpo)
    {
        porque = "no character in the world";
        return false;
    }

    const bool ok = ConanApi::Call<bool>(
        corpo, "SpawnTemplateItem",
        int32_t(item.templateId),
        ConanApi::Nome(g_cfg.contexto.c_str()),
        int32_t(item.quantidade),
        float(1.0f),      // durabilityPercentage: a brand-new item
        float(0.0f),      // durability: 0 = use the item's own
        bool(true));      // ShowNotification: the game says "you received"

    // Two different questions, and both matter:
    //   UltimaChamadaExecutou() -> the function EXISTS and ran on this build
    //   ok                      -> the function ran and said it succeeded
    // Without the first, a function renamed in a patch would return false and
    // the log would blame a full inventory.
    if (!g_api->UltimaChamadaExecutou())
    {
        porque = "SpawnTemplateItem did not respond on this game build";
        return false;
    }
    if (!ok) { porque = "the game refused the delivery (inventory full?)"; return false; }
    return true;
}

// ── the commands ────────────────────────────────────────────────────────────

static bool PodeComprar(const Jogador& j, const Shop::Item& item)
{
    const ConanPermApi* perm = ConanPermObter();
    if (!perm || !perm->tem) return true;   // no Permission, no restriction
    if (!g_cfg.permComprar.empty() &&
        perm->tem(j.id.c_str(), g_cfg.permComprar.c_str()) != 1) return false;
    if (!item.permissao.empty() &&
        perm->tem(j.id.c_str(), item.permissao.c_str()) != 1) return false;
    return true;
}

static void ComandoPontos(const Jogador& j)
{
    const int64_t s = Shop::Saldo(j.id);
    if (s < 0)
    {
        // Telling someone with 500 points that they have 0 is worse than
        // telling them the shop is down.
        Falar(j, "A loja esta fora do ar (o banco nao respondeu). Avise um administrador.");
        return;
    }
    Falar(j, Shop::Formatar(g_cfg.Msg("seus_pontos", "Voce tem {0} ponto(s)."),
                      { std::to_string(s) }));
}

static void ComandoLoja(const Jogador& j, int pagina)
{
    // Builds only what the player CAN buy: listing what they can't and refusing
    // afterwards makes the person try in order to find out.
    std::vector<const Shop::Item*> visiveis;
    for (const Shop::Item& i : g_cfg.itens)
        if (PodeComprar(j, i)) visiveis.push_back(&i);

    const int porPagina = g_cfg.itensPorPagina;
    const int paginas = int((visiveis.size() + size_t(porPagina) - 1) / size_t(porPagina));
    if (pagina < 1) pagina = 1;

    if (visiveis.empty() || pagina > paginas)
    {
        Falar(j, Shop::Formatar(g_cfg.Msg("pagina_vazia", "Nao ha pagina {0}. A loja tem {1} pagina(s)."),
                          { std::to_string(pagina), std::to_string(paginas) }));
        return;
    }

    const size_t inicio = size_t(pagina - 1) * size_t(porPagina);
    const size_t fim = std::min(inicio + size_t(porPagina), visiveis.size());

    std::string corpo;
    std::string categoriaAtual;
    for (size_t i = inicio; i < fim; ++i)
    {
        const Shop::Item* it = visiveis[i];
        if (it->categoria != categoriaAtual)
        {
            categoriaAtual = it->categoria;
            corpo += "\n[" + categoriaAtual + "]\n";
        }
        char linha[256];
        std::snprintf(linha, sizeof(linha), "  %-22s x%-4d %lld pts   (%s)\n",
                      it->nome.substr(0, 22).c_str(), it->quantidade,
                      (long long)it->preco, it->chave.c_str());
        corpo += linha;
    }

    corpo += "\n";
    corpo += Shop::Formatar(g_cfg.Msg("rodape_da_lista",
                                "!comprar <id>   ·   pagina {0} de {1}   ·   !shop {2} para a proxima"),
                      { std::to_string(pagina), std::to_string(paginas),
                        std::to_string(pagina < paginas ? pagina + 1 : 1) });

    if (g_cfg.usarTela) MostrarNaTela(j, g_cfg.tituloDaTela, corpo);
    else                Falar(j, corpo);
}

static void ComandoComprar(const Jogador& j, const std::string& chave, int quantas)
{
    if (chave.empty())
    {
        Falar(j, g_cfg.Msg("uso_comprar", "Use: !comprar <id> [quantidade]"));
        return;
    }
    const Shop::Item* item = g_cfg.Achar(chave);
    if (!item || !PodeComprar(j, *item))
    {
        Falar(j, Shop::Formatar(g_cfg.Msg("id_desconhecido",
                                    "Nao existe item com id \"{0}\". Veja a lista com !shop"),
                          { chave }));
        return;
    }
    if (quantas < 1) quantas = 1;
    if (quantas > 100) quantas = 100;    // teto de sanidade

    const int64_t custo = item->preco * quantas;

    // The character is checked BEFORE the debit. Debiting and then finding
    // there's nowhere to deliver forces a refund, and every refund is one more
    // chance for something to go wrong in between.
    void* corpo = MembroPonteiro(j.controller, "Character");
    if (!corpo) corpo = MembroPonteiro(j.controller, "Pawn");
    if (!corpo)
    {
        Falar(j, g_cfg.Msg("sem_personagem",
                           "Voce precisa estar com o personagem no mundo para comprar."));
        return;
    }

    const Shop::Gasto r = Shop::Debitar(j.id, custo, ("compra:" + item->chave).c_str());
    if (r == Shop::Gasto::SemSaldo)
    {
        const int64_t s = Shop::Saldo(j.id);
        Falar(j, Shop::Formatar(g_cfg.Msg("sem_pontos", "Pontos insuficientes: custa {0} e voce tem {1}."),
                          { std::to_string(custo), std::to_string(s < 0 ? 0 : s) }));
        return;
    }
    if (r == Shop::Gasto::Erro)
    {
        Falar(j, "A loja esta fora do ar (o banco nao respondeu). NADA foi cobrado.");
        return;
    }

    // Debitado. A partir daqui, qualquer falha DEVOLVE.
    std::string porque;
    Shop::Item pedido = *item;
    pedido.quantidade = item->quantidade * quantas;

    if (!Entregar(j, pedido, porque))
    {
        Shop::Devolver(j.id, custo, ("devolucao:" + item->chave + " (" + porque + ")").c_str());
        g_api->Log("[shop] delivery failed for %s (%s): %s — %lld point(s) refunded",
                   j.nome.c_str(), item->chave.c_str(), porque.c_str(), (long long)custo);
        Falar(j, g_cfg.Msg("entrega_falhou",
                           "Nao consegui entregar o item. Seus pontos foram devolvidos."));
        return;
    }

    const int64_t saldo = Shop::Saldo(j.id);
    Falar(j, Shop::Formatar(g_cfg.Msg("comprou", "Comprou {0} x{1} por {2} ponto(s). Saldo: {3}"),
                      { item->nome, std::to_string(pedido.quantidade),
                        std::to_string(custo), std::to_string(saldo < 0 ? 0 : saldo) }));
    g_api->Log("[shop] %s comprou %s x%d por %lld",
               j.nome.c_str(), item->chave.c_str(), pedido.quantidade, (long long)custo);
}

// ── !shopdar <player> <amount> — an admin granting points from in game ──────
//
// It exists because the file queue serves automation, not someone who is
// playing and wants to reward a person on the spot. Guarded by the same
// permission as the reload.
static bool EhAdmin(const Jogador& j)
{
    const ConanPermApi* perm = ConanPermObter();
    if (!perm || !perm->tem) return false;    // sem Permission, ninguem e' admin
    if (g_cfg.permAdmin.empty()) return false;
    return perm->tem(j.id.c_str(), g_cfg.permAdmin.c_str()) == 1;
}

static void ComandoDar(const Jogador& j, const std::string& resto)
{
    if (!EhAdmin(j))
    {
        Falar(j, g_cfg.Msg("sem_permissao", "Voce nao tem permissao para isso."));
        return;
    }

    // "<name> <amount>" — can the name contain a space? No: a Conan name is
    // "Indio#76973", with none. Splitting on the LAST space covers both cases
    // without needing quotes.
    const size_t esp = resto.rfind(' ');
    if (esp == std::string::npos || esp + 1 >= resto.size())
    {
        Falar(j, "Use: " + g_cfg.cmdDar + " <jogador> <quantidade>");
        return;
    }
    const std::string quem = resto.substr(0, esp);
    const int64_t quanto = std::atoll(resto.substr(esp + 1).c_str());
    if (quanto <= 0)
    {
        Falar(j, "A quantidade precisa ser um numero maior que zero.");
        return;
    }

    std::string id;
    if (!Shop::ResolverJogadorPorNome(quem, id))
    {
        // They may have typed the id directly.
        if (quem.find('#') == std::string::npos) id = quem;
        else
        {
            Falar(j, "\"" + quem + "\" nao esta online. Para quem esta offline, "
                     "use o id da conta.");
            return;
        }
    }

    if (!Shop::Creditar(id, quanto, ("admin " + j.nome).c_str()))
    {
        Falar(j, "O banco recusou. NADA foi creditado.");
        return;
    }
    Falar(j, "Dei " + std::to_string(quanto) + " ponto(s) a " + quem +
             " (saldo " + std::to_string(Shop::Saldo(id)) + ").");
    g_api->Log("[shop] %s deu %lld ponto(s) a %s",
               j.nome.c_str(), (long long)quanto, id.c_str());

    // Tell the recipient, if they're online: receiving points in silence leaves
    // the player thinking the admin forgot them.
    Jogador alvo;
    void* pcs[128];
    const int n = g_api->FindObjects("ConanPlayerController", pcs, 128, 1);
    for (int i = 0; i < n; ++i)
        if (Identificar(pcs[i], alvo) && alvo.id == id)
        {
            Falar(alvo, Shop::Formatar(g_cfg.Msg("recebeu_pontos",
                                           "Voce recebeu {0} ponto(s). Total: {1}"),
                                 { std::to_string(quanto),
                                   std::to_string(Shop::Saldo(id)) }));
            break;
        }
}

static void ComandoRecarregar(const Jogador& j)
{
    const ConanPermApi* perm = ConanPermObter();
    if (perm && perm->tem && !g_cfg.permAdmin.empty() &&
        perm->tem(j.id.c_str(), g_cfg.permAdmin.c_str()) != 1)
    {
        Falar(j, g_cfg.Msg("sem_permissao", "Voce nao tem permissao para isso."));
        return;
    }

    Shop::Config nova;
    std::string erro;
    if (!Shop::LerConfig(g_caminhoConfig.c_str(), nova, erro))
    {
        // A configuracao ANTERIOR continua valendo. Isso e' o ponto.
        Falar(j, Shop::Formatar(g_cfg.Msg("recarregar_falhou",
                                    "O arquivo tem erro e NAO foi aplicado. "
                                    "Continuo com a configuracao anterior. Motivo: {0}"),
                          { erro }));
        g_api->Log("[shop] !shopreload RECUSADO: %s", erro.c_str());
        return;
    }

    // The database is deliberately NOT reopened here: switching backends with
    // players online would swap the wallets mid-purchase. Changing the backend
    // calls for a restart, and that's said in chat instead of happening
    // silently.
    const bool bancoMudou = (nova.banco.mysql != g_cfg.banco.mysql);
    const size_t quantos = nova.itens.size();
    g_cfg = std::move(nova);

    Falar(j, Shop::Formatar(g_cfg.Msg("recarregado", "Configuracao recarregada: {0} item(ns)."),
                      { std::to_string(quantos) }));
    if (bancoMudou)
        Falar(j, "Atencao: banco.tipo mudou, mas trocar de banco exige reiniciar o servidor. "
                 "Continuo no banco anterior.");

    auto aviso = g_cfg.msg.find("_aviso_itens_recusados");
    if (aviso != g_cfg.msg.end())
        Falar(j, "Aviso: " + aviso->second + " item(ns) do arquivo foram recusados "
                 "(template_id ausente ou zero, preco negativo, ou chave repetida).");

    g_api->Log("[shop] configuracao recarregada por %s: %d itens",
               j.nome.c_str(), int(quantos));
}

// ── chat ────────────────────────────────────────────────────────────────────

// ChatRpcData::Message sits at 0x068 on this build. This is the plugin's ONLY
// raw offset, and it's here because the parameter is an RPC struct that
// reflection doesn't decompose. It was measured, it's proven live by
// ExemploJogador, and something checks it: if the build changes, the API refuses
// to load.
static const uint32_t OFF_MENSAGEM_DO_CHAT = 0x068;

extern "C" ConanAcao AoFalar(ConanChamada* c)
{
    char texto[256] = {0};
    if (g_api->LerTextoDoJogo(c->Parms, OFF_MENSAGEM_DO_CHAT, texto, sizeof(texto)) <= 0)
        return CONAN_CONTINUAR;
    if (texto[0] != '!' && texto[0] != '/') return CONAN_CONTINUAR;

    const std::string msg = texto;
    std::string resto;

    // Identification only happens if the message LOOKS like one of our commands.
    // It costs a Permission lookup, and on a busy server every typed line comes
    // through here.
    const bool nosso =
        msg.compare(0, g_cfg.cmdLoja.size(), g_cfg.cmdLoja) == 0 ||
        msg.compare(0, g_cfg.cmdComprar.size(), g_cfg.cmdComprar) == 0 ||
        msg.compare(0, g_cfg.cmdPontos.size(), g_cfg.cmdPontos) == 0 ||
        msg.compare(0, g_cfg.cmdAjuda.size(), g_cfg.cmdAjuda) == 0 ||
        msg.compare(0, g_cfg.cmdRecarregar.size(), g_cfg.cmdRecarregar) == 0 ||
        msg.compare(0, g_cfg.cmdDar.size(), g_cfg.cmdDar) == 0;
    if (!nosso) return CONAN_CONTINUAR;

    Jogador j;
    if (!Identificar(c->Obj, j))
    {
        g_api->Log("[shop] nao consegui identificar quem digitou \"%s\". "
                   "O ConanPermission.dll esta instalado?", texto);
        return CONAN_CONTINUAR;
    }

    // A ordem importa: o mais especifico primeiro, senao "!shopreload" seria
    // engolido por "!shop".
    if (Shop::Prefixo(msg, g_cfg.cmdRecarregar, resto)) { ComandoRecarregar(j); return CONAN_CANCELAR; }
    if (Shop::Prefixo(msg, g_cfg.cmdDar, resto))        { ComandoDar(j, resto);  return CONAN_CANCELAR; }
    if (Shop::Prefixo(msg, g_cfg.cmdComprar, resto))
    {
        std::string chave = resto, qtd;
        const size_t esp = resto.find(' ');
        if (esp != std::string::npos) { chave = resto.substr(0, esp); qtd = resto.substr(esp + 1); }
        ComandoComprar(j, chave, qtd.empty() ? 1 : std::atoi(qtd.c_str()));
        return CONAN_CANCELAR;
    }
    if (Shop::Prefixo(msg, g_cfg.cmdPontos, resto)) { ComandoPontos(j); return CONAN_CANCELAR; }
    if (Shop::Prefixo(msg, g_cfg.cmdAjuda, resto))
    {
        Falar(j, g_cfg.cmdLoja + " [pagina]  ·  " + g_cfg.cmdComprar +
                 " <id> [qtd]  ·  " + g_cfg.cmdPontos);
        return CONAN_CANCELAR;
    }
    if (Shop::Prefixo(msg, g_cfg.cmdLoja, resto))
    {
        ComandoLoja(j, resto.empty() ? 1 : std::atoi(resto.c_str()));
        return CONAN_CANCELAR;
    }
    return CONAN_CONTINUAR;
}

// ── timed points ────────────────────────────────────────────────────────────
//
// A single scheduled task sweeps whoever is online, rather than one timer per
// player. With 30 players that's 30 tasks against one — and the online list is
// read from the world, so nobody keeps earning points after leaving.
static void CreditarOnline(void*)
{
    if (!g_cfg.pontosLigados || g_cfg.pontosPorGrupo.empty()) return;

    void* pcs[128];
    const int n = g_api->FindObjects("ConanPlayerController", pcs, 128, /*filhas=*/1);
    if (n <= 0) return;

    const ConanPermApi* perm = ConanPermObter();

    for (int i = 0; i < n; ++i)
    {
        Jogador j;
        if (!Identificar(pcs[i], j)) continue;

        // How much this player earns: the highest value among their groups, or
        // the sum, depending on "somar". Without Permission they fall into the
        // "default" group, and a server without Permission still works, just
        // without VIP.
        int64_t quanto = 0;
        bool achouGrupo = false;

        if (perm && perm->no_grupo)
        {
            for (const auto& par : g_cfg.pontosPorGrupo)
            {
                if (perm->no_grupo(j.id.c_str(), par.first.c_str()) != 1) continue;
                achouGrupo = true;
                if (g_cfg.pontosSomar) quanto += par.second;
                else if (par.second > quanto) quanto = par.second;
            }
        }
        if (!achouGrupo)
        {
            auto d = g_cfg.pontosPorGrupo.find("default");
            if (d != g_cfg.pontosPorGrupo.end()) quanto = d->second;
        }
        if (quanto <= 0) continue;

        if (!Shop::Creditar(j.id, quanto, "tempo online")) continue;

        if (g_cfg.pontosAvisar)
        {
            const int64_t s = Shop::Saldo(j.id);
            Falar(j, Shop::Formatar(g_cfg.Msg("recebeu_pontos",
                                        "Voce recebeu {0} ponto(s). Total: {1}"),
                              { std::to_string(quanto), std::to_string(s < 0 ? 0 : s) }));
        }
    }
}

static void LogDoBanco(const char* linha) { if (g_api) g_api->Log("%s", linha); }

// ── what the commands module needs from the shop ────────────────────────────
//
// Three functions, and nothing else. The queue administers points and reloads
// the config; it doesn't need (and shouldn't have) reach into the rest of the
// plugin.
namespace Shop
{
    const ConanApiTabela* ApiDaLoja()   { return g_api; }
    const Config&         ConfigDaLoja(){ return g_cfg; }

    bool RecarregarConfig(std::string& erro, size_t& quantos)
    {
        Config nova;
        if (!LerConfig(g_caminhoConfig.c_str(), nova, erro)) return false;
        quantos = nova.itens.size();
        g_cfg = std::move(nova);
        return true;
    }

    // The owner has the NAME in hand (it's what RCON's `listplayers` shows);
    // the database stores the account ID. This is the bridge, and it only works
    // while the player is ONLINE, which the queue's reply says rather than
    // failing quietly.
    bool ResolverJogadorPorNome(const std::string& nome, std::string& id)
    {
        void* pcs[128];
        const int n = g_api->FindObjects("ConanPlayerController", pcs, 128, 1);
        for (int i = 0; i < n; ++i)
        {
            Jogador j;
            if (!Identificar(pcs[i], j)) continue;
            if (j.nome == nome) { id = j.id; return true; }
        }
        return false;
    }
}

static void AtenderFilaDeComandos(void*) { Shop::AtenderFila(); }

extern "C" __declspec(dllexport)
void ConanPluginCarregar(const ConanApiTabela* api)
{
    if (!api || api->tamanho < sizeof(ConanApiTabela)) return;
    g_api = api;
    ConanApi::UsarTabela(api);

    g_api->Log("=====================================================");
    g_api->Log(" ConanShop — loja por pontos");
    g_api->Log("=====================================================");

    g_caminhoConfig = g_api->CaminhoConfig("ConanShop");
    std::string erro;
    if (!Shop::LerConfig(g_caminhoConfig.c_str(), g_cfg, erro))
    {
        // Refusing to start is deliberate. A shop with a bad configuration
        // charges points and delivers nothing, which is worse than no shop.
        g_api->Log("[shop] NAO SUBI: %s", erro.c_str());
        g_api->Log("[shop] arquivo: %s", g_caminhoConfig.c_str());
        return;
    }
    g_api->Log("[shop] configuracao: %d item(ns), %d grupo(s) de pontos",
               int(g_cfg.itens.size()), int(g_cfg.pontosPorGrupo.size()));

    Shop::DefinirLog(&LogDoBanco);
    if (g_cfg.banco.caminhoLocal.empty())
        g_cfg.banco.caminhoLocal = g_api->CaminhoDados("ConanShop", "conanshop.db");

    if (!Shop::Abrir(g_cfg.banco, erro))
    {
        g_api->Log("[shop] NAO SUBI: o banco nao abriu — %s", erro.c_str());
        return;
    }

    if (g_api->HookProcessEvent("ServerSendChatMessage", AoFalar, nullptr, 100) == 0)
    {
        g_api->Log("[shop] NAO SUBI: nao consegui enganchar o chat. Sem chat nao ha' loja.");
        Shop::Fechar();
        return;
    }

    if (g_cfg.pontosLigados)
    {
        const uint32_t seg = uint32_t(g_cfg.pontosMinutos) * 60u;
        g_api->AgendarNaThreadDoJogo(CreditarOnline, seg, nullptr, /*repetir=*/1);
        g_api->Log("[shop] pontos: a cada %d minuto(s)", g_cfg.pontosMinutos);
    }

    // ── the door from outside the game ──────────────────────────────────────
    //
    // Conan's RCON doesn't accept plugin commands (measured: see Comandos.h).
    // This queue is how a web panel, a script or SSH grant points.
    if (const char* raiz = g_api->CaminhoRaiz())
    {
        Shop::DefinirCaminhos(raiz);
        g_api->AgendarNaThreadDoJogo(AtenderFilaDeComandos, 3, nullptr, /*repetir=*/1);
        g_api->Log("[shop] comandos de fora: escreva em %s", Shop::CaminhoDaFila().c_str());
        g_api->Log("[shop]    dar|tirar|definir <jogador> <qtd> [motivo]  ·  saldo <jogador>  ·  recarregar");
        g_api->Log("[shop]    a resposta sai em %s", Shop::CaminhoDasRespostas().c_str());
    }

    const ConanPermApi* perm = ConanPermObter();
    g_api->Log("[shop] Permission: %s", perm ? "presente (VIP e identidade ok)"
                                             : "AUSENTE — sem VIP e sem identidade estavel");
    g_api->Log("[shop] pronto: %s  ·  %s  ·  %s",
               g_cfg.cmdLoja.c_str(), g_cfg.cmdComprar.c_str(), g_cfg.cmdPontos.c_str());
    g_api->Log("=====================================================");
}

extern "C" __declspec(dllexport)
void ConanPluginDescarregar(void)
{
    Shop::Fechar();
}
