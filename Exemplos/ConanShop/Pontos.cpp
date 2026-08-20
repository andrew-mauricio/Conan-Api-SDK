// Pontos (points) — implementation. The reasoning behind the decisions is in
// Pontos.h.
//
// TWO BACKENDS, ONE BEHAVIOUR
// ---------------------------
// SQLite and MySQL come in through the same four functions, and the shop's code
// doesn't know which one is live. What is NOT negotiable between them:
//
//   · the debit is a single UPDATE with the balance condition inside it;
//   · "how many rows changed" is read from the database, never inferred;
//   · every operation that fails returns failure. None returns "it worked" when
//     the database didn't answer.
//
// That third item looks obvious and is where shop plugins usually leak: an
// `Executar()` that returns void, or a swallowed error, becomes points that
// vanish or double with no trace left behind.
#include "Pontos.h"

#include "terceiros/sqlite3/sqlite3.h"
#include "MySqlCliente.h"

#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <mutex>

namespace Shop
{
namespace
{
    // ── the lock ────────────────────────────────────────────────────────────
    //
    // Timed credits run on the game thread; so do purchases. Today they're the
    // same thread, which is why this lock almost never contends. It exists
    // because "today they're the same" is a property of the API's scheduler, not
    // a guarantee of this class — and the day that stops being true, the symptom
    // would be a wrong balance under load, which is the most expensive kind of
    // defect to diagnose.
    std::mutex          g_trava;

    bool                g_mysql = false;
    sqlite3*            g_db    = nullptr;
    Perm::MySqlCliente* g_my    = nullptr;
    std::string         g_erroUltimo;

    // Optional log; the caller installs it. Without this, a database failure
    // dies silently inside the plugin.
    void (*g_log)(const char*) = nullptr;
    void Registrar(const char* fmt, ...)
    {
        if (!g_log) return;
        char linha[512];
        va_list a; va_start(a, fmt);
        std::vsnprintf(linha, sizeof(linha), fmt, a);
        va_end(a);
        g_log(linha);
    }

    // ── the schema ──────────────────────────────────────────────────────────
    //
    // `pontos` with CHECK >= 0 on SQLite: if some future path tries to leave the
    // balance negative, the database REFUSES instead of writing. It's the net
    // under the net — the condition in the UPDATE already prevents it, and this
    // line is what's left if someone adds a new path and forgets the condition.
    //
    // The ledger isn't decoration: when a player says "I bought it and never got
    // it", it's the only way to know what happened.
    const char* const ESQUEMA_SQLITE[] = {
        "CREATE TABLE IF NOT EXISTS carteira ("
        "  jogador TEXT PRIMARY KEY,"
        "  pontos  INTEGER NOT NULL DEFAULT 0 CHECK (pontos >= 0),"
        "  gasto   INTEGER NOT NULL DEFAULT 0,"
        "  visto   INTEGER NOT NULL DEFAULT 0);",
        "CREATE TABLE IF NOT EXISTS diario ("
        "  id      INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  quando  INTEGER NOT NULL,"
        "  jogador TEXT NOT NULL,"
        "  delta   INTEGER NOT NULL,"
        "  motivo  TEXT);",
        "CREATE INDEX IF NOT EXISTS diario_jogador ON diario(jogador, quando);",
        nullptr
    };

    const char* const ESQUEMA_MYSQL[] = {
        "CREATE TABLE IF NOT EXISTS carteira ("
        "  jogador VARCHAR(64) NOT NULL PRIMARY KEY,"
        "  pontos  BIGINT NOT NULL DEFAULT 0,"
        "  gasto   BIGINT NOT NULL DEFAULT 0,"
        "  visto   BIGINT NOT NULL DEFAULT 0"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;",
        "CREATE TABLE IF NOT EXISTS diario ("
        "  id      BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY,"
        "  quando  BIGINT NOT NULL,"
        "  jogador VARCHAR(64) NOT NULL,"
        "  delta   BIGINT NOT NULL,"
        "  motivo  VARCHAR(190),"
        "  KEY diario_jogador (jogador, quando)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;",
        nullptr
    };

