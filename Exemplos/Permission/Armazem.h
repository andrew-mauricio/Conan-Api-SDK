// ============================================================================
//  Armazem.h — o núcleo de dados do Permission
//
//  DE PROPÓSITO NÃO DEPENDE DA ConanApi.
//  Nem de <windows.h> do jogo, nem da reflexão, nem de UObject. Só SQLite e
//  a biblioteca padrão. Isso não é pureza arquitetural: é o que permite
//  compilar este mesmo código num .exe de teste e RODAR o teste sob Wine, sem
//  servidor de Conan nenhum. Núcleo que só roda dentro do jogo é núcleo que só
//  se testa em produção — e a regra aqui é a oposta: compilar não prova nada,
//  só rodar prova.
//
//  A DIVISÃO QUE MANDA EM TUDO
//  --------------------------
//  LEITURA acontece no laço do jogo. Não pode abrir arquivo, não pode alocar,
//  não pode pegar trava que um escritor segure, não pode tocar em SQLite.
//  Responde de um INSTANTÂNEO em memória, já resolvido (herança de grupo
//  achatada), publicado por troca atômica de ponteiro.
//
//  ESCRITA acontece fora do laço, numa thread própria, e termina em COMMIT do
//  banco. Quem chama `Conceder` volta na hora: a tarefa foi enfileirada.
//  Depois do commit, um instantâneo novo é montado e publicado. Leitor nenhum
//  espera por escritor nenhum, em momento nenhum.
//
//  DOIS BANCOS, UMA LÓGICA (18/08/2026)
//  ------------------------------------
//  O "banco" deixou de ser SQLite direto e passou a ser Perm::IBanco (Banco.h):
//  BancoSqlite (o padrão, arquivo local em WAL) ou BancoMysql (o dono aponta
//  para um MySQL no config.json). A decisão — QUE dados guardar, como achatar
//  herança, como pesar o casamento de nó — não sabe qual dos dois está embaixo.
//
//  E a divisão acima ficou MAIS importante, não menos: com MySQL, "escrita fora
//  do laço" deixou de ser questão de gosto e virou a única coisa que separa um
//  banco lento de um servidor de jogo travado. Nenhuma consulta de permissão
//  toca no banco — nem no SQLite local.
//
//  ╔══════════════════════════════════════════════════════════════════════════╗
//  ║  O QUE ACONTECE QUANDO O BANCO DO DONO NÃO COLABORA (18/08/2026)         ║
//  ║                                                                          ║
//  ║  Isto roda no servidor DOS OUTROS, configurado por gente que não         ║
//  ║  programa. MySQL em host errado, porta fechada, senha trocada, banco     ║
//  ║  inexistente, sem GRANT, caindo no meio da noite, 2 s por consulta —     ║
//  ║  não são hipóteses, é a semana normal de quem hospeda servidor.          ║
//  ║                                                                          ║
//  ║  INV-ARMAZEM-001 · O SERVIDOR DE JOGO NÃO PODE CAIR NEM TRAVAR POR       ║
//  ║      CAUSA DO BANCO. Um MySQL fora do ar é problema do banco. Ponto.     ║
//  ║      Camadas: leitura só do instantâneo · escrita em thread própria ·    ║
//  ║      fila com teto · prazo em toda operação de rede · desligamento que   ║
//  ║      interrompe operação presa.                                          ║
//  ║                                                                          ║
//  ║  INV-ARMAZEM-002 · BANCO INDISPONÍVEL = PERMISSION AUSENTE, NUNCA        ║
//  ║      PERMISSION MENTINDO. Enquanto o banco escolhido no config.json      ║
//  ║      nunca abriu, ConanPermissionObterApi devolve nullptr: para quem     ║
//  ║      chama, é o mesmo que o plugin não estar instalado, e o              ║
//  ║      ConanPermission.h já obriga cada plugin a declarar o `se_ausente`   ║
//  ║      dele. Nada de cair para o SQLite local (INV-BANCO-006, em Banco.h): ║
//  ║      gravar VIP em outro lugar é criar duas verdades irreconciliáveis.   ║
//  ║                                                                          ║
//  ║  INV-ARMAZEM-003 · FALHA NA PARTIDA NÃO É DEFINITIVA. O plugin fica      ║
//  ║      CARREGADO e tenta de novo em segundo plano, com espera crescente e  ║
//  ║      RELENDO o config.json a cada tentativa. Quando o dono corrigir a    ║
//  ║      senha, criar o banco, dar o GRANT ou o container do MySQL           ║
//  ║      terminar de subir, o Permission entra sozinho.                      ║
//  ║                                                                          ║
//  ║      POR QUE ISTO NÃO É LUXO: neste jogo, cada reinício do servidor      ║
//  ║      abre 6 a 9 minutos em que NINGUÉM consegue entrar (medido; ver      ║
//  ║      conan-restart-abre-janela-morta). "Reinicie o servidor" como preço  ║
//  ║      de uma senha corrigida é caro demais para um erro de digitação —    ║
//  ║      e o caso mais comum de todos (docker compose subindo o jogo antes   ║
//  ║      do MySQL) se conserta sozinho em segundos.                          ║
//  ║                                                                          ║
//  ║  INV-ARMAZEM-004 · BANCO FORA DO AR NÃO VIRA MEMÓRIA NEM DISCO SEM       ║
//  ║      TETO. A fila de escrita tem teto e o log de falha tem freio. Sem    ║
//  ║      isso, um banco lento derruba o servidor por consumo — o problema    ║
//  ║      do banco virando problema do jogo por outro caminho.                ║
//  ╚══════════════════════════════════════════════════════════════════════════╝
// ============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <deque>
#include <memory>

