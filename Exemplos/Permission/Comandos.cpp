// Comandos — the implementation. The why is in Comandos.h.
#include "Comandos.h"

#include "Conan/ConanPluginApi.h"
// ConanBase: where ConanApi::Call, ConanApi::TextoRico and ConanApi::Nome come
// from — the three that get the answer to the player.
#include "Conan/ConanBase.h"
// This plugin's public header, for CONAN_PERM_MAX_ID. It's the same constant
// the ABI promises third parties; a loose number here would be a second truth
// about the same size.
#include "Conan/ConanPermission.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <ctime>
#include <string>
#include <vector>

namespace Perm
{
// ── the bridges to Permission.cpp ───────────────────────────────────────────
//
// `g_armazem` lives in an anonymous namespace over there, and that's how it
// should be: it's the plugin's single instance, not a program-wide global.
// These functions are the minimum surface the commands need — and nothing
// beyond it.
extern const ConanApiTabela* ApiDoPermission();
extern int32_t CmdTem       (const char* jogador, const char* no);
extern int32_t CmdGrupos    (const char* jogador, char* saida, int32_t tam);
extern int32_t CmdNoGrupo   (const char* jogador, const char* grupo);
extern int64_t CmdExpiraEm  (const char* jogador, const char* grupo);
extern int32_t CmdConceder  (const char* jogador, const char* grupo,
                             int64_t expiraEm, const char* quem);
extern int32_t CmdRevogar   (const char* jogador, const char* grupo, const char* quem);
extern bool    CmdRecarregar();
extern int32_t CmdIdDoController(void* controller, char* saida, int32_t tam);

namespace
{
    const ConanApiTabela* g_api = nullptr;
    uint32_t              g_hook = 0;

    // The node that unlocks the write commands. permission.json's `admin`
    // group has `*`, so it already passes — and the first admin gets in by
    // editing that file, which is how every permission plugin bootstraps.
    const char* const NO_ADMIN = "perm.admin";

    // ChatRpcData: `Message` at 0x068, `userName` at 0x048. Measured, and it's
    // the same offset ExemploJogador and ConanShop have been using live for
    // days. It's this file's only raw offset, and it's here because the
    // parameter is an RPC struct that reflection doesn't decompose.
    const uint32_t OFF_MENSAGEM = 0x068;

    std::string Aparado(const std::string& s)
    {
        size_t a = 0, b = s.size();
        while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
        while (b > a && (s[b-1] == ' ' || s[b-1] == '\t' ||
                         s[b-1] == '\r' || s[b-1] == '\n')) --b;
        return s.substr(a, b - a);
    }

    std::vector<std::string> Partir(const std::string& linha)
    {
        std::vector<std::string> p;
        std::string atual;
        for (char c : linha)
        {
            if (c == ' ' || c == '\t')
            { if (!atual.empty()) { p.push_back(atual); atual.clear(); } continue; }
            atual += c;
        }
        if (!atual.empty()) p.push_back(atual);
        return p;
    }

    // ── read a pointer member BY NAME ───────────────────────────────────────
    void* MembroPonteiro(void* obj, const char* nome)
    {
        if (!obj || !g_api) return nullptr;
        const int32_t off = g_api->OffsetDoMembro(obj, nome);
        if (off < 0) return nullptr;
        void* p = nullptr;
        if (g_api->LerMembro(obj, uint32_t(off), &p, sizeof(p)) <= 0) return nullptr;
        if (!p || !g_api->Legivel(p, 8)) return nullptr;
        return p;
    }

    struct Quem
    {
        std::string id, nome;
        void*       controller = nullptr;
    };

    bool Identificar(void* controller, Quem& q)
    {
        if (!controller) return false;
        q.controller = controller;
        if (void* ps = MembroPonteiro(controller, "PlayerState"))
        {
            const int32_t off = g_api->OffsetDoMembro(ps, "PlayerNamePrivate");
            if (off >= 0)
            {
                char n[128] = {0};
                if (g_api->LerTextoDoJogo(ps, uint32_t(off), n, sizeof(n)) > 0) q.nome = n;
            }
        }
        char id[CONAN_PERM_MAX_ID] = {0};
        if (CmdIdDoController(controller, id, sizeof(id)) > 0 && id[0])
        { q.id = id; return true; }
        return false;
    }

