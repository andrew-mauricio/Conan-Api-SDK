// ============================================================================
//  BancoSqlite.cpp — the medium that was always here, now behind the interface
//
//  THIS CHANGE IS MECHANICAL ON PURPOSE. The sqlite3_* calls that were
//  scattered through Armazem are here, with the SAME arguments, the SAME
//  pragmas and the SAME proof that WAL took. No behaviour was "improved in
//  passing": anyone wanting to check that SQLite still does what it did
//  compares this file with the previous Armazem.cpp, and needs to read nothing
//  else.
//
//  WHAT MUST NOT DISAPPEAR FROM HERE (and why)
//  -------------------------------------------
//  · WAL. The game's own save is SQLite in WAL, under Wine, in this very
//    process — it's the path the game's maker proved in production. It gives
//    atomic commit and recovery on open: a crash mid-write either counts the
//    whole transaction or none of it.
//  · synchronous=FULL. With NORMAL, a POWER cut can lose the last commits — the
//    file stays intact and the purchased VIP vanishes. Writing here is rare (a
//    few an hour), so the fsync is free in practice.
//  · THE PROOF that WAL took. Asking isn't getting: on a filesystem without
//    shared mmap, SQLite falls back to "delete" SILENTLY, and the guarantee
//    SQLite was chosen for stops existing with nobody being told.
//  · journal_size_limit. With no cap the -wal grows without end on a server
//    that stays up for months — and it's the -wal that this project's wipe
//    lesson taught us not to forget (deleting the .db without deleting the -wal
//    makes the data COME BACK).
//  · FULLMUTEX. The writer thread and the startup thread touch the same handle.
//    NOMUTEX would be faster and would need a proof that two threads are never
//    on the handle — a proof a future change breaks in silence.
// ============================================================================
#include "Banco.h"
#include "terceiros/sqlite3/sqlite3.h"

#include <cstdio>
#include <cstdarg>
#include <cstring>

namespace Perm
{
namespace
{
class LinhaSqlite : public ILinha
{
public:
    explicit LinhaSqlite(sqlite3_stmt* st) : m_st(st) {}
    int Colunas() const override { return sqlite3_column_count(m_st); }
    // An out-of-range index is UNDEFINED behaviour in sqlite3_column_*; the
    // barrier is here and not in the documentation, because a wrong index is an
    // ordinary typo and its price can't be an out-of-bounds read inside the
    // game server's process.
    const char* Texto(int c) const override
    {
        if (c < 0 || c >= sqlite3_column_count(m_st)) return nullptr;
        const unsigned char* t = sqlite3_column_text(m_st, c);
        return t ? reinterpret_cast<const char*>(t) : nullptr;
    }
    int64_t Inteiro(int c) const override
    {
        if (c < 0 || c >= sqlite3_column_count(m_st)) return 0;
        return sqlite3_column_int64(m_st, c);
    }
private:
    sqlite3_stmt* m_st;
};

class BancoSqlite;

class ComandoSqlite : public IComando
{
public:
    ComandoSqlite(BancoSqlite* b, sqlite3* db, sqlite3_stmt* st)
        : m_b(b), m_db(db), m_st(st) {}
    ~ComandoSqlite() override { if (m_st) sqlite3_finalize(m_st); }

    bool LigarTexto(int pos, const char* v) override;
    bool LigarInteiro(int pos, int64_t v) override;
    bool Executar() override;
    bool Consultar(FnLinha fn, void* ctx) override;
    int64_t Mudancas() const override { return m_mudancas; }

private:
    BancoSqlite*  m_b;
    sqlite3*      m_db;
    sqlite3_stmt* m_st;
    int64_t       m_mudancas = 0;
};

class BancoSqlite : public IBanco
{
public:
    BancoSqlite(const ConfigBanco& cfg, FnLog log) : m_cfg(cfg), m_log(log) {}
    ~BancoSqlite() override { Fechar(); }

    bool Abrir() override;
    void Fechar() override;
    bool Vivo() const override { return m_db != nullptr; }

    // A local file doesn't "drop mid-way". If the handle exists it serves; if
    // it doesn't, reopening here would hide why Abrir failed. Returning the
    // real state is the right call — the layer above already handles false.
    bool Reconectar() override
    {
        if (m_db) return true;
        m_erro = "o banco sqlite nao esta aberto; nao ha o que reconectar";
        return false;
    }