    int64_t Agora()
    {
        // time() is enough: the ledger is for humans to read, not for
        // millisecond-precision ordering.
        return int64_t(std::time(nullptr));
    }

    // ── building SQL without truncating silently ────────────────────────────
    //
    // WHY THIS IS A FUNCTION, AND NOT A LOOSE snprintf IN EACH PLACE
    //
    // snprintf truncates quietly and returns the size it WOULD have needed. In a
    // statement about money, the most expensive possible cut falls between the
    // table and the condition:
    //
    //     UPDATE carteira SET pontos = pontos - 10 WHERE jogador='xxx
    //                                              ^ cut here
    //
    // If the cut leaves a quote open, MySQL refuses, and that's luck. If it
    // lands just AFTER the closing quote and before the `AND pontos >= N`, out
    // comes a valid UPDATE WITHOUT the balance condition, and the database
    // executes it happily. Worse still on a credit: a cut before the WHERE
    // credits the ENTIRE TABLE.
    //
    // With the account id capped at 64 characters this doesn't happen today; the
    // worst case sits near 230 bytes in a 512-byte buffer. But "doesn't happen
    // today" was never a guarantee: the id only has to grow, or someone has to
    // change the wallet's key, and the defect comes back without a line of
    // warning.
    //
    // Returns false when it didn't fit. The caller MUST treat that as failure
    // and execute nothing.
    bool MontarSql(char* destino, size_t tam, const char* fmt, ...)
    {
        va_list a;
        va_start(a, fmt);
        const int n = std::vsnprintf(destino, tam, fmt, a);
        va_end(a);
        if (n >= 0 && size_t(n) < tam) return true;
        Registrar("[shop] SQL statement did NOT fit (%d bytes, %d available). "
                  "Operation refused; nothing was changed.", n, int(tam) - 1);
        if (tam) destino[0] = 0;
        return false;
    }

    // ── SQLite ──────────────────────────────────────────────────────────────

    bool SqliteExecutar(const char* sql)
    {
        char* err = nullptr;
        if (sqlite3_exec(g_db, sql, nullptr, nullptr, &err) == SQLITE_OK) return true;
        g_erroUltimo = err ? err : "erro sem mensagem do sqlite";
        sqlite3_free(err);
        return false;
    }