#include "Banco.h"

namespace Perm
{
    // Toda consulta tem TRÊS respostas. Ver ConanPermission.h para o porquê.
    enum Resposta : int32_t { NAO_SEI = -1, NEGADO = 0, PERMITIDO = 1 };

    // Tem de casar com CONAN_PERM_MAX_ID do header público. Repetido aqui em vez
    // de incluído porque este arquivo não depende do header público de propósito
    // (o teste compila sem ele); Permission.cpp faz o static_assert que garante
    // que as duas cópias não divergiram — duas cópias da mesma verdade divergem
    // se ninguém conferir.
    constexpr int MAX_ID = 64;

    // Os outros dois tetos, pelo mesmo motivo e com o mesmo static_assert em
    // Permission.cpp: CONAN_PERM_MAX_GRUPO e CONAN_PERM_MAX_NO.
    constexpr int MAX_GRUPO = 64;
    constexpr int MAX_NO    = 128;

    // Texto livre que só vai para o diário de auditoria ("quem" concedeu, nome
    // de exibição do jogador). Não é chave de nada; o teto existe só para não
    // haver entrada da ABI sem teto.
    constexpr int MAX_TEXTO = 256;

    // ── entrada da ABI: `const char*` SEM comprimento ────────────────────────
    //
    // O DEFEITO QUE ISTO CONSERTA
    // A ABI é de C e passa só o ponteiro (ConanPermission.h). Todas as entradas
    // conferiam `!s || !*s` — o que pega ponteiro nulo e string vazia — e
    // DEPOIS entregavam a string a código que caminha até o '\0':
    // `Tabela::Hash` (`while (*s)`), `std::string::operator==(const char*)` em
    // `Casa`, e `t.jogador = jogador` em `Conceder`. Um plugin mal escrito (não
    // precisa ser malicioso) que passasse bytes sem terminador levava esse laço
    // para fora do mapa, e a falha acontece na thread do jogo, FORA da guarda
    // SEH do loader: mata o processo, não só o plugin.
    //
    // A defesa é o teto. Toda chave guardada é menor que o teto (Inserir recusa
    // o que não cabe), então parar de ler ali não muda resposta nenhuma para
    // entrada válida — e transforma "leio até achar zero, onde quer que ele
    // esteja" em "leio no máximo N bytes e recuso".
    //
    // O QUE ISTO **NÃO** COBRE, e não tem como cobrir sem mudar a ABI:
    //   · ponteiro inválido já no primeiro byte. Para ler qualquer coisa de um
    //     `const char*` é preciso dereferenciá-lo; não há defesa possível aqui;
    //   · uma string sem terminador que comece a menos de N bytes do fim de uma
    //     região mapeada — a leitura limitada ainda cruza a borda.
    // Cobrir os dois exigiria VirtualQuery por chamada (2.551 ns medidos sob
    // Wine, contra os 145 ns da consulta inteira: 17x mais caro no laço do
    // jogo) ou variantes com comprimento na ABI. Fica escrito em vez de
    // prometido.
    //
    // Devolve o comprimento quando há '\0' dentro de `teto` bytes; -1 quando
    // não há (e aí ninguém toca no resto), quando o ponteiro é nulo, ou quando
    // a string é vazia — vazia nunca foi chave válida aqui.
    inline int ComprimentoLimitado(const char* s, int teto)
    {
        if (!s || teto <= 0) return -1;
        for (int i = 0; i < teto; ++i)
            if (!s[i]) return i > 0 ? i : -1;
        return -1;
    }