    // There's nothing to interrupt: sqlite is a local file and the longest
    // operation here is an fsync. Nothing here waits on a network, so the
    // writer thread is never stuck for a time worth cutting short.
    //
    // This isn't "nothing to do, so whatever": it's this medium's RIGHT answer,
    // and it's written down so nobody thinks the implementation is missing.
    // Closing the handle from here would be the opposite of what was asked —
    // sqlite3_close with a statement in use on the other thread returns
    // SQLITE_BUSY and leaves the database half-closed.
    void Interromper() override {}

    bool Executar(const char* sql) override;
    bool Consultar(const char* sql, FnLinha fn, void* ctx) override;
    std::unique_ptr<IComando> Preparar(const char* sql) override;

    // BEGIN IMMEDIATE, not BEGIN: it takes the write lock NOW, instead of
    // finding out at the first INSERT that another process is writing and
    // having to undo what it already read.
    bool Iniciar()   override { return Executar("BEGIN IMMEDIATE;"); }
    bool Confirmar() override { return Executar("COMMIT;"); }
    bool Desfazer()  override { return Executar("ROLLBACK;"); }

    const char* Erro() const override { return m_erro.c_str(); }
    const char* Nome() const override { return "sqlite"; }
    const Sql&  S()    const override { return SqlDoSqlite(); }
    const char* DicaEsquema() const override { return m_dica.c_str(); }

    void Registrar(const char* fmt, ...);
    void GuardarErro(const char* onde);

    sqlite3* Db() { return m_db; }

private:
    ConfigBanco m_cfg;
    FnLog       m_log = nullptr;
    sqlite3*    m_db  = nullptr;
    std::string m_erro;
    std::string m_dica =
        "No sqlite isso quase sempre e disco cheio, arquivo somente-leitura, ou "
        "um permission.db de uma versao mais nova do plugin. Confira o espaco em "
        "disco e a permissao de escrita na pasta do plugin.";
};

void BancoSqlite::Registrar(const char* fmt, ...)
{
    if (!m_log) return;
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    m_log(buf);
}

void BancoSqlite::GuardarErro(const char* onde)
{
    char b[900];
    std::snprintf(b, sizeof(b), "%s: %s", onde,
                  m_db ? sqlite3_errmsg(m_db) : "banco fechado");
    m_erro = b;
}

bool BancoSqlite::Abrir()
{
    if (m_db) { m_erro = "Abrir chamado duas vezes"; return false; }
    if (m_cfg.caminhoSqlite.empty()) { m_erro = "caminho do banco vazio"; return false; }

    int rc = sqlite3_open_v2(m_cfg.caminhoSqlite.c_str(), &m_db,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                             SQLITE_OPEN_FULLMUTEX, nullptr);
    if (rc != SQLITE_OK)
    {
        char b[900];
        std::snprintf(b, sizeof(b),
            "nao consegui abrir o banco em '%s' (codigo %d do sqlite: %s). "
            "Confira se a pasta existe e se o servidor tem permissao de escrita nela.",
            m_cfg.caminhoSqlite.c_str(), rc, m_db ? sqlite3_errmsg(m_db) : "sem handle");
        m_erro = b;
        if (m_db) { sqlite3_close(m_db); m_db = nullptr; }
        return false;
    }

    // Database locked by something else: wait rather than fail.
    sqlite3_busy_timeout(m_db, 5000);

    char* err = nullptr;
    const char* pragmas =
        "PRAGMA journal_mode=WAL;"
        "PRAGMA synchronous=FULL;"
        "PRAGMA foreign_keys=ON;"
        "PRAGMA journal_size_limit=4194304;";
    if (sqlite3_exec(m_db, pragmas, nullptr, nullptr, &err) != SQLITE_OK)
    {
        char b[900];
        std::snprintf(b, sizeof(b), "pragmas falharam: %s", err ? err : "?");
        m_erro = b;
        sqlite3_free(err);
        sqlite3_close(m_db); m_db = nullptr;
        return false;
    }
    sqlite3_free(err);

    // The proof that WAL took. See this file's header.
    {
        sqlite3_stmt* st = nullptr;
        std::string modo;
        if (sqlite3_prepare_v2(m_db, "PRAGMA journal_mode;", -1, &st, nullptr) == SQLITE_OK
            && sqlite3_step(st) == SQLITE_ROW)
        {
            const unsigned char* t = sqlite3_column_text(st, 0);
            modo = t ? reinterpret_cast<const char*>(t) : "";
        }
        sqlite3_finalize(st);
        if (modo != "wal")
            Registrar("[permission] ATENCAO: journal_mode saiu '%s', nao 'wal'. "
                      "O commit continua atomico, mas a recuperacao e mais fraca. "
                      "Isso costuma ser sistema de arquivos de rede.", modo.c_str());
        else
            Registrar("[permission] banco em WAL: %s", m_cfg.caminhoSqlite.c_str());
    }
    return true;
}

void BancoSqlite::Fechar()
{
    if (m_db) { sqlite3_close(m_db); m_db = nullptr; }
}

bool BancoSqlite::Executar(const char* sql)
{
    if (!m_db) { m_erro = "banco fechado"; return false; }
    char* err = nullptr;
    if (sqlite3_exec(m_db, sql, nullptr, nullptr, &err) != SQLITE_OK)
    {
        char b[900];
        std::snprintf(b, sizeof(b), "%s   [SQL: %.200s]", err ? err : "?", sql);
        m_erro = b;
        sqlite3_free(err);
        return false;
    }
    sqlite3_free(err);
    return true;
}

bool BancoSqlite::Consultar(const char* sql, FnLinha fn, void* ctx)
{
    if (!m_db) { m_erro = "banco fechado"; return false; }
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &st, nullptr) != SQLITE_OK)
    {
        char b[900];
        std::snprintf(b, sizeof(b), "%s   [SQL: %.200s]", sqlite3_errmsg(m_db), sql);
        m_erro = b;
        return false;
    }
    LinhaSqlite l(st);
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) fn(l, ctx);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) { GuardarErro("consulta parou no meio"); return false; }
    return true;
}

