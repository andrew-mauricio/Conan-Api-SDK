// ============================================================================
//  Banco.cpp — the TWO dialects side by side, and reading config.json
//
//  THIS IS THE FILE YOU AUDIT WHEN THE TWO DATABASES DISAGREE.
//
//  The rule that organises it: what is deliberately THE SAME in both is written
//  ONCE (namespace `comum`); what differs is written TWICE, one under the
//  other, so the difference is visible at a glance. There is no "SQL that
//  happens to serve both" here — what there is, is SQL that's identical because
//  it was checked against both real servers, and SQL that differs because it
//  has to.
//
//  WHAT WAS MEASURED, NOT ASSUMED (2026-08-18, MySQL 8.4.11 and MariaDB 10.11)
//  ---------------------------------------------------------------------------
//  · the identifier `no` without quotes: accepted by both. `NO` is a NON-
//    reserved word in MySQL. Even so the MySQL schema backticks everything,
//    because the schema is text exclusive to it and backticks cost nothing;
//  · `INSERT ... SELECT ... ON DUPLICATE KEY UPDATE`: accepted by both;
//  · the deterministic id SELECT (a derived table + ORDER BY ord LIMIT 1):
//    accepted by both, and by SQLite too;
//  · COLLATE utf8mb4_bin makes comparison CASE SENSITIVE, which is how SQLite
//    compares TEXT by default (BINARY). Without it, MySQL 8's default
//    (utf8mb4_0900_ai_ci) would make "vip" and "VIP" the SAME group, and two
//    players with different ids the same player. Checked: with utf8mb4_bin both
//    databases store 'vip' and 'VIP' as separate rows;
//  · A KNOWN AND DECLARED DIVERGENCE: utf8mb4_bin is PAD SPACE. `chave='vip '`
//    matches 'vip' on MySQL and does NOT match on SQLite (measured: 1 against
//    0). For configuration this is CLOSED at the source — ValidarChave()
//    refuses a key with a trailing space. For the player id arriving through
//    the ABI there's no closing it without changing the ABI: it stands as a
//    declared residual risk.
// ============================================================================
#include "Banco.h"
#include "Armazem.h"                     // only for MAX_ID/MAX_GRUPO/MAX_NO caps
#include "terceiros/sqlite3/sqlite3.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>

