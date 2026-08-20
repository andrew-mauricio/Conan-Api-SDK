// ============================================================================
//  Banco.h — the boundary between the DECISION (what to store) and the MEDIUM
//            (where)
//
//  WHY THIS FILE EXISTS
//  --------------------
//  Armazem had 161 sqlite3_* calls scattered through its body. The permission
//  logic (flattened inheritance, match weight, specific denial, the snapshot
//  published by pointer swap) is PROVED in production and can't be rewritten to
//  fit MySQL. So what changes is the medium, not the decision: Armazem talks to
//  this interface, and there are two implementations.
//
//  The server owner picks in config.json with one line ("Database": "sqlite" or
//  "mysql") and writes no code.
//
//  ┌──────────────────────────────────────────────────────────────────────┐
//  │  INV-BANCO-001 · NOTHING HERE IS CALLED ON THE GAME'S THREAD.        │
//  │                                                                      │
//  │  Reading a permission (Armazem::Tem, NoGrupo, ExpiraEm, Grupos)      │
//  │  answers from the in-memory SNAPSHOT and touches no database at all  │
//  │  — not even SQLite. Only the writer thread (Armazem::Trabalhar) and  │
//  │  startup (Armazem::Abrir) call this interface.                       │
//  │                                                                      │
//  │  This isn't a design preference: BancoMysql talks over a socket. A   │
//  │  MySQL on the other side of the country, behind a bad network, is    │
//  │  the database's problem. If that wait happens in the game loop, the  │
//  │  tick stops, players time out and drop — the database's problem      │
//  │  becomes the game server's problem, which is exactly what must not   │
//  │  happen.                                                             │
//  └──────────────────────────────────────────────────────────────────────┘
//
//  INV-BANCO-002 · A database failure never becomes a process failure.
//      Every operation returns bool and leaves the text in Erro(). Never an
//      exception crossing the boundary, never a crash. A database that's down
//      means a refused write and a log line — the published snapshot keeps
//      serving reads with the last good state.
//
//  INV-BANCO-003 · A published snapshot is never half-built.
//      Reconstruir() assembles in memory and only publishes at the end. Any
//      read failure aborts BEFORE publication, and the previous snapshot stays
//      in force. A MySQL that dies mid-rebuild leaves the server with old,
//      correct permissions, never with half of them.
//
//  INV-BANCO-004 · The SQL isn't the same on both, and that's explicit.
//      There's no "one SQL that serves both". Each dialect has its own text,
//      side by side in Banco.cpp, and what is DELIBERATELY identical is written
//      once (namespace `comum`) so there aren't two copies of the same truth
//      quietly drifting apart. What proves the two agree isn't the text: it's
//      the behavioural suite running against both real databases
//      (testes/teste_banco.cpp).
//
//  INV-BANCO-006 · NEVER say you're using one database and use another.
//      If config.json says "mysql", Permission either talks to THAT MySQL or
//      answers nothing. There's no falling back to the local SQLite — not
//      silently, and not with a warning in giant letters.
//
//      THE DAMAGE IF BROKEN, and it's the worst in this file: the VIPs sold
//      today go into a local file nobody looks at. When MySQL comes back there
//      are two truths about who is VIP, and neither of them is the truth —
//      there's no reconciling them, because nobody knows which write came from
//      where. A log warning doesn't fix that: the owner only reads the log
//      AFTER a player complains, and by then the damage is written in both
//      places.
//
//      The degradation we chose is in Armazem.h (INV-ARMAZEM-002): database
//      down = Permission ABSENT, and absent is a state the API already knows
//      how to handle (`se_ausente` in ConanPermission.h). Absent is honest;
//      "using a different database" is a lie written to disk.
//
//  INV-BANCO-005 · Parameter interpolation only exists where there's no
//                  prepared statement.
//      SQLite gets `?1` and binds the value through sqlite3_bind_*: the value
//      never touches the SQL text. This house's MySQL has no prepared
//      statements (see MySqlCliente.h), so BancoMysql replaces `?N` with the
//      literal already passed through MySqlCliente::Citar() — which escapes AND
//      wraps in quotes. The substituter SKIPS anything inside a literal or
//      backticks, and REFUSES the command if a `?N` is left without a value:
//      SQL with a loose placeholder is never sent.
// ============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <memory>