    bool AbrirSqlite(const ConfigBanco& cfg, std::string& erro)
    {
        if (cfg.caminhoLocal.empty()) { erro = "caminho do banco local vazio"; return false; }

        const int rc = sqlite3_open_v2(cfg.caminhoLocal.c_str(), &g_db,
                                       SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
        if (rc != SQLITE_OK)
        {
            char m[512];
            std::snprintf(m, sizeof(m),
                "nao consegui abrir o banco em '%s' (codigo %d: %s). "
                "Quase sempre e' disco cheio, pasta somente-leitura, ou o caminho nao existir.",
                cfg.caminhoLocal.c_str(), rc, g_db ? sqlite3_errmsg(g_db) : "sem handle");
            erro = m;
            if (g_db) { sqlite3_close(g_db); g_db = nullptr; }
            return false;
        }

        // busy_timeout: Conan writes the world to SQLite as well, and a busy
        // disk must not turn into a refused purchase. Five seconds of waiting
        // before giving up.
        sqlite3_busy_timeout(g_db, 5000);

        // WAL so a reader doesn't block the writer. Anyone deleting the .db
        // has to delete -wal and -shm along with it, and that's stated in
        // config.json.
        if (!SqliteExecutar("PRAGMA journal_mode=WAL; PRAGMA foreign_keys=ON;"))
        { erro = g_erroUltimo; return false; }

        for (int i = 0; ESQUEMA_SQLITE[i]; ++i)
            if (!SqliteExecutar(ESQUEMA_SQLITE[i])) { erro = g_erroUltimo; return false; }

        return true;
    }

    // ── MySQL ───────────────────────────────────────────────────────────────

    bool AbrirMysql(const ConfigBanco& cfg, std::string& erro)
    {
        // These refusals are deliberate. Falling back to a default here —
        // 'root', or an invented database name — writes the players' points
        // somewhere the owner didn't choose and won't go looking.
        if (cfg.usuario.empty())
        { erro = "com banco mysql, mysql_usuario nao pode ficar vazio"; return false; }
        if (cfg.banco.empty())
        { erro = "com banco mysql, mysql_banco nao pode ficar vazio"; return false; }

        // Whitespace at either end doesn't show on screen and makes the value
        // look correct. The brackets in the message are what make it visible.
        auto temEspacoNaPonta = [](const std::string& s)
        { return !s.empty() && (std::isspace((unsigned char)s.front()) ||
                                std::isspace((unsigned char)s.back())); };
        for (auto par : { std::make_pair("mysql_host", &cfg.host),
                          std::make_pair("mysql_usuario", &cfg.usuario),
                          std::make_pair("mysql_banco", &cfg.banco) })
        {
            if (temEspacoNaPonta(*par.second))
            {
                char m[256];
                std::snprintf(m, sizeof(m),
                    "%s tem espaco no comeco ou no fim: [%s]. Copiar e colar traz espaco junto.",
                    par.first, par.second->c_str());
                erro = m;
                return false;
            }
        }

        g_my = new Perm::MySqlCliente();
        g_my->DefinirTempos(cfg.prazoConectarMs, cfg.prazoOperarMs);

        char err[512] = {0};
        if (!g_my->Conectar(cfg.host.c_str(), uint16_t(cfg.porta), cfg.usuario.c_str(),
                            cfg.senha.c_str(), cfg.banco.c_str(), err, sizeof(err)))
        {
            erro = err[0] ? err : "nao consegui conectar ao MySQL";
            delete g_my; g_my = nullptr;
            return false;
        }

        for (int i = 0; ESQUEMA_MYSQL[i]; ++i)
            if (!g_my->Executar(ESQUEMA_MYSQL[i], err, sizeof(err)))
            { erro = err[0] ? err : "falha ao criar as tabelas"; return false; }

        return true;
    }

    // Ensures the player's row exists. Without it the debit's UPDATE finds
    // nothing, and "no balance" and "new player" would be indistinguishable.
    bool GarantirJogador(const std::string& jogador)
    {
        if (g_mysql)
        {
            char sql[512], err[512];
            if (!MontarSql(sql, sizeof(sql),
                    "INSERT IGNORE INTO carteira (jogador, pontos, gasto, visto) VALUES (%s,0,0,%lld);",
                    Perm::MySqlCliente::Citar(jogador.c_str()).c_str(), (long long)Agora()))
                return false;
            return g_my->Executar(sql, err, sizeof(err));
        }
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(g_db,
                "INSERT OR IGNORE INTO carteira (jogador, pontos, gasto, visto) VALUES (?1,0,0,?2);",
                -1, &st, nullptr) != SQLITE_OK) return false;
        sqlite3_bind_text(st, 1, jogador.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 2, Agora());
        const bool ok = sqlite3_step(st) == SQLITE_DONE;
        sqlite3_finalize(st);
        return ok;
    }

