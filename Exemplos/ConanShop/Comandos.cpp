// Comandos (commands) — implementation. The reasoning is in Comandos.h.
#include "Comandos.h"
#include "Pontos.h"
#include "Config.h"

#include "Conan/ConanPluginApi.h"
#include "Conan/ConanPermission.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

namespace Shop
{
// Vem do ConanShop.cpp: a tabela da API e a configuracao viva. Declarados aqui
// rather than passed as parameters, because this module is called by the
// scheduler, which carries no context.
extern const ConanApiTabela* ApiDaLoja();
extern const Config&         ConfigDaLoja();
extern bool                  RecarregarConfig(std::string& erro, size_t& quantos);
extern bool                  ResolverJogadorPorNome(const std::string& nome, std::string& id);

namespace
{
    std::string g_fila, g_respostas;

    // ── WHAT'S LEFT TO CONFIRM ON THE NEXT PASS ─────────────────────────────
    //
    // Permission's writes are ASYNCHRONOUS: `conceder` puts the task on its
    // writer thread's queue and returns 1 if it managed to ENQUEUE it. The
    // group's validation happens later, in there.
    //
    // The first version treated that 1 as success and answered
    // "ok grupo A-4QR7CRS0F -> naoexiste" for a group that DOESN'T EXIST.
    // Permission did everything right (it refused and logged "conceder
    // ignorado: grupo 'naoexiste' nao existe"). It was MY answer that lied.
    //
    // The result is now ASKED FOR afterwards, with `no_grupo`, on the queue's
    // next pass (3 s). Whoever reads SHOP-RESPOSTAS sees "enqueued" first and,
    // a moment later, "CONFIRMED" or "DIDN'T GO IN".
    struct Pendente
    {
        int         linha;
        std::string jogador, grupo;
        bool        esperado;      // true = should be in the group; false = should have left
    };
    std::vector<Pendente> g_pendentes;

    void Aparar(std::string& s)
    {
        while (!s.empty() && (s.back()  == '\r' || s.back()  == '\n' ||
                              s.back()  == ' '  || s.back()  == '\t')) s.pop_back();
        size_t i = 0;
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
        if (i) s.erase(0, i);
    }

    std::vector<std::string> Partir(const std::string& linha)
    {
        std::vector<std::string> p;
        std::string atual;
        for (char c : linha)
        {
            if (c == ' ' || c == '\t')
            {
                if (!atual.empty()) { p.push_back(atual); atual.clear(); }
                continue;
            }
            atual += c;
        }
        if (!atual.empty()) p.push_back(atual);
        return p;
    }

