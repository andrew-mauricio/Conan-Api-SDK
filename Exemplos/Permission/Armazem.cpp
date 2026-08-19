// ============================================================================
//  Armazem.cpp — a DECISÃO (que dados guardar) + instantâneo em memória
//
//  O MEIO SAIU DAQUI (18/08/2026)
//  ------------------------------
//  Este arquivo tinha 161 chamadas sqlite3_* no corpo. Agora fala com
//  Perm::IBanco (Banco.h), e existem duas implementações: BancoSqlite — o
//  padrão, e é o texto abaixo que explica por que ele é bom — e BancoMysql,
//  para o dono que quiser apontar o plugin ao MySQL dele mexendo numa linha do
//  config.json.
//
//  O que NÃO mudou, e é o ponto: a lógica de permissão. Herança achatada uma
//  vez fora do laço, peso do casamento, negação específica vencendo curinga,
//  instantâneo publicado por troca atômica de ponteiro, contagem de leitores.
//  Nada disso sabe qual banco está embaixo, e nada disso foi reescrito.
//
//  POR QUE SQLITE CONTINUA SENDO O PADRÃO, E NÃO ARQUIVO PRÓPRIO
//  ------------------------------------------------------------
//  Não foi escolha por gosto. Três fatos deste ambiente decidiram:
//
//  1. O PRÓPRIO JOGO JÁ FAZ ISSO, AQUI, AGORA. O save do Conan é um SQLite:
//     ConanSandbox/Saved/game_0.db, em modo WAL — os arquivos game_0.db-wal e
//     game_0.db-shm existem ao lado dele com o servidor no ar. Ou seja: SQLite
//     em WAL, sob Wine, dentro deste processo, já é um caminho PROVADO em
//     produção pelo fabricante do jogo. Nenhum formato próprio chega perto
//     desse nível de evidência.
//
//  2. QUEDA NO MEIO DA ESCRITA. Um arquivo JSON reescrito por cima perde tudo
//     se o servidor morrer no meio; com rename atômico perde a última escrita.
//     O WAL do SQLite dá commit atômico e recuperação automática na abertura:
//     ou a transação inteira valeu, ou nenhuma parte dela valeu, e o arquivo
//     nunca fica pela metade. Para "o VIP que o jogador acabou de comprar", a
//     diferença entre essas duas garantias é dinheiro.
//
//  3. A LIÇÃO DO WAL, QUE ESTE PROJETO JÁ PAGOU. Apagar um SQLite sem apagar o
//     -wal faz o dado RESSUSCITAR, e o apagamento parece feito. Isso não é
//     argumento contra o WAL: é argumento para documentar e para dar o comando
//     certo. Ver README-PERMISSION.md, seção "apagar de verdade". A alternativa
//     (formato próprio) não tem WAL e tem um problema pior: nenhuma garantia.
//
//  E o custo? Zero na leitura, porque a leitura NÃO PASSA POR AQUI. SQLite é o
//  armazém durável; o laço do jogo lê de um instantâneo em memória.
//
//  O QUE NÃO SE FAZ, EM HIPÓTESE NENHUMA
//  ------------------------------------
//  Não se toca no game_0.db do jogo. Nem para ler. O banco do Permission é
//  arquivo próprio, em pasta própria. Se corrompêssemos o save, o dono do
//  servidor perderia o mundo — e não há permissão de VIP no mundo que pague
//  isso. Um SELECT nosso concorrendo com o writer do jogo também é risco de
//  travar o save. Fica fora.
//
//  JSON SEM PARSER PRÓPRIO
//  -----------------------
//  A configuração continua sendo lida com o json1 do SQLite (json_valid,
//  json_extract, json_each), não com um parser escrito à mão. Parser de JSON à
//  mão é ~200 linhas de código novo que ninguém revisou, lendo arquivo que o
//  dono do servidor edita à mão — ou seja, entrada hostil por definição.
//
//  O que mudou é ONDE: antes o json1 cruzava direto com as tabelas reais num
//  SQL só, o que não atravessa para o MySQL (json_each é função de tabela do
//  SQLite; o MySQL 5.7 não tem JSON_TABLE nenhum). Agora o json1 roda num
//  SQLite EM MEMÓRIA, devolve struct, e quem grava é a interface — mesmo
//  caminho de configuração para os dois bancos. Ver LerConfigPermissao, em
//  Banco.cpp.
// ============================================================================
#include "Armazem.h"

#include <windows.h>      // só pelo relógio; ver Agora(). O motivo está lá.
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <ctime>
#include <algorithm>

namespace Perm
{
namespace
{
    // ── o esquema mudou de arquivo, e vale dizer para onde ──────────────────
    //
    // O texto do esquema (nas DUAS formas: sqlite e mysql) mora em Banco.cpp,
    // porque é a parte que depende do meio. O que continua sendo assunto DAQUI
    // é o que ele significa:
    //
    // As TRÊS camadas de nome de grupo, e por que existem três:
    //
    //   grupo.id     inteiro, imutável, invisível. É o que jogador_grupo
    //                referencia. Renomear qualquer coisa NUNCA mexe nele — é
    //                exatamente por isso que renomear não perde dado.
    //   grupo.chave  o nome técnico, o que plugin e comando usam ("vip").
    //                Renomeável: ver "era" no permission.json. Ao renomear, a
    //                chave antiga fica gravada como APELIDO, então o plugin de
    //                VIP que foi compilado perguntando por "vip" continua
    //                acertando depois do renome para "premium".
    //   grupo.nome   o rótulo que o jogador vê no chat ("Patrono do Exílio").
    //                Livre, ninguém referencia, muda quando quiser.
    //
    // Sem essas três camadas, "permita alterar o nome" só tem duas saídas
    // ruins: ou o renome quebra os plugins, ou não renomeia nada de verdade.

    constexpr int ESQUEMA_VERSAO = 1;

    // ── o relógio, e o defeito que a MEDIÇÃO achou ───────────────────────────
    //
    // A primeira versão desta função era `return std::time(nullptr);`, com um
    // comentário meu afirmando que era barato porque "no Windows lê
    // KUSER_SHARED_DATA, sem syscall". O comentário estava ERRADO, e o teste
    // rodando sob Wine cobrou:
    //
    //     std::time(nullptr)          32.418,0 ns/op      <<< 32 microssegundos
    //     GetTickCount64()                  2,5 ns/op
    //     GetSystemTimeAsFileTime()       201,1 ns/op
    //
    // 32 µs por leitura de permissão. Com 40 jogadores a 60 Hz isso é 78% de um
    // núcleo gasto perguntando a hora. E o pior: TUDO compilava, TUDO passava
    // nos 44 testes de comportamento. Só o teste de custo, rodando de verdade,
    // separou — exatamente o que "compilar não prova nada" quer dizer.
    //
    // O conserto NÃO é uma thread que atualiza um relógio em cache. Aquela
    // versão tem um modo de falha silencioso: se a thread morrer ou atrasar, o
    // relógio congela e VIP vencido vale para sempre, sem log.
    //
    // Aqui o segundo é DERIVADO do tique monotônico. Mesmo que nada nunca
    // reancore, a resposta continua andando, porque GetTickCount64 anda sozinho.
    // Não existe estado que possa congelar: o pior caso é a deriva entre o
    // relógio de parede e o tique, que é de milissegundos por hora — irrelevante
    // para "o VIP vence hoje às 20h".
    inline int64_t Agora()
    {
        static std::atomic<int64_t>  s_seg{0};      // segundos desde 1970 na âncora
        static std::atomic<uint64_t> s_tick{0};     // GetTickCount64 na âncora

        const uint64_t t      = GetTickCount64();               // 2,5 ns
        const uint64_t ancora = s_tick.load(std::memory_order_relaxed);
        const int64_t  base   = s_seg.load(std::memory_order_relaxed);

        if (base == 0 || t - ancora >= 1000)        // reancora no máximo 1×/s
        {
            FILETIME ft;
            GetSystemTimeAsFileTime(&ft);           // 201 ns, e é o relógio real
            const uint64_t u = (static_cast<uint64_t>(ft.dwHighDateTime) << 32)
                             | ft.dwLowDateTime;
            // FILETIME conta 100 ns desde 1601; 11644473600 s separam 1601 de 1970
            const int64_t seg = static_cast<int64_t>(u / 10000000ULL) - 11644473600LL;
            s_seg.store(seg,  std::memory_order_relaxed);
            s_tick.store(t,   std::memory_order_relaxed);
            return seg;
        }
        return base + static_cast<int64_t>((t - ancora) / 1000);
    }

    inline bool Vencido(int64_t expira, int64_t agora)
    { return expira != 0 && expira <= agora; }

    // ── tetos da fila de escrita (INV-ARMAZEM-004) ──────────────────────────
    //
    // O DEFEITO QUE ISTO CONSERTA, e ele derrubava o servidor de jogo
    // -------------------------------------------------------------
    // A fila não tinha teto. `VerJogador` é chamado de dentro de
    // `ConanPermId()` (Permission.cpp), que é uma função de laço de jogo: todo
    // plugin que resolve identidade enfileira um `visto` a cada 250 ms por
    // objeto do jogo. Com 40 jogadores e três objetos cada, isso é da ordem de
    // centenas de tarefas por segundo — e o SQLite dá conta.
    //
    // O MySQL do dono, não necessariamente. Com 2 s por consulta (rede ruim,
    // banco do outro lado do país — o cenário 8 desta tarefa), a thread
    // escritora drena menos de uma tarefa por segundo enquanto a fila cresce a
    // centenas. Cada tarefa carrega quatro std::string. Isso não é lentidão:
    // é o processo do SERVIDOR DE JOGO subindo de memória até morrer, por causa
    // de um banco lento. Exatamente o que esta tarefa proíbe.
    //
    // Os números, e por que estes:
    //   4.096 tarefas × ~200 B ≈ 800 KB. Teto pequeno o bastante para nunca
    //   importar num servidor de jogo e grande o bastante para segurar uma
    //   concessão em lote de loja web inteira sem recusar nada. Acima disso já
    //   não é rajada: a 2 s por escrita, 4.096 pendências levam mais de duas
    //   horas para drenar, e o que chegasse depois estaria perdido de todo
    //   jeito — melhor recusar na hora e DIZER, que é o que Conceder faz.
    //
    //   256 `visto` dentro dos 4.096. `visto` é cosmético (deixa o admin
    //   digitar nome em vez de número) e é o ÚNICO que chega em rajada do laço
    //   do jogo. Separá-lo garante que uma enxurrada de `visto` nunca ocupe o
    //   lugar de um `conceder`, que é o que vale dinheiro.
    constexpr size_t FILA_TETO       = 4096;
    constexpr size_t FILA_TETO_VISTO = 256;

    // ── espera crescente entre tentativas de abrir/reconectar ───────────────
    //
    // 5 · 10 · 20 · 40 · 80 · 160 · 300 (teto), em segundos. O porquê dos dois
    // extremos está em Armazem.h, junto do campo m_esperaSegundos.
    constexpr int ESPERA_PRIMEIRA = 5;
    constexpr int ESPERA_TETO     = 300;