namespace Perm
{
    // Also declared in Armazem.h. An identical repeated alias is legal C++, and
    // this file is included by Armazem.h — anyone including only this one still
    // compiles.
    using FnLog = void (*)(const char*);

    // ── one row of a result ──────────────────────────────────────────────────
    //
    // Text and integers only, because that's all this schema stores. No
    // floating point, no BLOBs. A small interface is an interface both
    // implementations can honour the same way.
    class ILinha
    {
    public:
        virtual ~ILinha() = default;
        virtual int         Colunas()        const = 0;
        // nullptr when the column is NULL. An empty string comes back as "" —
        // they're different things and both databases tell them apart.
        virtual const char* Texto(int col)   const = 0;
        // 0 when NULL, or when the text isn't a number. The schema only calls
        // this on a column declared as an integer.
        virtual int64_t     Inteiro(int col) const = 0;
    };

    using FnLinha = void (*)(const ILinha&, void*);

    // ── a command with parameters ────────────────────────────────────────────
    class IComando
    {
    public:
        virtual ~IComando() = default;

        // Position 1..N, like SQL's `?1`. `nullptr` binds SQL NULL (and not an
        // empty string: binding "" where you meant NULL writes wrong data with
        // no error at all).
        virtual bool LigarTexto(int pos, const char* v)   = 0;
        virtual bool LigarInteiro(int pos, int64_t v)     = 0;

        virtual bool Executar()                           = 0;
        virtual bool Consultar(FnLinha fn, void* ctx)     = 0;

        // Rows affected by this command's LAST Executar.
        //
        // A RECORDED TRAP: on MySQL this number is NOT usable to find out
        // whether a key exists. `INSERT ... ON DUPLICATE KEY UPDATE` returns 0
        // when the row already existed with the same values — indistinguishable
        // from "found nothing to insert". The code that needed that answer
        // (granting to a group that doesn't exist) now ASKS for the group's id
        // first, instead of inferring it from the counter. See
        // Armazem::Trabalhar.
        virtual int64_t Mudancas() const                  = 0;
    };

    // ── one dialect's SQL ────────────────────────────────────────────────────
    //
    // Each field is ONE command. `esquema` is a nullptr-terminated array
    // because this house's MySQL has no CLIENT_MULTI_STATEMENTS (switched off
    // on purpose, see MySqlCliente.h) and so won't take DDL with several `;` in
    // one packet. SQLite would accept it; it runs them one at a time anyway, so
    // that both paths are the same path.
    struct Sql
    {
        const char* const* esquema;

        const char* meta_gravar_versao;
        const char* meta_ler_versao;

        // configuration (permission.json -> database)
        const char* grupo_apelido_do_renome;
        const char* grupo_renomear;
        const char* grupo_upsert;
        const char* grupo_herda_limpar;
        const char* grupo_herda_inserir;
        const char* grupo_permissao_limpar;
        const char* grupo_permissao_inserir;
        const char* permissao_apelido_limpar;
        const char* permissao_apelido_inserir;
        const char* contar_padrao;

        // rebuilding the snapshot
        const char* ler_grupos;
        const char* ler_heranca;
        const char* ler_grupo_permissao;
        const char* ler_apelidos_de_grupo;
        const char* ler_apelidos_de_no;
        const char* ler_jogador_grupo;
        const char* ler_jogador_permissao;

        // writing
        const char* jogador_garantir;
        const char* grupo_id_por_chave_ou_apelido;
        const char* vinculo_upsert;
        const char* vinculo_apagar;
        const char* visto_upsert;
        const char* diario_inserir;
        const char* faxina_vencidos;
    };