    // Accepts an account id OR the name of someone online. The id is the right
    // path (it doesn't change); the name exists because it's what the owner has
    // in hand when looking at RCON's `listplayers`.
    bool Resolver(const std::string& quem, std::string& id, std::string& porque)
    {
        // A Conan account id starts with "A-" and has no "#". A display name
        // has one (Indio#76973). Trying the id first avoids walking the world
        // for nothing.
        if (quem.find('#') == std::string::npos)
        {
            id = quem;
            return true;
        }
        if (ResolverJogadorPorNome(quem, id)) return true;
        porque = "jogador nao esta online (por nome so' funciona conectado; "
                 "use o id da conta para quem esta offline)";
        return false;
    }
}

void DefinirCaminhos(const std::string& raiz)
{
    g_fila      = raiz + "\\SHOP-COMANDOS";
    g_respostas = raiz + "\\SHOP-RESPOSTAS";
}

const std::string& CaminhoDaFila()      { return g_fila; }
const std::string& CaminhoDasRespostas(){ return g_respostas; }

// Confirms with Permission whatever was left pending from the previous pass.
// Returns the reply lines, which are APPENDED to the file so that whoever
// automates this reads the outcome of each request.
static std::vector<std::string> ConfirmarPendentes()
{
    std::vector<std::string> saida;
    if (g_pendentes.empty()) return saida;

    const ConanPermApi* perm = ConanPermObter();
    for (const Pendente& d : g_pendentes)
    {
        char linha[512];
        if (!perm || !perm->no_grupo)
        {
            std::snprintf(linha, sizeof(linha),
                "linha %d: NAO CONFERI %s -> %s (o Permission sumiu no meio)",
                d.linha, d.jogador.c_str(), d.grupo.c_str());
        }
        else
        {
            const int32_t esta = perm->no_grupo(d.jogador.c_str(), d.grupo.c_str());
            const bool ok = (esta == 1) == d.esperado;
            if (ok)
                std::snprintf(linha, sizeof(linha), "linha %d: CONFIRMADO %s %s %s",
                              d.linha, d.jogador.c_str(),
                              d.esperado ? "esta em" : "saiu de", d.grupo.c_str());
            else if (esta < 0)
                std::snprintf(linha, sizeof(linha),
                    "linha %d: NAO CONFERI %s -> %s (o Permission nao respondeu)",
                    d.linha, d.jogador.c_str(), d.grupo.c_str());
            else
                std::snprintf(linha, sizeof(linha),
                    "linha %d: NAO ENTROU — %s NAO esta em \"%s\". O grupo existe no "
                    "permission.json? As chaves diferenciam maiusculas (\"vip\" nao e' \"VIP\"). "
                    "O motivo exato esta no log, procure por [permission].",
                    d.linha, d.jogador.c_str(), d.grupo.c_str());
        }
        saida.push_back(linha);
        if (const ConanApiTabela* api = ApiDaLoja()) api->Log("[shop] fila: %s", linha);
    }
    g_pendentes.clear();
    return saida;
}

void AtenderFila()
{
    if (g_fila.empty()) return;

    // Confirmations come BEFORE returning for lack of a file: the request was
    // made on an earlier pass, and its outcome has to come out even if nobody
    // writes anything new now.
    const std::vector<std::string> confirmacoes = ConfirmarPendentes();

    FILE* f = std::fopen(g_fila.c_str(), "rb");
    if (!f)
    {
        if (confirmacoes.empty()) return;   // the common case: nothing to do
        // Confirmations only: APPENDS to the replies file, so the "enqueued"
        // line the owner just read isn't wiped.
        FILE* r = std::fopen(g_respostas.c_str(), "ab");
        if (r)
        {
            for (const std::string& c : confirmacoes) std::fprintf(r, "%s\n", c.c_str());
            std::fclose(r);
        }
        return;
    }

    std::string todo;
    {
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) todo.append(buf, n);
    }
    std::fclose(f);

    // ── DELETE BEFORE EXECUTING ─────────────────────────────────────────────
    //
    // If the server dies midway through a "give 500 points", the queue must not
    // still be there on the way back to do it again. Losing a command is
    // recoverable (the owner repeats it); giving points twice shows up to
    // nobody.
    std::remove(g_fila.c_str());

    const ConanApiTabela* api = ApiDaLoja();
    std::vector<std::string> respostas = confirmacoes;
    int linhaN = 0;

