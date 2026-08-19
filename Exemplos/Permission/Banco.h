// ============================================================================
//  Banco.h — a fronteira entre a DECISÃO (que dados guardar) e o MEIO (onde)
//
//  POR QUE ESTE ARQUIVO EXISTE
//  ---------------------------
//  O Armazem tinha 161 chamadas sqlite3_* espalhadas pelo corpo. A lógica de
//  permissão (herança achatada, peso do casamento, negação específica, o
//  instantâneo publicado por troca de ponteiro) está PROVADA rodando e não pode
//  ser reescrita para caber num MySQL. Então quem muda é o meio, não a decisão:
//  o Armazem passa a falar com esta interface, e existem duas implementações.
//
//  O dono de servidor escolhe no config.json com uma linha ("Database":
//  "sqlite" ou "mysql") e não programa nada.
//
//  ┌──────────────────────────────────────────────────────────────────────┐
//  │  INV-BANCO-001 · NADA AQUI É CHAMADO NA THREAD DO JOGO.              │
//  │                                                                      │
//  │  A leitura de permissão (Armazem::Tem, NoGrupo, ExpiraEm, Grupos)    │
//  │  responde do INSTANTÂNEO em memória e não toca em banco nenhum —     │
//  │  nem SQLite. Só a thread escritora (Armazem::Trabalhar) e o arranque │
//  │  (Armazem::Abrir) chamam esta interface.                             │
//  │                                                                      │
//  │  Isto não é preferência de desenho: BancoMysql fala em socket. Um    │
//  │  MySQL do outro lado do país, atrás de rede ruim, é problema do      │
//  │  banco. Se essa espera acontecer no laço do jogo, o tick para, os    │
//  │  jogadores levam timeout e caem — o problema do banco vira problema  │
//  │  do servidor de jogo, que é justamente o que não pode acontecer.     │
//  └──────────────────────────────────────────────────────────────────────┘
//
//  INV-BANCO-002 · Falha de banco nunca vira falha de processo.
//      Toda operação devolve bool e deixa o texto em Erro(). Nunca exceção
//      atravessando a fronteira, nunca crash. Banco fora do ar significa
//      escrita recusada e log — o instantâneo publicado continua servindo as
//      leituras com o último estado bom.
//
//  INV-BANCO-003 · Instantâneo publicado nunca é meio construído.
//      Reconstruir() monta em memória e só publica no fim. Qualquer falha de
//      leitura aborta ANTES da publicação, e o instantâneo anterior continua
//      valendo. Um MySQL que cai no meio da reconstrução deixa o servidor com
//      permissões velhas e corretas, nunca com permissões pela metade.
//
//  INV-BANCO-004 · O SQL não é o mesmo nos dois, e isso é explícito.
//      Não existe "um SQL que serve aos dois". Cada dialeto tem o seu texto,
//      lado a lado em Banco.cpp, e o que é DELIBERADAMENTE igual está escrito
//      uma vez só (namespace `comum`) para não haver duas cópias da mesma
//      verdade divergindo em silêncio. O que prova que os dois concordam não é
//      o texto: é a bateria de comportamento rodando contra os dois bancos de
//      verdade (testes/teste_banco.cpp).
//
//  INV-BANCO-006 · NUNCA dizer que usa um banco e usar outro.
//      Se o config.json diz "mysql", o Permission ou fala com AQUELE MySQL ou
//      não responde nada. Não existe queda para o SQLite local — nem calada,
//      nem avisada em letras garrafais.
//
//      DANO SE QUEBRADO, e é o pior deste arquivo: os VIPs vendidos hoje vão
//      para um arquivo local que ninguém olha. Quando o MySQL voltar, existem
//      duas verdades sobre quem é VIP, e nenhuma delas é a verdade — não há
//      como reconciliar, porque ninguém sabe qual escrita veio de qual. Um
//      aviso no log não conserta isso: o dono só lê o log DEPOIS de o jogador
//      reclamar, e a essa altura o estrago já está gravado nos dois lugares.
//
//      A degradação escolhida está em Armazem.h (INV-ARMAZEM-002): banco fora
//      do ar = Permission AUSENTE, e ausente é um estado que a API já sabe
//      tratar (`se_ausente` do ConanPermission.h). Ausente é honesto; "usando
//      outro banco" é mentira gravada em disco.
//
//  INV-BANCO-005 · Interpolação de parâmetro só existe onde não há preparado.
//      O SQLite recebe `?1` e liga o valor pelo sqlite3_bind_*: o valor nunca
//      encosta no texto do SQL. O MySQL desta casa não tem prepared statement
//      (ver MySqlCliente.h), então BancoMysql substitui `?N` pelo literal já
//      passado por MySqlCliente::Citar() — que escapa E envolve em aspas. O
//      substituidor PULA o que está dentro de literal e de crase, e RECUSA o
//      comando se sobrar um `?N` sem valor: SQL com marcador solto não é
//      enviado nunca.
// ============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <memory>