    // ── what config.json says about the database ─────────────────────────────
    //
    // The key names follow the convention the survival-server community
    // already knows — anyone arriving from another API recognises them at once
    // and doesn't have to learn new names.
    struct ConfigBanco
    {
        enum Tipo { SQLITE, MYSQL };

        Tipo        tipo   = SQLITE;
        std::string caminhoSqlite;        // already resolved (DbPathOverride applied)
        std::string host   = "localhost";
        // This is NOT the default the owner gets: with tipo=MYSQL,
        // LerConfigBanco REFUSES when MysqlUser is empty (INV-CONFIG-003),
        // precisely so this "root" never applies by omission — picking the
        // database's most powerful account on your own is a fallback that
        // widens permission (§11). It survives here only because on sqlite the
        // field is inert. Anyone building a ConfigBanco by hand, without going
        // through LerConfigBanco, bypasses the guard: fill in the user.
        std::string usuario= "root";
        std::string senha;
        // NO default, on purpose: "where to write" is not something to guess.
        // With Database=mysql and no MysqlDB, LerConfigBanco REFUSES. See there
        // for the defect this fixes (a guessed database is data written in the
        // wrong place with no error at all).
        std::string banco;
        uint16_t    porta  = 3306;

        // The MySQL client's timeouts, in milliseconds. They're in the config
        // because "my VPS's MySQL takes 8 s to accept" is a real case, and a
        // 5 s default would turn into "Permission never comes up" with no
        // explanation. Zero or negative falls back to the default — "no limit"
        // is not an option (INV-MYSQL-002).
        int         msConectar = 5000;
        int         msOperar   = 10000;
    };

    // ── the database ─────────────────────────────────────────────────────────
    class IBanco
    {
    public:
        virtual ~IBanco() = default;

        virtual bool Abrir()  = 0;
        virtual void Fechar() = 0;

        // false once the connection has dropped. On SQLite it's "the handle
        // exists"; in a local file there's nothing to drop mid-way, and that's
        // why the difference between the two lives here instead of scattered
        // through Armazem.
        virtual bool Vivo() const = 0;

        // Only the writer thread calls this. Returns false and leaves the
        // reason in Erro(). It does NOT re-run anything on its own: deciding to
        // retry is Armazem's call, since it knows whether the task is
        // idempotent.
        virtual bool Reconectar() = 0;

        // ── the ONLY thing here another thread may call ─────────────────────
        //
        // Called by Armazem::Fechar(), from the thread shutting the server
        // down, WHILE the writer thread may be inside an operation. It makes
        // the in-flight operation come back with an error immediately, so the
        // join doesn't wait out the database's timeout.
        //
        // WHY THIS EXISTS: with a MySQL on a host that accepted the connection
        // and then went silent, a rebuild is 7 queries of up to msOperar each —
        // up to ~70 s of server hanging at shutdown with the defaults. The
        // owner does what anyone would: kills the process. Killing Conan during
        // shutdown loses the world save. A DATABASE problem must not end in a
        // lost save.
        //
        // IT IS FINAL: after this the database never serves again. The only
        // caller is shutdown.
        virtual void Interromper() = 0;

        virtual bool Executar(const char* sql)                          = 0;
        virtual bool Consultar(const char* sql, FnLinha fn, void* ctx)  = 0;
        virtual std::unique_ptr<IComando> Preparar(const char* sql)     = 0;

        virtual bool Iniciar()   = 0;      // transaction
        virtual bool Confirmar() = 0;
        virtual bool Desfazer()  = 0;

        virtual const char* Erro() const = 0;
        virtual const char* Nome() const = 0;      // "sqlite" or "mysql"
        virtual const Sql&  S()    const = 0;