    // Quanto Abrir() segura a thread de carga do plugin esperando a primeira
    // abertura. O porquê do número — e os 120,5 s medidos que o motivaram —
    // está no comentário de Armazem::Abrir, em Armazem.h.
    constexpr int ARRANQUE_ESPERA_MS = 15000;

    // De quanto em quanto tempo o log repete que o banco está fora. Repetir é
    // obrigatório e não é ruído: senha errada não se conserta sozinha, e o dono
    // lê o log HORAS depois, quando o jogador reclama — a essa altura a linha
    // do arranque está soterrada sob o log do jogo.
    constexpr int64_t AVISO_A_CADA = 300;

    // Casamento de nó. Devolve o peso do casamento, ou -1 se não casa.
    //
    //   exato       "vip.kit.diario"  contra "vip.kit.diario"  -> len+1
    //   curinga     "vip.kit.*"       contra "vip.kit.diario"  -> 8 (prefixo)
    //   tudo        "*"               contra qualquer coisa    -> 0
    //
    // O peso é o comprimento do que casou, e o exato ganha +1 para vencer
    // sempre um curinga do mesmo comprimento. É o que faz "vip.*" permitir e
    // "-vip.teleporte" negar só o teleporte, sem ordem de declaração importar.
    int32_t Casa(const std::string& padrao, const char* no, bool& curinga)
    {
        curinga = false;
        if (padrao.size() == 1 && padrao[0] == '*') { curinga = true; return 0; }
        if (padrao.size() >= 2 && padrao[padrao.size() - 1] == '*')
        {
            // aceita "vip.*" e também "vip*"
            const size_t pref = padrao.size() - 1;
            if (std::strncmp(padrao.c_str(), no, pref) != 0) return -1;
            curinga = true;
            return static_cast<int32_t>(pref);
        }
        if (padrao == no) return static_cast<int32_t>(padrao.size()) + 1;
        return -1;
    }