    // ── answering the player ────────────────────────────────────────────────
    //
    // Straight at the controller, without looking anybody up by name. The path
    // and the order come from what was MEASURED with a real player on
    // 2026-08-20, in ConanShop: its first attempt used `PlayerMessage` (which
    // has to find a CheatManager and takes the NAME) and answered nothing — the
    // hook fired, the message was cancelled, and the reply died without a line
    // in the log.
    //
    // On this build what works is `ClientHUDShowNotification`, which is what
    // the game uses for its own notices. The other two stay as fallbacks, and
    // the log says which one caught — so the day a patch touches this, it shows
    // up in the log instead of silencing the commands.
    void Falar(const Quem& q, const std::string& texto)
    {
        if (!q.controller || texto.empty() || !g_api) return;
        static int caminho = 0;

        auto tentar = [&](int qual) -> bool
        {
            if (qual == 1)
            {
                ConanApi::Call<void>(q.controller, "ClientHUDShowNotification",
                                     ConanApi::TextoRico(texto.c_str()),
                                     bool(true), bool(false));
                return g_api->UltimaChamadaExecutou() != 0;
            }
            if (qual == 2)
            {
                ConanApi::Call<void>(q.controller, "ClientMessage",
                                     ConanApi::Texto(texto.c_str()),
                                     ConanApi::Nome("Event"), float(8.0f));
                return g_api->UltimaChamadaExecutou() != 0;
            }
            if (qual == 3 && !q.nome.empty())
                return g_api->MensagemParaJogador(q.nome.c_str(), texto.c_str()) != 0;
            return false;
        };

        if (caminho > 0 && tentar(caminho)) return;
        for (int i = 1; i <= 3; ++i)
        {
            if (i == caminho || !tentar(i)) continue;
            if (caminho != i)
            {
                static const char* const N[] = { "", "ClientHUDShowNotification",
                                                 "ClientMessage", "MensagemParaJogador" };
                g_api->Log("[permission] respondendo ao jogador por %s", N[i]);
                caminho = i;
            }
            return;
        }
        if (caminho != -1)
        {
            caminho = -1;
            g_api->Log("[permission] NAO CONSIGO responder ao jogador: nenhum dos tres "
                       "caminhos funcionou nesta build. Jogador: %s", q.id.c_str());
        }
    }

    // ── "30d", "12h", "0" ───────────────────────────────────────────────────
    //
    // The README specified this format, and it's what a server owner types
    // without thinking. `0` (or absent) = never expires.
    //
    // Returns false when the text isn't a duration — and the caller REFUSES
    // rather than assuming "never". Assuming would hand eternal VIP to somebody
    // who typed "30x" by mistake, with nobody noticing until the invoice.
    bool LerPrazo(const std::string& s, int64_t agora, int64_t& expira, std::string& porque)
    {
        expira = 0;
        if (s.empty() || s == "0") return true;

        char* fim = nullptr;
        const long long n = std::strtoll(s.c_str(), &fim, 10);
        if (!fim || n <= 0) { porque = "prazo tem de ser um numero maior que zero"; return false; }

        std::string suf = fim;
        for (char& c : suf) c = char(std::tolower((unsigned char)c));

        if (suf.empty() || suf == "d")  { expira = agora + n * 86400;  return true; }
        if (suf == "h")                 { expira = agora + n * 3600;   return true; }
        if (suf == "m")                 { expira = agora + n * 60;     return true; }
        porque = "prazo aceita d (dias), h (horas) ou m (minutos). Ex.: 30d, 12h, 0 = nunca";
        return false;
    }

    std::string QuandoVence(int64_t expira)
    {
        if (expira <= 0) return "nunca vence";
        const int64_t falta = expira - int64_t(std::time(nullptr));
        if (falta <= 0) return "VENCIDO";
        char b[64];
        if (falta >= 86400) std::snprintf(b, sizeof(b), "vence em %lld dia(s)", (long long)(falta / 86400));
        else if (falta >= 3600) std::snprintf(b, sizeof(b), "vence em %lld hora(s)", (long long)(falta / 3600));
        else std::snprintf(b, sizeof(b), "vence em %lld minuto(s)", (long long)(falta / 60));
        return b;
    }

