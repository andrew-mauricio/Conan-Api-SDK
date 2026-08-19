// ============================================================================
//  Banco.cpp — os DOIS dialetos lado a lado, e a leitura do config.json
//
//  ESTE É O ARQUIVO QUE SE AUDITA QUANDO OS DOIS BANCOS DISCORDAM.
//
//  A regra que o organiza: o que é deliberadamente IGUAL nos dois está escrito
//  UMA vez (namespace `comum`); o que é diferente está escrito DUAS vezes, uma
//  embaixo da outra, para a diferença ser visível de relance. Não existe aqui
//  "um SQL que serve aos dois por sorte" — o que existe é SQL igual porque foi
//  conferido nos dois servidores de verdade, e SQL diferente porque tem de ser.
//
//  O QUE FOI MEDIDO, NÃO SUPOSTO (18/08/2026, MySQL 8.4.11 e MariaDB 10.11)
//  ------------------------------------------------------------------------
//  · identificador `no` sem aspas: aceito nos dois. `NO` é palavra NÃO
//    reservada no MySQL. Ainda assim o esquema do MySQL escreve tudo com crase,
//    porque o esquema é texto exclusivo dele e crase não custa nada;
//  · `INSERT ... SELECT ... ON DUPLICATE KEY UPDATE`: aceito nos dois;
//  · o SELECT determinístico de id (tabela derivada + ORDER BY ord LIMIT 1):
//    aceito nos dois, e no SQLite também;
//  · COLLATE utf8mb4_bin deixa a comparação SENSÍVEL A MAIÚSCULAS, que é como
//    o SQLite compara TEXT por padrão (BINARY). Sem isto, o padrão do MySQL 8
//    (utf8mb4_0900_ai_ci) faria "vip" e "VIP" virarem o MESMO grupo, e dois
//    jogadores de id diferente virarem o mesmo jogador. Conferido: com
//    utf8mb4_bin os dois bancos guardam 'vip' e 'VIP' como linhas separadas;
//  · DIVERGÊNCIA CONHECIDA E DECLARADA: utf8mb4_bin é PAD SPACE. `chave='vip '`
//    casa com 'vip' no MySQL e NÃO casa no SQLite (medido: 1 contra 0). Para a
//    configuração isso está FECHADO na origem — ValidarChave() recusa chave com
//    espaço na ponta. Para o id de jogador que chega pela ABI, não há como
//    fechar sem mudar a ABI: fica como risco residual declarado.
// ============================================================================
#include "Banco.h"
#include "Armazem.h"                     // só pelos tetos MAX_ID/MAX_GRUPO/MAX_NO
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
//  1 · ESQUEMA — escrito duas vezes, de propósito
// ============================================================================

// ── SQLite ──────────────────────────────────────────────────────────────────
// Byte a byte o mesmo esquema que já rodava, só partido em um comando por
// entrada (ver o comentário de Sql::esquema em Banco.h).
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