    // ── tabela de hash sem alocação na consulta ──────────────────────────────
    //
    // Existe porque `std::unordered_map<std::string,...>::find(const char*)`
    // CONSTRÓI um std::string a cada consulta, e um id de plataforma tem 17
    // caracteres — passa do SSO de 15 da libstdc++ e vai para o malloc. Medido
    // sob Wine: 145 ns por consulta, com um malloc/free dentro. malloc no laço
    // do jogo, 60 vezes por segundo por jogador, para responder uma pergunta
    // que é só comparação de texto.
    //
    // Endereçamento aberto, chave embutida (sem ponteiro para fora), sondagem
    // linear. Zero alocação, zero indireção.
    struct Tabela
    {
        struct Item { char id[MAX_ID]; int32_t valor; };
        std::vector<int32_t> balde;      // -1 = vazio; senão índice em `itens`
        std::vector<Item>    itens;

        void    Reservar(size_t n);
        bool    Inserir(const char* chave, int32_t valor);  // false se a chave não couber
        int32_t Achar(const char* chave) const;             // -1 se não achou
        static uint64_t Hash(const char* s);
    };

    // ── um nó de permissão já resolvido para um jogador ──────────────────────
    struct NoResolvido
    {
        std::string padrao;      // "vip.kit.diario", "vip.*" ou "*"
        int32_t     casaLen;     // peso do casamento; ver Decidir()
        bool        curinga;
        bool        nega;        // negação explícita ("-vip.teleporte")
        bool        individual;  // veio de jogador_permissao, não de grupo
        int64_t     expira;      // 0 = nunca; herda o menor da cadeia
    };

    struct GrupoDoJogador
    {
        std::string chave;
        int64_t     expira;      // 0 = nunca
    };

    struct JogadorResolvido
    {
        std::vector<NoResolvido>    nos;
        std::vector<GrupoDoJogador> grupos;
    };

    // ── o instantâneo publicado ──────────────────────────────────────────────
    //
    // Imutável depois de publicado. Nunca se altera um instantâneo vivo: monta-se
    // outro e troca-se o ponteiro. Um leitor que já pegou o ponteiro antigo
    // continua lendo dado coerente e velho por alguns milissegundos — e dado
    // coerente e velho é infinitamente melhor que dado sendo alterado embaixo dele.
    struct Instantaneo
    {
        std::vector<JogadorResolvido> jogadores;
        Tabela                        idxJogador;      // id -> índice em `jogadores`

        // Quem nunca apareceu no banco também tem direitos: os do(s) grupo(s)
        // marcado(s) como padrão. Sem isto, o primeiro login de todo jogador
        // seria um jogador sem nada — e o plugin de terceiro veria "negado"
        // onde a configuração diz "permitido".
        JogadorResolvido padrao;