    // Lists somebody's groups, each with its expiry.
    std::string GruposDe(const std::string& id)
    {
        char buf[512] = {0};
        const int32_t n = CmdGrupos(id.c_str(), buf, sizeof(buf));
        if (n < 0)  return "(nao consegui ler — o banco respondeu?)";
        if (!buf[0]) return "(nenhum)";

        // Armazem returns them comma-separated.
        std::string r;
        std::string atual;
        std::string todos = buf;
        todos += ',';
        for (char c : todos)
        {
            if (c != ',') { if (c != ' ') atual += c; continue; }
            if (atual.empty()) continue;
            if (!r.empty()) r += ", ";
            r += atual;
            const int64_t e = CmdExpiraEm(id.c_str(), atual.c_str());
            if (e > 0) { r += " ("; r += QuandoVence(e); r += ")"; }
            atual.clear();
        }
        return r.empty() ? "(nenhum)" : r;
    }

    bool EhAdmin(const Quem& q) { return CmdTem(q.id.c_str(), NO_ADMIN) == 1; }

    // ── CONFIRM LATER, BECAUSE THE WRITE IS ASYNCHRONOUS ────────────────────
    //
    // `Conceder`/`Revogar` QUEUE and return 1 if the task was accepted — the
    // group's validation happens afterwards, on the writer thread. Treating
    // that 1 as success means telling the admin they granted VIP when
    // Permission refused. It happened, on the same day, with ConanShop's queue:
    // it answered "ok grupo A-... -> naoexiste" while the log said
    // "conceder ignorado: grupo 'naoexiste' nao existe".
    //
    // There's no waiting here (we're in the game loop), so the answer comes in
    // two parts: "requested" now, and the outcome 2 s later, when the scheduled
    // check asks `NoGrupo` and speaks to the requester again.
    struct Pendente
    {
        std::string quemPediu;     // id of whoever sent the command
        std::string alvo, grupo;
        bool        esperado;      // true = should be in; false = should be out
        int64_t     expira;
    };
    std::vector<Pendente> g_pendentes;

    // Finds an ONLINE player again by id. The controller may have died between
    // the request and the confirmation — keeping the pointer would mean reading
    // freed memory.
    bool AcharOnline(const std::string& id, Quem& q)
    {
        void* pcs[128];
        const int n = g_api->FindObjects("ConanPlayerController", pcs, 128, 1);
        for (int i = 0; i < n; ++i)
        {
            Quem outro;
            if (Identificar(pcs[i], outro) && outro.id == id) { q = outro; return true; }
        }
        return false;
    }

    void ConfirmarPendentes(void*)
    {
        if (g_pendentes.empty()) return;
        std::vector<Pendente> lote;
        lote.swap(g_pendentes);

        for (const Pendente& d : lote)
        {
            const int32_t esta = CmdNoGrupo(d.alvo.c_str(), d.grupo.c_str());
            Quem quemPediu;
            const bool achou = AcharOnline(d.quemPediu, quemPediu);

            if (esta < 0)
            {
                if (achou) Falar(quemPediu, "Nao consegui confirmar " + d.alvo + " -> " +
                                            d.grupo + ": o banco nao respondeu.");
                g_api->Log("[permission] !perm: NAO CONFERI %s -> %s", d.alvo.c_str(), d.grupo.c_str());
                continue;
            }
            if ((esta == 1) == d.esperado)
            {
                if (achou)
                    Falar(quemPediu, d.esperado
                          ? (d.alvo + " esta em " + d.grupo + " (" + QuandoVence(d.expira) + ").")
                          : (d.alvo + " saiu de " + d.grupo + "."));
                g_api->Log("[permission] !perm CONFIRMADO: %s %s %s", d.alvo.c_str(),
                           d.esperado ? "esta em" : "saiu de", d.grupo.c_str());
                // Tell the target, if they're online and actually JOINED.
                Quem alvo;
                if (d.esperado && AcharOnline(d.alvo, alvo))
                    Falar(alvo, "Voce entrou no grupo " + d.grupo +
                                " (" + QuandoVence(d.expira) + ").");
                continue;
            }
            if (achou)
                Falar(quemPediu, "NAO deu certo: o grupo \"" + d.grupo + "\" existe no "
                                 "permission.json? As chaves diferenciam maiusculas.");
            g_api->Log("[permission] !perm NAO ENTROU: %s -> %s (grupo inexistente?)",
                       d.alvo.c_str(), d.grupo.c_str());
        }
    }