std::unique_ptr<IComando> BancoSqlite::Preparar(const char* sql)
{
    if (!m_db) { m_erro = "banco fechado"; return nullptr; }
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &st, nullptr) != SQLITE_OK)
    {
        char b[900];
        std::snprintf(b, sizeof(b), "%s   [SQL: %.200s]", sqlite3_errmsg(m_db), sql);
        m_erro = b;
        if (st) sqlite3_finalize(st);
        return nullptr;
    }
    return std::unique_ptr<IComando>(new ComandoSqlite(this, m_db, st));
}

// SQLITE_TRANSIENT: SQLite COPIES the string now. With SQLITE_STATIC it would
// keep the pointer, and anyone binding a temporary `std::string` would have the
// text freed underneath before the step. It's the value the previous code
// already used on the write paths, and it's kept.
bool ComandoSqlite::LigarTexto(int pos, const char* v)
{
    const int rc = v ? sqlite3_bind_text(m_st, pos, v, -1, SQLITE_TRANSIENT)
                     : sqlite3_bind_null(m_st, pos);
    if (rc != SQLITE_OK) { m_b->GuardarErro("ligar texto"); return false; }
    return true;
}

bool ComandoSqlite::LigarInteiro(int pos, int64_t v)
{
    if (sqlite3_bind_int64(m_st, pos, v) != SQLITE_OK)
    { m_b->GuardarErro("ligar inteiro"); return false; }
    return true;
}

bool ComandoSqlite::Executar()
{
    m_mudancas = 0;
    sqlite3_reset(m_st);
    // A "no result" command that returns rows (a SELECT by mistake) is read to
    // the end; stopping halfway would leave the transaction with an open
    // statement.
    int rc;
    while ((rc = sqlite3_step(m_st)) == SQLITE_ROW) {}
    if (rc != SQLITE_DONE) { m_b->GuardarErro("executar"); return false; }
    m_mudancas = sqlite3_changes(m_db);
    return true;
}

bool ComandoSqlite::Consultar(FnLinha fn, void* ctx)
{
    m_mudancas = 0;
    sqlite3_reset(m_st);
    LinhaSqlite l(m_st);
    int rc;
    while ((rc = sqlite3_step(m_st)) == SQLITE_ROW) { fn(l, ctx); ++m_mudancas; }
    if (rc != SQLITE_DONE) { m_b->GuardarErro("consultar"); return false; }
    return true;
}
}   // anonymous namespace

std::unique_ptr<IBanco> CriarBancoSqlite(const ConfigBanco& cfg, FnLog log)
{ return std::unique_ptr<IBanco>(new BancoSqlite(cfg, log)); }

}   // namespace Perm