namespace Perm
{
namespace
{
// ============================================================================
//  1 · THE SCHEMA — written twice, on purpose
// ============================================================================

// ── SQLite ──────────────────────────────────────────────────────────────────
// Byte for byte the same schema that was already running, only split into one
// command per entry (see Sql::esquema's comment in Banco.h).
const char* const ESQUEMA_SQLITE[] = {
"CREATE TABLE IF NOT EXISTS meta("
"    chave TEXT PRIMARY KEY,"
"    valor TEXT NOT NULL"
");",

"CREATE TABLE IF NOT EXISTS grupo("
"    id         INTEGER PRIMARY KEY AUTOINCREMENT,"
"    chave      TEXT    NOT NULL UNIQUE,"
"    nome       TEXT    NOT NULL DEFAULT '',"
"    prioridade INTEGER NOT NULL DEFAULT 0,"
"    padrao     INTEGER NOT NULL DEFAULT 0"
");",

"CREATE TABLE IF NOT EXISTS grupo_apelido("
"    de    TEXT    PRIMARY KEY,"
"    grupo INTEGER NOT NULL REFERENCES grupo(id) ON DELETE CASCADE"
");",

"CREATE TABLE IF NOT EXISTS grupo_herda("
"    filho INTEGER NOT NULL REFERENCES grupo(id) ON DELETE CASCADE,"
"    pai   INTEGER NOT NULL REFERENCES grupo(id) ON DELETE CASCADE,"
"    PRIMARY KEY(filho, pai)"
");",

"CREATE TABLE IF NOT EXISTS grupo_permissao("
"    grupo INTEGER NOT NULL REFERENCES grupo(id) ON DELETE CASCADE,"
"    no    TEXT    NOT NULL,"
"    nega  INTEGER NOT NULL DEFAULT 0,"
"    PRIMARY KEY(grupo, no)"
");",

"CREATE TABLE IF NOT EXISTS jogador("
"    id         TEXT PRIMARY KEY,"
"    visto_nome TEXT    NOT NULL DEFAULT '',"
"    visto_em   INTEGER NOT NULL DEFAULT 0"
");",

"CREATE TABLE IF NOT EXISTS jogador_grupo("
"    jogador       TEXT    NOT NULL REFERENCES jogador(id) ON DELETE CASCADE,"
"    grupo         INTEGER NOT NULL REFERENCES grupo(id)   ON DELETE CASCADE,"
"    expira_em     INTEGER NOT NULL DEFAULT 0,"
"    concedido_em  INTEGER NOT NULL DEFAULT 0,"
"    concedido_por TEXT    NOT NULL DEFAULT '',"
"    PRIMARY KEY(jogador, grupo)"
");",

"CREATE TABLE IF NOT EXISTS jogador_permissao("
"    jogador   TEXT    NOT NULL REFERENCES jogador(id) ON DELETE CASCADE,"
"    no        TEXT    NOT NULL,"
"    nega      INTEGER NOT NULL DEFAULT 0,"
"    expira_em INTEGER NOT NULL DEFAULT 0,"
"    PRIMARY KEY(jogador, no)"
");",

"CREATE TABLE IF NOT EXISTS permissao_apelido("
"    de   TEXT PRIMARY KEY,"
"    para TEXT NOT NULL"
");",

// The audit log. It exists because "who gave that guy VIP?" is the first
// question every server owner with more than one admin asks.
"CREATE TABLE IF NOT EXISTS diario("
"    id      INTEGER PRIMARY KEY AUTOINCREMENT,"
"    em      INTEGER NOT NULL,"
"    quem    TEXT    NOT NULL DEFAULT '',"
"    acao    TEXT    NOT NULL,"
"    alvo    TEXT    NOT NULL DEFAULT '',"
"    detalhe TEXT    NOT NULL DEFAULT ''"
");",

"CREATE INDEX IF NOT EXISTS jg_por_grupo ON jogador_grupo(grupo);",
"CREATE INDEX IF NOT EXISTS diario_em    ON diario(em);",
nullptr };

// ── MySQL / MariaDB ─────────────────────────────────────────────────────────
//
// THE DECISIONS THAT AREN'T MECHANICAL TRANSLATION, AND THE DEFECT EACH AVOIDS:
//
//  · An explicit ENGINE=InnoDB. MyISAM ACCEPTS FOREIGN KEY syntax and IGNORES
//    it silently — the schema "creates" and the foreign keys simply don't
//    exist. On a server with default-storage-engine=MyISAM, ON DELETE CASCADE
//    would become decoration and deleting a group would leave orphaned links.
//    InnoDB written by hand is the only way that doesn't depend on the owner
//    machine's configuration.
//
//  · COLLATE=utf8mb4_bin on every table. See the header: without it, "vip" and
//    "VIP" become the same group and two players become one. It's the
//    difference that most quietly moves data between owners.
//
//  · TEXT (and not VARCHAR) for every free field — nome, visto_nome, quem,
//    alvo, detalhe, valor. Reason: these servers' MySQL runs with
//    STRICT_TRANS_TABLES (measured), and in that mode writing 300 characters
//    into a VARCHAR(256) is an ERROR, not truncation. SQLite would accept it. A
//    long group label in permission.json would take down the whole
//    configuration on MySQL alone. TEXT has no such limit and makes both behave
//    the same.
//
//  · VARCHAR sized to the ABI's cap wherever the column is INDEXED (an index
//    needs a length): group id/key/alias = 64 (MAX_ID/MAX_GRUPO), permission
//    node = 127 (MAX_NO-1, the longest node the ABI can even ask about —
//    ComprimentoLimitado requires the '\0' within MAX_NO bytes). Storing 128
//    would mean storing a value no query can reach.
//
//    ┌ CORRECTING A CLAIM OF MINE THAT WAS FALSE (2026-08-18) ───────────────┐
//    │ This comment used to say the 127 existed to fit old InnoDB's 767-byte │
//    │ limit, because `PRIMARY KEY(jogador,no)` would give 64*4 + 128*4 =    │
//    │ 768. I went and measured on a real MySQL 5.7.44, started with         │
//    │ innodb_large_prefix=OFF and ROW_FORMAT=COMPACT (the worst case), and  │
//    │ the claim did NOT hold: the table with VARCHAR(128) was created with  │
//    │ no error at all.                                                      │
//    │                                                                       │
//    │ The 767-byte limit is PER INDEXED COLUMN, not the sum of a composite  │
//    │ key. Measured on the same server:                                     │
//    │   VARCHAR(200) (800 B) in one column  -> ERROR 1071 "key was too long"│
//    │   VARCHAR(191) (764 B) in one column  -> created                      │
//    │   (64 + 128) = 768 B across two columns -> created                    │
//    │                                                                       │
//    │ So: the reason was wrong, the number is right for a different reason. │
//    │ It's written down because a comment justifying itself with a danger   │
//    │ that doesn't exist is worse than no comment — the next reader         │
//    │ believes it.                                                          │
//    └───────────────────────────────────────────────────────────────────────┘
//
//  · BIGINT AUTO_INCREMENT in place of INTEGER AUTOINCREMENT.
//
//  · An explicit index on the FK column (MySQL requires it; SQLite doesn't).
const char* const ESQUEMA_MYSQL[] = {
"CREATE TABLE IF NOT EXISTS `meta`("
"    `chave` VARCHAR(64) NOT NULL,"
"    `valor` TEXT        NOT NULL,"
"    PRIMARY KEY(`chave`)"
") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin;",

"CREATE TABLE IF NOT EXISTS `grupo`("
"    `id`         BIGINT      NOT NULL AUTO_INCREMENT,"
"    `chave`      VARCHAR(64) NOT NULL,"
"    `nome`       TEXT        NOT NULL,"
"    `prioridade` BIGINT      NOT NULL DEFAULT 0,"
"    `padrao`     TINYINT     NOT NULL DEFAULT 0,"
"    PRIMARY KEY(`id`),"
"    UNIQUE KEY `grupo_chave`(`chave`)"
") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin;",

"CREATE TABLE IF NOT EXISTS `grupo_apelido`("
"    `de`    VARCHAR(64) NOT NULL,"
"    `grupo` BIGINT      NOT NULL,"
"    PRIMARY KEY(`de`),"
"    KEY `ga_grupo`(`grupo`),"
"    CONSTRAINT `fk_ga_grupo` FOREIGN KEY(`grupo`) REFERENCES `grupo`(`id`) ON DELETE CASCADE"
") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin;",

"CREATE TABLE IF NOT EXISTS `grupo_herda`("
"    `filho` BIGINT NOT NULL,"
"    `pai`   BIGINT NOT NULL,"
"    PRIMARY KEY(`filho`,`pai`),"
"    KEY `gh_pai`(`pai`),"
"    CONSTRAINT `fk_gh_filho` FOREIGN KEY(`filho`) REFERENCES `grupo`(`id`) ON DELETE CASCADE,"
"    CONSTRAINT `fk_gh_pai`   FOREIGN KEY(`pai`)   REFERENCES `grupo`(`id`) ON DELETE CASCADE"
") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin;",

"CREATE TABLE IF NOT EXISTS `grupo_permissao`("
"    `grupo` BIGINT       NOT NULL,"
"    `no`    VARCHAR(127) NOT NULL,"
"    `nega`  TINYINT      NOT NULL DEFAULT 0,"
"    PRIMARY KEY(`grupo`,`no`),"
"    CONSTRAINT `fk_gp_grupo` FOREIGN KEY(`grupo`) REFERENCES `grupo`(`id`) ON DELETE CASCADE"
") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin;",

"CREATE TABLE IF NOT EXISTS `jogador`("
"    `id`         VARCHAR(64) NOT NULL,"
"    `visto_nome` TEXT        NOT NULL,"
"    `visto_em`   BIGINT      NOT NULL DEFAULT 0,"
"    PRIMARY KEY(`id`)"
") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin;",

"CREATE TABLE IF NOT EXISTS `jogador_grupo`("
"    `jogador`       VARCHAR(64) NOT NULL,"
"    `grupo`         BIGINT      NOT NULL,"
"    `expira_em`     BIGINT      NOT NULL DEFAULT 0,"
"    `concedido_em`  BIGINT      NOT NULL DEFAULT 0,"
"    `concedido_por` TEXT        NOT NULL,"
"    PRIMARY KEY(`jogador`,`grupo`),"
"    KEY `jg_por_grupo`(`grupo`),"
"    CONSTRAINT `fk_jg_jogador` FOREIGN KEY(`jogador`) REFERENCES `jogador`(`id`) ON DELETE CASCADE,"
"    CONSTRAINT `fk_jg_grupo`   FOREIGN KEY(`grupo`)   REFERENCES `grupo`(`id`)   ON DELETE CASCADE"
") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin;",

"CREATE TABLE IF NOT EXISTS `jogador_permissao`("
"    `jogador`   VARCHAR(64)  NOT NULL,"
"    `no`        VARCHAR(127) NOT NULL,"
"    `nega`      TINYINT      NOT NULL DEFAULT 0,"
"    `expira_em` BIGINT       NOT NULL DEFAULT 0,"
"    PRIMARY KEY(`jogador`,`no`),"
"    CONSTRAINT `fk_jp_jogador` FOREIGN KEY(`jogador`) REFERENCES `jogador`(`id`) ON DELETE CASCADE"
") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin;",

"CREATE TABLE IF NOT EXISTS `permissao_apelido`("
"    `de`   VARCHAR(127) NOT NULL,"
"    `para` VARCHAR(127) NOT NULL,"
"    PRIMARY KEY(`de`)"
") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin;",

"CREATE TABLE IF NOT EXISTS `diario`("
"    `id`      BIGINT NOT NULL AUTO_INCREMENT,"
"    `em`      BIGINT NOT NULL,"
"    `quem`    TEXT   NOT NULL,"
"    `acao`    VARCHAR(32) NOT NULL,"
"    `alvo`    TEXT   NOT NULL,"
"    `detalhe` TEXT   NOT NULL,"
"    PRIMARY KEY(`id`),"
"    KEY `diario_em`(`em`)"
") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin;",
nullptr };

// ============================================================================
//  2 · COMMANDS IDENTICAL IN BOTH — written once, on purpose
//
//  Writing these twice would create two copies of the same truth. They drift,
//  and nobody notices until the day it matters. What proves they're right in
//  BOTH isn't the text: it's the behavioural suite running against real SQLite
//  and real MySQL (testes/teste_banco.cpp).
// ============================================================================
namespace comum
{
    const char* const META_LER_VERSAO =
        "SELECT valor FROM meta WHERE chave='esquema_versao';";

