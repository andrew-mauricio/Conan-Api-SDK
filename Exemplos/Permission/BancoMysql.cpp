// ============================================================================
//  BancoMysql.cpp — the same Armazem, talking to the server owner's MySQL
//
//  ┌──────────────────────────────────────────────────────────────────────┐
//  │  THIS BLOCKS. Connecting waits on the network; Executar waits for a  │
//  │  reply. NOTHING HERE MAY BE CALLED ON THE GAME'S THREAD.             │
//  │  What guarantees that is Armazem's design: a permission read answers │
//  │  from the in-memory snapshot and touches no database at all; only    │
//  │  the writer thread and startup get this far.                         │
//  └──────────────────────────────────────────────────────────────────────┘
//
//  THE PART THAT DESERVES A CLOSE LOOK: THERE ARE NO PREPARED STATEMENTS
//  ---------------------------------------------------------------------
//  This house's MySqlCliente speaks COM_QUERY and nothing else (see
//  MySqlCliente.h). So `?1` isn't bound by the protocol: it is SUBSTITUTED into
//  the text, by the literal MySqlCliente::Citar() produced — Citar escapes AND
//  wraps in quotes, and the escape/quote pair was exercised with 15 injection
//  payloads against two real servers.
//
//  The substitution is this file's new risk surface, which is why it's
//  restrictive rather than permissive:
//    · it SKIPS anything inside a literal ('...') and inside backticks
//      (`...`), including both escape forms ('' and \'). A `?1` written inside
//      a piece of text is text, and replacing it would corrupt the data;
//    · a `?` not followed by a digit is a REFUSAL — there's no positional `?`
//      in this code, and accepting one would open the door to what wasn't
//      anticipated;
//    · a placeholder with no bound value is a REFUSAL. SQL with a loose `?3`
//      is NOT sent. Fail closed: a command that didn't run is a visible
//      problem; a command that ran with a placeholder turned into a literal is
//      wrong data in silence.
//
//  That's the third layer, not the first. The other two, proved running in
//  MySqlCliente: CLIENT_MULTI_STATEMENTS OFF (a `'; DROP TABLE` has no way to
//  become a second command) and utf8mb4 forced (in GBK/SJIS/BIG5, byte-by-byte
//  escaping has holes).
//
//  WHAT THIS FILE DOES NOT DO
//  --------------------------
//  · it doesn't reconnect on its own in the middle of an operation. A hidden
//    reconnect would re-run a command that may already have been applied on the
//    server. Deciding to retry is Armazem's job, since it knows which tasks are
//    idempotent;
//  · it doesn't speak TLS. The password never goes over the wire in the clear,
//    but the queries travel unencrypted (see the top of MySqlCliente.cpp).
//    Acceptable for a MySQL on the same machine or on a private network; not
//    for a MySQL exposed to the internet.
// ============================================================================
#include "Banco.h"
#include "MySqlCliente.h"

#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdlib>

namespace Perm
{
namespace
{
// ── one row of a MySQL result ───────────────────────────────────────────────
//
// The text protocol hands EVERYTHING back as a string; `Inteiro` converts. NULL
// arrives as nullptr and becomes 0 — the columns where Armazem calls Inteiro
// are all declared NOT NULL, so the 0 is a safety net, not the normal path.
class LinhaMysql : public ILinha
{
public:
    LinhaMysql(int n, const char* const* v) : m_n(n), m_v(v) {}
    int Colunas() const override { return m_n; }
    const char* Texto(int c) const override
    { return (c >= 0 && c < m_n) ? m_v[c] : nullptr; }
    int64_t Inteiro(int c) const override
    {
        const char* t = Texto(c);
        return t ? std::strtoll(t, nullptr, 10) : 0;
    }
private:
    int                m_n;
    const char* const* m_v;
};

class BancoMysql;

// ── substituting `?N` ───────────────────────────────────────────────────────
//
// Returns false and writes the reason into `erro` rather than producing
// questionable SQL.
bool Substituir(const char* sql, const std::vector<std::string>& valores,
                const std::vector<bool>& ligado, std::string& saida, std::string& erro)
{
    saida.clear();
    if (!sql) { erro = "SQL nulo"; return false; }
    saida.reserve(std::strlen(sql) + 64);

    for (const char* p = sql; *p; )
    {
        // a text literal: copy it whole without looking inside
        if (*p == '\'')
        {
            saida += *p++;
            while (*p)
            {
                if (*p == '\\' && p[1]) { saida += *p++; saida += *p++; continue; }
                if (*p == '\'' && p[1] == '\'') { saida += *p++; saida += *p++; continue; }
                if (*p == '\'') { saida += *p++; break; }
                saida += *p++;
            }
            continue;
        }
        // a backticked identifier: same rule
        if (*p == '`')
        {
            saida += *p++;
            while (*p) { const bool fim = (*p == '`'); saida += *p++; if (fim) break; }
            continue;
        }
        if (*p != '?') { saida += *p++; continue; }

        ++p;
        if (*p < '0' || *p > '9')
        {
            erro = "o SQL tem um '?' que nao e seguido de numero. Este codigo so "
                   "usa marcador numerado (?1, ?2, ...); recusei em vez de adivinhar.";
            return false;
        }
        int idx = 0;
        while (*p >= '0' && *p <= '9') { idx = idx * 10 + (*p - '0'); ++p; }
        if (idx < 1 || static_cast<size_t>(idx) > ligado.size() || !ligado[idx - 1])
        {
            char b[220];
            std::snprintf(b, sizeof(b),
                "o marcador ?%d nao tem valor ligado. NAO enviei este comando: "
                "SQL com marcador solto grava dado errado sem dar erro.", idx);
            erro = b;
            return false;
        }
        saida += valores[static_cast<size_t>(idx) - 1];
    }
    return true;
}

class ComandoMysql : public IComando
{
public:
    ComandoMysql(BancoMysql* b, const char* sql) : m_b(b), m_sql(sql ? sql : "") {}