    void Anotar(const std::string& jogador, int64_t delta, const char* motivo)
    {
        // The ledger never fails the operation: losing a line of history is
        // bad, refusing a credit because the history failed would be worse.
        if (g_mysql)
        {
            char sql[768], err[512];
            if (MontarSql(sql, sizeof(sql),
                    "INSERT INTO diario (quando, jogador, delta, motivo) VALUES (%lld,%s,%lld,%s);",
                    (long long)Agora(), Perm::MySqlCliente::Citar(jogador.c_str()).c_str(),
                    (long long)delta, Perm::MySqlCliente::Citar(motivo ? motivo : "").c_str()))
                g_my->Executar(sql, err, sizeof(err));
            return;
        }
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(g_db,
                "INSERT INTO diario (quando, jogador, delta, motivo) VALUES (?1,?2,?3,?4);",
                -1, &st, nullptr) != SQLITE_OK) return;
        sqlite3_bind_int64(st, 1, Agora());
        sqlite3_bind_text (st, 2, jogador.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 3, delta);
        sqlite3_bind_text (st, 4, motivo ? motivo : "", -1, SQLITE_TRANSIENT);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
}  // namespace

void DefinirLog(void (*log)(const char*)) { g_log = log; }

bool Abrir(const ConfigBanco& cfg, std::string& erro)
{
    std::lock_guard<std::mutex> t(g_trava);
    g_mysql = cfg.mysql;
    const bool ok = g_mysql ? AbrirMysql(cfg, erro) : AbrirSqlite(cfg, erro);
    if (ok)
        Registrar("[shop] banco %s pronto.", g_mysql ? "mysql" : "local (sqlite)");
    return ok;
}

void Fechar()
{
    std::lock_guard<std::mutex> t(g_trava);
    if (g_db) { sqlite3_close(g_db); g_db = nullptr; }
    if (g_my) { g_my->Desconectar(); delete g_my; g_my = nullptr; }
}

int64_t Saldo(const std::string& jogador)
{
    std::lock_guard<std::mutex> t(g_trava);
    if (jogador.empty()) return -1;

    if (g_mysql)
    {
        if (!g_my) return -1;
        struct Ctx { int64_t v; bool achou; } ctx{0, false};
        char sql[384], err[512];
        if (!MontarSql(sql, sizeof(sql), "SELECT pontos FROM carteira WHERE jogador=%s;",
                       Perm::MySqlCliente::Citar(jogador.c_str()).c_str()))
            return -1;
        const bool ok = g_my->Consultar(sql,
            [](void* c, int n, const char* const* v)
            {
                Ctx* x = static_cast<Ctx*>(c);
                if (n > 0 && v[0]) { x->v = std::strtoll(v[0], nullptr, 10); x->achou = true; }
            }, &ctx, err, sizeof(err));
        if (!ok) { Registrar("[shop] saldo: %s", err); return -1; }
        return ctx.achou ? ctx.v : 0;   // a player with no row yet has zero
    }

    if (!g_db) return -1;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(g_db, "SELECT pontos FROM carteira WHERE jogador=?1;",
                           -1, &st, nullptr) != SQLITE_OK) return -1;
    sqlite3_bind_text(st, 1, jogador.c_str(), -1, SQLITE_TRANSIENT);
    int64_t v = 0;
    const int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) v = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) return -1;
    return v;
}

bool Creditar(const std::string& jogador, int64_t quanto, const char* motivo)
{
    if (jogador.empty() || quanto <= 0) return false;
    std::lock_guard<std::mutex> t(g_trava);
    if (!GarantirJogador(jogador)) return false;

    if (g_mysql)
    {
        char sql[384], err[512];
        // A cut BEFORE the WHERE would credit EVERYONE's wallet.
        if (!MontarSql(sql, sizeof(sql),
                "UPDATE carteira SET pontos = pontos + %lld WHERE jogador=%s;",
                (long long)quanto, Perm::MySqlCliente::Citar(jogador.c_str()).c_str()))
            return false;
        if (!g_my->Executar(sql, err, sizeof(err)))
        { Registrar("[shop] creditar: %s", err); return false; }
    }
    else
    {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(g_db,
                "UPDATE carteira SET pontos = pontos + ?1 WHERE jogador=?2;",
                -1, &st, nullptr) != SQLITE_OK) return false;
        sqlite3_bind_int64(st, 1, quanto);
        sqlite3_bind_text (st, 2, jogador.c_str(), -1, SQLITE_TRANSIENT);
        const bool ok = sqlite3_step(st) == SQLITE_DONE;
        sqlite3_finalize(st);
        if (!ok) return false;
    }

    Anotar(jogador, quanto, motivo);
    return true;
}