// Auditoria. Existe porque "quem deu VIP para esse cara?" é a primeira
// pergunta de todo dono de servidor com mais de um admin.
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
// AS DECISÕES QUE NÃO SÃO TRADUÇÃO MECÂNICA, E O DEFEITO QUE CADA UMA EVITA:
//
//  · ENGINE=InnoDB explícito. MyISAM ACEITA a sintaxe de FOREIGN KEY e a
//    IGNORA em silêncio — o esquema "cria" e as chaves estrangeiras
//    simplesmente não existem. Num servidor com default-storage-engine=MyISAM
//    o ON DELETE CASCADE viraria decoração e apagar um grupo deixaria vínculo
//    órfão. InnoDB escrito à mão é a única forma de isso não depender da
//    configuração da máquina do dono.
//
//  · COLLATE=utf8mb4_bin em toda tabela. Ver o cabeçalho: sem isto, "vip" e
//    "VIP" viram o mesmo grupo e dois jogadores viram um. É a diferença que
//    mais silenciosamente troca dado de dono.
//
//  · TEXT (e não VARCHAR) para todo campo livre — nome, visto_nome, quem,
//    alvo, detalhe, valor. Motivo: o MySQL destes servidores roda com
//    STRICT_TRANS_TABLES (medido), e nesse modo gravar 300 caracteres num
//    VARCHAR(256) é ERRO, não truncamento. O SQLite aceitaria. Um rótulo de
//    grupo comprido no permission.json derrubaria a configuração inteira só no
//    MySQL. TEXT não tem esse limite e iguala o comportamento dos dois.
//
//  · VARCHAR com tamanho igual ao teto da ABI onde a coluna é INDEXADA
//    (índice precisa de tamanho): id/chave/apelido de grupo = 64
//    (MAX_ID/MAX_GRUPO), nó de permissão = 127 (MAX_NO-1, que é o maior nó que
//    a ABI consegue sequer perguntar — ComprimentoLimitado exige o '\0' dentro
//    de MAX_NO bytes). Guardar 128 seria guardar um valor que nenhuma consulta
//    alcança.
//
//    ┌ CORREÇÃO DE UMA AFIRMAÇÃO MINHA QUE ERA FALSA (18/08/2026) ───────────┐
//    │ Este comentário dizia que o 127 existia para caber no limite de 767   │
//    │ bytes do InnoDB antigo, porque `PRIMARY KEY(jogador,no)` daria        │
//    │ 64*4 + 128*4 = 768. Fui medir num MySQL 5.7.44 de verdade, subido com │
//    │ innodb_large_prefix=OFF e ROW_FORMAT=COMPACT (o pior caso), e a       │
//    │ afirmação NÃO se sustentou: a tabela com VARCHAR(128) foi criada sem  │
//    │ erro nenhum.                                                          │
//    │                                                                       │
//    │ O limite de 767 bytes é POR COLUNA indexada, não pela soma da chave   │
//    │ composta. Medido no mesmo servidor:                                   │
//    │   VARCHAR(200) (800 B) numa coluna só -> ERROR 1071 "key was too long"│
//    │   VARCHAR(191) (764 B) numa coluna só -> criou                        │
//    │   (64 + 128) = 768 B somados em duas colunas -> criou                 │
//    │                                                                       │
//    │ Ou seja: o motivo estava errado, o número está certo por outro        │
//    │ motivo. Ficou escrito porque comentário que justifica com um perigo   │
//    │ inexistente é pior que comentário nenhum — o próximo a ler acredita.  │
//    └───────────────────────────────────────────────────────────────────────┘
//
//  · BIGINT AUTO_INCREMENT no lugar de INTEGER AUTOINCREMENT.
//
//  · Índice explícito na coluna de FK (o MySQL exige; o SQLite não).
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
//  2 · COMANDOS IGUAIS NOS DOIS — escritos uma vez só, de propósito
//
//  Escrever estes duas vezes seria criar duas cópias da mesma verdade. Elas
//  divergem, e ninguém percebe até o dia em que importa. O que prova que estão
//  certos NOS DOIS não é o texto: é a bateria de comportamento rodando contra
//  o SQLite e contra o MySQL de verdade (testes/teste_banco.cpp).
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

    // Sem INSERT OR IGNORE / INSERT IGNORE nos três abaixo, e isso é decisão,
    // não esquecimento. As três tabelas acabaram de ser esvaziadas, então
    // duplicata só pode vir do próprio permission.json repetindo uma linha —
    // e isso é resolvido ANTES, deduplicando em C++ (ver AplicarConfig), com
    // log. O `INSERT IGNORE` do MySQL, além de ignorar chave repetida, engole
    // violação de chave estrangeira e truncamento de dado: usá-lo aqui
    // transformaria um erro real em silêncio.
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

    // ── reconstrução do instantâneo ─────────────────────────────────────────
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

    // ── escrita ─────────────────────────────────────────────────────────────
    //
    // O id do grupo pela chave ATUAL ou por qualquer APELIDO dele. É o que faz
    // `/perm dar Fulano vip` continuar funcionando depois de o grupo ter sido
    // renomeado para "premium".
    //
    // POR QUE A TABELA DERIVADA COM `ord`, em vez de UNION ALL + LIMIT 1: sem
    // o ORDER BY, qual das duas metades vem primeiro é escolha do otimizador.
    // Se alguém criar um grupo cuja chave é o APELIDO de outro grupo, as duas
    // metades devolvem linha e o resultado passa a depender do plano de
    // execução — o mesmo comando dando respostas diferentes em bancos
    // diferentes, ou no mesmo banco depois de um ANALYZE. `ord` fixa a
    // precedência: chave atual ganha do apelido, sempre.
    const char* const GRUPO_ID_POR_CHAVE_OU_APELIDO =
        "SELECT id FROM ("
        "  SELECT 0 AS ord, id    AS id FROM grupo         WHERE chave=?1 "
        "  UNION ALL "
        "  SELECT 1 AS ord, grupo AS id FROM grupo_apelido WHERE de=?1"
        ") t ORDER BY ord LIMIT 1;";

    // Agora que o id vem resolvido, apagar é por id: sem subconsulta, sem
    // dialeto, e o número de linhas apagadas quer dizer exatamente o que
    // parece querer dizer nos dois bancos.
    const char* const VINCULO_APAGAR =
        "DELETE FROM jogador_grupo WHERE jogador=?1 AND grupo=?2;";

    // A ação vira parâmetro (era literal 'conceder'/'revogar' em dois comandos
    // quase iguais). Um comando só, um lugar para conferir.
    const char* const DIARIO_INSERIR =
        "INSERT INTO diario(em,quem,acao,alvo,detalhe) VALUES(?1,?2,?3,?4,?5);";

    const char* const FAXINA_VENCIDOS =
        "DELETE FROM jogador_grupo WHERE expira_em<>0 AND expira_em<=?1;";
}