        // apelido de grupo -> chave atual. É o que faz o RENOME não quebrar
        // plugin já compilado que fala "vip".
        Tabela                   idxApelidoGrupo;
        std::vector<std::string> apelidoGrupoPara;
        // apelido de nó de permissão -> nó atual
        Tabela                   idxApelidoNo;
        std::vector<std::string> apelidoNoPara;

        uint64_t geracao = 0;

        // Traduz apelido -> atual sem alocar nada. Devolve o próprio `chave`
        // quando não há apelido, e um ponteiro para dentro do instantâneo
        // quando há — o instantâneo é imutável e vive mais que a consulta.
        const char* GrupoAtual(const char* chave) const;
        const char* NoAtual(const char* chave) const;
        const JogadorResolvido* Achar(const char* id) const;
    };

    class Armazem
    {
    public:
        Armazem() = default;
        ~Armazem();

        Armazem(const Armazem&)            = delete;
        Armazem& operator=(const Armazem&) = delete;

        // Abre (ou cria) o banco, aplica o esquema, lê o JSON de configuração,
        // monta o primeiro instantâneo e sobe a thread escritora.
        // Devolve false SEM subir nada se qualquer passo falhar — Permission
        // meio de pé é pior que Permission ausente, porque o plugin de terceiro
        // acha que a resposta "negado" é verdade.
        //
        // `caminhoDb` continua sendo o que a ConanApi mandou
        // (CaminhoDados("Permission","permission.db")). Ele é o PADRÃO: se o
        // config.json trouxer "DbPathOverride", vale o do config; se trouxer
        // "Database": "mysql", o caminho não é usado. A assinatura não mudou de
        // propósito — Permission.cpp não precisa saber que existe MySQL.
        //
        // ONDE ISTO É CHAMADO: na carga do plugin, NÃO no laço do jogo. Com
        // MySQL, esta chamada espera rede: até MysqlTempoConectarMs (padrão
        // 5 s) para conectar, mais o esquema. É tempo de arranque do servidor,
        // não de tick — e está limitado de propósito.
        //
        // ── QUANTO TEMPO ISTO SEGURA O ARRANQUE, E POR QUE TEM TETO ─────────
        //
        // MEDIDO, não estimado: contra um MySQL a 2 s por comando (proxy lento
        // real, testes/teste_banco.cpp, cenário 8), o arranque completo são 61
        // comandos — conexão, esquema, configuração e primeiro instantâneo —
        // e levou **120,5 segundos**. O ConanLoader carrega os plugins em
        // sequência, então esses dois minutos eram do servidor inteiro, e o
        // dono via o servidor "pendurado" no boot sem saber por quê.
        //
        // A conta não tem como encolher: o trabalho é esse e a rede é a que é.
        // O que dá para escolher é ONDE se espera. Então: a abertura acontece
        // na thread escritora, e Abrir() espera por ela no MÁXIMO 15 s. Passou
        // disso, Abrir() volta e o arranque CONTINUA em segundo plano; o
        // Permission fica ausente até terminar, e entra sozinho quando
        // terminar. Nenhum outro plugin fica preso esperando o banco de quem
        // escolheu MySQL.
        //
        // 15 s foi escolhido por ser: (a) folgado para qualquer MySQL saudável,
        // local ou remoto — o caso normal termina em dezenas de milissegundos e
        // NADA muda para ele; (b) abaixo do tempo em que um operador começa a
        // achar que travou.
        //
        // ── O QUE `false` SIGNIFICA AQUI, E O QUE MUDOU (18/08/2026) ─────────
        //
        // ANTES: qualquer tropeço do banco (senha errada, MySQL ainda subindo,
        // banco não criado, sem GRANT) devolvia false, e o Permission ficava
        // MORTO ATÉ ALGUÉM REINICIAR O SERVIDOR DE JOGO. Reiniciar custa 6-9
        // minutos com ninguém conseguindo entrar. Um erro de digitação na senha
        // custava isso, e a causa mais comum de todas — o container do MySQL
        // terminando de subir 20 s depois do jogo — custava isso à toa.
        //
        // AGORA: `false` só sai quando NÃO HÁ COMO NEM TENTAR (caminho vazio,
        // Abrir chamado duas vezes). Banco que não abriu devolve TRUE: o plugin
        // fica carregado, em estado AUSENTE (Pronto() == false, e nesse estado
        // ConanPermissionObterApi devolve nullptr — ver INV-ARMAZEM-002), e a
        // thread escritora retenta em segundo plano com espera crescente,
        // relendo o config.json a cada tentativa. Quando o banco atender, o
        // Permission entra sozinho, sem reiniciar nada.
        //
        // O que NÃO acontece em hipótese nenhuma: usar outro banco que não o
        // que o config.json mandou usar (INV-BANCO-006).
        bool Abrir(const char* caminhoDb, const char* caminhoJson, FnLog log);