    const char* const GRUPO_RENOMEAR =
        "UPDATE grupo SET chave=?1 WHERE chave=?2;";

    const char* const GRUPO_HERDA_LIMPAR       = "DELETE FROM grupo_herda;";
    const char* const GRUPO_PERMISSAO_LIMPAR   = "DELETE FROM grupo_permissao;";
    const char* const PERMISSAO_APELIDO_LIMPAR = "DELETE FROM permissao_apelido;";

    // No INSERT OR IGNORE / INSERT IGNORE in the three below, and that's a
    // decision, not an oversight. The three tables were just emptied, so a
    // duplicate can only come from permission.json itself repeating a line —
    // and that's handled EARLIER, by deduplicating in C++ (see AplicarConfig),
    // with a log line. MySQL's `INSERT IGNORE`, besides ignoring a repeated
    // key, swallows foreign-key violations and data truncation: using it here
    // would turn a real error into silence.
    const char* const GRUPO_HERDA_INSERIR =
        "INSERT INTO grupo_herda(filho,pai) "
        "SELECT f.id, p.id FROM grupo f, grupo p "
        "WHERE f.chave=?1 AND p.chave=?2 AND f.id<>p.id;";

    const char* const GRUPO_PERMISSAO_INSERIR =
        "INSERT INTO grupo_permissao(grupo,no,nega) "
        "SELECT g.id, ?2, ?3 FROM grupo g WHERE g.chave=?1;";

    const char* const PERMISSAO_APELIDO_INSERIR =
        "INSERT INTO permissao_apelido(de,para) VALUES(?1,?2);";

    const char* const CONTAR_PADRAO =
        "SELECT count(*) FROM grupo WHERE padrao=1;";

    // ── rebuilding the snapshot ─────────────────────────────────────────────
    const char* const LER_GRUPOS          = "SELECT id,chave,prioridade,padrao FROM grupo;";
    const char* const LER_HERANCA         = "SELECT filho,pai FROM grupo_herda;";
    const char* const LER_GRUPO_PERMISSAO = "SELECT grupo,no,nega FROM grupo_permissao;";
    const char* const LER_APELIDOS_GRUPO  =
        "SELECT a.de, g.chave FROM grupo_apelido a JOIN grupo g ON g.id=a.grupo;";
    const char* const LER_APELIDOS_NO     = "SELECT de,para FROM permissao_apelido;";
    const char* const LER_JOGADOR_GRUPO   =
        "SELECT jg.jogador, jg.grupo, jg.expira_em, g.chave "
        "FROM jogador_grupo jg JOIN grupo g ON g.id = jg.grupo;";
    const char* const LER_JOGADOR_PERMISSAO =
        "SELECT jogador,no,nega,expira_em FROM jogador_permissao;";

    // ── writing ─────────────────────────────────────────────────────────────
    //
    // The group's id by its CURRENT key or by any of its ALIASES. It's what
    // keeps `!perm dar Fulano vip` working after the group was renamed to
    // "premium".
    //
    // WHY THE DERIVED TABLE WITH `ord`, instead of UNION ALL + LIMIT 1: without
    // the ORDER BY, which of the two halves comes first is the optimiser's
    // choice. If somebody creates a group whose key is another group's ALIAS,
    // both halves return a row and the result starts depending on the execution
    // plan — the same command giving different answers on different databases,
    // or on the same database after an ANALYZE. `ord` pins the precedence: the
    // current key beats the alias, always.
    const char* const GRUPO_ID_POR_CHAVE_OU_APELIDO =
        "SELECT id FROM ("
        "  SELECT 0 AS ord, id    AS id FROM grupo         WHERE chave=?1 "
        "  UNION ALL "
        "  SELECT 1 AS ord, grupo AS id FROM grupo_apelido WHERE de=?1"
        ") t ORDER BY ord LIMIT 1;";

    // Now that the id arrives already resolved, deleting is by id: no
    // subquery, no dialect, and the number of deleted rows means exactly what
    // it looks like it means on both databases.
    const char* const VINCULO_APAGAR =
        "DELETE FROM jogador_grupo WHERE jogador=?1 AND grupo=?2;";

    // The action becomes a parameter (it used to be a literal
    // 'conceder'/'revogar' in two nearly identical commands). One command, one
    // place to check.
    const char* const DIARIO_INSERIR =
        "INSERT INTO diario(em,quem,acao,alvo,detalhe) VALUES(?1,?2,?3,?4,?5);";