    bool LigarTexto(int pos, const char* v) override
    {
        if (!Espaco(pos)) return false;
        // Citar() escapes AND adds the quotes. nullptr becomes SQL NULL —
        // which is a different thing from an empty string, and the database
        // tells the two apart.
        m_valores[static_cast<size_t>(pos) - 1] = v ? MySqlCliente::Citar(v) : std::string("NULL");
        m_ligado[static_cast<size_t>(pos) - 1]  = true;
        return true;
    }

    bool LigarInteiro(int pos, int64_t v) override
    {
        if (!Espaco(pos)) return false;
        char b[32];
        std::snprintf(b, sizeof(b), "%lld", static_cast<long long>(v));
        m_valores[static_cast<size_t>(pos) - 1] = b;
        m_ligado[static_cast<size_t>(pos) - 1]  = true;
        return true;
    }

    bool Executar() override;
    bool Consultar(FnLinha fn, void* ctx) override;
    int64_t Mudancas() const override { return m_mudancas; }

private:
    bool Espaco(int pos);

    BancoMysql*              m_b;
    std::string              m_sql;
    std::vector<std::string> m_valores;
    std::vector<bool>        m_ligado;
    int64_t                  m_mudancas = 0;
};

class BancoMysql : public IBanco
{
public:
    BancoMysql(const ConfigBanco& cfg, FnLog log) : m_cfg(cfg), m_log(log) {}
    ~BancoMysql() override { Fechar(); }

    bool Abrir() override;
    void Fechar() override { m_c.Desconectar(); }
    bool Vivo() const override { return m_c.Conectado(); }
    bool Reconectar() override;

    // Called from ANOTHER thread, only at shutdown. See
    // MySqlCliente::Interromper for why shutdown() and not close(): closing the
    // descriptor from here, with the writer thread still inside a recv(),
    // leaves the OS free to reuse the number — and the other sockets in this
    // process belong to the game server.
    void Interromper() override { m_c.Interromper(); }

    bool Executar(const char* sql) override;
    bool Consultar(const char* sql, FnLinha fn, void* ctx) override;
    std::unique_ptr<IComando> Preparar(const char* sql) override
    { return std::unique_ptr<IComando>(new ComandoMysql(this, sql)); }

    bool Iniciar()   override { return Executar("START TRANSACTION;"); }
    bool Confirmar() override { return Executar("COMMIT;"); }
    bool Desfazer()  override { return Executar("ROLLBACK;"); }

    const char* Erro() const override { return m_erro.c_str(); }
    const char* Nome() const override { return "mysql"; }
    const Sql&  S()    const override { return SqlDoMysql(); }
    const char* DicaEsquema() const override { return m_dica.c_str(); }

    MySqlCliente& C()               { return m_c; }
    void          PorErro(const std::string& e) { m_erro = e; }
    void Registrar(const char* fmt, ...);

private:
    bool Conectar();