        // Manda a thread escritora sair e ESPERA por ela. Antes de esperar,
        // interrompe a operação de banco em curso (IBanco::Interromper) — sem
        // isso, um MySQL que aceitou a conexão e emudeceu prende o
        // desligamento do servidor por até msOperar a cada comando.
        void Fechar();

        // ── laço do jogo: só isto roda lá ───────────────────────────────────
        int32_t Tem(const char* jogador, const char* no) const;
        int32_t NoGrupo(const char* jogador, const char* grupo) const;
        int64_t ExpiraEm(const char* jogador, const char* grupo) const;
        int32_t Grupos(const char* jogador, char* saida, int32_t tam) const;

        // ── fora do laço ────────────────────────────────────────────────────
        //
        // As três devolvem PERMITIDO quando a tarefa foi ACEITA para gravação
        // (não quando ela já foi gravada — quem grava é a thread escritora), e
        // NAO_SEI quando o Armazém sabe de antemão que não vai gravar:
        //
        //   · o banco não está servindo agora (nunca abriu, ou caiu). Dizer
        //     "ok" para um comando que não vai acontecer é o defeito de boa-fé
        //     mais caro daqui: o admin digita `dar fulano vip`, lê "pronto", e
        //     o jogador reclama amanhã. Com NAO_SEI, quem chamou pode avisar na
        //     hora que o banco está fora;
        //   · a fila está no teto (INV-ARMAZEM-004). Também é NAO_SEI: recusar
        //     é honesto, crescer sem limite mata o processo do jogo.
        int32_t Conceder(const char* jogador, const char* grupo,
                         int64_t expiraEm, const char* quem);
        int32_t Revogar(const char* jogador, const char* grupo, const char* quem);
        int32_t VerJogador(const char* jogador, const char* nome);  // registra "visto"
        bool    Recarregar();

        // O banco escolhido no config.json está atendendo AGORA? Leitura
        // atômica, barata, chamável de qualquer thread. Quem escreve é só a
        // thread escritora.
        bool BancoServindo() const { return m_bancoServe.load(std::memory_order_acquire); }

        // Só para o teste: espera a fila de escrita esvaziar. NÃO existe para o
        // jogo — no jogo ninguém espera escrita.
        void EsperarFila();

        uint64_t Geracao() const;
        bool     Pronto() const { return m_atual.load(std::memory_order_acquire) != nullptr; }

    private:
        struct Tarefa
        {
            enum Tipo { CONCEDER, REVOGAR, VISTO, RECARREGAR, SAIR } tipo;
            std::string jogador, grupo, quem, nome;
            int64_t     expira = 0;
            // Só o reenvio depois de uma queda de conexão liga isto. Serve para
            // (a) reenviar no máximo UMA vez e (b) marcar a linha do diário,
            // para uma auditoria com duas linhas iguais ter explicação escrita
            // em vez de virar mistério. Ver Trabalhar().
            bool        reenvio = false;
        };