// ============================================================================
//  3 · COMANDOS QUE DIFEREM — escritos duas vezes, um embaixo do outro
//
//  Todos são upsert. O SQLite fala ON CONFLICT(...) DO UPDATE/DO NOTHING
//  (sintaxe do PostgreSQL, que ele adotou); o MySQL nunca teve ON CONFLICT e
//  fala ON DUPLICATE KEY UPDATE. Não há tradução automática entre as duas:
//  a do SQLite nomeia a coluna do conflito, a do MySQL vale para QUALQUER
//  chave única da tabela.
//
//  O `x=x` do lado MySQL é o "não faça nada" dele: um UPDATE que não muda
//  nada. Escrever `INSERT IGNORE` no lugar seria mais curto e estaria errado —
//  IGNORE também engole erro de verdade (chave estrangeira, truncamento).
// ============================================================================

// meta ───────────────────────────────────────────────────────────────────────
const char* const META_GRAVAR_VERSAO_SQLITE =
    "INSERT INTO meta(chave,valor) VALUES('esquema_versao',?1) "
    "ON CONFLICT(chave) DO NOTHING;";
const char* const META_GRAVAR_VERSAO_MYSQL =
    "INSERT INTO meta(chave,valor) VALUES('esquema_versao',?1) "
    "ON DUPLICATE KEY UPDATE chave=chave;";

// o apelido que o renome deixa para trás ─────────────────────────────────────
// Guarda a chave VELHA apontando para o grupo que tem essa chave AGORA (antes
// da troca). É o que mantém certo o plugin de terceiro compilado perguntando
// por "vip" depois de o grupo virar "premium".
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
// (o `visto_nome` explícito existe porque a coluna é TEXT NOT NULL e TEXT no
//  MySQL não aceita DEFAULT; no SQLite ela tem DEFAULT '' e a diferença some.)

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
//  4 · as duas tabelas montadas
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
//  5 · o JSON, lido pelo json1 num SQLite EM MEMÓRIA
//
//  O porquê está em Banco.h (LerConfigPermissao). Em uma linha: json_each é
//  função de tabela do SQLite e não atravessa para o MySQL; escrever um parser
//  de JSON à mão seria código novo lendo arquivo que o dono edita à mão. Então
//  o mesmo json1 que já fazia o trabalho continua fazendo — num banco de
//  memória que existe só para isso.
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

    // O json1 é o mesmo parser que a Funcom já carrega no processo. `json_valid`
    // ANTES de qualquer json_extract: extrair de JSON quebrado devolve NULL em
    // silêncio, e configuração vazia com cara de configuração lida é pior que
    // erro.
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

    // Roda `sql` com o texto do JSON ligado em ?1 e chama `fn` por linha.
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

// Lê o arquivo inteiro. Ausência NÃO é erro (o banco já pode estar pronto).
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