namespace Perm
{
    // Também declarado em Armazem.h. Alias idêntico repetido é legal em C++, e
    // este arquivo é incluído por Armazem.h — quem incluir só este continua
    // compilando.
    using FnLog = void (*)(const char*);

    // ── uma linha do resultado ───────────────────────────────────────────────
    //
    // Só texto e inteiro, porque é só isso que este esquema guarda. Não há
    // ponto flutuante, não há BLOB. Interface pequena é interface que as duas
    // implementações conseguem cumprir do mesmo jeito.
    class ILinha
    {
    public:
        virtual ~ILinha() = default;
        virtual int         Colunas()        const = 0;
        // nullptr quando a coluna é NULL. String vazia vem como "" — são
        // coisas diferentes e os dois bancos distinguem as duas.
        virtual const char* Texto(int col)   const = 0;
        // 0 quando NULL ou quando o texto não é número. O esquema só chama
        // isto em coluna declarada inteira.
        virtual int64_t     Inteiro(int col) const = 0;
    };

    using FnLinha = void (*)(const ILinha&, void*);

    // ── um comando com parâmetros ────────────────────────────────────────────
    class IComando
    {
    public:
        virtual ~IComando() = default;

        // Posição 1..N, como o `?1` do SQL. `nullptr` liga SQL NULL (e não
        // string vazia: ligar "" onde se queria NULL grava dado errado sem
        // erro nenhum).
        virtual bool LigarTexto(int pos, const char* v)   = 0;
        virtual bool LigarInteiro(int pos, int64_t v)     = 0;

        virtual bool Executar()                           = 0;
        virtual bool Consultar(FnLinha fn, void* ctx)     = 0;

        // Linhas afetadas pelo ÚLTIMO Executar deste comando.
        //
        // CUIDADO REGISTRADO: no MySQL este número NÃO serve para descobrir se
        // uma chave existe. `INSERT ... ON DUPLICATE KEY UPDATE` devolve 0
        // quando a linha já existia com os mesmos valores — indistinguível de
        // "não achei nada para inserir". O código que precisava dessa resposta
        // (conceder para um grupo inexistente) passou a PERGUNTAR o id do
        // grupo antes, em vez de inferir do contador. Ver Armazem::Trabalhar.
        virtual int64_t Mudancas() const                  = 0;
    };

    // ── o SQL de um dialeto ──────────────────────────────────────────────────
    //
    // Cada campo é UM comando. `esquema` é vetor terminado em nullptr porque o
    // MySQL desta casa não tem CLIENT_MULTI_STATEMENTS (desligado de propósito,
    // ver MySqlCliente.h) e portanto não aceita DDL com vários `;` num pacote
    // só. O SQLite aceitaria; roda um por um do mesmo jeito, para os dois
    // caminhos serem o mesmo caminho.
    struct Sql
    {
        const char* const* esquema;

        const char* meta_gravar_versao;
        const char* meta_ler_versao;

        // configuração (permission.json -> banco)
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

        // reconstrução do instantâneo
        const char* ler_grupos;
        const char* ler_heranca;
        const char* ler_grupo_permissao;
        const char* ler_apelidos_de_grupo;
        const char* ler_apelidos_de_no;
        const char* ler_jogador_grupo;
        const char* ler_jogador_permissao;

        // escrita
        const char* jogador_garantir;
        const char* grupo_id_por_chave_ou_apelido;
        const char* vinculo_upsert;
        const char* vinculo_apagar;
        const char* visto_upsert;
        const char* diario_inserir;
        const char* faxina_vencidos;
    };