    const char* const FAXINA_VENCIDOS =
        "DELETE FROM jogador_grupo WHERE expira_em<>0 AND expira_em<=?1;";
}

// ============================================================================
//  3 · COMMANDS THAT DIFFER — written twice, one under the other
//
//  They're all upserts. SQLite speaks ON CONFLICT(...) DO UPDATE/DO NOTHING
//  (PostgreSQL's syntax, which it adopted); MySQL never had ON CONFLICT and
//  speaks ON DUPLICATE KEY UPDATE. There's no automatic translation between
//  them: SQLite's names the conflicting column, MySQL's applies to ANY unique
//  key on the table.
//
//  The `x=x` on the MySQL side is its "do nothing": an UPDATE that changes
//  nothing. Writing `INSERT IGNORE` instead would be shorter and would be wrong
//  — IGNORE also swallows real errors (foreign keys, truncation).
// ============================================================================

// meta ───────────────────────────────────────────────────────────────────────
const char* const META_GRAVAR_VERSAO_SQLITE =
    "INSERT INTO meta(chave,valor) VALUES('esquema_versao',?1) "
    "ON CONFLICT(chave) DO NOTHING;";
const char* const META_GRAVAR_VERSAO_MYSQL =
    "INSERT INTO meta(chave,valor) VALUES('esquema_versao',?1) "
    "ON DUPLICATE KEY UPDATE chave=chave;";

// the alias a rename leaves behind ──────────────────────────────────────────
// It stores the OLD key pointing at the group that holds that key NOW (before
// the swap). It's what keeps a compiled third-party plugin correct when it asks
// for "vip" after the group became "premium".
const char* const GRUPO_APELIDO_RENOME_SQLITE =
    "INSERT INTO grupo_apelido(de,grupo) SELECT ?1, id FROM grupo WHERE chave=?1 "
    "ON CONFLICT(de) DO NOTHING;";
const char* const GRUPO_APELIDO_RENOME_MYSQL =
    "INSERT INTO grupo_apelido(de,grupo) SELECT ?1, id FROM grupo WHERE chave=?1 "
    "ON DUPLICATE KEY UPDATE grupo_apelido.de=grupo_apelido.de;";

// grupo ──────────────────────────────────────────────────────────────────────
const char* const GRUPO_UPSERT_SQLITE =
    "INSERT INTO grupo(chave,nome,prioridade,padrao) VALUES(?1,?2,?3,?4) "
    "ON CONFLICT(chave) DO UPDATE SET nome=?2, prioridade=?3, padrao=?4;";
const char* const GRUPO_UPSERT_MYSQL =
    "INSERT INTO grupo(chave,nome,prioridade,padrao) VALUES(?1,?2,?3,?4) "
    "ON DUPLICATE KEY UPDATE nome=?2, prioridade=?3, padrao=?4;";

// jogador ────────────────────────────────────────────────────────────────────
const char* const JOGADOR_GARANTIR_SQLITE =
    "INSERT INTO jogador(id) VALUES(?1) ON CONFLICT(id) DO NOTHING;";
const char* const JOGADOR_GARANTIR_MYSQL =
    "INSERT INTO jogador(id,visto_nome) VALUES(?1,'') "
    "ON DUPLICATE KEY UPDATE id=id;";
// (the explicit `visto_nome` exists because the column is TEXT NOT NULL and
//  TEXT on MySQL takes no DEFAULT; on SQLite it has DEFAULT '' and the
//  difference disappears.)

const char* const VINCULO_UPSERT_SQLITE =
    "INSERT INTO jogador_grupo(jogador,grupo,expira_em,concedido_em,concedido_por) "
    "VALUES(?1,?2,?3,?4,?5) "
    "ON CONFLICT(jogador,grupo) DO UPDATE SET "
    "  expira_em=?3, concedido_em=?4, concedido_por=?5;";
const char* const VINCULO_UPSERT_MYSQL =
    "INSERT INTO jogador_grupo(jogador,grupo,expira_em,concedido_em,concedido_por) "
    "VALUES(?1,?2,?3,?4,?5) "
    "ON DUPLICATE KEY UPDATE "
    "  expira_em=?3, concedido_em=?4, concedido_por=?5;";

const char* const VISTO_UPSERT_SQLITE =
    "INSERT INTO jogador(id,visto_nome,visto_em) VALUES(?1,?2,?3) "
    "ON CONFLICT(id) DO UPDATE SET visto_nome=?2, visto_em=?3;";
const char* const VISTO_UPSERT_MYSQL =
    "INSERT INTO jogador(id,visto_nome,visto_em) VALUES(?1,?2,?3) "
    "ON DUPLICATE KEY UPDATE visto_nome=?2, visto_em=?3;";

// ============================================================================
//  4 · the two assembled tables
// ============================================================================
const Sql SQL_SQLITE = {
    ESQUEMA_SQLITE,
    META_GRAVAR_VERSAO_SQLITE,
    comum::META_LER_VERSAO,
    GRUPO_APELIDO_RENOME_SQLITE,
    comum::GRUPO_RENOMEAR,
    GRUPO_UPSERT_SQLITE,
    comum::GRUPO_HERDA_LIMPAR,
    comum::GRUPO_HERDA_INSERIR,
    comum::GRUPO_PERMISSAO_LIMPAR,
    comum::GRUPO_PERMISSAO_INSERIR,
    comum::PERMISSAO_APELIDO_LIMPAR,
    comum::PERMISSAO_APELIDO_INSERIR,
    comum::CONTAR_PADRAO,
    comum::LER_GRUPOS,
    comum::LER_HERANCA,
    comum::LER_GRUPO_PERMISSAO,
    comum::LER_APELIDOS_GRUPO,
    comum::LER_APELIDOS_NO,
    comum::LER_JOGADOR_GRUPO,
    comum::LER_JOGADOR_PERMISSAO,
    JOGADOR_GARANTIR_SQLITE,
    comum::GRUPO_ID_POR_CHAVE_OU_APELIDO,
    VINCULO_UPSERT_SQLITE,
    comum::VINCULO_APAGAR,
    VISTO_UPSERT_SQLITE,
    comum::DIARIO_INSERIR,
    comum::FAXINA_VENCIDOS,
};

const Sql SQL_MYSQL = {
    ESQUEMA_MYSQL,
    META_GRAVAR_VERSAO_MYSQL,
    comum::META_LER_VERSAO,
    GRUPO_APELIDO_RENOME_MYSQL,
    comum::GRUPO_RENOMEAR,
    GRUPO_UPSERT_MYSQL,
    comum::GRUPO_HERDA_LIMPAR,
    comum::GRUPO_HERDA_INSERIR,
    comum::GRUPO_PERMISSAO_LIMPAR,
    comum::GRUPO_PERMISSAO_INSERIR,
    comum::PERMISSAO_APELIDO_LIMPAR,
    comum::PERMISSAO_APELIDO_INSERIR,
    comum::CONTAR_PADRAO,
    comum::LER_GRUPOS,
    comum::LER_HERANCA,
    comum::LER_GRUPO_PERMISSAO,
    comum::LER_APELIDOS_GRUPO,
    comum::LER_APELIDOS_NO,
    comum::LER_JOGADOR_GRUPO,
    comum::LER_JOGADOR_PERMISSAO,
    JOGADOR_GARANTIR_MYSQL,
    comum::GRUPO_ID_POR_CHAVE_OU_APELIDO,
    VINCULO_UPSERT_MYSQL,
    comum::VINCULO_APAGAR,
    VISTO_UPSERT_MYSQL,
    comum::DIARIO_INSERIR,
    comum::FAXINA_VENCIDOS,
};

// ============================================================================
//  5 · the JSON, read by json1 in an IN-MEMORY SQLite
//
//  The why is in Banco.h (LerConfigPermissao). In one line: json_each is a
//  SQLite table function and doesn't carry over to MySQL; writing a JSON parser
//  by hand would be new code reading a file the owner edits by hand. So the
//  same json1 that already did the job keeps doing it — in an in-memory
//  database that exists for nothing else.
// ============================================================================
class JsonEmMemoria
{
public:
    ~JsonEmMemoria() { if (m_db) sqlite3_close(m_db); }