        void  Trabalhar();                       // corpo da thread escritora
        bool  ExecutarTarefa(const Tarefa& t, bool& mexeu);

        // Cria o IBanco a partir do config.json, abre, aplica esquema e
        // configuração e publica o primeiro instantâneo. RELÊ o config a cada
        // chamada de propósito (INV-ARMAZEM-003): é o que faz senha corrigida,
        // porta corrigida e "Database" corrigido valerem sem reiniciar o jogo.
        // Chamada pelo arranque e pela thread escritora — nunca pelo laço.
        bool  AbrirBanco();

        // Chamada pela thread escritora a cada volta. Decide se é hora de
        // tentar abrir/reconectar, aplica a espera crescente e mantém
        // m_bancoServe. Toda a política de "o banco não colabora" mora aqui.
        void  CuidarDaConexao();

        // Enfileira aplicando os tetos. false = recusada (fila cheia). Não
        // confere se o banco está vivo: quem faz isso é o chamador, porque a
        // mensagem certa depende de qual comando era.
        bool  Enfileirar(Tarefa&& t);

        bool  AplicarEsquema();
        bool  AplicarConfig(const char* caminhoJson);
        bool  Reconstruir();                     // banco -> instantâneo novo
        void  Publicar(Instantaneo* novo);
        void  Registrar(const char* fmt, ...) const;
        const Instantaneo* Atual() const { return m_atual.load(std::memory_order_acquire); }

        // ── leitura protegida: o instantâneo não pode ser liberado embaixo ────
        //
        // O DEFEITO QUE ISTO CONSERTA (o servidor CAÍA)
        // ---------------------------------------------
        // A versão anterior liberava o instantâneo que sobrevivesse a DUAS
        // publicações, argumentando que "duas trocas levam segundos no mínimo".
        // Medido sob Wine: uma publicação leva ~4 ms. A janela de vida de um
        // aposentado era de ~8 ms — menos que um quantum de escalonamento, e a
        // premissa estava errada por ~250×.
        //
        // Consequência reproduzida 3 vezes em 3: admin dando VIP em lote (loja
        // web, laço de /perm dar) enquanto qualquer plugin lê permissão no laço
        // do jogo →
        //
        //   wine: Unhandled page fault on read access to 00007FFFFF0654DC
        //   =>0 strcmp(str1=*** invalid address ***, str2="76561198000010000")
        //
        // Access violation NA THREAD DO JOGO, que é justamente onde a guarda de
        // plugin NÃO alcança. Servidor no chão, sem rastro perto da causa.
        //
        // O conserto não é aumentar o número de publicações: qualquer número
        // seria um palpite sobre quanto tempo um leitor demora. É CONTAR os
        // leitores. Só se libera aposentado quando não existe nenhum leitor
        // ativo — e aí é certeza, não estimativa.
        //
        // Ordem que torna isto correto: Publicar troca m_atual PRIMEIRO. Depois
        // disso, todo leitor novo enxerga o instantâneo novo. Se o contador
        // estiver em zero nesse momento, nenhum leitor pode estar segurando o
        // velho.
        class Leitura
        {
        public:
            explicit Leitura(const Armazem& a) : m_a(a)
            {
                m_a.m_leitores.fetch_add(1, std::memory_order_acq_rel);
                m_s = m_a.m_atual.load(std::memory_order_acquire);
            }
            ~Leitura() { m_a.m_leitores.fetch_sub(1, std::memory_order_acq_rel); }
            Leitura(const Leitura&) = delete;
            Leitura& operator=(const Leitura&) = delete;
            const Instantaneo* operator->() const { return m_s; }
            const Instantaneo* get() const { return m_s; }
            explicit operator bool() const { return m_s != nullptr; }
        private:
            const Armazem&     m_a;
            const Instantaneo* m_s;
        };