    ConfigBanco  m_cfg;
    FnLog        m_log = nullptr;
    MySqlCliente m_c;
    std::string  m_erro;
    std::string  m_dica;
};

void BancoMysql::Registrar(const char* fmt, ...)
{
    if (!m_log) return;
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    m_log(buf);
}

// The client's diagnostics (server version, authentication method, RSA cost)
// go into the plugin's own log. Without that, "why did the connection take
// 4 s?" has no answer on the owner's machine.
void PonteDeLog(void* ctx, const char* linha)
{
    BancoMysql* b = static_cast<BancoMysql*>(ctx);
    if (b && linha) b->Registrar("[permission] %s", linha);
}

bool BancoMysql::Conectar()
{
    m_c.DefinirTempos(m_cfg.msConectar, m_cfg.msOperar);
    m_c.DefinirLog(PonteDeLog, this);

    char erro[1024] = {0};
    if (!m_c.Conectar(m_cfg.host.c_str(), m_cfg.porta, m_cfg.usuario.c_str(),
                      m_cfg.senha.c_str(), m_cfg.banco.c_str(), erro, sizeof(erro)))
    {
        m_erro = erro[0] ? erro : "nao consegui conectar ao MySQL";
        return false;
    }
    return true;
}

bool BancoMysql::Abrir()
{
    // The hint is built here because it names THIS server owner's database and
    // user. A generic error message ("permission denied") makes a non-programmer
    // open a ticket; a message with the command ready to paste lets them fix it
    // themselves.
    {
        char b[700];
        std::snprintf(b, sizeof(b),
            "No MySQL isso quase sempre e falta de permissao para criar tabela. "
            "Rode, como root do MySQL: "
            "CREATE DATABASE IF NOT EXISTS `%s` CHARACTER SET utf8mb4; "
            "GRANT ALL ON `%s`.* TO '%s'@'%%'; FLUSH PRIVILEGES;",
            m_cfg.banco.c_str(), m_cfg.banco.c_str(), m_cfg.usuario.c_str());
        m_dica = b;
    }

    Registrar("[permission] banco: MySQL em %s:%u, banco '%s', usuario '%s' "
              "(prazos: %d ms para conectar, %d ms por operacao)",
              m_cfg.host.c_str(), unsigned(m_cfg.porta), m_cfg.banco.c_str(),
              m_cfg.usuario.c_str(), m_cfg.msConectar, m_cfg.msOperar);
    return Conectar();
}

bool BancoMysql::Reconectar()
{
    m_c.Desconectar();
    if (!Conectar()) return false;
    Registrar("[permission] reconectado ao MySQL (%s)", m_c.VersaoServidor());
    return true;
}

bool BancoMysql::Executar(const char* sql)
{
    char erro[1024] = {0};
    if (!m_c.Executar(sql, erro, sizeof(erro)))
    {
        char b[1300];
        std::snprintf(b, sizeof(b), "%s   [SQL: %.200s]", erro[0] ? erro : "?", sql);
        m_erro = b;
        return false;
    }
    return true;
}

struct PonteLinha { FnLinha fn; void* ctx; int64_t contadas; };

void AoChegarLinha(void* p, int ncols, const char* const* valores)
{
    PonteLinha* b = static_cast<PonteLinha*>(p);
    LinhaMysql l(ncols, valores);
    b->fn(l, b->ctx);
    ++b->contadas;
}

bool BancoMysql::Consultar(const char* sql, FnLinha fn, void* ctx)
{
    PonteLinha ponte{ fn, ctx, 0 };
    char erro[1024] = {0};
    if (!m_c.Consultar(sql, AoChegarLinha, &ponte, erro, sizeof(erro)))
    {
        char b[1300];
        std::snprintf(b, sizeof(b), "%s   [SQL: %.200s]", erro[0] ? erro : "?", sql);
        m_erro = b;
        return false;
    }
    return true;
}

bool ComandoMysql::Espaco(int pos)
{
    if (pos < 1 || pos > 64)
    {
        m_b->PorErro("posicao de parametro fora da faixa (1..64)");
        return false;
    }
    if (m_valores.size() < static_cast<size_t>(pos))
    {
        m_valores.resize(static_cast<size_t>(pos));
        m_ligado.resize(static_cast<size_t>(pos), false);
    }
    return true;
}

bool ComandoMysql::Executar()
{
    m_mudancas = 0;
    std::string sql, erro;
    if (!Substituir(m_sql.c_str(), m_valores, m_ligado, sql, erro))
    {
        char b[1300];
        std::snprintf(b, sizeof(b), "%s   [SQL: %.200s]", erro.c_str(), m_sql.c_str());
        m_b->PorErro(b);
        return false;
    }
    if (!m_b->Executar(sql.c_str())) return false;
    m_mudancas = static_cast<int64_t>(m_b->C().LinhasAfetadas());
    return true;
}

bool ComandoMysql::Consultar(FnLinha fn, void* ctx)
{
    m_mudancas = 0;
    std::string sql, erro;
    if (!Substituir(m_sql.c_str(), m_valores, m_ligado, sql, erro))
    {
        char b[1300];
        std::snprintf(b, sizeof(b), "%s   [SQL: %.200s]", erro.c_str(), m_sql.c_str());
        m_b->PorErro(b);
        return false;
    }
    if (!m_b->Consultar(sql.c_str(), fn, ctx)) return false;
    m_mudancas = static_cast<int64_t>(m_b->C().LinhasAfetadas());
    return true;
}
}   // anonymous namespace

std::unique_ptr<IBanco> CriarBancoMysql(const ConfigBanco& cfg, FnLog log)
{ return std::unique_ptr<IBanco>(new BancoMysql(cfg, log)); }

// ── exposed purely so the test can calibrate the substitution ───────────────
//
// A new guard needs proof from BOTH sides: that it refuses what it must refuse
// and accepts what it must accept. Without this door, the substituter would
// only be exercised indirectly and "0 defects" would be worth nothing.
bool SubstituirMarcadoresParaTeste(const char* sql,
                                   const std::vector<std::string>& valoresJaCitados,
                                   const std::vector<bool>& ligado,
                                   std::string& saida, std::string& erro)
{ return Substituir(sql, valoresJaCitados, ligado, saida, erro); }

}   // namespace Perm