    bool Abrir(std::string& erro)
    {
        if (sqlite3_open_v2(":memory:", &m_db,
                            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK)
        {
            erro = "nao consegui abrir o SQLite de memoria para ler o JSON";
            if (m_db) { sqlite3_close(m_db); m_db = nullptr; }
            return false;
        }
        return true;
    }

    // json1 is the same parser Funcom already loads into the process.
    // `json_valid` BEFORE any json_extract: extracting from broken JSON returns
    // NULL silently, and an empty configuration wearing the face of a loaded
    // one is worse than an error.
    bool Valido(const std::string& texto)
    {
        sqlite3_stmt* st = nullptr;
        int ok = 0;
        if (sqlite3_prepare_v2(m_db, "SELECT json_valid(?1);", -1, &st, nullptr) == SQLITE_OK)
        {
            sqlite3_bind_text(st, 1, texto.c_str(), -1, SQLITE_STATIC);
            if (sqlite3_step(st) == SQLITE_ROW) ok = sqlite3_column_int(st, 0);
        }
        sqlite3_finalize(st);
        return ok != 0;
    }

    // Runs `sql` with the JSON text bound to ?1 and calls `fn` per row.
    bool Percorrer(const char* sql, const std::string& texto,
                   void (*fn)(sqlite3_stmt*, void*), void* ctx, std::string& erro)
    {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(m_db, sql, -1, &st, nullptr) != SQLITE_OK)
        {
            erro  = "consulta ao JSON falhou: ";
            erro += sqlite3_errmsg(m_db);
            return false;
        }
        sqlite3_bind_text(st, 1, texto.c_str(), -1, SQLITE_STATIC);
        int rc;
        while ((rc = sqlite3_step(st)) == SQLITE_ROW) fn(st, ctx);
        sqlite3_finalize(st);
        if (rc != SQLITE_DONE)
        {
            erro  = "leitura do JSON parou no meio: ";
            erro += sqlite3_errmsg(m_db);
            return false;
        }
        return true;
    }

private:
    sqlite3* m_db = nullptr;
};

// Reads the whole file. Absence is NOT an error (the database may already be
// populated).
bool LerArquivo(const char* caminho, std::string& texto, bool& existe, std::string& erro)
{
    existe = false;
    FILE* f = std::fopen(caminho, "rb");
    if (!f) return true;
    existe = true;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n < 0 || n > 4 * 1024 * 1024)
    {
        std::fclose(f);
        char b[160];
        std::snprintf(b, sizeof(b),
            "permission.json com tamanho absurdo (%ld bytes). O teto e 4 MiB.", n);
        erro = b;
        return false;
    }
    texto.resize(static_cast<size_t>(n));
    size_t lidos = n ? std::fread(&texto[0], 1, static_cast<size_t>(n), f) : 0;
    std::fclose(f);
    texto.resize(lidos);
    return true;
}

const char* Txt(sqlite3_stmt* st, int c)
{
    const unsigned char* t = sqlite3_column_text(st, c);
    return t ? reinterpret_cast<const char*>(t) : nullptr;
}

// ── key validation, and the defect it closes ─────────────────────────────────
//
// A NEW GUARD — and a new guard without calibration is a guard nobody trusts.
// Both sides are exercised in testes/teste_banco.cpp: it FAILS a key that's too
// long, empty, or has a trailing space, and PASSES the keys in the
// permission.json that ships in the package.
//
// WHAT IT CLOSES:
//  1. length — the indexed MySQL columns are VARCHAR(64)/VARCHAR(127) and it
//     runs in STRICT mode: a 200-character key would be an ERROR and would take
//     down the whole configuration. On SQLite it would be WRITTEN and then
//     silently discarded while building the snapshot ("apelido longo
//     demais"). Two different behaviours for the same file. Refusing at the
//     door, naming the key, makes both the same and tells the owner what's
//     wrong;
//  2. a trailing space — utf8mb4_bin is PAD SPACE: on MySQL "vip " and "vip"
//     are the SAME key; on SQLite they're two. Refusing here closes the
//     divergence at its source, which is the only place it can be closed.
bool ValidarChave(const char* valor, int teto, const char* oQueE,
                  const char* onde, std::string& erro)
{
    char b[400];
    if (!valor || !*valor)
    {
        std::snprintf(b, sizeof(b), "%s vazio em %s. Toda chave precisa de nome.", oQueE, onde);
        erro = b; return false;
    }
    const size_t n = std::strlen(valor);
    if (n >= static_cast<size_t>(teto))
    {
        std::snprintf(b, sizeof(b),
            "%s '%.60s...' em %s tem %zu caracteres e o limite e %d. "
            "O limite nao e capricho: e o mesmo teto que a ABI usa, e a coluna "
            "indexada do MySQL tem esse tamanho.", oQueE, valor, onde, n, teto - 1);
        erro = b; return false;
    }
    if (valor[0] == ' ' || valor[0] == '\t' || valor[n-1] == ' ' || valor[n-1] == '\t')
    {
        std::snprintf(b, sizeof(b),
            "%s '%.80s' em %s comeca ou termina com espaco. Recusado de proposito: "
            "no MySQL 'vip ' e 'vip' sao a MESMA chave e no SQLite sao duas, "
            "entao o mesmo arquivo se comportaria diferente nos dois bancos. "
            "Tire o espaco.", oQueE, valor, onde);
        erro = b; return false;
    }
    return true;
}

// ============================================================================
//  ValidarValorDeConexao — MysqlHost / MysqlUser / MysqlDB
//
//  INVARIANT   INV-CONFIG-002
//  Name:        a leading or trailing space in host, user and database is
//               refused AT THE DOOR
//  Description: MysqlHost, MysqlUser and MysqlDB may not begin or end with a
//               space or a tab.
//  Damage if broken: the owner can't fix what they can't SEE.
//  Layers:      this function — and only it can be, because it's the last point
//               where the file's text still exists the way they typed it.
//
//  THE REAL DEFECT THAT MOTIVATED IT, measured on 2026-08-18 running under Wine
//  against the test MySQL 8.4.11 (testes/simulador_dono.cpp, one config at a
//  time):
//
//    MysqlHost "127.0.0.1 "        -> "nao consegui resolver o endereco
//                                      '127.0.0.1 '"
//    MysqlUser "conan "            -> error 1045, "Access denied for user
//                                      'conan '@..."
//    MysqlDB   "conan_permission " -> RAW error 1102, "Incorrect database name
//                                      'conan_permission '" — no translation at
//                                      all and no hint of what to do.
//
//  All three messages are TRUE and USELESS: the space doesn't show on screen.
//  The owner reads '127.0.0.1 ', recognises the address they typed themselves,
//  checks the file, sees the same thing — and concludes the plugin is broken.
//  They have no way to see the difference; we do.
//
//  WHY REFUSE INSTEAD OF TRIMMING: trimming silently would have the plugin
//  connect as a DIFFERENT user — or to a DIFFERENT database — than what's
//  written in the file. This code already settled that same question once, for
//  group keys (ValidarChave, just above), for the same reason. Two different
//  answers to the same problem in the same file is what confuses people.
//
//  AND WHY THE PASSWORD IS LEFT OUT: a password with a trailing space is a
//  legitimate password. Refusing would break whoever has one. The password is
//  named in error 1045's message, which is where it shows up.
// ============================================================================
bool ValidarValorDeConexao(const std::string& valor, const char* chave,
                           std::string& erro)
{
    if (valor.empty()) return true;      // absent/empty is handled by the caller

    const char p = valor.front(), u = valor.back();
    const bool comeca = (p == ' ' || p == '\t');
    const bool termina = (u == ' ' || u == '\t');
    if (!comeca && !termina) return true;

    std::string limpo = valor;
    while (!limpo.empty() && (limpo.front() == ' ' || limpo.front() == '\t')) limpo.erase(limpo.begin());
    while (!limpo.empty() && (limpo.back()  == ' ' || limpo.back()  == '\t')) limpo.pop_back();

    // The brackets are the instrument: they give the value an edge, and it's
    // against that edge that the gap of a space becomes visible. Without them
    // the message would be as invisible as the three it replaces.
    //
    // 600 and not 420: the fixed text is ~335 bytes and the two %.80s can add
    // up to 160, which would overflow 420 and make snprintf cut off exactly the
    // end — which is where the instruction lives ("delete the space"). A
    // message truncated at the advice is what you're left with when the host
    // name is long, and that's exactly the owner using a provider's address.
    // Registrar() takes 1024 and the prefix costs 35, so 600 fits with room to
    // spare.
    char b[600];
    std::snprintf(b, sizeof(b),
        "\"%s\" %s com ESPACO. Espaco nao aparece na tela — por isso o valor "
        "parece certo quando voce confere o arquivo. Entre colchetes ele fica "
        "visivel: [%.80s] tem %zu caracteres; o certo e [%.80s], com %zu. "
        "Apague o espaco no config.json. Nao aparo sozinho de proposito: isso "
        "mudaria calado o endereco, o usuario ou o banco que voce escreveu.",
        chave,
        (comeca && termina) ? "comeca E termina" : (comeca ? "comeca" : "termina"),
        valor.c_str(), valor.size(), limpo.c_str(), limpo.size());
    erro = b;
    return false;
}
}   // anonymous namespace