// ── validação de chave, e o defeito que ela fecha ────────────────────────────
//
// GUARDA NOVA — e guarda nova sem calibração é guarda em que não se confia. Os
// dois lados estão exercitados em testes/teste_banco.cpp: ela REPROVA chave
// longa demais, vazia e com espaço na ponta, e APROVA as chaves do
// permission.json que vem no pacote.
//
// O QUE ELA FECHA:
//  1. tamanho — o MySQL indexado tem VARCHAR(64)/VARCHAR(127) e roda em modo
//     STRICT: uma chave de 200 caracteres seria ERRO e derrubaria a
//     configuração inteira. No SQLite ela seria GRAVADA e depois descartada
//     em silêncio na montagem do instantâneo ("apelido longo demais"). Dois
//     comportamentos diferentes para o mesmo arquivo. Recusar na entrada,
//     dizendo qual é a chave, iguala os dois e ainda conta ao dono o que
//     está errado;
//  2. espaço na ponta — utf8mb4_bin é PAD SPACE: no MySQL "vip " e "vip" são
//     a MESMA chave; no SQLite são duas. Recusar aqui fecha a divergência na
//     origem, que é o único lugar onde dá para fechar.
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
//  INVARIANTE  INV-CONFIG-002
//  Nome:        espaço na ponta de host, usuário e banco é recusado NA ENTRADA
//  Descrição:   MysqlHost, MysqlUser e MysqlDB não podem começar nem terminar
//               com espaço ou tab.
//  Dano se quebrado: o dono não consegue consertar o que não consegue VER.
//  Camadas:     esta função — e só ela pode ser, porque é o último ponto em que
//               o texto do arquivo ainda existe do jeito que ele digitou.
//
//  O DEFEITO REAL QUE MOTIVOU, medido em 18/08/2026 rodando sob Wine contra o
//  MySQL 8.4.11 de teste (testes/simulador_dono.cpp, um config por vez):
//
//    MysqlHost "127.0.0.1 "        -> "nao consegui resolver o endereco
//                                      '127.0.0.1 '"
//    MysqlUser "conan "            -> erro 1045, "Access denied for user
//                                      'conan '@..."
//    MysqlDB   "conan_permission " -> erro 1102 CRU, "Incorrect database name
//                                      'conan_permission '" — sem tradução
//                                      nenhuma e sem dizer o que fazer.
//
//  As três mensagens são VERDADEIRAS e INÚTEIS: o espaço não aparece na tela.
//  O dono lê '127.0.0.1 ', reconhece o endereço que ele mesmo digitou, confere
//  o arquivo, vê a mesma coisa — e conclui que o plugin está com defeito. Ele
//  não tem como ver a diferença; nós temos.
//
//  POR QUE RECUSAR E NÃO APARAR SOZINHO: aparar em silêncio faria o plugin
//  conectar como um usuário — ou num banco — DIFERENTE do que está escrito no
//  arquivo. Este código já decidiu essa mesma questão uma vez, para chave de
//  grupo (ValidarChave, logo acima), pelo mesmo motivo. Duas respostas
//  diferentes para o mesmo problema no mesmo arquivo é o que confunde.
//
//  E POR QUE A SENHA FICA DE FORA: senha com espaço na ponta é uma senha
//  legítima. Recusar quebraria quem tem uma. A senha é dita na mensagem do
//  erro 1045, que é onde ela aparece.
// ============================================================================
bool ValidarValorDeConexao(const std::string& valor, const char* chave,
                           std::string& erro)
{
    if (valor.empty()) return true;      // ausente/vazio é tratado por quem chama

    const char p = valor.front(), u = valor.back();
    const bool comeca = (p == ' ' || p == '\t');
    const bool termina = (u == ' ' || u == '\t');
    if (!comeca && !termina) return true;

    std::string limpo = valor;
    while (!limpo.empty() && (limpo.front() == ' ' || limpo.front() == '\t')) limpo.erase(limpo.begin());
    while (!limpo.empty() && (limpo.back()  == ' ' || limpo.back()  == '\t')) limpo.pop_back();

    // Os colchetes são o instrumento: eles dão uma borda ao valor, e é contra a
    // borda que o vão do espaço aparece. Sem eles a mensagem seria tão invisível
    // quanto as três que ela substitui.
    //
    // 600 e não 420: o texto fixo tem ~335 bytes e os dois %.80s podem somar
    // 160, o que estouraria 420 e faria o snprintf cortar justamente o fim —
    // que é onde está a instrução ("apague o espaço"). Mensagem truncada no
    // conselho é a que sobra quando o nome do host é longo, e é exatamente o
    // caso do dono que usa um endereço de provedor. Registrar() aceita 1024 e o
    // prefixo custa 35, então 600 cabe com folga.
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
}   // namespace anônimo

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

    if (!caminhoJson || !*caminhoJson) return true;      // sem config: padrões

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
        // O que o dono ESCREVEU na MysqlPort, em texto. Existe só para a
        // mensagem de erro — ver INV-CONFIG-001 abaixo, na recusa da porta.
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
            // O int64 vem PRIMEIRO, como sempre veio: é ele que decide, e a
            // ordem preservada é a que os testes já exercitaram. O texto é
            // colhido depois e só alimenta a mensagem de erro.
            l->porta = sqlite3_column_int64(st, 5); l->temPorta = true;
            if ((t = Txt(st, 5))) l->portaComoEscrita = t;
        }
        if ((t = Txt(st, 6))) l->caminho = t;
        if (sqlite3_column_type(st, 7) != SQLITE_NULL) l->msConectar = sqlite3_column_int64(st, 7);
        if (sqlite3_column_type(st, 8) != SQLITE_NULL) l->msOperar   = sqlite3_column_int64(st, 8);
    }, &L, erro)) return false;

    // ── chave escrita na caixa errada ───────────────────────────────────────
    //
    // INV-CONFIG-002. `json_extract` casa a chave EXATAMENTE: "$.Database" não
    // encontra "database" nem "DATABASE". Quem escrevesse a chave na caixa
    // errada tinha o bloco inteiro ignorado, caía no sqlite calado e só
    // descobria semanas depois — procurando o dado no MySQL e não achando.
    //
    // É o MESMO dano que a recusa de valor logo abaixo já fecha ("mysqll"), por
    // um caminho diferente. Fechar um e deixar o outro aberto é a pior das
    // combinações: dá a impressão de que a configuração é conferida.
    //
    // Aqui só se RECUSA — nunca se adivinha. Aceitar "database" calado criaria
    // uma segunda grafia válida que não está em documento nenhum, e o próximo
    // dono de servidor copiaria a grafia errada de um fórum.
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
                // Só reclama de quem ERROU a caixa. Chave idêntica passa direto,
                // e chave que não parece nenhuma das nossas é ignorada de
                // propósito: o dono pode ter anotações no arquivo dele.
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

    // ── qual banco ──────────────────────────────────────────────────────────
    //
    // Ausente = sqlite, que é o padrão e o que o plugin sempre fez. Valor
    // desconhecido = RECUSA, e isto é deliberado: quem escreve "mysqll" e cai
    // no sqlite em silêncio grava os VIPs num arquivo local que ninguém olha,
    // e só descobre semanas depois, procurando o dado no MySQL.
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
    saida.senha = L.senha;                       // senha vazia é escolha válida

    // ── DEFEITO ACHADO PELO TESTE, e não por revisão de mesa ────────────────
    //
    // A primeira versão deixava `banco` cair no padrão "conan_permission"
    // quando o config não trazia MysqlDB. O teste que exigia RECUSA reprovou —
    // e estava certo. Dois estragos:
    //   1. o dono que esqueceu a chave recebia um erro do MySQL sobre um banco
    //      que ele nunca escreveu em lugar nenhum ("Unknown database
    //      'conan_permission'"), e ia procurar de onde saiu esse nome;
    //   2. o dono que POR ACASO tem um banco chamado conan_permission (é o
    //      nome que a documentação usa de exemplo) escreveria os VIPs nele
    //      sem erro nenhum, achando que estava usando outro.
    // Fica sem padrão de propósito: "onde gravar" não é coisa para adivinhar.
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
        saida.banco.clear();          // no sqlite este campo não é usado
    }
    else saida.banco = L.banco;

    // ── INV-CONFIG-003: sem MysqlUser, NÃO se cai em "root" ──────────────────
    //
    //  DEFEITO REAL, medido em 18/08/2026 (testes/simulador_dono.cpp). Com
    //  "Database": "mysql" e a chave MysqlUser ausente ou vazia, o padrão de
    //  ConfigBanco — usuario = "root" — passava a valer, e o log dizia:
    //        o MySQL recusou o login do usuario 'root'
    //  O dono nunca escreveu "root" em lugar nenhum. Três estragos:
    //
    //   1. é um FALLBACK QUE AMPLIA PERMISSÃO: na falta de instrução, o plugin
    //      escolhia sozinho a conta mais poderosa do banco. A §11 da
    //      ENGENHARIA-DE-ALTA-GARANTIA proíbe isso por nome;
    //   2. o comentário do próprio permission.json manda o contrário — "NAO use
    //      o root: crie um usuario so para isto". Arquivo e código diziam coisas
    //      opostas sobre a mesma chave, e o código vencia calado;
    //   3. quando o root do dono POR ACASO aceita a senha que ele colou (o caso
    //      comum num MySQL local recém-instalado), não há erro nenhum: o plugin
    //      entra como root e cria as tabelas como root, em silêncio. É o mesmo
    //      estrago que o MysqlDB sem padrão evita logo acima, e pela mesma
    //      razão: o que não foi dito não se adivinha.
    //
    //  VEM DEPOIS DO MysqlDB de propósito: quando as duas faltam, o dono ouve
    //  primeiro a mesma queixa que ouvia antes desta guarda existir. Mudar a
    //  ordem só para pôr a novidade na frente trocaria uma mensagem já decidida
    //  (e testada em 9b) por nada.
    if (saida.tipo == ConfigBanco::MYSQL && L.usuario.empty())
    {
        erro = "\"Database\": \"mysql\" sem \"MysqlUser\". Diga com qual usuario "
               "entrar no MySQL — ex.: \"MysqlUser\": \"conan\". Nao uso o root "
               "por conta propria: escolher sozinho a conta mais poderosa do "
               "banco, so porque voce esqueceu uma linha, e o tipo de ajuda que "
               "sai caro. Crie um usuario so para este plugin.";
        return false;
    }

    // ── INV-CONFIG-002: espaço na ponta, recusado aqui e não lá adiante ──────
    //
    // SÓ no caminho MySQL, e isso é deliberado: no sqlite estas três chaves não
    // são usadas para nada, e recusar o arranque de quem roda sqlite por causa
    // de um espaço num campo que ninguém lê seria transformar um detalhe inerte
    // em servidor parado. O arquivo de exemplo já vem com as chaves presentes,
    // então esse caso não é hipotético.
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
            //  A mensagem de recusa mostra o que o dono ESCREVEU, não o que o
            //  parser entendeu.
            //
            //  DEFEITO REAL, medido em 18/08/2026: com "MysqlPort": "a porta
            //  padrao" no arquivo, esta linha dizia
            //        "MysqlPort": 0 nao e uma porta
            //  O `0` não existe em lugar nenhum do arquivo dele. O dono procura
            //  um zero que ele nunca digitou, não acha, e a mensagem — que
            //  estava tecnicamente certa — não o leva a lugar nenhum.
            //
            //  O json_extract devolve o texto quando o valor é texto; é ele que
            //  entra aqui. Note que "33061" ENTRE ASPAS continua funcionando e
            //  não chega nesta recusa: aspas em volta de um número é o erro mais
            //  comum de quem edita JSON pela primeira vez, e ele é inofensivo —
            //  o valor lido é o mesmo. Não se recusa o que não faz mal.
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

    // Prazo <= 0 cai no padrão. "Sem limite" não é opção que este código ofereça
    // — ver INV-MYSQL-002 em MySqlCliente.h.
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
//  LerConfigPermissao — grupos, herança, permissões e apelidos
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

    // ── grupos ──────────────────────────────────────────────────────────────
    //
    // O CASE de `padrao` é o mesmo que já rodava: json_extract de um `true`
    // devolve o inteiro 1, e alguém que escreveu "true" entre aspas também
    // acerta. Copiado sem mudar uma vírgula de propósito.
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

    // ── herança ─────────────────────────────────────────────────────────────
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

    // ── permissões ──────────────────────────────────────────────────────────
    //
    // "-vip.teleporte" é negação explícita: o '-' sai do nó e vira nega=1.
    // Mesma regra de antes, agora em C++ em vez de num CASE do SQL — porque
    // o SQL agora tem de rodar nos dois bancos e este pedaço não precisa.
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

    // ── apelidos de nó de permissão ─────────────────────────────────────────
    //
    // DEFEITO REAL ACHADO LENDO O CÓDIGO ANTIGO: a consulta original era
    // `SELECT a.key, a.value FROM json_each(?1,'$.apelidos_de_permissao') a` —
    // sem filtro nenhum. O permission.json que vai no pacote tem, dentro desse
    // objeto, a chave de documentação "_leia_isto". Ou seja: toda instalação
    // ganhava um apelido chamado `_leia_isto` apontando para a frase de ajuda.
    // Não fazia mal (ninguém pergunta permissão para "_leia_isto"), mas era uma
    // linha de lixo no banco de todo mundo, e o arquivo usa `_` como prefixo de
    // documentação em TODOS os outros lugares (_leia_isto, _identidade,
    // _fronteira, _privacidade). Aqui o prefixo passa a valer também dentro
    // deste objeto.
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

    // ── validação: o que não passa aqui não chega ao banco ──────────────────
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

    // Chave de grupo repetida: o SQL antigo resolvia por ON CONFLICT (o último
    // ganhava, calado). Recusar é melhor — um permission.json com dois grupos
    // "vip" é erro de edição, e o dono precisa saber qual dos dois o servidor
    // ia usar.
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