    void Copiar(char* saida, int32_t tam, const std::string& s)
    {
        if (!saida || tam <= 0) return;
        const size_t n = std::min<size_t>(s.size(), static_cast<size_t>(tam) - 1);
        std::memcpy(saida, s.data(), n);
        saida[n] = 0;
    }
}

// ============================================================================
//  Tabela — hash sem alocação, para a consulta do laço do jogo
// ============================================================================
uint64_t Tabela::Hash(const char* s)
{
    // FNV-1a de 64 bits. Escolhido por ser trivial de auditar e não ter estado.
    uint64_t h = 1469598103934665603ULL;
    // ── o laço TEM teto ─────────────────────────────────────────────────────
    //
    // Era `while (*s)`, sem limite. Chave sem terminador levava este laço para
    // fora do mapa, na thread do jogo, fora da guarda SEH do loader — derrubava
    // o processo, não só o plugin. Ver ComprimentoLimitado, em Armazem.h.
    //
    // MAX_NO é o maior dos três tetos da ABI (id 64, grupo 64, nó 128), e esta
    // Tabela indexa os três. Nenhuma chave GUARDADA passa de MAX_ID (Inserir
    // recusa o que não cabe em Item::id), então parar aqui não muda o resultado
    // de consulta nenhuma: chave maior que isso simplesmente não existe na
    // tabela e continua devolvendo "não achei".
    for (int i = 0; i < MAX_NO && s[i]; ++i)
    { h ^= static_cast<unsigned char>(s[i]); h *= 1099511628211ULL; }
    return h;
}

void Tabela::Reservar(size_t n)
{
    itens.clear();
    itens.reserve(n);
    // Fator de carga máximo de 50%: sondagem linear degrada rápido acima disso.
    size_t cap = 16;
    while (cap < n * 2) cap <<= 1;
    balde.assign(cap, -1);
}

bool Tabela::Inserir(const char* chave, int32_t valor)
{
    if (!chave) return false;
    // Era `std::strlen(chave)`, que caminha até o '\0' onde quer que ele esteja.
    // Aqui o teto é MAX_ID porque Item::id mede MAX_ID: uma chave maior seria
    // recusada logo abaixo de qualquer jeito, então medir além disso só serviria
    // para ler memória que talvez não seja nossa.
    const int lenLim = ComprimentoLimitado(chave, MAX_ID);
    if (lenLim < 0) return false;
    const size_t len = static_cast<size_t>(lenLim);
    // Chave que não cabe é RECUSADA, nunca truncada. Truncar faria dois
    // jogadores diferentes virarem o mesmo jogador — um deles herdaria o VIP do
    // outro, em silêncio. Recusar é feio e visível; truncar é bonito e errado.
    if (len == 0 || len >= static_cast<size_t>(MAX_ID)) return false;
    if (balde.empty()) Reservar(16);
    if (itens.size() * 2 >= balde.size())
    {
        // rehash: guarda os itens e remonta
        std::vector<Item> velhos = std::move(itens);
        size_t cap = balde.size() * 2;
        balde.assign(cap, -1);
        itens.clear(); itens.reserve(velhos.size() + 1);
        for (const Item& it : velhos) Inserir(it.id, it.valor);
    }
    const size_t mask = balde.size() - 1;
    size_t i = static_cast<size_t>(Hash(chave)) & mask;
    while (balde[i] >= 0)
    {
        if (std::strcmp(itens[static_cast<size_t>(balde[i])].id, chave) == 0)
        { itens[static_cast<size_t>(balde[i])].valor = valor; return true; }
        i = (i + 1) & mask;
    }
    Item novo{};
    std::memcpy(novo.id, chave, len + 1);
    novo.valor = valor;
    itens.push_back(novo);
    balde[i] = static_cast<int32_t>(itens.size()) - 1;
    return true;
}

int32_t Tabela::Achar(const char* chave) const
{
    if (!chave || !*chave || balde.empty()) return -1;
    const size_t mask = balde.size() - 1;
    size_t i = static_cast<size_t>(Hash(chave)) & mask;
    size_t guarda = 0;
    while (balde[i] >= 0 && guarda++ <= balde.size())
    {
        const Item& it = itens[static_cast<size_t>(balde[i])];
        if (std::strcmp(it.id, chave) == 0) return it.valor;
        i = (i + 1) & mask;
    }
    return -1;
}

const char* Instantaneo::GrupoAtual(const char* chave) const
{
    const int32_t i = idxApelidoGrupo.Achar(chave);
    return (i >= 0 && static_cast<size_t>(i) < apelidoGrupoPara.size())
           ? apelidoGrupoPara[static_cast<size_t>(i)].c_str() : chave;
}

const char* Instantaneo::NoAtual(const char* chave) const
{
    const int32_t i = idxApelidoNo.Achar(chave);
    return (i >= 0 && static_cast<size_t>(i) < apelidoNoPara.size())
           ? apelidoNoPara[static_cast<size_t>(i)].c_str() : chave;
}

const JogadorResolvido* Instantaneo::Achar(const char* id) const
{
    const int32_t i = idxJogador.Achar(id);
    return (i >= 0 && static_cast<size_t>(i) < jogadores.size())
           ? &jogadores[static_cast<size_t>(i)] : nullptr;
}

// ============================================================================
//  ciclo de vida
// ============================================================================
Armazem::~Armazem() { Fechar(); }

void Armazem::Registrar(const char* fmt, ...) const
{
    if (!m_log) return;
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    m_log(buf);
}

// ============================================================================
//  AbrirBanco — do config.json ao primeiro instantâneo publicado
//
//  Chamada pelo arranque E pela thread escritora (retentativa). NUNCA pelo laço
//  do jogo: com MySQL isto espera rede.
//
//  RELÊ O CONFIG A CADA CHAMADA, de propósito (INV-ARMAZEM-003). É o que faz o
//  dono corrigir a senha, a porta, o nome do banco ou o "Database" no arquivo e
//  o conserto valer sozinho — sem reiniciar o servidor de jogo, que custa 6 a 9
//  minutos com ninguém conseguindo entrar.
// ============================================================================
bool Armazem::AbrirBanco()
{
    // ── 1. quem é o banco: o config.json decide ─────────────────────────────
    //
    // Isto vem ANTES de abrir qualquer coisa, e é a única ordem possível: o
    // arquivo diz para onde ir. Config quebrada aqui é RECUSA, não "cai no
    // padrão" — ver LerConfigBanco em Banco.cpp para o porquê (quem escreve
    // "mysqll" e cai no sqlite calado grava os VIPs num arquivo que ninguém
    // olha).
    ConfigBanco cfg;
    std::string erro;
    if (!LerConfigBanco(m_caminhoJson.c_str(), m_caminhoDb.c_str(), cfg, erro))
    {
        Registrar("[permission] configuracao recusada: %s", erro.c_str());
        return false;
    }

    std::unique_ptr<IBanco> novo = CriarBanco(cfg, m_log);
    if (!novo) { Registrar("[permission] tipo de banco desconhecido"); return false; }

    if (cfg.tipo == ConfigBanco::SQLITE)
        Registrar("[permission] banco: sqlite em %s%s", cfg.caminhoSqlite.c_str(),
                  (cfg.caminhoSqlite == m_caminhoDb) ? "" : "  (via DbPathOverride)");

    // ── o banco entra no lugar ANTES de tentar abrir, e isso é deliberado ────
    //
    // O único jeito de Fechar() conseguir interromper uma abertura em curso é
    // ela estar alcançável. Sem isto, um desligamento pedido durante o
    // arranque — que é justamente quando ele demora — esperaria a abertura
    // inteira: 120 s medidos contra um MySQL a 2 s por comando.
    //
    // O preço é que m_banco passa a existir num estado "criado e não aberto".
    // Ele é pago aqui e só aqui: TODO caminho de falha abaixo devolve
    // m_banco a nullptr, então quem lê m_banco continua podendo entender
    // "existe" como "já abriu alguma vez".
    { std::lock_guard<std::mutex> g(m_mtxBanco); m_banco = std::move(novo); }

    if (!m_banco->Abrir())
    {
        // A mensagem do meio já vem pronta e em português (porta fechada, nome
        // que não resolve, senha errada, banco inexistente). Repassar inteira é
        // o certo: encurtar aqui seria jogar fora a única informação acionável.
        Registrar("[permission] nao consegui abrir o banco: %s", m_banco->Erro());
        { std::lock_guard<std::mutex> g(m_mtxBanco); m_banco.reset(); }
        return false;
    }

    if (!AplicarEsquema())
    {
        m_banco->Fechar();
        { std::lock_guard<std::mutex> g(m_mtxBanco); m_banco.reset(); }
        return false;
    }

    if (!m_caminhoJson.empty() && !AplicarConfig(m_caminhoJson.c_str()))
    {
        // Config inválida NÃO derruba o Permission: o banco já tem os grupos da
        // última vez que o JSON estava bom. Melhor rodar com a configuração de
        // ontem e gritar no log do que ficar sem permissão nenhuma porque
        // alguém esqueceu uma vírgula às 3 da manhã.
        Registrar("[permission] permission.json rejeitado; seguindo com o que ja "
                  "estava no banco. Nada foi perdido.");
    }

    if (!Reconstruir())
    {
        m_banco->Fechar();
        { std::lock_guard<std::mutex> g(m_mtxBanco); m_banco.reset(); }
        return false;
    }
    return true;
}

bool Armazem::Abrir(const char* caminhoDb, const char* caminhoJson, FnLog log)
{
    m_log = log;
    // A guarda passou a ser a THREAD, e não o banco: agora existe um estado
    // legítimo em que o Armazém está de pé e o banco ainda não abriu, e
    // conferir m_banco aqui deixaria Abrir ser chamado duas vezes justamente
    // nesse estado — duas threads escritoras sobre a mesma fila.
    if (m_thread.joinable()) { Registrar("[permission] Abrir chamado duas vezes; ignorado"); return false; }
    if (!caminhoDb || !*caminhoDb) { Registrar("[permission] caminho do banco vazio"); return false; }

    m_caminhoDb   = caminhoDb;
    m_caminhoJson = caminhoJson ? caminhoJson : "";

    // ── quem abre o banco é a THREAD, e Abrir só espera por ela — com teto ───
    //
    // Ver o comentário de Abrir em Armazem.h para o número medido (120,5 s de
    // arranque contra um MySQL a 2 s por comando) e para o porquê dos 15 s.
    //
    // No caso normal — sqlite local, ou MySQL saudável — isto termina em
    // dezenas de milissegundos e o comportamento é idêntico ao de antes:
    // Abrir() volta com o Permission já de pé.
    m_rodando.store(true, std::memory_order_release);
    m_thread = std::thread(&Armazem::Trabalhar, this);

    for (int i = 0; i < ARRANQUE_ESPERA_MS / 5; ++i)
    {
        if (m_arranqueFeito.load(std::memory_order_acquire)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    if (!m_arranqueFeito.load(std::memory_order_acquire))
    {
        // Nem falhou nem subiu: está demorando. Dizer isso é o que separa "o
        // servidor travou" de "o banco dele é lento e o plugin avisou".
        Registrar("[permission] o banco esta demorando mais de %d s para abrir. NAO vou "
                  "segurar o arranque do servidor por causa disso: sigo abrindo em "
                  "segundo plano e o Permission entra sozinho quando terminar. Ate la "
                  "ele responde como AUSENTE.", ARRANQUE_ESPERA_MS / 1000);
        return true;
    }

    // O aviso em moldura do banco que não abriu NÃO sai daqui: sai de
    // CuidarDaConexao, na thread que descobriu a falha. Se saísse daqui, um
    // banco que demora mais que ARRANQUE_ESPERA_MS para falhar (DNS lento, por
    // exemplo) falharia sem nunca imprimir o aviso — e a mensagem que o dono
    // não encontra é a mensagem que não existe.
    return true;
}

void Armazem::Fechar()
{
    if (m_rodando.exchange(false))
    {
        {
            std::lock_guard<std::mutex> g(m_mtxFila);
            Tarefa sair; sair.tipo = Tarefa::SAIR;
            m_fila.push_back(std::move(sair));
        }
        m_cvFila.notify_all();

        // ── interromper ANTES de esperar ────────────────────────────────────
        //
        // A thread escritora pode estar dentro de um recv() contra um MySQL que
        // aceitou a conexão e emudeceu. Sem isto, o join espera o prazo do
        // banco — msOperar POR COMANDO, e uma reconstrução são 7 consultas: até
        // ~70 s de servidor pendurado no desligamento, com os prazos padrão. Aí
        // o dono faz o que qualquer um faria e mata o processo — e matar o
        // Conan no desligamento perde o save do mundo. Um problema do banco não
        // pode terminar em save perdido.
        //
        // Interromper() é seguro de outra thread por contrato (Banco.h) e é
        // definitivo: daqui em diante este banco não serve mais para nada, que
        // é exatamente o que se quer de um processo que está morrendo.
        { std::lock_guard<std::mutex> g(m_mtxBanco); if (m_banco) m_banco->Interromper(); }

        if (m_thread.joinable()) m_thread.join();
    }
    m_bancoServe.store(false, std::memory_order_release);
    // Só agora se libera instantâneo: a thread escritora já parou, e no jogo o
    // Fechar acontece no descarregamento do processo.
    const Instantaneo* a = m_atual.exchange(nullptr, std::memory_order_acq_rel);
    delete a;
    for (const Instantaneo* v : m_aposentados) delete v;
    m_aposentados.clear();
    if (m_banco) { m_banco->Fechar(); m_banco.reset(); }
}

bool Armazem::AplicarEsquema()
{
    // Um comando por vez, nos dois meios. O SQLite aceitaria o esquema inteiro
    // num exec só; o MySQL desta casa não, porque CLIENT_MULTI_STATEMENTS está
    // desligado de propósito (MySqlCliente.h). Rodar um por um nos dois faz o
    // caminho ser o mesmo caminho — e a falha aponta QUAL comando falhou, em
    // vez de "o esquema falhou".
    for (const char* const* p = m_banco->S().esquema; *p; ++p)
        if (!m_banco->Executar(*p))
        {
            Registrar("[permission] esquema falhou (%s): %s", m_banco->Nome(), m_banco->Erro());
            Registrar("[permission] %s", m_banco->DicaEsquema());
            return false;
        }

    {
        std::unique_ptr<IComando> c = m_banco->Preparar(m_banco->S().meta_gravar_versao);
        if (!c) { Registrar("[permission] meta: %s", m_banco->Erro()); return false; }
        char v[16]; std::snprintf(v, sizeof(v), "%d", ESQUEMA_VERSAO);
        if (!c->LigarTexto(1, v) || !c->Executar())
        { Registrar("[permission] meta: %s", m_banco->Erro()); return false; }
    }

    // Versão de esquema MAIOR que a que este binário conhece: recusar. Rodar
    // por cima de um banco de uma versão futura é o caminho mais curto para
    // apagar dado que não se entende.
    int versao = 0;
    if (!m_banco->Consultar(m_banco->S().meta_ler_versao,
        [](const ILinha& l, void* p) { *static_cast<int*>(p) = static_cast<int>(l.Inteiro(0)); },
        &versao))
    { Registrar("[permission] nao consegui ler a versao do esquema: %s", m_banco->Erro()); return false; }

    if (versao > ESQUEMA_VERSAO)
    {
        Registrar("[permission] RECUSADO: banco e da versao de esquema %d e este "
                  "Permission conhece %d. Atualize o plugin; nao vou adivinhar.",
                  versao, ESQUEMA_VERSAO);
        return false;
    }
    return true;
}

// ============================================================================
//  configuração: permission.json -> banco
//
//  Regra que governa este trecho: o JSON MANDA na FORMA (quais grupos existem,
//  o que cada um pode, quem herda de quem) e NUNCA na POSSE (quem é VIP). Posse
//  mora só no banco, e nada aqui apaga posse.
//
//  Isso é o que faz "renomear sem perder o que está gravado" funcionar: mexer
//  no JSON reescreve permissões de grupo à vontade; jogador_grupo não é tocado.
//
//  O QUE MUDOU COM OS DOIS BANCOS
//  ------------------------------
//  Antes isto era um punhado de SQL grande, com json_each cruzando direto com
//  as tabelas reais. O json1 continua lendo o arquivo (agora num SQLite de
//  memória, ver Banco.cpp), mas a GRAVAÇÃO passou a ser comando a comando,
//  parametrizado — que é o único jeito de o mesmo código servir aos dois.
//
//  A ORDEM É A MESMA, e ela não é arbitrária: renomes ANTES de qualquer upsert.
//  Se o upsert rodasse primeiro, ele criaria um grupo NOVO com a chave nova, e
//  o antigo — com todos os VIPs dentro — ficaria órfão. O sintoma seria
//  exatamente o que o dono pediu para não acontecer: renomeei e perdi os dados.
// ============================================================================
bool Armazem::AplicarConfig(const char* caminhoJson)
{
    ConfigPermissao cfg;
    bool existe = false;
    std::string erro;
    if (!LerConfigPermissao(caminhoJson, cfg, existe, erro))
    {
        Registrar("[permission] permission.json recusado: %s", erro.c_str());
        return false;
    }
    if (!existe)
    {
        Registrar("[permission] sem permission.json em %s — usando o banco como esta",
                  caminhoJson);
        return true;      // ausência não é erro: o banco já pode estar pronto
    }

    // Tudo ou nada. Config aplicada pela metade é o pior estado possível: o
    // grupo existe e as permissões dele não.
    if (!m_banco->Iniciar())
    { Registrar("[permission] nao consegui abrir transacao para a config: %s",
                m_banco->Erro()); return false; }

    bool bom = true;
    auto falhar = [&](const char* onde)
    {
        Registrar("[permission] config: %s: %s", onde, m_banco->Erro());
        bom = false;
    };

    // ── 1. renomes explícitos ("era") ───────────────────────────────────────
    for (const ConfigGrupo& g : cfg.grupos)
    {
        if (!bom) break;
        if (g.era.empty() || g.era == g.chave) continue;

        // Guarda a chave velha como APELIDO antes de trocar: é o que mantém
        // certo o plugin de terceiro que foi compilado perguntando por "vip".
        {
            std::unique_ptr<IComando> c = m_banco->Preparar(m_banco->S().grupo_apelido_do_renome);
            if (!c) { falhar("preparar apelido do renome"); break; }
            if (!c->LigarTexto(1, g.era.c_str()) || !c->Executar())
            { falhar("gravar apelido do renome"); break; }
        }
        {
            std::unique_ptr<IComando> c = m_banco->Preparar(m_banco->S().grupo_renomear);
            if (!c) { falhar("preparar renome"); break; }
            if (!c->LigarTexto(1, g.chave.c_str()) || !c->LigarTexto(2, g.era.c_str())
                || !c->Executar())
            { falhar("renomear grupo"); break; }
            if (c->Mudancas() > 0)
                Registrar("[permission] grupo renomeado: '%s' -> '%s' "
                          "(membros intactos, '%s' continua valendo como apelido)",
                          g.era.c_str(), g.chave.c_str(), g.era.c_str());
        }
    }

    // ── 2. os grupos ────────────────────────────────────────────────────────
    if (bom)
        for (const ConfigGrupo& g : cfg.grupos)
        {
            std::unique_ptr<IComando> c = m_banco->Preparar(m_banco->S().grupo_upsert);
            if (!c) { falhar("preparar grupos"); break; }
            if (!c->LigarTexto(1, g.chave.c_str()) || !c->LigarTexto(2, g.nome.c_str())
                || !c->LigarInteiro(3, g.prioridade) || !c->LigarInteiro(4, g.padrao ? 1 : 0)
                || !c->Executar())
            { falhar("gravar grupos"); break; }
        }

    // ── 3. herança e permissões: o JSON é a verdade, então reescreve ────────
    //
    // Apagar-e-regravar SÓ estas três tabelas. jogador_grupo e
    // jogador_permissao ficam de fora — é ali que mora a posse.
    if (bom && !m_banco->Executar(m_banco->S().grupo_herda_limpar))       falhar("limpar heranca");
    if (bom)
    {
        // Deduplicar aqui, e não com INSERT OR IGNORE / INSERT IGNORE: o
        // IGNORE do MySQL também engole violação de chave estrangeira e
        // truncamento de dado, e uma tabela recém-esvaziada só recebe
        // duplicata se o próprio permission.json repetir a linha. Repetição no
        // arquivo é erro de edição inofensivo — some, com log — e erro de
        // verdade continua aparecendo.
        std::vector<std::pair<std::string,std::string>> vistos;
        for (const ConfigGrupo& g : cfg.grupos)
        {
            if (!bom) break;
            for (const std::string& pai : g.herda)
            {
                bool repetido = false;
                for (const auto& v : vistos)
                    if (v.first == g.chave && v.second == pai) { repetido = true; break; }
                if (repetido)
                {
                    Registrar("[permission] config: '%s' herda de '%s' duas vezes; "
                              "a segunda foi ignorada.", g.chave.c_str(), pai.c_str());
                    continue;
                }
                vistos.emplace_back(g.chave, pai);

                std::unique_ptr<IComando> c = m_banco->Preparar(m_banco->S().grupo_herda_inserir);
                if (!c) { falhar("preparar heranca"); break; }
                if (!c->LigarTexto(1, g.chave.c_str()) || !c->LigarTexto(2, pai.c_str())
                    || !c->Executar())
                { falhar("gravar heranca"); break; }
                // 0 linhas = o pai não existe (ou é o próprio filho). Antes isto
                // era silencioso, e "herda": ["deafult"] escrito errado virava um
                // grupo sem herança nenhuma sem uma palavra no log.
                if (c->Mudancas() == 0)
                    Registrar("[permission] config: '%s' diz herdar de '%s', mas esse "
                              "grupo nao existe no permission.json. A heranca foi "
                              "ignorada — confira a grafia.",
                              g.chave.c_str(), pai.c_str());
            }
        }
    }

    if (bom && !m_banco->Executar(m_banco->S().grupo_permissao_limpar)) falhar("limpar permissoes");
    if (bom)
    {
        std::vector<std::pair<std::string,std::string>> vistos;
        for (const ConfigGrupo& g : cfg.grupos)
        {
            if (!bom) break;
            for (const auto& pr : g.permissoes)
            {
                bool repetido = false;
                for (const auto& v : vistos)
                    if (v.first == g.chave && v.second == pr.first) { repetido = true; break; }
                if (repetido)
                {
                    // Isto pega o caso que mais confunde: ["vip.a", "-vip.a"] no
                    // mesmo grupo. A chave da tabela é (grupo,no) e `nega` não
                    // entra nela, então só um dos dois pode existir. O primeiro
                    // ganha — como já ganhava com INSERT OR IGNORE — mas agora
                    // o dono fica sabendo, em vez de ver a negação sumir.
                    Registrar("[permission] config: o grupo '%s' declara '%s' duas "
                              "vezes (uma delas negada?). Valeu a primeira; a "
                              "segunda foi ignorada.", g.chave.c_str(), pr.first.c_str());
                    continue;
                }
                vistos.emplace_back(g.chave, pr.first);

                std::unique_ptr<IComando> c = m_banco->Preparar(m_banco->S().grupo_permissao_inserir);
                if (!c) { falhar("preparar permissoes"); break; }
                if (!c->LigarTexto(1, g.chave.c_str()) || !c->LigarTexto(2, pr.first.c_str())
                    || !c->LigarInteiro(3, pr.second ? 1 : 0) || !c->Executar())
                { falhar("gravar permissoes"); break; }
            }
        }
    }

    if (bom && !m_banco->Executar(m_banco->S().permissao_apelido_limpar)) falhar("limpar apelidos");
    if (bom)
    {
        std::vector<std::string> vistos;
        for (const auto& a : cfg.apelidosDeNo)
        {
            if (!bom) break;
            bool repetido = false;
            for (const std::string& v : vistos) if (v == a.first) { repetido = true; break; }
            if (repetido)
            {
                Registrar("[permission] config: o apelido de permissao '%s' aparece "
                          "duas vezes; valeu o primeiro.", a.first.c_str());
                continue;
            }
            vistos.push_back(a.first);

            std::unique_ptr<IComando> c = m_banco->Preparar(m_banco->S().permissao_apelido_inserir);
            if (!c) { falhar("preparar apelidos"); break; }
            if (!c->LigarTexto(1, a.first.c_str()) || !c->LigarTexto(2, a.second.c_str())
                || !c->Executar())
            { falhar("gravar apelidos"); break; }
        }
    }

    // ── 4. um grupo padrão tem de existir ───────────────────────────────────
    //
    // Sem grupo padrão, todo jogador que nunca foi tocado por um admin é um
    // jogador sem NADA — e o servidor inteiro parece quebrado sem uma
    // mensagem de erro. Falhar alto aqui é muito melhor.
    if (bom)
    {
        int n = 0;
        if (!m_banco->Consultar(m_banco->S().contar_padrao,
            [](const ILinha& l, void* p) { *static_cast<int*>(p) = static_cast<int>(l.Inteiro(0)); },
            &n))
            falhar("contar grupos padrao");
        else if (n == 0)
        {
            Registrar("[permission] RECUSADO: nenhum grupo com \"padrao\": true. "
                      "Sem isso, jogador novo nao tem permissao nenhuma e o "
                      "servidor parece quebrado sem motivo aparente.");
            bom = false;
        }
    }

    // ── COMMIT que falha PRECISA de ROLLBACK ────────────────────────────────
    //
    // Achado na revisão da própria alteração (§19), não rodando. No sqlite um
    // COMMIT que falha — disco cheio, banco travado por outro processo — NÃO
    // fecha a transação: ela fica ABERTA. O próximo Iniciar() morre então com
    // "cannot start a transaction within a transaction", e a partir dali TODA
    // escrita do plugin falha por causa de um erro que aconteceu uma vez,
    // minutos antes. O sintoma não tem nenhuma relação visível com a causa.
    if (bom && !m_banco->Confirmar())
    {
        Registrar("[permission] COMMIT da config falhou: %s", m_banco->Erro());
        bom = false;
        // ── DESFAZER É OBRIGATÓRIO AQUI ─────────────────────────────────────
        //
        // COMMIT que falha NÃO fecha a transação: ela continua aberta. E com uma
        // transação aberta pendurada, TODA escrita seguinte falha — conceder VIP,
        // revogar, criar grupo. O log culparia a operação da vez, e a causa real
        // (um COMMIT que falhou minutos antes) não apareceria em lugar nenhum.
        //
        // Cenário concreto: disco enche por um instante, o COMMIT da config
        // falha, o disco libera — e o Permission fica inutilizável até o
        // servidor reiniciar, dizendo "erro ao gravar" para tudo.
        //
        // ROLLBACK sobre transação já desfeita é inofensivo; deixar aberta não.
        m_banco->Desfazer();
    }
    if (!bom) m_banco->Desfazer();
    if (bom) Registrar("[permission] permission.json aplicado");
    return bom;
}

// ============================================================================
//  banco -> instantâneo
// ============================================================================
bool Armazem::Reconstruir()
{
    std::unique_ptr<Instantaneo> novo(new Instantaneo());

    struct Grupo
    {
        int64_t id = 0; std::string chave; int32_t prioridade = 0; bool padrao = false;
        std::vector<int64_t> pais;
        std::vector<std::pair<std::string,bool>> nos;   // (nó, nega)
    };
    std::unordered_map<int64_t, Grupo> grupos;
    std::unordered_map<std::string, int64_t> porChave;

    // Uma leitura que falha ABORTA a reconstrução antes de qualquer publicação
    // (INV-BANCO-003): o instantâneo anterior continua valendo. Com MySQL isto
    // deixou de ser hipótese — a conexão pode cair no meio da terceira consulta
    // — e o comportamento certo é servidor com permissões velhas e corretas,
    // nunca com permissões pela metade.
    auto ler = [&](const char* sql, FnLinha fn, void* ctx) -> bool
    {
        if (!m_banco->Consultar(sql, fn, ctx))
        {
            Registrar("[permission] reconstruir: %s", m_banco->Erro());
            return false;
        }
        return true;
    };

    struct CtxG { std::unordered_map<int64_t,Grupo>* g; std::unordered_map<std::string,int64_t>* c; };
    CtxG cg{&grupos, &porChave};
    if (!ler(m_banco->S().ler_grupos,
        [](const ILinha& l, void* p)
        {
            auto* c = static_cast<CtxG*>(p);
            Grupo g;
            g.id         = l.Inteiro(0);
            const char* t = l.Texto(1);
            g.chave      = t ? t : "";
            g.prioridade = static_cast<int32_t>(l.Inteiro(2));
            g.padrao     = l.Inteiro(3) != 0;
            (*c->c)[g.chave] = g.id;
            (*c->g)[g.id]    = std::move(g);
        }, &cg)) return false;

    if (!ler(m_banco->S().ler_heranca,
        [](const ILinha& l, void* p)
        {
            auto* g = static_cast<std::unordered_map<int64_t,Grupo>*>(p);
            auto it = g->find(l.Inteiro(0));
            if (it != g->end()) it->second.pais.push_back(l.Inteiro(1));
        }, &grupos)) return false;

    if (!ler(m_banco->S().ler_grupo_permissao,
        [](const ILinha& l, void* p)
        {
            auto* g = static_cast<std::unordered_map<int64_t,Grupo>*>(p);
            auto it = g->find(l.Inteiro(0));
            if (it == g->end()) return;
            const char* t = l.Texto(1);
            if (t) it->second.nos.emplace_back(t, l.Inteiro(2) != 0);
        }, &grupos)) return false;

    // ── apelidos, montados direto na tabela sem alocação de consulta ─────────
    struct CtxA { Tabela* idx; std::vector<std::string>* para; FnLog log; };
    auto lerApelidos = [&](const char* sql, Tabela& idx, std::vector<std::string>& para) -> bool
    {
        CtxA c{&idx, &para, m_log};
        return ler(sql, [](const ILinha& l, void* p)
        {
            auto* c = static_cast<CtxA*>(p);
            const char* d = l.Texto(0);
            const char* v = l.Texto(1);
            if (!d || !v) return;
            c->para->push_back(v);
            if (!c->idx->Inserir(d, static_cast<int32_t>(c->para->size()) - 1))
            {
                c->para->pop_back();
                if (c->log) c->log("[permission] apelido longo demais; ignorado");
            }
        }, &c);
    };
    if (!lerApelidos(m_banco->S().ler_apelidos_de_grupo,
                     novo->idxApelidoGrupo, novo->apelidoGrupoPara)) return false;
    if (!lerApelidos(m_banco->S().ler_apelidos_de_no,
                     novo->idxApelidoNo, novo->apelidoNoPara)) return false;

    // ── achatar a herança de um grupo ───────────────────────────────────────
    //
    // Feito UMA vez, aqui, fora do laço. É por isso que a leitura no jogo é um
    // hash e não uma travessia de grafo. O guarda de profundidade e o conjunto
    // de visitados existem porque herança circular ("a herda de b, b herda de
    // a") é um erro de digitação normal num arquivo editado à mão — e num
    // grafo sem guarda isso é recursão infinita dentro do processo do
    // servidor, ou seja, servidor no chão por causa de uma vírgula.
    auto achatar = [&](int64_t raizId, bool individualNao, int64_t expiraDoVinculo,
                       std::vector<NoResolvido>& saida)
    {
        std::vector<int64_t> pilha{raizId};
        std::vector<int64_t> vistos;
        int guarda = 0;
        while (!pilha.empty() && guarda++ < 4096)
        {
            const int64_t id = pilha.back(); pilha.pop_back();
            if (std::find(vistos.begin(), vistos.end(), id) != vistos.end()) continue;
            vistos.push_back(id);
            auto it = grupos.find(id);
            if (it == grupos.end()) continue;
            for (const auto& pr : it->second.nos)
            {
                NoResolvido n;
                n.padrao     = pr.first;
                n.nega       = pr.second;
                n.individual = individualNao;
                n.expira     = expiraDoVinculo;
                n.curinga    = !n.padrao.empty() && n.padrao[n.padrao.size()-1] == '*';
                saida.push_back(std::move(n));
            }
            for (int64_t pai : it->second.pais) pilha.push_back(pai);
        }
        if (guarda >= 4096)
            Registrar("[permission] heranca de grupo muito profunda ou circular; "
                      "cortada no limite. Confira 'herda' no permission.json.");
    };

    // ── o jogador que ainda não existe no banco ─────────────────────────────
    for (const auto& kv : grupos)
        if (kv.second.padrao)
        {
            achatar(kv.first, false, 0, novo->padrao.nos);
            novo->padrao.grupos.push_back(GrupoDoJogador{kv.second.chave, 0});
        }

    // ── os jogadores ────────────────────────────────────────────────────────
    //
    // Montados num mapa (alocar aqui é livre: isto roda na thread escritora,
    // não no laço do jogo) e depois ACHATADOS em vetor + Tabela, que é o que a
    // consulta usa.
    std::unordered_map<std::string, JogadorResolvido> emMontagem;
    {
        // ── por que as linhas são COLETADAS antes de processadas ─────────────
        //
        // `achatar` é uma lambda com capturas (precisa do mapa `grupos`), e a
        // interface do banco recebe ponteiro de função puro — de propósito: um
        // std::function no caminho de leitura seria alocação escondida numa
        // fronteira que os dois meios têm de cumprir igual.
        //
        // Coletar primeiro e processar depois custa um vetor temporário e
        // resolve isso sem tocar na lógica. Alocar AQUI é livre: isto roda na
        // thread escritora, nunca no laço do jogo. E tem um efeito colateral
        // bom: a conexão fica ocupada só o tempo de ler, o que importa quando
        // do outro lado há um MySQL numa rede lenta.
        struct Vinculo { std::string jogador, chave; int64_t gid, expira; };
        std::vector<Vinculo> linhas;
        if (!ler(m_banco->S().ler_jogador_grupo,
            [](const ILinha& l, void* p)
            {
                const char* j = l.Texto(0);
                if (!j) return;
                const char* c = l.Texto(3);
                static_cast<std::vector<Vinculo>*>(p)->push_back(
                    Vinculo{ j, c ? c : "", l.Inteiro(1), l.Inteiro(2) });
            }, &linhas)) return false;

        for (const Vinculo& v : linhas)
        {
            JogadorResolvido& jr = emMontagem[v.jogador];
            jr.grupos.push_back(GrupoDoJogador{ v.chave, v.expira });
            achatar(v.gid, false, v.expira, jr.nos);
        }
    }

    // exceções individuais: ganham de qualquer grupo, por construção do peso
    {
        struct Ctx { std::unordered_map<std::string, JogadorResolvido>* m; };
        Ctx c{ &emMontagem };
        if (!ler(m_banco->S().ler_jogador_permissao,
            [](const ILinha& l, void* p)
            {
                const char* j = l.Texto(0);
                const char* n = l.Texto(1);
                if (!j || !n) return;
                NoResolvido no;
                no.padrao     = n;
                no.nega       = l.Inteiro(2) != 0;
                no.individual = true;
                no.expira     = l.Inteiro(3);
                no.curinga    = !no.padrao.empty() && no.padrao[no.padrao.size()-1] == '*';
                (*static_cast<Ctx*>(p)->m)[j].nos.push_back(std::move(no));
            }, &c)) return false;
    }

    // Todo jogador conhecido também tem os direitos do grupo padrão. Sem isto,
    // dar VIP a alguém REMOVERIA dele o que o grupo padrão dava — porque ele
    // deixaria de cair no caminho do "desconhecido". Defeito clássico e
    // silencioso: o admin promove o jogador e o jogador perde funções.
    for (auto& kv : emMontagem)
    {
        bool temPadrao = false;
        for (const auto& g : kv.second.grupos)
        {
            auto it = porChave.find(g.chave);
            if (it != porChave.end())
            {
                auto ig = grupos.find(it->second);
                if (ig != grupos.end() && ig->second.padrao) { temPadrao = true; break; }
            }
        }
        if (!temPadrao)
        {
            kv.second.nos.insert(kv.second.nos.end(),
                                 novo->padrao.nos.begin(), novo->padrao.nos.end());
            kv.second.grupos.insert(kv.second.grupos.end(),
                                    novo->padrao.grupos.begin(), novo->padrao.grupos.end());
        }
    }

    // ── achatar em vetor + Tabela: é esta forma que a consulta lê ────────────
    novo->jogadores.reserve(emMontagem.size());
    novo->idxJogador.Reservar(emMontagem.size() + 1);
    int recusados = 0;
    for (auto& kv : emMontagem)
    {
        novo->jogadores.push_back(std::move(kv.second));
        if (!novo->idxJogador.Inserir(kv.first.c_str(),
                                      static_cast<int32_t>(novo->jogadores.size()) - 1))
        {
            novo->jogadores.pop_back();
            ++recusados;
        }
    }
    if (recusados)
        Registrar("[permission] %d id(s) de jogador com mais de %d caracteres foram "
                  "RECUSADOS. Truncar faria dois jogadores virarem um so, e um "
                  "herdaria o VIP do outro em silencio.", recusados, MAX_ID - 1);

    const Instantaneo* velho = Atual();
    novo->geracao = velho ? velho->geracao + 1 : 1;
    Registrar("[permission] instantaneo #%llu: %zu grupo(s), %zu jogador(es), "
              "%zu no(s) no padrao",
              static_cast<unsigned long long>(novo->geracao),
              grupos.size(), novo->jogadores.size(), novo->padrao.nos.size());
    Publicar(novo.release());
    return true;
}

void Armazem::Publicar(Instantaneo* novo)
{
    const Instantaneo* velho = m_atual.exchange(novo, std::memory_order_acq_rel);

    // ── por que NÃO se libera o velho agora ─────────────────────────────────
    //
    // Um leitor do laço do jogo pode ter pegado o ponteiro velho um
    // nanossegundo antes desta troca e ainda estar dentro dele. `delete` aqui
    // seria use-after-free dentro do processo do servidor — o defeito que este
    // projeto proíbe acima de todos, porque não dá erro: dá queda, mais tarde,
    // em outro lugar, sem rastro.
    //
    // A troca já aconteceu acima: daqui para a frente, TODO leitor novo enxerga
    // o instantâneo novo. Então se o contador de leitores estiver em zero neste
    // instante, nenhum leitor pode estar segurando um aposentado — e liberar é
    // certeza, não estimativa.
    //
    // A versão anterior contava PUBLICAÇÕES em vez de leitores, apostando que
    // "duas trocas levam segundos". Levam ~4 ms. O servidor caía. Ver o
    // comentário de Armazem::Leitura, em Armazem.h, para a medição e o rastro.
    //
    // Reader-writer lock resolveria também, ao custo de o laço do jogo poder
    // esperar por um escritor. Não é negociável: o laço não espera. O contador
    // de leitores dá a mesma garantia sem nunca bloquear quem lê.
    if (velho) m_aposentados.push_back(velho);

    if (m_leitores.load(std::memory_order_acquire) == 0)
    {
        for (const Instantaneo* v : m_aposentados) delete v;
        m_aposentados.clear();
    }
    else if (m_aposentados.size() > 64)
    {
        // Leitura presa por muito tempo com escrita em rajada. Não se libera
        // nada — acumular memória é ruim, use-after-free é inaceitável — mas o
        // log tem de contar, porque 64 aposentados vivos significa que alguma
        // leitura não está terminando, e isso é um defeito em outro lugar.
        Registrar("[permission] ATENCAO: %zu instantaneos aposentados e %d leitor(es) "
                  "ativo(s). Nada foi liberado (de proposito). Se este numero cresce "
                  "sem parar, alguma leitura de permissao nao esta terminando.",
                  m_aposentados.size(), m_leitores.load(std::memory_order_relaxed));
    }
}

uint64_t Armazem::Geracao() const
{ const Instantaneo* a = Atual(); return a ? a->geracao : 0; }

// ============================================================================
//  leitura — o que roda no laço do jogo
// ============================================================================
int32_t Armazem::Tem(const char* jogador, const char* no) const
{
    const Leitura lida(*this);
    const Instantaneo* s = lida.get();
    if (!s) return NAO_SEI;
    // Entrada da ABI: confere TERMINADOR, não só nulo/vazio. Ver
    // ComprimentoLimitado em Armazem.h para o defeito e para o que a defesa
    // não cobre. Depois daqui, `no` pode ser caminhado por
    // `std::string::operator==` dentro de Casa() sem levar o processo junto.
    if (ComprimentoLimitado(jogador, MAX_ID) < 0) return NAO_SEI;
    if (ComprimentoLimitado(no,      MAX_NO) < 0) return NAO_SEI;

    // apelido de nó: renomear permissão sem invalidar plugin já compilado.
    // Devolve ponteiro para dentro do instantâneo — sem cópia, sem alocação.
    const char* alvo = s->NoAtual(no);

    const JogadorResolvido* achado = s->Achar(jogador);
    const JogadorResolvido& j = achado ? *achado : s->padrao;

    const int64_t agora = Agora();
    int32_t melhorPeso = -1;
    bool    melhorNega = false;
    for (const NoResolvido& n : j.nos)
    {
        if (Vencido(n.expira, agora)) continue;
        bool cur = false;
        const int32_t len = Casa(n.padrao, alvo, cur);
        if (len < 0) continue;
        // peso: individual >> comprimento do casamento >> negação desempata
        const int32_t peso = (n.individual ? (1 << 24) : 0) + len * 2 + (n.nega ? 1 : 0);
        if (peso > melhorPeso) { melhorPeso = peso; melhorNega = n.nega; }
    }
    if (melhorPeso < 0) return NEGADO;      // nada casou: negado por omissão
    return melhorNega ? NEGADO : PERMITIDO;
}

int32_t Armazem::NoGrupo(const char* jogador, const char* grupo) const
{
    const Leitura lida(*this);
    const Instantaneo* s = lida.get();
    if (!s) return NAO_SEI;
    if (ComprimentoLimitado(jogador, MAX_ID)    < 0) return NAO_SEI;
    if (ComprimentoLimitado(grupo,   MAX_GRUPO) < 0) return NAO_SEI;

    const char* alvo = s->GrupoAtual(grupo);
    const JogadorResolvido* achado = s->Achar(jogador);
    const JogadorResolvido& j = achado ? *achado : s->padrao;

    const int64_t agora = Agora();
    for (const GrupoDoJogador& g : j.grupos)
        if (g.chave == alvo && !Vencido(g.expira, agora)) return PERMITIDO;
    return NEGADO;
}

int64_t Armazem::ExpiraEm(const char* jogador, const char* grupo) const
{
    const Leitura lida(*this);
    const Instantaneo* s = lida.get();
    if (!s) return -1;
    if (ComprimentoLimitado(jogador, MAX_ID)    < 0) return -1;
    if (ComprimentoLimitado(grupo,   MAX_GRUPO) < 0) return -1;

    const char* alvo = s->GrupoAtual(grupo);
    const JogadorResolvido* achado = s->Achar(jogador);
    const JogadorResolvido& j = achado ? *achado : s->padrao;
    const int64_t agora = Agora();
    for (const GrupoDoJogador& g : j.grupos)
        if (g.chave == alvo && !Vencido(g.expira, agora)) return g.expira;
    return -1;
}

int32_t Armazem::Grupos(const char* jogador, char* saida, int32_t tam) const
{
    if (saida && tam > 0) saida[0] = 0;
    const Leitura lida(*this);
    const Instantaneo* s = lida.get();
    if (!s) return -1;
    if (ComprimentoLimitado(jogador, MAX_ID) < 0) return -1;

    const JogadorResolvido* achado = s->Achar(jogador);
    const JogadorResolvido& j = achado ? *achado : s->padrao;

    // Grupos() aloca (monta um std::string) e é a ÚNICA consulta que aloca.
    // Aceitável porque ela existe para o comando de chat e para o log — não
    // para o laço do jogo. Quem chamar isto por tick está usando errado, e a
    // documentação diz isso.
    const int64_t agora = Agora();
    std::string acc;
    for (const GrupoDoJogador& g : j.grupos)
    {
        if (Vencido(g.expira, agora)) continue;
        if (acc.find(g.chave + "\n") == 0 ||
            acc.find("\n" + g.chave + "\n") != std::string::npos) continue;  // sem repetir
        acc += g.chave; acc += '\n';
    }
    Copiar(saida, tam, acc);
    return static_cast<int32_t>(acc.size()) + 1;   // convenção de snprintf
}

// ============================================================================
//  escrita — fila + thread própria
// ============================================================================
// ── enfileirar com teto ──────────────────────────────────────────────────────
//
// Roda no LAÇO DO JOGO. Tudo aqui é O(1) e sob a trava por microssegundos: nada
// de percorrer a fila (ver o porquê no campo m_vistosNaFila, em Armazem.h).
//
// false = recusada por teto. Quem chamou traduz isso para o retorno certo; a
// mensagem de log fica com a thread escritora, que é quem pode ter freio de
// repetição sem custar nada ao tick.
bool Armazem::Enfileirar(Tarefa&& t)
{
    const bool visto = (t.tipo == Tarefa::VISTO);
    {
        std::lock_guard<std::mutex> g(m_mtxFila);
        if (m_fila.size() >= FILA_TETO) { ++m_descartadasDesdeAviso; return false; }
        if (visto && m_vistosNaFila >= FILA_TETO_VISTO)
        { ++m_descartadasDesdeAviso; return false; }
        if (visto) ++m_vistosNaFila;
        m_fila.push_back(std::move(t));
    }
    m_enfileiradas.fetch_add(1, std::memory_order_relaxed);
    m_cvFila.notify_one();
    return true;
}

int32_t Armazem::Conceder(const char* jogador, const char* grupo,
                          int64_t expiraEm, const char* quem)
{
    if (!m_rodando.load(std::memory_order_acquire)) return NAO_SEI;
    // ── o banco está fora: NAO_SEI, e não PERMITIDO ─────────────────────────
    //
    // DEFEITO DE BOA-FÉ que isto conserta: com o MySQL do dono fora do ar, a
    // versão anterior enfileirava, devolvia PERMITIDO e a tarefa era recusada
    // depois, na thread escritora, só no log. O admin digitava `dar fulano
    // vip`, lia "pronto", e o jogador reclamava no dia seguinte. Quem chamou
    // precisa saber AGORA que não vai ser gravado — é o que permite responder
    // "o banco esta fora, tente de novo" em vez de mentir.
    if (!m_bancoServe.load(std::memory_order_acquire)) return NAO_SEI;
    // `t.jogador = jogador` constrói um std::string caminhando até o '\0': é o
    // MESMO defeito da consulta, só que no caminho de escrita. Conferir antes.
    if (ComprimentoLimitado(jogador, MAX_ID)    < 0) return NEGADO;
    if (ComprimentoLimitado(grupo,   MAX_GRUPO) < 0) return NEGADO;
    // `quem` é opcional e só vai para o diário: nulo vira "", mas texto sem
    // terminador é recusado — enfileirar isso derrubaria a thread escritora.
    if (quem && ComprimentoLimitado(quem, MAX_TEXTO) < 0) return NEGADO;
    Tarefa t; t.tipo = Tarefa::CONCEDER;
    t.jogador = jogador; t.grupo = grupo; t.quem = quem ? quem : ""; t.expira = expiraEm;
    return Enfileirar(std::move(t)) ? PERMITIDO : NAO_SEI;
}

int32_t Armazem::Revogar(const char* jogador, const char* grupo, const char* quem)
{
    if (!m_rodando.load(std::memory_order_acquire)) return NAO_SEI;
    if (!m_bancoServe.load(std::memory_order_acquire)) return NAO_SEI;
    if (ComprimentoLimitado(jogador, MAX_ID)    < 0) return NEGADO;
    if (ComprimentoLimitado(grupo,   MAX_GRUPO) < 0) return NEGADO;
    if (quem && ComprimentoLimitado(quem, MAX_TEXTO) < 0) return NEGADO;
    Tarefa t; t.tipo = Tarefa::REVOGAR;
    t.jogador = jogador; t.grupo = grupo; t.quem = quem ? quem : "";
    return Enfileirar(std::move(t)) ? PERMITIDO : NAO_SEI;
}

int32_t Armazem::VerJogador(const char* jogador, const char* nome)
{
    if (!m_rodando.load(std::memory_order_acquire)) return NAO_SEI;
    // Este é o caminho que chega em rajada do laço do jogo (ConanPermId ->
    // VerJogador). Com o banco fora, sair aqui evita até o custo de construir
    // as strings da tarefa, 60 vezes por segundo por jogador, para nada.
    if (!m_bancoServe.load(std::memory_order_acquire)) return NAO_SEI;
    if (ComprimentoLimitado(jogador, MAX_ID) < 0) return NEGADO;
    // O nome de exibição vem do jogo (FString já copiada para buffer nosso),
    // mas VerJogador é público no Armazém e o teste chama direto. Teto igual.
    if (nome && ComprimentoLimitado(nome, MAX_TEXTO) < 0) return NEGADO;
    Tarefa t; t.tipo = Tarefa::VISTO;
    t.jogador = jogador; t.nome = nome ? nome : "";
    return Enfileirar(std::move(t)) ? PERMITIDO : NAO_SEI;
}

bool Armazem::Recarregar()
{
    if (!m_rodando.load(std::memory_order_acquire)) return false;
    // Recarregar com o banco fora não é erro de quem pediu: é só cedo demais.
    // Devolver false deixa quem pediu dizer isso, em vez de a recarga sumir.
    if (!m_bancoServe.load(std::memory_order_acquire)) return false;
    Tarefa t; t.tipo = Tarefa::RECARREGAR;
    return Enfileirar(std::move(t));
}

void Armazem::EsperarFila()
{
    // Só o teste usa. No jogo ninguém espera escrita — é o ponto do desenho.
    for (int i = 0; i < 20000; ++i)
    {
        if (m_feitas.load(std::memory_order_acquire) >=
            m_enfileiradas.load(std::memory_order_acquire)) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// ============================================================================
//  ExecutarTarefa — uma tarefa de escrita, do jeito que os dois bancos aceitam
//
//  Devolve false quando a tarefa NÃO foi aplicada. `mexeu` sai true só quando
//  algo mudou de verdade e o instantâneo precisa ser refeito.
// ============================================================================
bool Armazem::ExecutarTarefa(const Tarefa& t, bool& mexeu)
{
    mexeu = false;
    const Sql& S = m_banco->S();

    switch (t.tipo)
    {
    case Tarefa::CONCEDER:
    {
        // ── por que o id do grupo é PERGUNTADO, e não inferido ──────────────
        //
        // A versão anterior fazia `INSERT ... SELECT ... FROM grupo WHERE
        // chave=?` e concluía "grupo nao existe" quando o contador de linhas
        // afetadas vinha 0. No SQLite isso funciona. No MySQL, NÃO: um
        // `ON DUPLICATE KEY UPDATE` que reescreve a linha com os MESMOS valores
        // devolve 0 afetadas — indistinguível de "não achei o grupo".
        //
        // O sintoma seria de boa-fé puro: o admin dá VIP de novo para quem já
        // tem, e o log responde "grupo 'vip' nao existe". Ele vai procurar um
        // defeito que não está lá. Perguntar o id primeiro troca uma inferência
        // por um fato, e vale igual nos dois bancos.
        int64_t gid = -1;
        {
            std::unique_ptr<IComando> c = m_banco->Preparar(S.grupo_id_por_chave_ou_apelido);
            if (!c || !c->LigarTexto(1, t.grupo.c_str())) return false;
            if (!c->Consultar([](const ILinha& l, void* p) { *static_cast<int64_t*>(p) = l.Inteiro(0); },
                              &gid))
                return false;
        }
        if (gid < 0)
        {
            // Não é falha do banco: é comando errado. Devolve true (a tarefa foi
            // tratada) para o reenvio não tentar de novo o que nunca daria certo.
            Registrar("[permission] conceder ignorado: grupo '%s' nao existe "
                      "(nem como chave nem como apelido)", t.grupo.c_str());
            return true;
        }

        // BEGIN: as três escritas (jogador, vínculo, diário) ou valem juntas ou
        // não valem. Se o servidor cair no meio, o banco desfaz o pedaço — não
        // fica jogador criado sem grupo nem diário registrando o que não
        // aconteceu.
        if (!m_banco->Iniciar())
        { Registrar("[permission] conceder: nao abriu transacao: %s", m_banco->Erro()); return false; }

        bool ok = true;
        {
            std::unique_ptr<IComando> c = m_banco->Preparar(S.jogador_garantir);
            ok = c && c->LigarTexto(1, t.jogador.c_str()) && c->Executar();
        }
        if (ok)
        {
            std::unique_ptr<IComando> c = m_banco->Preparar(S.vinculo_upsert);
            ok = c && c->LigarTexto (1, t.jogador.c_str())
                   && c->LigarInteiro(2, gid)
                   && c->LigarInteiro(3, t.expira)
                   && c->LigarInteiro(4, Agora())
                   && c->LigarTexto (5, t.quem.c_str())
                   && c->Executar();
        }
        if (ok)
        {
            char det[256];
            // O reenvio é MARCADO no diário. Sem isto, uma auditoria com duas
            // linhas iguais para a mesma concessão vira mistério; com isto, ela
            // se explica sozinha. Ver Trabalhar() para quando o reenvio acontece.
            std::snprintf(det, sizeof(det), "grupo=%s expira=%lld%s",
                          t.grupo.c_str(), static_cast<long long>(t.expira),
                          t.reenvio ? " (reenviado apos queda da conexao com o banco)" : "");
            std::unique_ptr<IComando> c = m_banco->Preparar(S.diario_inserir);
            ok = c && c->LigarInteiro(1, Agora())
                   && c->LigarTexto (2, t.quem.c_str())
                   && c->LigarTexto (3, "conceder")
                   && c->LigarTexto (4, t.jogador.c_str())
                   && c->LigarTexto (5, det)
                   && c->Executar();
        }

        // COMMIT que falha precisa de ROLLBACK: sem ele a transacao fica
        // ABERTA e todo Iniciar() seguinte morre com "cannot start a
        // transaction within a transaction". Ver o mesmo conserto, e o porque
        // completo, no fim de AplicarConfig.
        if (ok && !m_banco->Confirmar()) ok = false;
        if (!ok) m_banco->Desfazer();   // COMMIT falho deixa a transação ABERTA

        if (!ok)
        {
            Registrar("[permission] conceder falhou (%s -> %s): %s",
                      t.jogador.c_str(), t.grupo.c_str(), m_banco->Erro());
            return false;
        }
        mexeu = true;
        return true;
    }

    case Tarefa::REVOGAR:
    {
        int64_t gid = -1;
        {
            std::unique_ptr<IComando> c = m_banco->Preparar(S.grupo_id_por_chave_ou_apelido);
            if (!c || !c->LigarTexto(1, t.grupo.c_str())) return false;
            if (!c->Consultar([](const ILinha& l, void* p) { *static_cast<int64_t*>(p) = l.Inteiro(0); },
                              &gid))
                return false;
        }
        if (gid < 0)
        {
            Registrar("[permission] revogar ignorado: grupo '%s' nao existe", t.grupo.c_str());
            return true;
        }

        if (!m_banco->Iniciar())
        { Registrar("[permission] revogar: nao abriu transacao: %s", m_banco->Erro()); return false; }

        bool ok = true;
        int64_t apagadas = 0;
        {
            std::unique_ptr<IComando> c = m_banco->Preparar(S.vinculo_apagar);
            ok = c && c->LigarTexto(1, t.jogador.c_str()) && c->LigarInteiro(2, gid)
                   && c->Executar();
            if (ok) apagadas = c->Mudancas();
        }
        if (ok)
        {
            // O detalhe ACRESCENTA a marca do reenvio em vez de SUBSTITUIR o
            // nome do grupo. A versão anterior trocava um pelo outro, e a linha
            // do diário de uma revogação reenviada saía sem dizer qual grupo
            // foi revogado — perdendo justamente o dado que faz o diário
            // existir ("quem tirou o VIP de quem?").
            char det[256];
            std::snprintf(det, sizeof(det), "%s%s", t.grupo.c_str(),
                          t.reenvio ? " (reenviado apos queda da conexao com o banco)" : "");
            std::unique_ptr<IComando> c = m_banco->Preparar(S.diario_inserir);
            ok = c && c->LigarInteiro(1, Agora())
                   && c->LigarTexto (2, t.quem.c_str())
                   && c->LigarTexto (3, "revogar")
                   && c->LigarTexto (4, t.jogador.c_str())
                   && c->LigarTexto (5, det)
                   && c->Executar();
        }

        // COMMIT que falha precisa de ROLLBACK: sem ele a transacao fica
        // ABERTA e todo Iniciar() seguinte morre com "cannot start a
        // transaction within a transaction". Ver o mesmo conserto, e o porque
        // completo, no fim de AplicarConfig.
        if (ok && !m_banco->Confirmar()) ok = false;
        if (!ok) m_banco->Desfazer();   // COMMIT falho deixa a transação ABERTA

        if (!ok)
        {
            Registrar("[permission] revogar falhou (%s -> %s): %s",
                      t.jogador.c_str(), t.grupo.c_str(), m_banco->Erro());
            return false;
        }
        mexeu = apagadas > 0;
        return true;
    }

    case Tarefa::VISTO:
    {
        std::unique_ptr<IComando> c = m_banco->Preparar(S.visto_upsert);
        const bool ok = c && c->LigarTexto (1, t.jogador.c_str())
                          && c->LigarTexto (2, t.nome.c_str())
                          && c->LigarInteiro(3, Agora())
                          && c->Executar();
        if (!ok)
        { Registrar("[permission] visto falhou (%s): %s", t.jogador.c_str(), m_banco->Erro()); return false; }
        // Não reconstrói: "visto" não muda permissão nenhuma. Reconstruir a
        // cada login seria pagar um instantâneo inteiro por dado que só serve
        // para o admin ler nome no lugar de número.
        mexeu = false;
        return true;
    }

    case Tarefa::RECARREGAR:
        if (!m_caminhoJson.empty()) AplicarConfig(m_caminhoJson.c_str());
        // Reconstrói mesmo se a config foi recusada: é barato, e devolver o
        // instantâneo coerente com o que está NO BANCO é sempre certo.
        mexeu = true;
        return true;

    default:
        return true;
    }
}

// ============================================================================
//  CuidarDaConexao — toda a política de "o banco do dono não colabora"
//
//  Roda na thread escritora, uma vez por volta do laço. Nunca no laço do jogo.
//
//  Trata os dois casos com o MESMO mecanismo, porque para o dono do servidor
//  eles são o mesmo problema ("meu MySQL não está atendendo"):
//     · nunca abriu     — senha errada, banco não criado, sem GRANT, MySQL
//                         ainda subindo, porta fechada, host errado;
//     · abriu e caiu    — reinício do banco, KILL de conexão, wait_timeout,
//                         blip de rede.
// ============================================================================
void Armazem::CuidarDaConexao()
{
    // Desligando: não se começa abertura nenhuma. Sem isto, um Fechar() pedido
    // durante o arranque esperaria a abertura inteira — 120 s medidos contra um
    // MySQL a 2 s por comando. (A abertura JÁ EM CURSO é cortada por outro
    // caminho: Fechar chama IBanco::Interromper.)
    if (!m_rodando.load(std::memory_order_acquire)) return;

    const bool temBanco = (m_banco != nullptr);
    const bool vivo     = temBanco && m_banco->Vivo();

    if (vivo && m_bancoServe.load(std::memory_order_acquire))
        return;                                   // está tudo bem; nada a fazer

    const int64_t agora = Agora();

    // Acabou de cair: registra na hora, uma vez, e zera a espera para a
    // primeira tentativa ser rápida (um blip de 3 s tem de se resolver em 5 s,
    // não em 5 minutos).
    //
    // Quem decide se a linha já foi escrita é m_estavaServindo, e NÃO
    // m_bancoServe: a queda quase sempre é descoberta DENTRO de uma tarefa que
    // falhou, e essa tarefa já derrubou m_bancoServe para o laço do jogo parar
    // de aceitar escrita. Usando m_bancoServe aqui, o caso comum — o único que
    // acontece de verdade — passava em silêncio, e o dono só via a mensagem
    // periódica cinco minutos depois, escrita como se ele já soubesse.
    if (!vivo && m_estavaServindo)
    {
        m_estavaServindo = false;
        m_bancoServe.store(false, std::memory_order_release);
        Registrar("[permission] a conexao com o banco (%s) caiu: %s",
                  temBanco ? m_banco->Nome() : "?", temBanco ? m_banco->Erro() : "");
        Registrar("[permission] as CONSULTAS de permissao CONTINUAM sendo respondidas, "
                  "do instantaneo em memoria — dado coerente, do ultimo estado bom. O "
                  "que para e a ESCRITA: conceder/revogar passam a ser recusados na "
                  "hora (quem chamou recebe 'nao sei', em vez de um 'pronto' mentiroso) "
                  "ate o banco voltar.");
        m_esperaSegundos   = 0;
        m_proximaTentativa = 0;
        m_falhasReconexao  = 0;
    }

    if (agora < m_proximaTentativa) return;       // freio: ainda não é a hora

    // Espera crescente. Marcada ANTES de tentar, de propósito: a tentativa em
    // si pode demorar (até MysqlTempoConectarMs), e marcar depois faria a
    // espera real ser "espera + tentativa", que é uma coisa diferente da que
    // está escrita no log.
    m_esperaSegundos = m_esperaSegundos ? std::min(m_esperaSegundos * 2, ESPERA_TETO)
                                        : ESPERA_PRIMEIRA;
    m_proximaTentativa = agora + m_esperaSegundos;

    bool ok = false;
    // ── reconexão barata primeiro, refazer do zero depois ───────────────────
    //
    // Reconectar() reusa o que já foi lido do config e não repete esquema nem
    // configuração: é o certo para o caso comum (o banco piscou). Mas ele NÃO
    // relê o config.json, então uma senha corrigida não valeria nunca. Depois
    // de duas falhas seguidas o banco é solto e a próxima volta refaz tudo a
    // partir do arquivo — que é o caminho que deixa o dono consertar sem
    // reiniciar o servidor de jogo.
    if (temBanco && m_falhasReconexao < 2)
    {
        ok = m_banco->Reconectar();
        if (ok)
        {
            // Reconectar depois de uma queda pode significar que perdemos
            // escritas nossas ou de outro servidor apontado para o mesmo banco;
            // refazer o instantâneo do que está no banco é sempre certo.
            ok = Reconstruir();
        }
        if (!ok)
        {
            ++m_falhasReconexao;
            if (m_falhasReconexao >= 2)
            {
                m_banco->Fechar();
                { std::lock_guard<std::mutex> g(m_mtxBanco); m_banco.reset(); }
                Registrar("[permission] duas reconexoes seguidas falharam. Vou reler o "
                          "config.json e comecar do zero na proxima tentativa — se voce "
                          "corrigiu senha, porta, host ou nome do banco, e agora que "
                          "isso passa a valer.");
            }
        }
    }
    else
    {
        ok = AbrirBanco();       // relê o config.json inteiro
    }

    if (ok)
    {
        m_esperaSegundos   = 0;
        m_proximaTentativa = 0;
        m_falhasReconexao  = 0;
        m_ultimoAvisoConexao = 0;
        m_estavaServindo   = true;
        m_bancoServe.store(true, std::memory_order_release);
        uint64_t descartadas = 0;
        { std::lock_guard<std::mutex> g(m_mtxFila);
          descartadas = m_descartadasDesdeAviso; m_descartadasDesdeAviso = 0; }

        // "DE VOLTA" só se ele já tinha ido embora. Na primeira abertura de
        // todas isto é o arranque normal, e anunciar uma volta que não houve
        // faria o dono procurar uma queda que nunca aconteceu — o mesmo tipo
        // de mensagem enganosa que esta tarefa existe para tirar do caminho.
        if (m_arranqueFeito.load(std::memory_order_acquire))
        {
            Registrar("[permission] BANCO DE VOLTA (%s). O Permission esta respondendo "
                      "normalmente de novo.", m_banco->Nome());
            if (m_recusadasDesdeAviso || descartadas)
                Registrar("[permission] enquanto esteve fora: %llu comando(s) recusado(s) e "
                          "%llu descartado(s) por fila cheia. Eles NAO foram gravados — "
                          "refaca o que ainda fizer sentido.",
                          static_cast<unsigned long long>(m_recusadasDesdeAviso),
                          static_cast<unsigned long long>(descartadas));
        }
        m_recusadasDesdeAviso = 0;
        return;
    }

    m_bancoServe.store(false, std::memory_order_release);

    // ── a PRIMEIRA falha ganha o aviso em moldura ───────────────────────────
    //
    // `!m_arranqueFeito` é exatamente "esta é a primeira tentativa de todas" —
    // quem marca essa bandeira é Trabalhar(), logo depois da primeira chamada
    // desta função. São as linhas que o dono do servidor vai ler e colar num
    // fórum, e elas dizem, nesta ordem: o que NÃO está acontecendo, por que
    // não inventei outro lugar para gravar, e o que vai acontecer sozinho.
    if (!m_arranqueFeito.load(std::memory_order_acquire))
    {
        Registrar("[permission] ############################################################");
        Registrar("[permission] # O BANCO NAO ABRIU — o Permission esta AUSENTE.");
        Registrar("[permission] # O motivo esta na linha logo acima desta moldura.");
        Registrar("[permission] # Nenhuma permissao sera respondida: para os outros plugins");
        Registrar("[permission] # e como se o Permission nao estivesse instalado, e cada um");
        Registrar("[permission] # usa o padrao que ELE escolheu (se_ausente).");
        Registrar("[permission] # NAO gravei em outro lugar. Cair para um banco local seria");
        Registrar("[permission] # criar dois lugares com VIPs diferentes e nenhum jeito de");
        Registrar("[permission] # juntar os dois depois.");
        Registrar("[permission] # Vou tentar de novo sozinho, com espera crescente (5 s, 10 s,");
        Registrar("[permission] # 20 s ... ate 5 min), RELENDO o config.json a cada tentativa.");
        Registrar("[permission] # Corrija o que a linha acima aponta e o Permission entra");
        Registrar("[permission] # sozinho — sem reiniciar o servidor de jogo.");
        Registrar("[permission] ############################################################");
        m_ultimoAvisoConexao = agora;      // o periódico começa a contar daqui
        return;
    }

    // ── aviso periódico ─────────────────────────────────────────────────────
    //
    // Repetir a cada 5 min é obrigatório, não ruído: senha errada e falta de
    // GRANT não se consertam sozinhas, e o dono lê o log HORAS depois — quando
    // o jogador reclama. A essa altura a linha do arranque está soterrada sob
    // o log do jogo, e uma mensagem que ele não encontra é uma mensagem que
    // não existe.
    if (agora - m_ultimoAvisoConexao >= AVISO_A_CADA)
    {
        m_ultimoAvisoConexao = agora;
        uint64_t descartadas = 0;
        { std::lock_guard<std::mutex> g(m_mtxFila);
          descartadas = m_descartadasDesdeAviso; m_descartadasDesdeAviso = 0; }

        Registrar("[permission] o banco continua fora do ar. Ultimo motivo: %s",
                  m_banco ? m_banco->Erro() : "config.json recusado (veja acima)");
        Registrar("[permission] estado: consultas de permissao %s; escrita RECUSADA; "
                  "proxima tentativa em %d s.",
                  Pronto() ? "respondidas do instantaneo em memoria (ultimo estado bom)"
                           : "NAO respondidas — para os outros plugins o Permission esta AUSENTE",
                  m_esperaSegundos);
        if (m_recusadasDesdeAviso || descartadas)
            Registrar("[permission] nos ultimos %lld s: %llu comando(s) recusado(s), "
                      "%llu descartado(s) por fila cheia.",
                      static_cast<long long>(AVISO_A_CADA),
                      static_cast<unsigned long long>(m_recusadasDesdeAviso),
                      static_cast<unsigned long long>(descartadas));
        m_recusadasDesdeAviso = 0;
    }
}

void Armazem::Trabalhar()
{
    // ── a PRIMEIRA abertura acontece aqui, antes de esperar por qualquer coisa
    //
    // É o que tira o custo do banco de dentro do arranque do servidor: Abrir()
    // só espera por este resultado, e no máximo ARRANQUE_ESPERA_MS. Se o banco
    // do dono for lento, o servidor sobe e esta thread continua trabalhando.
    CuidarDaConexao();
    m_arranqueFeito.store(true, std::memory_order_release);

    while (true)
    {
        Tarefa t;
        bool   temTarefa = false;
        {
            std::unique_lock<std::mutex> g(m_mtxFila);

            // Quanto esperar: 60 s no caso bom (é o relógio da faxina de
            // vencidos). Com o banco fora, no máximo até a hora da próxima
            // tentativa — senão a recuperação ficaria refém de alguém mandar um
            // comando, e um servidor ocioso com o MySQL de volta continuaria
            // ausente até o primeiro `dar`.
            int64_t espera = 60;
            if (!m_bancoServe.load(std::memory_order_acquire))
            {
                const int64_t falta = m_proximaTentativa - Agora();
                espera = falta < 1 ? 1 : (falta > 60 ? 60 : falta);
            }
            m_cvFila.wait_for(g, std::chrono::seconds(espera),
                              [&]{ return !m_fila.empty(); });

            if (!m_fila.empty())
            {
                t = std::move(m_fila.front());
                m_fila.pop_front();
                if (t.tipo == Tarefa::VISTO && m_vistosNaFila) --m_vistosNaFila;
                temTarefa = true;
            }
        }

        if (temTarefa && t.tipo == Tarefa::SAIR)
        { m_feitas.fetch_add(1, std::memory_order_release); return; }

        // Antes de qualquer tarefa: o banco está de pé? Isto cobre os dois
        // casos (nunca abriu / caiu depois) e é o único lugar que decide.
        CuidarDaConexao();

        if (!temTarefa)
        {
            if (!m_rodando.load(std::memory_order_acquire)) return;
            // ── faxina de vencidos ──────────────────────────────────────────
            // A leitura JÁ ignora vencido, então o VIP para de valer na hora
            // exata, sem depender desta faxina. Ela existe só para o banco não
            // acumular lixo e para o log/`grupos` ficarem limpos. Ordem
            // importa: primeiro a garantia, depois a arrumação.
            //
            // Com o banco fora do ar a faxina não roda — e isso está certo: ela
            // é arrumação, e arrumação não justifica insistir contra um banco
            // caído.
            if (m_bancoServe.load(std::memory_order_acquire) && m_banco)
            {
                std::unique_ptr<IComando> c = m_banco->Preparar(m_banco->S().faxina_vencidos);
                if (c && c->LigarInteiro(1, Agora()) && c->Executar() && c->Mudancas() > 0)
                {
                    Registrar("[permission] faxina: %lld vinculo(s) vencido(s) removido(s)",
                              static_cast<long long>(c->Mudancas()));
                    Reconstruir();
                }
            }
            continue;
        }

        bool mexeu = false;
        bool ok    = false;
        // ── `tentou` NÃO é decoração: sem ele o freio de 15 s não existe ────
        //
        // Achado na revisão da própria alteração (§19), antes de rodar. O
        // reenvio abaixo disparava com `!ok && !Vivo()`. Com o banco fora do
        // ar a tarefa NEM CHEGA a ser tentada — e mesmo assim `ok` é false e
        // `Vivo()` é false, então o reenvio disparava e chamava Reconectar()
        // direto, POR FORA do freio. Com uma fila de tarefas e um MySQL caído
        // isso vira uma tentativa de conexão atrás da outra, cada uma custando
        // até MysqlTempoConectarMs — exatamente a martelada no banco caído que
        // o freio existe para impedir. Reenvio só faz sentido para tarefa que
        // FOI tentada e pegou a queda no meio.
        bool tentou = false;
        if (m_banco && m_banco->Vivo())
        {
            tentou = true;
            ok = ExecutarTarefa(t, mexeu);
            // A queda pode ter acontecido AGORA, no meio desta tarefa. Publicar
            // na hora impede que o laço do jogo continue aceitando escrita por
            // até 60 s (o tempo até esta thread acordar de novo) com o banco já
            // morto — cada uma dessas viraria um "pronto" que não aconteceu.
            if (!ok && !m_banco->Vivo())
                m_bancoServe.store(false, std::memory_order_release);
        }
        else
        {
            // ── por que isto é CONTADO e não impresso ───────────────────────
            //
            // Uma linha de log por tarefa recusada parece diagnóstico e é uma
            // segunda forma de o banco derrubar o jogo: `VerJogador` chega em
            // rajada do laço do jogo (ConanPermId), e com o MySQL fora do ar
            // isso vira centenas de linhas por segundo no ConanApi.log. Disco
            // cheio derruba o servidor de Conan — e leva o save junto.
            // O total sai no aviso periódico, que tem freio de 5 min.
            ++m_recusadasDesdeAviso;
        }

        // ── reenvio: UMA vez, e só quando a CONEXÃO foi o motivo ────────────
        //
        // POR QUE REENVIAR É SEGURO AQUI, E SÓ AQUI
        // As três tarefas de escrita são idempotentes por construção:
        // conceder é upsert com os mesmos valores, revogar é DELETE de uma
        // linha que já pode ter sumido, visto é upsert. Aplicar duas vezes dá
        // o mesmo estado final. O que NÃO é idempotente é o diário, que é
        // append-only — por isso a linha reenviada vai MARCADA (ver
        // ExecutarTarefa), em vez de o reenvio ser escondido.
        //
        // POR QUE NÃO REENVIAR SEMPRE: se a tarefa falhou por SQL, dado ou
        // configuração, repetir só produz o mesmo erro. `Vivo()` separa as duas
        // causas — é a única pergunta que distingue "o banco sumiu" de "o
        // comando estava errado".
        if (tentou && !ok && !t.reenvio && m_banco && !m_banco->Vivo())
        {
            Registrar("[permission] a conexao com o banco caiu no meio desta tarefa. "
                      "Vou tentar reconectar e reenviar UMA vez.");
            if (m_banco->Reconectar())
            {
                Tarefa r = t; r.reenvio = true;
                ok = ExecutarTarefa(r, mexeu);
                Registrar("[permission] reenvio %s. Se o comando ja tinha sido "
                          "gravado antes da queda, o diario pode ter duas linhas "
                          "para ele (a segunda vem marcada) — o vinculo, nao: ele "
                          "e o mesmo.", ok ? "deu certo" : "tambem falhou");
                if (ok)
                {
                    m_estavaServindo = true;
                    m_bancoServe.store(true, std::memory_order_release);
                }
            }
            else
            {
                Registrar("[permission] nao consegui reconectar: %s", m_banco->Erro());
                // Esta tentativa CONTA para a política de reconexão: sem isso o
                // reenvio seria uma porta lateral que nunca chega ao "solta o
                // banco e relê o config.json", e uma senha corrigida só valeria
                // se, por acaso, nenhuma tarefa estivesse na fila na hora da
                // queda. Duas portas para a mesma decisão divergem.
                ++m_falhasReconexao;
            }
        }

        if (ok && mexeu) Reconstruir();
        m_feitas.fetch_add(1, std::memory_order_release);
    }
}

}   // namespace Perm