    // ── o que o config.json diz sobre o banco ────────────────────────────────
    //
    // Os nomes das chaves seguem a convenção que a comunidade de servidores de
    // sobrevivência já conhece — quem vem de outra API reconhece de imediato e
    // não precisa aprender nome novo.
    struct ConfigBanco
    {
        enum Tipo { SQLITE, MYSQL };

        Tipo        tipo   = SQLITE;
        std::string caminhoSqlite;        // já resolvido (DbPathOverride aplicado)
        std::string host   = "localhost";
        // NÃO é o padrão que o dono recebe: com tipo=MYSQL, LerConfigBanco
        // RECUSA quando MysqlUser vem vazio (INV-CONFIG-003), justamente para
        // que este "root" nunca valha por omissão — escolher sozinho a conta
        // mais poderosa do banco é fallback que amplia permissão (§11). Ele
        // sobrevive aqui só porque no sqlite o campo é inerte. Quem construir
        // um ConfigBanco na mão, sem passar por LerConfigBanco, contorna a
        // guarda: preencha o usuário.
        std::string usuario= "root";
        std::string senha;
        // SEM padrão, de propósito: "onde gravar" não se adivinha. Com
        // Database=mysql e sem MysqlDB, LerConfigBanco RECUSA. Ver lá o defeito
        // que isto conserta (um banco chutado é dado gravado no lugar errado
        // sem erro nenhum).
        std::string banco;
        uint16_t    porta  = 3306;

        // Prazos do cliente MySQL, em milissegundos. Existem no config porque
        // "o MySQL do meu VPS demora 8 s para aceitar" é um caso real, e o
        // padrão de 5 s viraria "o Permission nunca sobe" sem explicação. Zero
        // ou negativo cai no padrão — "sem limite" não é opção (INV-MYSQL-002).
        int         msConectar = 5000;
        int         msOperar   = 10000;
    };

    // ── o banco ──────────────────────────────────────────────────────────────
    class IBanco
    {
    public:
        virtual ~IBanco() = default;

        virtual bool Abrir()  = 0;
        virtual void Fechar() = 0;

        // false depois de a conexão cair. No SQLite é "o handle existe"; num
        // arquivo local não há o que cair no meio, e é por isso que a diferença
        // entre os dois está aqui e não espalhada pelo Armazem.
        virtual bool Vivo() const = 0;

        // Só a thread escritora chama. Devolve false e deixa o motivo em
        // Erro(). NÃO reexecuta nada por conta própria: quem decide repetir é
        // o Armazem, que sabe se a tarefa é idempotente.
        virtual bool Reconectar() = 0;

        // ── a ÚNICA coisa aqui que outra thread pode chamar ─────────────────
        //
        // Chamada por Armazem::Fechar(), da thread que desliga o servidor,
        // ENQUANTO a thread escritora pode estar dentro de uma operação. Faz a
        // operação em curso voltar com erro na hora, para o join não esperar o
        // prazo do banco.
        //
        // POR QUE ISTO EXISTE: com MySQL num host que aceitou a conexão e
        // emudeceu, uma reconstrução são 7 consultas de até msOperar cada —
        // até ~70 s de servidor pendurado no desligamento com os padrões. O
        // dono faz o que qualquer um faria: mata o processo. Matar o Conan no
        // desligamento perde o save do mundo. Um problema do BANCO não pode
        // terminar em save perdido.
        //
        // É DEFINITIVO: depois disto o banco não volta a servir. O único
        // chamador é o desligamento.
        virtual void Interromper() = 0;

        virtual bool Executar(const char* sql)                          = 0;
        virtual bool Consultar(const char* sql, FnLinha fn, void* ctx)  = 0;
        virtual std::unique_ptr<IComando> Preparar(const char* sql)     = 0;

        virtual bool Iniciar()   = 0;      // transação
        virtual bool Confirmar() = 0;
        virtual bool Desfazer()  = 0;

        virtual const char* Erro() const = 0;
        virtual const char* Nome() const = 0;      // "sqlite" ou "mysql"
        virtual const Sql&  S()    const = 0;

        // O que dizer ao dono do servidor quando a criação do esquema falha.
        // Mora aqui porque a resposta é diferente em cada meio — no sqlite é
        // pasta/permissão de arquivo, no MySQL é GRANT — e o Armazem não tem
        // (nem deve ter) opinião sobre isso.
        virtual const char* DicaEsquema() const = 0;
    };

