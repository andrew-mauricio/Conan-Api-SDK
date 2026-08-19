// ============================================================================
//  BancoSqlite.cpp — o meio que sempre existiu, agora atrás da interface
//
//  ESTA MUDANÇA É MECÂNICA DE PROPÓSITO. As chamadas sqlite3_* que estavam
//  espalhadas pelo Armazem estão aqui, com os MESMOS argumentos, os MESMOS
//  pragmas e a MESMA prova de que o WAL pegou. Nada de comportamento foi
//  "melhorado de passagem": quem quiser conferir que o SQLite continua fazendo
//  o que fazia compara este arquivo com o Armazem.cpp anterior, e não precisa
//  ler mais nada.
//
//  O QUE NÃO PODE SUMIR DAQUI (e por quê)
//  --------------------------------------
//  · WAL. O save do próprio jogo é SQLite em WAL, sob Wine, neste mesmo
//    processo — é o caminho provado em produção pelo fabricante do jogo. Dá
//    commit atômico e recuperação na abertura: queda no meio da escrita ou
//    vale a transação inteira, ou não vale nada dela.
//  · synchronous=FULL. Com NORMAL, queda de ENERGIA pode perder os últimos
//    commits — o arquivo continua íntegro e o VIP comprado some. Escrita aqui
//    é rara (poucas por hora), então o fsync é grátis na prática.
//  · A PROVA de que o WAL pegou. Pedir não é obter: em sistema de arquivos sem
//    mmap compartilhado o SQLite volta para "delete" em SILÊNCIO, e a garantia
//    pela qual se escolheu SQLite deixa de existir sem ninguém ser avisado.
//  · journal_size_limit. Sem teto o -wal cresce sem parar num servidor que fica
//    meses no ar — e é o -wal que a lição do wipe deste projeto ensinou a não
//    esquecer (apagar o .db sem apagar o -wal faz o dado RESSUSCITAR).
//  · FULLMUTEX. A thread escritora e a thread de arranque tocam o mesmo handle.
//    NOMUTEX seria mais rápido e exigiria provar que nunca há duas threads no
//    handle — prova que uma futura mudança quebra em silêncio.
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
    // Índice fora da faixa é comportamento INDEFINIDO no sqlite3_column_*; a
    // barreira é aqui e não na documentação, porque um índice errado é erro de
    // digitação normal e o preço dele não pode ser leitura fora dos limites
    // dentro do processo do servidor de jogo.
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

    // Um arquivo local não "cai no meio". Se o handle existe, ele serve; se não
    // existe, reabrir aqui esconderia o motivo de o Abrir ter falhado. Devolver
    // o estado real é o certo — a camada de cima já sabe lidar com false.
    bool Reconectar() override
    {
        if (m_db) return true;
        m_erro = "o banco sqlite nao esta aberto; nao ha o que reconectar";
        return false;
    }

    // Não há o que interromper: o sqlite é arquivo local e a operação mais
    // longa daqui é um fsync. Nada aqui espera rede, então a thread escritora
    // nunca fica presa por tempo que valha a pena cortar.
    //
    // Não é "nada a fazer, então tanto faz": é a resposta CERTA deste meio, e
    // ela está escrita para ninguém achar que faltou implementar. Fechar o
    // handle daqui seria o oposto do pedido — sqlite3_close com statement em
    // uso na outra thread devolve SQLITE_BUSY e deixa o banco meio fechado.
    void Interromper() override {}

    bool Executar(const char* sql) override;
    bool Consultar(const char* sql, FnLinha fn, void* ctx) override;
    std::unique_ptr<IComando> Preparar(const char* sql) override;

    // BEGIN IMMEDIATE, e não BEGIN: pega a trava de escrita JÁ, em vez de
    // descobrir no primeiro INSERT que outro processo está escrevendo e ter de
    // desfazer o que já leu.
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

    // Banco travado por outra coisa: esperar em vez de falhar.
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

    // A prova de que o WAL pegou. Ver o cabeçalho deste arquivo.
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

// SQLITE_TRANSIENT: o SQLite COPIA a string agora. Com SQLITE_STATIC ele
// guardaria o ponteiro, e quem liga um `std::string` temporário teria o texto
// liberado embaixo antes do step. É o valor que o código anterior já usava nos
// caminhos de escrita, e está mantido.
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
    // Um comando "sem resultado" que devolve linha (SELECT por engano) é lido
    // até o fim; parar no meio deixaria a transação com um statement aberto.
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
}   // namespace anônimo

std::unique_ptr<IBanco> CriarBancoSqlite(const ConfigBanco& cfg, FnLog log)
{ return std::unique_ptr<IBanco>(new BancoSqlite(cfg, log)); }

}   // namespace Perm