        // O MEIO. Quem manda no que ele é: "Database" no config.json. Só a
        // thread escritora e o arranque encostam nisto — ver INV-BANCO-001.
        //
        // A TROCA do ponteiro é protegida por m_mtxBanco, e só ela. O uso não
        // precisa de trava porque só existe uma thread usando; a trava existe
        // porque Fechar(), de OUTRA thread, precisa chamar Interromper() no
        // banco que estiver lá — e sem ela isso corre contra a troca.
        std::unique_ptr<IBanco> m_banco;
        mutable std::mutex      m_mtxBanco;

        FnLog       m_log = nullptr;
        std::string m_caminhoDb, m_caminhoJson;

        // ── política de "o banco não colabora" ──────────────────────────────
        //
        // O banco escolhido está atendendo AGORA. Escrito só pela thread
        // escritora (e por Abrir, antes de ela existir); lido pelo laço do
        // jogo em Conceder/Revogar/VerJogador, e por isso é atômico.
        std::atomic<bool> m_bancoServe{false};

        // A PRIMEIRA tentativa de abertura já terminou (com sucesso ou não)?
        // Só existe para Abrir() saber se o silêncio é "falhou" ou "ainda
        // estou abrindo" — e as duas exigem mensagens diferentes no log.
        std::atomic<bool> m_arranqueFeito{false};

        // Espera CRESCENTE entre tentativas, em segundos: 5, 10, 20, 40, 80,
        // 160, 300 (teto). Zero = está tudo bem, não há tentativa pendente.
        //
        // POR QUE CRESCENTE, e não os 15 s fixos de antes: as duas pontas
        // importam e são opostas. Um blip de rede de 3 s tem de se resolver
        // rápido (daí o primeiro degrau curto); um MySQL desligado há 6 horas
        // não pode receber uma tentativa a cada 15 s pela noite inteira — são
        // 1.440 conexões contra um banco morto, 1.440 linhas de log, e nenhuma
        // delas ia dar certo. O teto de 300 s existe pela ponta contrária: o
        // dono que acabou de consertar a senha não pode esperar mais que 5
        // minutos para o conserto valer.
        int64_t     m_proximaTentativa   = 0;   // relógio absoluto, em segundos
        int         m_esperaSegundos     = 0;
        int         m_falhasReconexao    = 0;   // 2 seguidas => refaz do config

        int64_t     m_ultimoAvisoConexao = 0;
        // Contadores desde o último aviso. Existem para o aviso periódico dizer
        // QUANTO se perdeu, em vez de repetir "o banco esta fora" sem tamanho —
        // é a diferença entre o dono saber que perdeu 3 comandos e achar que
        // perdeu a noite.
        uint64_t    m_recusadasDesdeAviso   = 0;
        uint64_t    m_descartadasDesdeAviso = 0;

        std::atomic<const Instantaneo*> m_atual{nullptr};
        // Instantâneos aposentados. Não se libera na hora: um leitor pode estar
        // com o ponteiro na mão. Ficam guardados e são liberados na próxima
        // publicação seguinte à seguinte — dois ciclos de folga. Custa alguns
        // KB e elimina a única forma de este desenho derrubar o servidor.
        std::vector<const Instantaneo*> m_aposentados;

        // Leitores ATIVOS agora. mutable porque a leitura é const do ponto de
        // vista de quem consulta permissão — e é, logicamente: nada muda.
        mutable std::atomic<int> m_leitores{0};

        mutable std::mutex      m_mtxFila;
        std::condition_variable m_cvFila;
        std::deque<Tarefa>      m_fila;
        // Quantos VISTO estão na fila agora. Contador, e não varredura da fila:
        // quem enfileira é o laço do jogo, e percorrer milhares de tarefas com
        // a trava na mão seria justamente pôr o custo do banco dentro do tick.
        size_t                  m_vistosNaFila = 0;
        std::thread             m_thread;
        std::atomic<bool>       m_rodando{false};
        std::atomic<uint64_t>   m_feitas{0};
        std::atomic<uint64_t>   m_enfileiradas{0};
    };
}