    // Fábrica. Devolve nullptr só se `cfg.tipo` for desconhecido — o que não
    // acontece, porque LerConfigBanco recusa valor inválido antes.
    std::unique_ptr<IBanco> CriarBanco(const ConfigBanco& cfg, FnLog log);

    // As duas implementações. Moram em BancoSqlite.cpp e BancoMysql.cpp para
    // que cada meio possa ser auditado sozinho; quem usa chama CriarBanco().
    std::unique_ptr<IBanco> CriarBancoSqlite(const ConfigBanco& cfg, FnLog log);
    std::unique_ptr<IBanco> CriarBancoMysql (const ConfigBanco& cfg, FnLog log);

    // Os dois dialetos, lado a lado em Banco.cpp. Expostos para que o teste
    // consiga percorrer TODO comando que o Armazem emite e executá-lo contra o
    // banco de verdade — um `const char*` que nunca foi preparado é SQL que
    // ninguém sabe se compila.
    const Sql& SqlDoSqlite();
    const Sql& SqlDoMysql();

    // ── leitura do config.json ───────────────────────────────────────────────
    //
    // Lê Database / MysqlHost / MysqlUser / MysqlPass / MysqlDB / MysqlPort /
    // DbPathOverride. As demais chaves do arquivo (grupos, apelidos, _fronteira,
    // _privacidade, identidade…) são preservadas e ignoradas aqui.
    //
    // `caminhoPadraoDb` é o que a ConanApi mandou (CaminhoDados("Permission",
    // "permission.db")); DbPathOverride, quando presente, substitui.
    //
    // Devolve false com motivo em `erro` quando o arquivo existe e está errado
    // — inclusive quando "Database" traz um valor que não é nem sqlite nem
    // mysql. Recusar é obrigatório: cair calado no sqlite depois de o dono
    // escrever "mysqll" grava os VIPs num arquivo local que ninguém olha, e o
    // sintoma só aparece quando alguém procura o dado no MySQL e não acha.
    //
    // Arquivo AUSENTE não é erro: devolve true com os padrões (sqlite no
    // caminho da ConanApi), que é exatamente como o plugin já se comportava.
    bool LerConfigBanco(const char* caminhoJson, const char* caminhoPadraoDb,
                        ConfigBanco& saida, std::string& erro);

    // ── o resto do config.json, já em memória ────────────────────────────────
    //
    // POR QUE O JSON CONTINUA SENDO LIDO PELO json1 DO SQLITE
    // Antes, a configuração virava banco por um SQL só (json_each + json_extract
    // cruzando com as tabelas reais). Isso não atravessa para o MySQL: json_each
    // é função de tabela do SQLite, o MySQL 5.7 não tem JSON_TABLE nenhum, e o
    // 8.0 tem uma sintaxe diferente. Escrever um parser de JSON à mão era a
    // outra saída — 200 linhas novas lendo arquivo que o dono edita à mão, ou
    // seja, entrada hostil por definição.
    //
    // A saída é a terceira: o json1 continua fazendo o trabalho que já fazia,
    // num SQLite EM MEMÓRIA (`:memory:`), que existe só para isso e some no
    // fim. O amalgamation já está dentro do .dll de qualquer jeito. O que muda
    // é que o resultado sai em struct, e quem grava é a interface — então o
    // mesmo caminho de configuração serve aos dois bancos.
    struct ConfigGrupo
    {
        std::string chave, nome, era;
        int64_t     prioridade = 0;
        bool        padrao     = false;
        std::vector<std::string>                 herda;
        std::vector<std::pair<std::string,bool>> permissoes;   // (nó, nega)
    };

    struct ConfigPermissao
    {
        std::vector<ConfigGrupo>                            grupos;
        std::vector<std::pair<std::string,std::string>>     apelidosDeNo;
    };

    // false quando o arquivo existe e não é JSON válido, ou quando um valor
    // não cabe nos tetos da ABI. `existe` sai false quando não há arquivo — e
    // isso não é erro, é "usa o que já está no banco".
    bool LerConfigPermissao(const char* caminhoJson, ConfigPermissao& saida,
                            bool& existe, std::string& erro);
}