    size_t ini = 0;
    while (ini <= todo.size())
    {
        size_t fim = todo.find('\n', ini);
        if (fim == std::string::npos) fim = todo.size();
        std::string linha = todo.substr(ini, fim - ini);
        ini = fim + 1;

        Aparar(linha);
        ++linhaN;
        if (linha.empty() || linha[0] == '#') continue;   // vazio e comentario

        const std::vector<std::string> p = Partir(linha);
        if (p.empty()) continue;

        std::string verbo = p[0];
        for (char& c : verbo) c = char(std::tolower((unsigned char)c));

        char resp[512];

        if (verbo == "recarregar")
        {
            std::string erro; size_t quantos = 0;
            if (RecarregarConfig(erro, quantos))
                std::snprintf(resp, sizeof(resp),
                              "linha %d: ok recarregar — %d item(ns)", linhaN, int(quantos));
            else
                std::snprintf(resp, sizeof(resp),
                              "linha %d: ERRO recarregar — %s (a configuracao ANTERIOR continua valendo)",
                              linhaN, erro.c_str());
            respostas.push_back(resp);
            continue;
        }

        if (p.size() < 2)
        {
            std::snprintf(resp, sizeof(resp),
                "linha %d: ERRO \"%s\" — falta o jogador. Use: "
                "dar|tirar|definir <jogador> <qtd> · saldo <jogador> · grupo <jogador> <grupo>",
                linhaN, verbo.c_str());
            respostas.push_back(resp);
            continue;
        }

        std::string id, porque;
        if (!Resolver(p[1], id, porque))
        {
            std::snprintf(resp, sizeof(resp), "linha %d: ERRO \"%s\" — %s",
                          linhaN, p[1].c_str(), porque.c_str());
            respostas.push_back(resp);
            continue;
        }

        // ── grupo/tirargrupo: administering VIP from outside the game ───────
        //
        // WHY THIS LIVES HERE AND NOT IN Permission
        //
        // Permission exposes `conceder`/`revogar` in its ABI, but it has no
        // outside door: anyone wanting to grant VIP needed a plugin just to call
        // the function, or had to restart the server after editing the database
        // by hand — and editing another plugin's database, with WAL and an
        // in-memory cache, is asking for corruption.
        //
        // ConanShop already links Permission (it needs it for VIP and for
        // identity) and already has a queue that web panels, scripts and SSH
        // know how to write. Adding two verbs here costs 30 lines and solves it.
        //
        // It works for ANY group in permission.json, not only the ones that
        // grant points: whoever creates a "patron" or "builder" group
        // administers it the same way.
        if (verbo == "grupo" || verbo == "tirargrupo")
        {
            if (p.size() < 3)
            {
                std::snprintf(resp, sizeof(resp),
                    "linha %d: ERRO \"%s\" — falta o grupo. Use: %s <jogador> <grupo>",
                    linhaN, verbo.c_str(), verbo.c_str());
                respostas.push_back(resp);
                continue;
            }
            const ConanPermApi* perm = ConanPermObter();
            if (!perm)
            {
                std::snprintf(resp, sizeof(resp),
                    "linha %d: ERRO \"%s\" — o ConanPermission.dll nao esta carregado",
                    linhaN, verbo.c_str());
                respostas.push_back(resp);
                continue;
            }

            // `expira_em` = 0 means never. A fourth field, if present, is the
            // duration in DAYS, which is how VIP is actually sold.
            int64_t expira = 0;
            if (verbo == "grupo" && p.size() >= 4)
            {
                char* fimD = nullptr;
                const long long dias = std::strtoll(p[3].c_str(), &fimD, 10);
                if (fimD && *fimD == 0 && dias > 0)
                    expira = int64_t(std::time(nullptr)) + dias * 86400;
            }

            const int32_t r = (verbo == "grupo")
                ? (perm->conceder ? perm->conceder(id.c_str(), p[2].c_str(),
                                                   expira, "fila do ConanShop") : -1)
                : (perm->revogar  ? perm->revogar (id.c_str(), p[2].c_str(),
                                                   "fila do ConanShop") : -1);

            // 1 means "ACCEPTED FOR WRITING", not "written" — see
            // ConfirmarPendentes.
            if (r == 1)
            {
                g_pendentes.push_back({ linhaN, id, p[2], verbo == "grupo" });
                if (expira)
                    std::snprintf(resp, sizeof(resp),
                        "linha %d: enfileirado %s %s -> %s (por %s dia(s)); confirmo em ate' 3 s",
                        linhaN, verbo.c_str(), id.c_str(), p[2].c_str(), p[3].c_str());
                else
                    std::snprintf(resp, sizeof(resp),
                        "linha %d: enfileirado %s %s -> %s; confirmo em ate' 3 s",
                        linhaN, verbo.c_str(), id.c_str(), p[2].c_str());
            }
            else if (r == 0)
                // The hint about CAPITALS isn't decoration: group keys are
                // compared exactly as they appear in permission.json, so "VIP"
                // is refused where "vip" passes. Without this line, someone who
                // types the right name in the wrong case reads "doesn't exist"
                // and goes looking for a group that's sitting right there.
                std::snprintf(resp, sizeof(resp),
                    "linha %d: RECUSADO %s — nao existe grupo \"%s\" no permission.json. "
                    "Confira a caixa: as chaves diferenciam maiusculas (\"vip\" nao e' \"VIP\").",
                    linhaN, verbo.c_str(), p[2].c_str());
            else
                std::snprintf(resp, sizeof(resp),
                    "linha %d: ERRO %s — o Permission nao respondeu (a escrita e' enfileirada; "
                    "confira o log dele)", linhaN, verbo.c_str());
            respostas.push_back(resp);
            continue;
        }

        if (verbo == "saldo")
        {
            const int64_t s = Saldo(id);
            if (s < 0) std::snprintf(resp, sizeof(resp),
                                     "linha %d: ERRO saldo %s — o banco nao respondeu",
                                     linhaN, id.c_str());
            else       std::snprintf(resp, sizeof(resp), "linha %d: ok saldo %s = %lld",
                                     linhaN, id.c_str(), (long long)s);
            respostas.push_back(resp);
            continue;
        }

        if (p.size() < 3)
        {
            std::snprintf(resp, sizeof(resp),
                          "linha %d: ERRO \"%s\" — falta a quantidade", linhaN, verbo.c_str());
            respostas.push_back(resp);
            continue;
        }

        char* fimNum = nullptr;
        const long long quanto = std::strtoll(p[2].c_str(), &fimNum, 10);
        if (!fimNum || *fimNum != 0 || quanto <= 0)
        {
            std::snprintf(resp, sizeof(resp),
                "linha %d: ERRO \"%s\" — quantidade tem de ser um numero inteiro maior que zero (veio \"%s\")",
                linhaN, verbo.c_str(), p[2].c_str());
            respostas.push_back(resp);
            continue;
        }

        // The reason goes into the ledger. Without it, an audit asking "where
        // did these 5,000 points come from" has no answer.
        std::string motivo = "admin";
        for (size_t i = 3; i < p.size(); ++i) { motivo += ' '; motivo += p[i]; }

        if (verbo == "dar")
        {
            if (Creditar(id, quanto, motivo.c_str()))
                std::snprintf(resp, sizeof(resp), "linha %d: ok dar %s +%lld (saldo %lld)",
                              linhaN, id.c_str(), quanto, (long long)Saldo(id));
            else
                std::snprintf(resp, sizeof(resp),
                              "linha %d: ERRO dar %s — o banco recusou. NADA foi creditado.",
                              linhaN, id.c_str());
        }
        else if (verbo == "tirar")
        {
            const Gasto g = Debitar(id, quanto, motivo.c_str());
            if (g == Gasto::Ok)
                std::snprintf(resp, sizeof(resp), "linha %d: ok tirar %s -%lld (saldo %lld)",
                              linhaN, id.c_str(), quanto, (long long)Saldo(id));
            else if (g == Gasto::SemSaldo)
                std::snprintf(resp, sizeof(resp),
                    "linha %d: RECUSADO tirar %s -%lld — saldo insuficiente (tem %lld). "
                    "NADA foi tirado; a carteira nunca fica negativa.",
                    linhaN, id.c_str(), quanto, (long long)Saldo(id));
            else
                std::snprintf(resp, sizeof(resp),
                              "linha %d: ERRO tirar %s — o banco recusou", linhaN, id.c_str());
        }
        else if (verbo == "definir")
        {
            // Setting is a credit or a debit depending on the difference.
            // There's deliberately no loose "SET pontos = N": going through
            // Creditar/Debitar keeps the ledger complete, and it's the ledger
            // that answers when a player complains.
            const int64_t atual = Saldo(id);
            if (atual < 0)
                std::snprintf(resp, sizeof(resp),
                              "linha %d: ERRO definir %s — nao consegui ler o saldo atual",
                              linhaN, id.c_str());
            else if (atual == quanto)
                std::snprintf(resp, sizeof(resp), "linha %d: ok definir %s = %lld (ja' estava)",
                              linhaN, id.c_str(), quanto);
            else if (atual < quanto)
            {
                const bool ok = Creditar(id, quanto - atual, motivo.c_str());
                std::snprintf(resp, sizeof(resp), "linha %d: %s definir %s = %lld (era %lld)",
                              linhaN, ok ? "ok" : "ERRO", id.c_str(), quanto, (long long)atual);
            }
            else
            {
                const Gasto g = Debitar(id, atual - quanto, motivo.c_str());
                std::snprintf(resp, sizeof(resp), "linha %d: %s definir %s = %lld (era %lld)",
                              linhaN, g == Gasto::Ok ? "ok" : "ERRO",
                              id.c_str(), quanto, (long long)atual);
            }
        }
        else
        {
            std::snprintf(resp, sizeof(resp),
                "linha %d: ERRO verbo \"%s\" nao existe. Use: dar, tirar, definir, saldo, "
                "grupo, tirargrupo, recarregar",
                linhaN, verbo.c_str());
        }
        respostas.push_back(resp);
    }

    if (respostas.empty()) return;

    // ── as respostas ────────────────────────────────────────────────────────
    //
    // Overwrites rather than accumulating: the file answers the LAST batch, and
    // whoever automates it reads that and knows it's the answer to what they
    // just sent. The permanent history is the ledger, in the database.
    FILE* r = std::fopen(g_respostas.c_str(), "wb");
    if (r)
    {
        const std::time_t agora = std::time(nullptr);
        char quando[64];
        std::strftime(quando, sizeof(quando), "%Y-%m-%d %H:%M:%S", std::localtime(&agora));
        std::fprintf(r, "# ConanShop — respostas de %s\n", quando);
        for (const std::string& s : respostas) std::fprintf(r, "%s\n", s.c_str());
        std::fclose(r);
    }

    if (api)
        for (const std::string& s : respostas) api->Log("[shop] fila: %s", s.c_str());
}

}  // namespace Shop