        // What to tell the server owner when creating the schema fails. It
        // lives here because the answer differs per medium — on sqlite it's a
        // folder or file permission, on MySQL it's a GRANT — and Armazem has no
        // opinion about that (and shouldn't).
        virtual const char* DicaEsquema() const = 0;
    };

    // Factory. Returns nullptr only if `cfg.tipo` is unknown — which doesn't
    // happen, because LerConfigBanco refuses an invalid value first.
    std::unique_ptr<IBanco> CriarBanco(const ConfigBanco& cfg, FnLog log);

    // The two implementations. They live in BancoSqlite.cpp and BancoMysql.cpp
    // so each medium can be audited on its own; callers use CriarBanco().
    std::unique_ptr<IBanco> CriarBancoSqlite(const ConfigBanco& cfg, FnLog log);
    std::unique_ptr<IBanco> CriarBancoMysql (const ConfigBanco& cfg, FnLog log);

    // The two dialects, side by side in Banco.cpp. Exposed so the test can walk
    // EVERY command Armazem emits and run it against the real database — a
    // `const char*` that was never prepared is SQL nobody knows compiles.
    const Sql& SqlDoSqlite();
    const Sql& SqlDoMysql();

    // ── reading config.json ──────────────────────────────────────────────────
    //
    // Reads Database / MysqlHost / MysqlUser / MysqlPass / MysqlDB / MysqlPort
    // / DbPathOverride. The file's other keys (groups, aliases, _fronteira,
    // _privacidade, identity…) are preserved and ignored here.
    //
    // `caminhoPadraoDb` is what ConanApi handed over
    // (CaminhoDados("Permission", "permission.db")); DbPathOverride, when
    // present, replaces it.
    //
    // Returns false with the reason in `erro` when the file exists and is wrong
    // — including when "Database" carries a value that's neither sqlite nor
    // mysql. Refusing is mandatory: quietly falling back to sqlite after the
    // owner typed "mysqll" writes the VIPs into a local file nobody looks at,
    // and the symptom only appears when somebody goes looking for the data in
    // MySQL and doesn't find it.
    //
    // A MISSING file is not an error: it returns true with the defaults (sqlite
    // at ConanApi's path), which is exactly how the plugin already behaved.
    bool LerConfigBanco(const char* caminhoJson, const char* caminhoPadraoDb,
                        ConfigBanco& saida, std::string& erro);

    // ── the rest of config.json, already in memory ───────────────────────────
    //
    // WHY THE JSON IS STILL READ BY SQLITE'S json1
    // Configuration used to become database rows through one SQL statement
    // (json_each + json_extract joined against the real tables). That doesn't
    // carry over to MySQL: json_each is a SQLite table function, MySQL 5.7 has
    // no JSON_TABLE at all, and 8.0's syntax is different. Writing a JSON
    // parser by hand was the other way out — 200 new lines reading a file the
    // owner edits by hand, which is hostile input by definition.
    //
    // The way out is a third one: json1 keeps doing the job it already did, in
    // an IN-MEMORY SQLite (`:memory:`) that exists only for this and vanishes
    // afterwards. The amalgamation is inside the .dll either way. What changes
    // is that the result comes out as a struct, and the interface does the
    // writing — so the same configuration path serves both databases.
    struct ConfigGrupo
    {
        std::string chave, nome, era;
        int64_t     prioridade = 0;
        bool        padrao     = false;
        std::vector<std::string>                 herda;
        std::vector<std::pair<std::string,bool>> permissoes;   // (node, deny)
    };

    struct ConfigPermissao
    {
        std::vector<ConfigGrupo>                            grupos;
        std::vector<std::pair<std::string,std::string>>     apelidosDeNo;
    };

    // false when the file exists and isn't valid JSON, or when a value doesn't
    // fit the ABI's caps. `existe` comes back false when there's no file — and
    // that isn't an error, it's "use whatever is already in the database".
    bool LerConfigPermissao(const char* caminhoJson, ConfigPermissao& saida,
                            bool& existe, std::string& erro);
}