const Sql& SqlDoSqlite() { return SQL_SQLITE; }
const Sql& SqlDoMysql()  { return SQL_MYSQL;  }

// ============================================================================
//  LerConfigBanco — Database / Mysql* / DbPathOverride
// ============================================================================
bool LerConfigBanco(const char* caminhoJson, const char* caminhoPadraoDb,
                    ConfigBanco& saida, std::string& erro)
{
    erro.clear();
    saida = ConfigBanco();
    saida.caminhoSqlite = caminhoPadraoDb ? caminhoPadraoDb : "";

    if (!caminhoJson || !*caminhoJson) return true;      // no config: defaults

    std::string texto;
    bool existe = false;
    if (!LerArquivo(caminhoJson, texto, existe, erro)) return false;
    if (!existe) return true;

    JsonEmMemoria j;
    if (!j.Abrir(erro)) return false;
    if (!j.Valido(texto))
    {
        erro = "permission.json nao e JSON valido. Procure virgula sobrando, "
               "aspas nao fechadas ou chave duplicada.";
        return false;
    }

    struct Lido
    {
        std::string tipo, host, usuario, senha, banco, caminho;
        int64_t porta = 0, msConectar = 0, msOperar = 0;
        bool temPorta = false;
        // What the owner WROTE in MysqlPort, as text. It exists only for the
        // error message — see INV-CONFIG-001 below, at the port's refusal.
        std::string portaComoEscrita;
    } L;

    const char* q =
        "SELECT json_extract(?1,'$.Database'), json_extract(?1,'$.MysqlHost'), "
        "       json_extract(?1,'$.MysqlUser'), json_extract(?1,'$.MysqlPass'), "
        "       json_extract(?1,'$.MysqlDB'),   json_extract(?1,'$.MysqlPort'), "
        "       json_extract(?1,'$.DbPathOverride'), "
        "       json_extract(?1,'$.MysqlTempoConectarMs'), "
        "       json_extract(?1,'$.MysqlTempoOperarMs');";
    if (!j.Percorrer(q, texto, [](sqlite3_stmt* st, void* p)
    {
        Lido* l = static_cast<Lido*>(p);
        const char* t;
        if ((t = Txt(st, 0))) l->tipo    = t;
        if ((t = Txt(st, 1))) l->host    = t;
        if ((t = Txt(st, 2))) l->usuario = t;
        if ((t = Txt(st, 3))) l->senha   = t;
        if ((t = Txt(st, 4))) l->banco   = t;
        if (sqlite3_column_type(st, 5) != SQLITE_NULL)
        {
            // The int64 comes FIRST, as it always did: it's what decides, and
            // the preserved order is the one the tests already exercised. The
            // text is collected afterwards and only feeds the error message.
            l->porta = sqlite3_column_int64(st, 5); l->temPorta = true;
            if ((t = Txt(st, 5))) l->portaComoEscrita = t;
        }
        if ((t = Txt(st, 6))) l->caminho = t;
        if (sqlite3_column_type(st, 7) != SQLITE_NULL) l->msConectar = sqlite3_column_int64(st, 7);
        if (sqlite3_column_type(st, 8) != SQLITE_NULL) l->msOperar   = sqlite3_column_int64(st, 8);
    }, &L, erro)) return false;

    // ── a key written in the wrong case ─────────────────────────────────────
    //
    // INV-CONFIG-002. `json_extract` matches the key EXACTLY: "$.Database"
    // finds neither "database" nor "DATABASE". Anyone writing the key in the
    // wrong case had the whole block ignored, fell through to sqlite quietly,
    // and only found out weeks later — looking for the data in MySQL and not
    // finding it.
    //
    // It's the SAME damage the value refusal just below already closes
    // ("mysqll"), by a different route. Closing one and leaving the other open
    // is the worst of both: it gives the impression that the configuration is
    // checked.
    //
    // Here we only REFUSE — never guess. Accepting "database" quietly would
    // create a second valid spelling that appears in no document, and the next
    // server owner would copy the wrong spelling off a forum.
    {
        static const char* const ESPERADAS[] = {
            "Database", "MysqlHost", "MysqlUser", "MysqlPass", "MysqlDB",
            "MysqlPort", "DbPathOverride", "MysqlTempoConectarMs",
            "MysqlTempoOperarMs"
        };
        std::string escritas;
        if (!j.Percorrer("SELECT key FROM json_each(?1);", texto,
                         [](sqlite3_stmt* st, void* p)
        {
            const char* k = Txt(st, 0);
            if (k) { *static_cast<std::string*>(p) += k; *static_cast<std::string*>(p) += '\n'; }
        }, &escritas, erro)) return false;

        size_t ini = 0;
        while (ini < escritas.size())
        {
            size_t fim = escritas.find('\n', ini);
            if (fim == std::string::npos) fim = escritas.size();
            std::string k = escritas.substr(ini, fim - ini);
            ini = fim + 1;
            if (k.empty()) continue;

            std::string kmin = k;
            for (char& c : kmin) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

            for (const char* e : ESPERADAS)
            {
                std::string emin = e;
                for (char& c : emin) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                // It only complains about a key whose CASE is wrong. An
                // identical key passes straight through, and a key that looks
                // like none of ours is ignored on purpose: the owner may keep
                // notes in their own file.
                if (kmin == emin && k != e)
                {
                    char b[320];
                    std::snprintf(b, sizeof(b),
                        "no permission.json a chave \"%.40s\" esta escrita com a "
                        "caixa errada — o nome certo e \"%s\" (maiusculas e "
                        "minusculas contam). Do jeito que esta, ela seria "
                        "IGNORADA e seus dados iriam para o banco em arquivo "
                        "local sem nenhum aviso.", k.c_str(), e);
                    erro = b;
                    return false;
                }
            }
        }
    }

    // ── which database ──────────────────────────────────────────────────────
    //
    // Absent = sqlite, which is the default and what the plugin always did. An
    // unknown value = REFUSAL, and that's deliberate: whoever writes "mysqll"
    // and falls through to sqlite quietly writes the VIPs into a local file
    // nobody looks at, and only finds out weeks later, looking for the data in
    // MySQL.
    {
        std::string t = L.tipo;
        for (char& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        while (!t.empty() && (t.front() == ' ' || t.front() == '\t')) t.erase(t.begin());
        while (!t.empty() && (t.back()  == ' ' || t.back()  == '\t')) t.pop_back();

        if (t.empty() || t == "sqlite" || t == "sqlite3") saida.tipo = ConfigBanco::SQLITE;
        else if (t == "mysql" || t == "mariadb")          saida.tipo = ConfigBanco::MYSQL;
        else
        {
            char b[320];
            std::snprintf(b, sizeof(b),
                "\"Database\": \"%.60s\" nao existe. Os valores aceitos sao "
                "\"sqlite\" (o padrao, banco em arquivo local) e \"mysql\". "
                "Nao vou escolher por voce: se eu caisse no sqlite calado, seus "
                "dados iriam para um arquivo local e voce so descobriria "
                "procurando no MySQL e nao achando.", L.tipo.c_str());
            erro = b;
            return false;
        }
    }

    if (!L.caminho.empty()) saida.caminhoSqlite = L.caminho;
    if (!L.host.empty())    saida.host    = L.host;
    if (!L.usuario.empty()) saida.usuario = L.usuario;
    saida.senha = L.senha;                       // an empty password is valid

    // ── A DEFECT FOUND BY THE TEST, not by desk review ──────────────────────
    //
    // The first version let `banco` fall back to the default "conan_permission"
    // when the config carried no MysqlDB. The test demanding a REFUSAL failed
    // it — and the test was right. Two kinds of damage:
    //   1. the owner who forgot the key got a MySQL error about a database they
    //      never wrote anywhere ("Unknown database 'conan_permission'"), and
    //      would go looking for where that name came from;
    //   2. the owner who BY CHANCE has a database called conan_permission (it's
    //      the name the documentation uses as an example) would write the VIPs
    //      into it with no error at all, believing they were using another.
    // It has no default on purpose: "where to write" is not something to
    // guess.
    if (L.banco.empty())
    {
        if (saida.tipo == ConfigBanco::MYSQL)
        {
            erro = "\"Database\": \"mysql\" sem \"MysqlDB\". Diga o nome do banco — "
                   "ex.: \"MysqlDB\": \"conan_permission\". Nao escolho por voce: se "
                   "eu chutasse um nome, seus dados poderiam ir para um banco que "
                   "voce nao pretendia usar, e sem erro nenhum.";
            return false;
        }
        saida.banco.clear();          // on sqlite this field isn't used
    }
    else saida.banco = L.banco;

    // ── INV-CONFIG-003: with no MysqlUser, we do NOT fall back to "root" ─────
    //
    //  A REAL DEFECT, measured on 2026-08-18 (testes/simulador_dono.cpp). With
    //  "Database": "mysql" and the MysqlUser key absent or empty,
    //  ConfigBanco's default — usuario = "root" — came into force, and the log
    //  said:
    //        o MySQL recusou o login do usuario 'root'
    //  The owner never wrote "root" anywhere. Three kinds of damage:
    //
    //   1. it's a FALLBACK THAT WIDENS PERMISSION: absent an instruction, the
    //      plugin picked the database's most powerful account on its own.
    //      §11 of ENGENHARIA-DE-ALTA-GARANTIA forbids that by name;
    //   2. permission.json's own comment says the opposite — "NAO use o root:
    //      crie um usuario so para isto". File and code said opposite things
    //      about the same key, and the code won quietly;
    //   3. when the owner's root BY CHANCE accepts the password they pasted
    //      (the common case on a freshly installed local MySQL), there's no
    //      error at all: the plugin logs in as root and creates the tables as
    //      root, silently. It's the same damage the defaultless MysqlDB avoids
    //      just above, and for the same reason: what wasn't said isn't guessed.
    //
    //  IT COMES AFTER MysqlDB on purpose: when both are missing, the owner
    //  hears first the same complaint they heard before this guard existed.
    //  Changing the order just to put the new thing in front would trade an
    //  already-decided message (tested in 9b) for nothing.
    if (saida.tipo == ConfigBanco::MYSQL && L.usuario.empty())
    {
        erro = "\"Database\": \"mysql\" sem \"MysqlUser\". Diga com qual usuario "
               "entrar no MySQL — ex.: \"MysqlUser\": \"conan\". Nao uso o root "
               "por conta propria: escolher sozinho a conta mais poderosa do "
               "banco, so porque voce esqueceu uma linha, e o tipo de ajuda que "
               "sai caro. Crie um usuario so para este plugin.";
        return false;
    }

    // ── INV-CONFIG-002: a trailing space, refused here and not further on ────
    //
    // ONLY on the MySQL path, and that's deliberate: on sqlite these three keys
    // are used for nothing, and refusing to start for somebody running sqlite
    // because of a space in a field nobody reads would turn an inert detail
    // into a stopped server. The example file already ships with the keys
    // present, so that case isn't hypothetical.
    if (saida.tipo == ConfigBanco::MYSQL)
    {
        if (!ValidarValorDeConexao(L.host,    "MysqlHost", erro)) return false;
        if (!ValidarValorDeConexao(L.usuario, "MysqlUser", erro)) return false;
        if (!ValidarValorDeConexao(L.banco,   "MysqlDB",   erro)) return false;
    }

    if (L.temPorta)
    {
        if (L.porta < 1 || L.porta > 65535)
        {
            // ── INV-CONFIG-001 ──────────────────────────────────────────────
            //  The refusal message shows what the owner WROTE, not what the
            //  parser understood.
            //
            //  A REAL DEFECT, measured on 2026-08-18: with
            //  "MysqlPort": "a porta padrao" in the file, this line said
            //        "MysqlPort": 0 nao e uma porta
            //  That `0` exists nowhere in their file. The owner goes looking
            //  for a zero they never typed, doesn't find it, and the message —
            //  technically correct — takes them nowhere.
            //
            //  json_extract returns the text when the value is text; that's
            //  what comes in here. Note that "33061" IN QUOTES still works and
            //  never reaches this refusal: quotes around a number are the most
            //  common mistake of somebody editing JSON for the first time, and
            //  it's harmless — the value read is the same. We don't refuse what
            //  does no harm.
            char b[400];
            const bool ehTexto = !L.portaComoEscrita.empty() &&
                                 L.portaComoEscrita != std::to_string(L.porta);
            if (ehTexto)
                std::snprintf(b, sizeof(b),
                    "\"MysqlPort\": \"%.60s\" nao e um numero de porta — isso e "
                    "texto, e o MySQL precisa do numero. Escreva  \"MysqlPort\": "
                    "3306  (3306 e o padrao do MySQL; use outro so se o seu "
                    "estiver escutando em outro lugar).",
                    L.portaComoEscrita.c_str());
            else
                std::snprintf(b, sizeof(b),
                    "\"MysqlPort\": %lld nao e uma porta. Use um numero de 1 a "
                    "65535 (o padrao do MySQL e 3306).",
                    static_cast<long long>(L.porta));
            erro = b;
            return false;
        }
        saida.porta = static_cast<uint16_t>(L.porta);
    }

    // A timeout <= 0 falls back to the default. "No limit" isn't an option
    // this code offers — see INV-MYSQL-002 in MySqlCliente.h.
    if (L.msConectar > 0) saida.msConectar = static_cast<int>(L.msConectar);
    if (L.msOperar   > 0) saida.msOperar   = static_cast<int>(L.msOperar);

    if (saida.tipo == ConfigBanco::SQLITE && saida.caminhoSqlite.empty())
    {
        erro = "sem caminho para o banco sqlite. Ou deixe \"DbPathOverride\" "
               "vazio (para usar a pasta do plugin), ou ponha um caminho valido.";
        return false;
    }
    return true;
}

// ============================================================================
//  LerConfigPermissao — groups, inheritance, permissions and aliases
// ============================================================================
bool LerConfigPermissao(const char* caminhoJson, ConfigPermissao& saida,
                        bool& existe, std::string& erro)
{
    erro.clear();
    saida = ConfigPermissao();
    existe = false;
    if (!caminhoJson || !*caminhoJson) return true;

    std::string texto;
    if (!LerArquivo(caminhoJson, texto, existe, erro)) return false;
    if (!existe) return true;

    JsonEmMemoria j;
    if (!j.Abrir(erro)) return false;
    if (!j.Valido(texto)) { erro = "permission.json nao e JSON valido"; return false; }

    // ── groups ──────────────────────────────────────────────────────────────
    //
    // The CASE for `padrao` is the same one that was already running:
    // json_extract of a `true` returns the integer 1, and somebody who wrote
    // "true" in quotes gets it right too. Copied without changing a comma, on
    // purpose.
    struct CtxG { std::vector<ConfigGrupo>* g; } cg{ &saida.grupos };
    const char* qG =
        "SELECT json_extract(v.value,'$.chave'), "
        "       coalesce(json_extract(v.value,'$.nome'),''), "
        "       coalesce(json_extract(v.value,'$.prioridade'),0), "
        "       CASE WHEN coalesce(json_extract(v.value,'$.padrao'),0) IN (1,'true') "
        "            THEN 1 ELSE 0 END, "
        "       coalesce(json_extract(v.value,'$.era'),'') "
        "FROM json_each(?1,'$.grupos') v "
        "WHERE json_extract(v.value,'$.chave') IS NOT NULL;";
    if (!j.Percorrer(qG, texto, [](sqlite3_stmt* st, void* p)
    {
        ConfigGrupo g;
        const char* t;
        if ((t = Txt(st, 0))) g.chave = t;
        if ((t = Txt(st, 1))) g.nome  = t;
        g.prioridade = sqlite3_column_int64(st, 2);
        g.padrao     = sqlite3_column_int(st, 3) != 0;
        if ((t = Txt(st, 4))) g.era   = t;
        static_cast<CtxG*>(p)->g->push_back(std::move(g));
    }, &cg, erro)) return false;

    auto acharGrupo = [&](const char* chave) -> ConfigGrupo*
    {
        if (!chave) return nullptr;
        for (ConfigGrupo& g : saida.grupos) if (g.chave == chave) return &g;
        return nullptr;
    };

    // ── inheritance ─────────────────────────────────────────────────────────
    struct CtxH { ConfigPermissao* c; } ch{ &saida };
    const char* qH =
        "SELECT json_extract(v.value,'$.chave'), h.value "
        "FROM json_each(?1,'$.grupos') v JOIN json_each(v.value,'$.herda') h;";
    if (!j.Percorrer(qH, texto, [](sqlite3_stmt* st, void* p)
    {
        const char* filho = Txt(st, 0);
        const char* pai   = Txt(st, 1);
        if (!filho || !pai) return;
        for (ConfigGrupo& g : static_cast<CtxH*>(p)->c->grupos)
            if (g.chave == filho) { g.herda.push_back(pai); return; }
    }, &ch, erro)) return false;

    // ── permissions ─────────────────────────────────────────────────────────
    //
    // "-vip.teleporte" is an explicit denial: the '-' comes off the node and
    // becomes nega=1. Same rule as before, now in C++ instead of a SQL CASE —
    // because the SQL now has to run on both databases and this bit doesn't
    // have to.
    const char* qP =
        "SELECT json_extract(v.value,'$.chave'), p.value "
        "FROM json_each(?1,'$.grupos') v JOIN json_each(v.value,'$.permissoes') p "
        "WHERE length(p.value) > 0;";
    if (!j.Percorrer(qP, texto, [](sqlite3_stmt* st, void* p)
    {
        const char* chave = Txt(st, 0);
        const char* no    = Txt(st, 1);
        if (!chave || !no || !*no) return;
        bool nega = (no[0] == '-');
        const char* limpo = nega ? no + 1 : no;
        if (!*limpo) return;
        for (ConfigGrupo& g : static_cast<CtxH*>(p)->c->grupos)
            if (g.chave == chave) { g.permissoes.emplace_back(limpo, nega); return; }
    }, &ch, erro)) return false;

    // ── permission-node aliases ─────────────────────────────────────────────
    //
    // A REAL DEFECT FOUND BY READING THE OLD CODE: the original query was
    // `SELECT a.key, a.value FROM json_each(?1,'$.apelidos_de_permissao') a` —
    // with no filter at all. The permission.json that ships in the package has,
    // inside that object, the documentation key "_leia_isto". Which means every
    // installation gained an alias called `_leia_isto` pointing at the help
    // sentence. It did no harm (nobody asks a permission for "_leia_isto"), but
    // it was a junk row in everybody's database, and the file uses `_` as its
    // documentation prefix EVERYWHERE else (_leia_isto, _identidade,
    // _fronteira, _privacidade). Here the prefix starts applying inside this
    // object too.
    struct CtxA { ConfigPermissao* c; } ca{ &saida };
    const char* qA =
        "SELECT a.key, a.value FROM json_each(?1,'$.apelidos_de_permissao') a "
        "WHERE substr(a.key,1,1) <> '_';";
    if (!j.Percorrer(qA, texto, [](sqlite3_stmt* st, void* p)
    {
        const char* de   = Txt(st, 0);
        const char* para = Txt(st, 1);
        if (!de || !para) return;
        static_cast<CtxA*>(p)->c->apelidosDeNo.emplace_back(de, para);
    }, &ca, erro)) return false;

    // ── validation: what doesn't pass here never reaches the database ───────
    for (const ConfigGrupo& g : saida.grupos)
    {
        if (!ValidarChave(g.chave.c_str(), MAX_GRUPO, "a chave de grupo",
                          "\"grupos\"", erro)) return false;
        if (!g.era.empty() &&
            !ValidarChave(g.era.c_str(), MAX_GRUPO, "o \"era\" (chave antiga)",
                          "\"grupos\"", erro)) return false;
        for (const std::string& h : g.herda)
            if (!ValidarChave(h.c_str(), MAX_GRUPO, "a chave em \"herda\"",
                              g.chave.c_str(), erro)) return false;
        for (const auto& pr : g.permissoes)
            if (!ValidarChave(pr.first.c_str(), MAX_NO, "o no de permissao",
                              g.chave.c_str(), erro)) return false;
    }
    for (const auto& a : saida.apelidosDeNo)
    {
        if (!ValidarChave(a.first.c_str(),  MAX_NO, "o apelido de permissao",
                          "\"apelidos_de_permissao\"", erro)) return false;
        if (!ValidarChave(a.second.c_str(), MAX_NO, "o destino do apelido",
                          "\"apelidos_de_permissao\"", erro)) return false;
    }

    // A repeated group key: the old SQL settled it with ON CONFLICT (the last
    // one won, quietly). Refusing is better — a permission.json with two "vip"
    // groups is an editing mistake, and the owner needs to know which of the
    // two the server was going to use.
    for (size_t i = 0; i < saida.grupos.size(); ++i)
        for (size_t k = i + 1; k < saida.grupos.size(); ++k)
            if (saida.grupos[i].chave == saida.grupos[k].chave)
            {
                char b[240];
                std::snprintf(b, sizeof(b),
                    "o grupo '%.60s' aparece duas vezes em \"grupos\". "
                    "Junte os dois num so — do jeito que esta, so um deles valeria "
                    "e voce nao teria como saber qual.", saida.grupos[i].chave.c_str());
                erro = b;
                return false;
            }
    (void)acharGrupo;
    return true;
}

std::unique_ptr<IBanco> CriarBanco(const ConfigBanco& cfg, FnLog log)
{
    switch (cfg.tipo)
    {
    case ConfigBanco::SQLITE: return CriarBancoSqlite(cfg, log);
    case ConfigBanco::MYSQL:  return CriarBancoMysql (cfg, log);
    }
    return nullptr;
}

}   // namespace Perm
