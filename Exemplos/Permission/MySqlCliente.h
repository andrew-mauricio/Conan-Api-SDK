// ============================================================================
//  MySqlCliente.h — cliente MySQL falando o protocolo direto no socket
//
//  POR QUE ESTE ARQUIVO EXISTE
//  ---------------------------
//  O Permission guarda tudo em SQLite local, e isso continua sendo o padrão.
//  O dono de servidor que QUISER pode apontar para um MySQL no config.json —
//  e para isso precisaria de um cliente MySQL. Não existe libmysqlclient para
//  este toolchain (mingw-w64), e mandar o dono instalar uma DLL é exatamente a
//  fricção que este projeto existe para não ter: quem sobe servidor de Conan
//  não é programador.
//
//  Então o cliente é escrito aqui, do mesmo jeito que o SQLite já vem embutido
//  (terceiros/sqlite3/, 261.454 linhas dentro do nosso .dll). O protocolo
//  cliente-servidor do MySQL é público e estável desde a versão 4.1.
//
//  ZERO DEPENDÊNCIA EXTERNA, DE VERDADE
//  Winsock no Windows, sockets BSD no Linux, e nada mais. SHA-1, SHA-256, RSA
//  e OAEP são implementados neste .cpp porque a autenticação do MySQL 8 exige
//  os quatro e não há OpenSSL neste toolchain. Nenhum .lib de terceiro, nenhum
//  vcpkg, nenhum pacote para o dono instalar.
//
//  ┌──────────────────────────────────────────────────────────────────────┐
//  │  A REGRA QUE MANDA EM TUDO NESTE ARQUIVO                             │
//  │                                                                      │
//  │  ISTO BLOQUEIA. Conectar espera rede. Executar espera resposta.      │
//  │  NADA AQUI PODE SER CHAMADO NA THREAD DO JOGO — nunca, em nenhuma    │
//  │  circunstância, nem "só uma consultinha rápida".                     │
//  │                                                                      │
//  │  Um MySQL do outro lado do país, atrás de uma rede ruim, é problema  │
//  │  do banco. Se essa espera acontecer no laço do jogo, vira problema   │
//  │  do SERVIDOR DE JOGO: o tick para, os jogadores levam timeout e      │
//  │  caem. O Armazem já tem a thread escritora certa para isto           │
//  │  (Armazem.h: "ESCRITA acontece fora do laço, numa thread própria").  │
//  │                                                                      │
//  │  Os tempos-limite abaixo limitam o dano, não o eliminam: 10 s de     │
//  │  congelamento no laço do jogo derruba o servidor do mesmo jeito.     │
//  └──────────────────────────────────────────────────────────────────────┘
//
//  INVARIANTES QUE ESTE ARQUIVO SE COMPROMETE A MANTER
//  ---------------------------------------------------
//  INV-MYSQL-001 · Nenhuma falha do banco pode virar falha do processo.
//      Qualquer erro — rede, protocolo, autenticação, servidor hostil,
//      resposta truncada, pacote gigante — sai como `false` + texto em
//      português. Nunca como exceção atravessando a fronteira, nunca como
//      leitura fora dos limites, nunca como espera infinita.
//
//  INV-MYSQL-002 · Nenhuma chamada pode esperar para sempre.
//      Toda leitura e toda escrita têm prazo. Conectar tem prazo próprio.
//      Um servidor que aceita a conexão e depois emudece é indistinguível de
//      um servidor no ar — só o relógio separa os dois.
//
//  INV-MYSQL-003 · O fluxo de pacotes nunca fica dessincronizado em silêncio.
//      Número de sequência conferido em cada pacote. Divergiu, a conexão é
//      DERRUBADA — nunca reaproveitada. Continuar lendo um fluxo desalinhado
//      devolve dado de outra consulta com cara de dado certo, e esse é o
//      defeito que ninguém acha depois.
//
//  INV-MYSQL-004 · Escapar() só é chamado num contexto onde escapar funciona.
//      Duas condições do servidor quebram o escape byte a byte, as duas em
//      silêncio: NO_BACKSLASH_ESCAPES no sql_mode e conjunto de caracteres
//      multibyte tipo GBK/SJIS/BIG5. Conectar() força utf8mb4 e recusa a
//      conexão se não conseguir sair do NO_BACKSLASH_ESCAPES. Falha fechada:
//      sem conexão é melhor que conexão com escape furado.
//
//  O QUE ESTE ARQUIVO **NÃO** FAZ, e é melhor estar escrito que descoberto:
//    · não é thread-safe — uma conexão, uma thread. Há uma guarda que RECUSA
//      o uso concorrente (ver `m_emUso` no .cpp), mas ela detecta o erro, não
//      o conserta;
//    · não reconecta sozinho. Caiu, `Conectado()` passa a devolver false e a
//      camada de cima decide. Reconectar escondido reexecuta comando que já
//      pode ter sido aplicado no servidor;
//    · não fala TLS. A senha é protegida na autenticação (nunca vai em claro
//      no fio), mas as CONSULTAS trafegam abertas. Ver "RISCO RESIDUAL" no
//      topo do .cpp;
//    · não tem prepared statements. Interpolação com Citar()/Escapar() é o
//      caminho, e CLIENT_MULTI_STATEMENTS fica desligado de propósito para
//      que um `'; DROP TABLE ...` não tenha como virar um segundo comando.
// ============================================================================
#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace Perm
{
    // Estado da conexão. Declarado aqui só como nome: a definição mora no
    // .cpp porque ela precisa de <winsock2.h>, e arrastar o winsock2 para
    // dentro de todo arquivo que inclui este header é receita conhecida de
    // erro em cascata (windows.h incluído antes dele redefine tudo).
    struct MySqlInterno;

    class MySqlCliente
    {
    public:
        MySqlCliente();
        ~MySqlCliente();

        MySqlCliente(const MySqlCliente&)            = delete;
        MySqlCliente& operator=(const MySqlCliente&) = delete;

        // ── ajustes, todos opcionais ────────────────────────────────────────

        // Prazos em milissegundos. Padrão: 5.000 para conectar, 10.000 para
        // operar. Vale para a PRÓXIMA conexão; mudar com conexão aberta não
        // mexe no socket já configurado.
        //
        // Valor <= 0 é recusado (volta ao padrão). "Sem limite" não é opção
        // que este arquivo ofereça: ver INV-MYSQL-002.
        void DefinirTempos(int msConectar, int msOperar);

        // Teto do payload de UM pacote lógico do servidor. Padrão 64 MiB.
        //
        // Existe porque o cabeçalho do MySQL permite corrente de pacotes de
        // 16 MiB cada, sem fim declarado. Sem teto, um servidor hostil (ou um
        // MySQL confuso do outro lado de um proxy) faz este processo alocar
        // até acabar a memória — e quem morre é o servidor de jogo.
        void DefinirTetoResposta(size_t bytes);

        // Diagnóstico opcional. Recebe linhas prontas em português. Pode ser
        // nulo (padrão). Chamado só nas threads que chamam esta classe.
        void DefinirLog(void (*log)(void* ctx, const char* linha), void* ctx);

        // ── conexão ─────────────────────────────────────────────────────────

        // Devolve false e preenche 'erro' com texto LEGÍVEL em português.
        //
        // 'banco' pode ser vazio ou nulo (conecta sem selecionar banco).
        // 'erro' pode ser nulo; 'tamErro' é o tamanho do buffer, e a mensagem
        // sempre sai terminada em '\0' (trunca se não couber).
        //
        // Chamar com conexão aberta desconecta a anterior antes.
        bool Conectar(const char* host, uint16_t porta, const char* usuario,
                      const char* senha, const char* banco, char* erro, int tamErro);

        void Desconectar();
        bool Conectado() const;

        // ── INTERROMPER: a única coisa desta classe que outra thread pode chamar
        //
        // O DEFEITO QUE ISTO CONSERTA
        // Todo o resto aqui é "uma conexão, uma thread". Mas existe um momento
        // em que a outra thread PRECISA falar: o desligamento do servidor de
        // jogo. O Armazem::Fechar() manda a thread escritora sair e espera por
        // ela (join). Se ela estiver dentro de um recv() contra um MySQL que
        // aceitou a conexão e emudeceu (firewall que corta pela metade, banco
        // travado), ela só volta quando o prazo estourar — msOperar por
        // comando, e uma reconstrução são 7 consultas: até ~70 s de servidor
        // "pendurado" no desligamento com os prazos padrão.
        //
        // E aí o dono faz o que qualquer pessoa faz: mata o processo. Matar o
        // servidor de Conan no meio do desligamento é perder o save do mundo —
        // um problema do BANCO virando o pior estrago possível no JOGO.
        //
        // Interromper() faz shutdown(SHUT_RDWR) no socket: a chamada bloqueada
        // na outra thread volta na hora, com erro, e a operação falha como
        // qualquer outra falha de rede (INV-MYSQL-001). Depois disso a conexão
        // recusa tudo — inclusive Conectar() — até uma nova instância.
        //
        // POR QUE shutdown() E NÃO close(): close() libera o descritor, e o SO
        // reaproveita o número na hora seguinte. Fechar aqui enquanto a outra
        // thread ainda usa o mesmo número é a receita clássica de mandar o
        // shutdown para um socket que virou OUTRA coisa nesse meio tempo — e
        // neste processo as outras coisas são os sockets do SERVIDOR DE JOGO.
        // shutdown() não libera o descritor; ele continua sendo o nosso.
        void Interromper();

        // ── comandos ────────────────────────────────────────────────────────

        // Executa SQL sem resultado (INSERT/UPDATE/CREATE). false = erro.
        //
        // Se o SQL devolver linhas por engano (um SELECT), elas são LIDAS e
        // descartadas — não deixar de ler dessincroniza o fluxo e a próxima
        // consulta lê a resposta desta (INV-MYSQL-003).
        bool Executar(const char* sql, char* erro, int tamErro);

        // Consulta com resultado. Chama 'linha' para cada registro.
        //
        // 'ncols' é o número de colunas; 'valores' tem 'ncols' entradas, e
        // valor NULL vem como nullptr (string vazia vem como "" — são coisas
        // diferentes e o MySQL distingue as duas).
        //
        // Os nomes das colunas estão em Colunas() e os comprimentos dos
        // valores da linha corrente em ComprimentosDaLinha() — os dois válidos
        // DURANTE a chamada de 'linha' (vetores paralelos a 'valores').
        //
        // 'linha' não deve lançar exceção. Se lançar, é capturada aqui, a
        // consulta é abortada e a conexão é derrubada (o resto do resultado
        // ficaria no fio, e ler pela metade é o defeito da INV-MYSQL-003).
        bool Consultar(const char* sql,
                       void (*linha)(void* ctx, int ncols, const char* const* valores),
                       void* ctx, char* erro, int tamErro);

        // ── interpolação segura ─────────────────────────────────────────────

        // Escapa string para interpolar em SQL. NÃO põe as aspas em volta —
        // quem esquece as aspas continua injetável. Prefira Citar().
        //
        // Pode lançar std::bad_alloc (é o único caminho de exceção da classe;
        // a assinatura devolve std::string e não tem por onde reportar erro).
        static std::string Escapar(const char* s);

        // Versão com comprimento: é a única que aceita NUL no meio do dado.
        // Um `const char*` sozinho não tem como carregar um 0x00 embutido —
        // ele TERMINA ali. Dado binário tem de vir por aqui.
        static std::string Escapar(const char* s, size_t n);

        // Escapa E envolve em aspas simples: devolve  'texto'  pronto para o
        // SQL. Existe porque esquecer as aspas em volta de Escapar() é o erro
        // de boa-fé mais fácil de cometer e o mais caro: escapar sem aspas
        // não protege nada.
        static std::string Citar(const char* s);
        static std::string Citar(const char* s, size_t n);

        // ── o que sobrou do último comando ──────────────────────────────────

        // Depois de Executar: linhas AFETADAS, como o servidor informou.
        // Depois de Consultar: linhas LIDAS. São perguntas diferentes e o
        // mesmo contador responde as duas — está escrito para ninguém
        // confundir uma com a outra.
        uint64_t    LinhasAfetadas()   const;
        uint64_t    UltimoIdInserido() const;   // AUTO_INCREMENT do último INSERT
        unsigned    UltimoCodigoMysql()const;   // 0 = sem erro do servidor
        const char* VersaoServidor()   const;   // string crua do handshake

        const std::vector<std::string>& Colunas() const;
        const std::vector<size_t>&      ComprimentosDaLinha() const;

    private:
        MySqlInterno* m_i;   // ponteiro opaco; ver o comentário lá em cima
    };
}