    // Resolves "who" in a command: an account id, or the name of somebody ONLINE.
    bool Alvo(const std::string& texto, std::string& id, std::string& porque)
    {
        if (texto.find('#') == std::string::npos) { id = texto; return true; }

        void* pcs[128];
        const int n = g_api->FindObjects("ConanPlayerController", pcs, 128, 1);
        for (int i = 0; i < n; ++i)
        {
            Quem outro;
            if (Identificar(pcs[i], outro) && outro.nome == texto) { id = outro.id; return true; }
        }
        porque = "\"" + texto + "\" nao esta online. Para quem esta offline, use o id da conta "
                 "(aparece no listplayers do RCON).";
        return false;
    }

    void Ajuda(const Quem& q, bool admin)
    {
        Falar(q, "!perm eu  ·  !perm ver <jogador>  ·  !perm grupos");
        if (admin)
            Falar(q, "!perm dar <jogador> <grupo> [30d|12h|0]  ·  !perm tirar <jogador> <grupo>"
                     "  ·  !perm recarregar");
    }

    // ── the command ─────────────────────────────────────────────────────────
    ConanAcao Atender(ConanChamada* c)
    {
        char texto[256] = {0};
        if (!g_api || g_api->LerTextoDoJogo(c->Parms, OFF_MENSAGEM, texto, sizeof(texto)) <= 0)
            return CONAN_CONTINUAR;

        const std::string msg = Aparado(texto);
        // Exactly `!perm`, or `!perm ` with an argument. Without this,
        // `!permitir` (another plugin's command) would be swallowed by this
        // one.
        if (msg.compare(0, 5, "!perm") != 0) return CONAN_CONTINUAR;
        if (msg.size() > 5 && msg[5] != ' ')  return CONAN_CONTINUAR;

        Quem q;
        if (!Identificar(c->Obj, q))
        {
            g_api->Log("[permission] !perm de alguem que nao consegui identificar.");
            return CONAN_CONTINUAR;
        }

        const std::vector<std::string> p = Partir(msg.size() > 5 ? msg.substr(6) : "");
        const bool admin = EhAdmin(q);

        if (p.empty()) { Ajuda(q, admin); return CONAN_CANCELAR; }

        std::string sub = p[0];
        for (char& ch : sub) ch = char(std::tolower((unsigned char)ch));

        // ── leitura: qualquer um ────────────────────────────────────────────
        if (sub == "eu")
        {
            Falar(q, "Seus grupos: " + GruposDe(q.id));
            return CONAN_CANCELAR;
        }
        if (sub == "ver")
        {
            if (p.size() < 2) { Falar(q, "Use: !perm ver <jogador>"); return CONAN_CANCELAR; }
            std::string id, porque;
            if (!Alvo(p[1], id, porque)) { Falar(q, porque); return CONAN_CANCELAR; }
            Falar(q, p[1] + ": " + GruposDe(id));
            return CONAN_CANCELAR;
        }
        if (sub == "grupos")
        {
            // The groups that EXIST come from the snapshot itself, asking one
            // by one for the names the player has — there's no group listing in
            // the ABI, and inventing a direct database read here would be a
            // second truth about the same thing.
            Falar(q, "Os grupos configurados estao no permission.json. "
                     "Os seus: " + GruposDe(q.id));
            return CONAN_CANCELAR;
        }
        if (sub == "ajuda" || sub == "help") { Ajuda(q, admin); return CONAN_CANCELAR; }

        // ── writing: admin only ─────────────────────────────────────────────
        if (!admin)
        {
            Falar(q, "Voce nao tem permissao para isso (" + std::string(NO_ADMIN) + ").");
            return CONAN_CANCELAR;
        }

        if (sub == "dar")
        {
            if (p.size() < 3)
            { Falar(q, "Use: !perm dar <jogador> <grupo> [30d|12h|0]"); return CONAN_CANCELAR; }

            std::string id, porque;
            if (!Alvo(p[1], id, porque)) { Falar(q, porque); return CONAN_CANCELAR; }

            int64_t expira = 0;
            if (p.size() >= 4 && !LerPrazo(p[3], int64_t(std::time(nullptr)), expira, porque))
            { Falar(q, porque); return CONAN_CANCELAR; }

            const int32_t r = CmdConceder(id.c_str(), p[2].c_str(), expira,
                                          ("!perm de " + q.nome).c_str());
            // 1 = ACCEPTED FOR WRITING, not "written". The outcome comes in 2 s.
            if (r == 1)
            {
                g_pendentes.push_back({ q.id, id, p[2], true, expira });
                g_api->AgendarNaThreadDoJogo(ConfirmarPendentes, 2, nullptr, 0);
                Falar(q, "Pedido enviado: " + p[1] + " -> " + p[2] + ". Confirmo em instantes.");
                g_api->Log("[permission] %s pediu \"%s\" para %s (%s) — aguardando gravacao",
                           q.nome.c_str(), p[2].c_str(), id.c_str(), QuandoVence(expira).c_str());
            }
            else if (r == 0)
                Falar(q, "Recusado: o grupo \"" + p[2] + "\" nao existe no permission.json.");
            else
                Falar(q, "Nao consegui: o banco nao esta respondendo. NADA foi alterado.");
            return CONAN_CANCELAR;
        }

        if (sub == "tirar")
        {
            if (p.size() < 3)
            { Falar(q, "Use: !perm tirar <jogador> <grupo>"); return CONAN_CANCELAR; }
            std::string id, porque;
            if (!Alvo(p[1], id, porque)) { Falar(q, porque); return CONAN_CANCELAR; }

            const int32_t r = CmdRevogar(id.c_str(), p[2].c_str(),
                                         ("!perm de " + q.nome).c_str());
            if (r == 1)
            {
                g_pendentes.push_back({ q.id, id, p[2], false, 0 });
                g_api->AgendarNaThreadDoJogo(ConfirmarPendentes, 2, nullptr, 0);
                Falar(q, "Pedido enviado: tirar " + p[1] + " de " + p[2] + ". Confirmo em instantes.");
                g_api->Log("[permission] %s pediu para tirar \"%s\" de %s — aguardando gravacao",
                           q.nome.c_str(), p[2].c_str(), id.c_str());
            }
            else if (r == 0) Falar(q, "Recusado: grupo ou id invalido.");
            else             Falar(q, "Nao consegui: o banco nao esta respondendo.");
            return CONAN_CANCELAR;
        }

        if (sub == "recarregar")
        {
            Falar(q, CmdRecarregar()
                     ? "permission.json relido."
                     : "NAO releu — o arquivo tem erro. A configuracao anterior continua valendo.");
            return CONAN_CANCELAR;
        }

        Falar(q, "Nao conheco \"" + sub + "\". Use: eu, ver, grupos, dar, tirar, recarregar");
        return CONAN_CANCELAR;
    }
}  // namespace

extern "C" ConanAcao Permission_AoFalar(ConanChamada* c) { return Atender(c); }

bool LigarComandos(const ConanApiTabela* api)
{
    if (!api) return false;
    g_api = api;
    ConanApi::UsarTabela(api);

    // Priority 90: below ConanShop's (100), because `!perm` and `!shop` don't
    // collide and the order between them doesn't matter — but leaving room
    // above keeps a future plugin from having to fight for it.
    g_hook = api->HookProcessEvent("ServerSendChatMessage", Permission_AoFalar, nullptr, 90);
    if (!g_hook)
    {
        api->Log("[permission] AVISO: nao consegui enganchar o chat. O plugin segue "
                 "funcionando como servico; so' os comandos !perm ficam de fora.");
        return false;
    }
    api->Log("[permission] comandos de chat: !perm eu · ver · grupos · dar · tirar · recarregar");
    return true;
}

void DesligarComandos()
{
    if (g_hook && g_api && g_api->RemoverHook) g_api->RemoverHook(g_hook);
    g_hook = 0;
}

}  // namespace Perm
