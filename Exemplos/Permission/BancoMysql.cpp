// ============================================================================
//  BancoMysql.cpp — o mesmo Armazem, falando com um MySQL do dono do servidor
//
//  ┌──────────────────────────────────────────────────────────────────────┐
//  │  ISTO BLOQUEIA. Conectar espera rede; Executar espera resposta.      │
//  │  NADA AQUI PODE SER CHAMADO NA THREAD DO JOGO.                       │
//  │  Quem garante isso é o desenho do Armazem: leitura de permissão      │
//  │  responde do instantâneo em memória e não encosta em banco nenhum;   │
//  │  só a thread escritora e o arranque chegam até aqui.                 │
//  └──────────────────────────────────────────────────────────────────────┘
//
//  A PARTE QUE MERECE OLHO: NÃO HÁ PREPARED STATEMENT
//  --------------------------------------------------
//  O MySqlCliente desta casa fala COM_QUERY e mais nada (ver MySqlCliente.h).
//  Então `?1` não é ligado pelo protocolo: ele é SUBSTITUÍDO no texto, pelo
//  literal que o MySqlCliente::Citar() produziu — Citar escapa E envolve em
//  aspas, e o par escape/aspas foi exercitado com 15 payloads de injeção em
//  dois servidores de verdade.
//
//  A substituição é a superfície nova de risco deste arquivo, e por isso ela é
//  restritiva, não permissiva:
//    · PULA o que está dentro de literal ('...') e de crase (`...`), incluindo
//      as duas formas de escape ('' e \'). Um `?1` escrito dentro de um texto
//      é texto, e trocá-lo seria corromper o dado;
//    · `?` que não é seguido de dígito é RECUSA — não existe `?` posicional
//      neste código, e aceitar um seria abrir a porta para o que não se
//      previu;
//    · marcador sem valor ligado é RECUSA. SQL com `?3` solto NÃO é enviado.
//      Falha fechada: comando não executado é problema visível; comando
//      executado com marcador virando literal é dado errado em silêncio.
//
//  Isso é a terceira camada, não a primeira. As outras duas, provadas rodando
//  no MySqlCliente: CLIENT_MULTI_STATEMENTS DESLIGADO (um `'; DROP TABLE` não
//  tem como virar um segundo comando) e utf8mb4 forçado (em GBK/SJIS/BIG5 o
//  escape byte a byte é furado).
//
//  O QUE ESTE ARQUIVO NÃO FAZ
//  --------------------------
//  · não reconecta sozinho no meio de uma operação. Reconectar escondido
//    reexecutaria comando que já pode ter sido aplicado no servidor. Quem
//    decide repetir é o Armazem, que sabe quais tarefas são idempotentes;
//  · não fala TLS. A senha nunca vai em claro no fio, mas as consultas
//    trafegam abertas (ver o topo de MySqlCliente.cpp). Aceitável para MySQL
//    na mesma máquina ou em rede privada; não para MySQL exposto na internet.
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
// ── uma linha do resultado do MySQL ─────────────────────────────────────────
//
// O protocolo de texto entrega TUDO como string; `Inteiro` converte. NULL vem
// como nullptr e vira 0 — as colunas onde o Armazem chama Inteiro são todas
// declaradas NOT NULL, então o 0 é rede de segurança, não caminho normal.
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

// ── substituição de `?N` ────────────────────────────────────────────────────
//
// Devolve false e escreve o motivo em `erro` em vez de produzir SQL duvidoso.
bool Substituir(const char* sql, const std::vector<std::string>& valores,
                const std::vector<bool>& ligado, std::string& saida, std::string& erro)
{
    saida.clear();
    if (!sql) { erro = "SQL nulo"; return false; }
    saida.reserve(std::strlen(sql) + 64);

    for (const char* p = sql; *p; )
    {
        // literal de texto: copia inteiro sem olhar para dentro
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
        // identificador com crase: mesma regra
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
        // Citar() escapa E põe as aspas. nullptr vira o NULL do SQL — que é
        // coisa diferente de string vazia, e o banco distingue as duas.
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

    // Chamada de OUTRA thread, só no desligamento. Ver MySqlCliente::Interromper
    // para o porquê de shutdown() e não close(): fechar o descritor daqui, com
    // a thread escritora ainda dentro de um recv(), põe o SO livre para
    // reaproveitar o número — e os outros sockets deste processo são os do
    // servidor de jogo.
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

// O diagnóstico do cliente (versão do servidor, método de autenticação, custo
// do RSA) vai para o mesmo log do plugin. Sem isso, "por que a conexão demorou
// 4 s?" não tem resposta na máquina do dono.
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
    // A dica é montada aqui porque ela cita o banco e o usuário DESTE dono de
    // servidor. Mensagem de erro genérica ("permissao negada") faz quem não é
    // programador abrir um tíquete; mensagem com o comando pronto faz ele
    // resolver sozinho.
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
}   // namespace anônimo

std::unique_ptr<IBanco> CriarBancoMysql(const ConfigBanco& cfg, FnLog log)
{ return std::unique_ptr<IBanco>(new BancoMysql(cfg, log)); }

// ── exposta só para o teste calibrar a substituição ─────────────────────────
//
// Guarda nova precisa de prova dos DOIS lados: que ela recusa o que tem de
// recusar e que aceita o que tem de aceitar. Sem esta porta, o substituidor
// só seria exercitado de forma indireta e o "0 defeitos" não valeria nada.
bool SubstituirMarcadoresParaTeste(const char* sql,
                                   const std::vector<std::string>& valoresJaCitados,
                                   const std::vector<bool>& ligado,
                                   std::string& saida, std::string& erro)
{ return Substituir(sql, valoresJaCitados, ligado, saida, erro); }

}   // namespace Perm