// ── the debit: the operation this file exists to get right ──────────────────
Gasto Debitar(const std::string& jogador, int64_t quanto, const char* motivo)
{
    if (jogador.empty() || quanto <= 0) return Gasto::Erro;
    std::lock_guard<std::mutex> t(g_trava);
    if (!GarantirJogador(jogador)) return Gasto::Erro;

    if (g_mysql)
    {
        char sql[512], err[512];
        // A condicao de saldo vai DENTRO do UPDATE. Ver o cabecalho do .h.
        if (!MontarSql(sql, sizeof(sql),
                "UPDATE carteira SET pontos = pontos - %lld, gasto = gasto + %lld "
                "WHERE jogador=%s AND pontos >= %lld;",
                (long long)quanto, (long long)quanto,
                Perm::MySqlCliente::Citar(jogador.c_str()).c_str(), (long long)quanto))
            return Gasto::Erro;

        // Truncation is refused inside MontarSql, above, and it says in the
        // log which statement didn't fit. Why that's fatal here is explained at
        // its definition.
        if (!g_my->Executar(sql, err, sizeof(err)))
        { Registrar("[shop] debitar: %s", err); return Gasto::Erro; }

        // "Quantas linhas mudaram" vem do BANCO. Zero significa que a condicao
        // balance condition didn't pass, and there's nothing to undo, because
        // nothing changed.
        if (g_my->LinhasAfetadas() == 0) return Gasto::SemSaldo;
    }
    else
    {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(g_db,
                "UPDATE carteira SET pontos = pontos - ?1, gasto = gasto + ?1 "
                "WHERE jogador=?2 AND pontos >= ?1;",
                -1, &st, nullptr) != SQLITE_OK) return Gasto::Erro;
        sqlite3_bind_int64(st, 1, quanto);
        sqlite3_bind_text (st, 2, jogador.c_str(), -1, SQLITE_TRANSIENT);
        const bool ok = sqlite3_step(st) == SQLITE_DONE;
        sqlite3_finalize(st);
        if (!ok) return Gasto::Erro;
        if (sqlite3_changes(g_db) == 0) return Gasto::SemSaldo;
    }

    Anotar(jogador, -quanto, motivo);
    return Gasto::Ok;
}

bool Devolver(const std::string& jogador, int64_t quanto, const char* motivo)
{
    // A refund also reduces `gasto`, otherwise the statistic of how much a
    // spent would start counting purchases that never happened.
    if (jogador.empty() || quanto <= 0) return false;
    {
        std::lock_guard<std::mutex> t(g_trava);
        if (g_mysql && g_my)
        {
            char sql[384], err[512];
            if (MontarSql(sql, sizeof(sql),
                    "UPDATE carteira SET gasto = GREATEST(gasto - %lld, 0) WHERE jogador=%s;",
                    (long long)quanto, Perm::MySqlCliente::Citar(jogador.c_str()).c_str()))
                g_my->Executar(sql, err, sizeof(err));
        }
        else if (g_db)
        {
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(g_db,
                    "UPDATE carteira SET gasto = MAX(gasto - ?1, 0) WHERE jogador=?2;",
                    -1, &st, nullptr) == SQLITE_OK)
            {
                sqlite3_bind_int64(st, 1, quanto);
                sqlite3_bind_text (st, 2, jogador.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_step(st);
                sqlite3_finalize(st);
            }
        }
    }
    return Creditar(jogador, quanto, motivo);
}

}  // namespace Shop
