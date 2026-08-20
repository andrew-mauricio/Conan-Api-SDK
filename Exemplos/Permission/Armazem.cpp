// ============================================================================
//  Armazem.cpp — the DECISION (what data to store) + the in-memory snapshot
//
//  THE MEDIUM MOVED OUT OF HERE (2026-08-18)
//  -----------------------------------------
//  This file used to have 161 sqlite3_* calls in its body. It now talks to
//  Perm::IBanco (Banco.h), and there are two implementations: BancoSqlite — the
//  default, and the text below is what explains why it's a good one — and
//  BancoMysql, for the owner who wants to point the plugin at their MySQL by
//  changing one line of config.json.
//
//  What did NOT change, and that's the point: the permission logic. Inheritance
//  flattened once outside the loop, match weight, a specific denial beating a
//  wildcard, the snapshot published by atomic pointer swap, the reader count.
//  None of it knows which database is underneath, and none of it was rewritten.
//
//  WHY SQLITE IS STILL THE DEFAULT, AND NOT A FORMAT OF OUR OWN
//  -----------------------------------------------------------
//  It wasn't a matter of taste. Three facts about this environment decided it:
//
//  1. THE GAME ITSELF ALREADY DOES THIS, HERE, NOW. Conan's save is a SQLite:
//     ConanSandbox/Saved/game_0.db, in WAL mode — the game_0.db-wal and
//     game_0.db-shm files sit beside it with the server up. So: SQLite in WAL,
//     under Wine, inside this process, is already a path PROVED in production
//     by the game's maker. No format of our own comes close to that level of
//     evidence.
//
//  2. A CRASH MID-WRITE. A JSON file rewritten in place loses everything if the
//     server dies halfway; with an atomic rename it loses the last write.
//     SQLite's WAL gives atomic commit and automatic recovery on open: either
//     the whole transaction counted or none of it did, and the file is never
//     left half-written. For "the VIP the player just bought", the difference
//     between those two guarantees is money.
//
//  3. THE WAL LESSON THIS PROJECT ALREADY PAID FOR. Deleting a SQLite without
//     deleting the -wal makes the data COME BACK, and the deletion looks done.
//     That isn't an argument against WAL: it's an argument for documenting it
//     and giving the right command. See README-PERMISSION.md, section "deleting
//     for real". The alternative (a format of our own) has no WAL and has a
//     worse problem: no guarantee at all.
//
//  And the cost? Zero on reads, because reads DON'T COME THROUGH HERE. SQLite
//  is the durable store; the game loop reads from an in-memory snapshot.
//
//  WHAT IS NEVER DONE, UNDER ANY CIRCUMSTANCES
//  -------------------------------------------
//  The game's game_0.db is not touched. Not even to read. Permission's database
//  is its own file, in its own folder. If we corrupted the save, the server
//  owner would lose their world — and there's no VIP permission on earth that
//  pays for that. A SELECT of ours competing with the game's writer also risks
//  locking the save. It stays out.
//
//  JSON WITHOUT A PARSER OF OUR OWN
//  --------------------------------
//  The configuration is still read with SQLite's json1 (json_valid,
//  json_extract, json_each), not with a hand-written parser. A hand-written
//  JSON parser is ~200 lines of new code nobody reviewed, reading a file the
//  server owner edits by hand — which is hostile input by definition.
//
//  What changed is WHERE: json1 used to join directly against the real tables
//  in a single SQL statement, which doesn't carry over to MySQL (json_each is a
//  SQLite table function; MySQL 5.7 has no JSON_TABLE at all). Now json1 runs
//  in an IN-MEMORY SQLite, returns a struct, and the interface does the
//  writing — the same configuration path for both databases. See
//  LerConfigPermissao, in Banco.cpp.
// ============================================================================
#include "Armazem.h"

#include <windows.h>      // for the clock only; see Agora(). The reason is there.
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <ctime>
#include <algorithm>

namespace Perm
{
namespace
{
    // ── the schema moved to another file, and it's worth saying where ───────
    //
    // The schema's text (in BOTH forms: sqlite and mysql) lives in Banco.cpp,
    // because that's the part that depends on the medium. What remains this
    // file's business is what it means:
    //
    // The THREE layers of a group's name, and why there are three:
    //
    //   grupo.id     an integer, immutable, invisible. It's what jogador_grupo
    //                references. Renaming anything NEVER touches it — which is
    //                exactly why renaming loses no data.
    //   grupo.chave  the technical name, what plugins and commands use ("vip").
    //                Renameable: see "era" in permission.json. On a rename the
    //                old key is stored as an ALIAS, so the VIP plugin compiled
    //                asking for "vip" keeps getting it right after the rename
    //                to "premium".
    //   grupo.nome   the label the player sees in chat ("Patrono do Exílio").
    //                Free, referenced by nobody, change it whenever.
    //
    // Without those three layers, "let them change the name" has only two bad
    // outcomes: either the rename breaks the plugins, or nothing is really
    // renamed.

    constexpr int ESQUEMA_VERSAO = 1;

    // ── the clock, and the defect MEASUREMENT found ──────────────────────────
    //
    // The first version of this function was `return std::time(nullptr);`, with
    // a comment of mine claiming it was cheap because "on Windows it reads
    // KUSER_SHARED_DATA, no syscall". The comment was WRONG, and the test
    // running under Wine charged for it:
    //
    //     std::time(nullptr)          32,418.0 ns/op      <<< 32 microseconds
    //     GetTickCount64()                  2.5 ns/op
    //     GetSystemTimeAsFileTime()       201.1 ns/op
    //
    // 32 us per permission read. With 40 players at 60 Hz that's 78% of a core
    // spent asking the time. And worse: EVERYTHING compiled, EVERYTHING passed
    // the 44 behavioural tests. Only the cost test, actually running, told them
    // apart — exactly what "compiling proves nothing" means.
    //
    // The fix is NOT a thread refreshing a cached clock. That version has a
    // silent failure mode: if the thread dies or falls behind, the clock
    // freezes and an expired VIP lasts forever, with no log line.
    //
    // Here the second is DERIVED from the monotonic tick. Even if nothing ever
    // re-anchors, the answer keeps moving, because GetTickCount64 moves on its
    // own. There's no state that can freeze: the worst case is drift between
    // wall clock and tick, which is milliseconds per hour — irrelevant to "the
    // VIP expires today at 8pm".
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
            // FILETIME counts 100 ns since 1601; 11,644,473,600 s separate 1601 from 1970
            const int64_t seg = static_cast<int64_t>(u / 10000000ULL) - 11644473600LL;
            s_seg.store(seg,  std::memory_order_relaxed);
            s_tick.store(t,   std::memory_order_relaxed);
            return seg;
        }
        return base + static_cast<int64_t>((t - ancora) / 1000);
    }

    inline bool Vencido(int64_t expira, int64_t agora)
    { return expira != 0 && expira <= agora; }

    // ── the write queue's caps (INV-ARMAZEM-004) ────────────────────────────
    //
    // THE DEFECT THIS FIXES, and it was taking the game server down
    // -------------------------------------------------------------
    // The queue had no cap. `VerJogador` is called from inside `ConanPermId()`
    // (Permission.cpp), which is a game-loop function: every plugin resolving
    // an identity queues a `seen` every 250 ms per game object. With 40 players
    // and three objects each, that's on the order of hundreds of tasks a second
    // — and SQLite keeps up.
    //
    // The owner's MySQL, not necessarily. At 2 s per query (a bad network, a
    // database on the other side of the country — scenario 8 of this task), the
    // writer thread drains less than one task a second while the queue grows by
    // hundreds. Each task carries four std::strings. That isn't slowness: it's
    // the GAME SERVER's process climbing in memory until it dies, because of a
    // slow database. Exactly what this task forbids.
    //
    // The numbers, and why these ones:
    //   4,096 tasks × ~200 B ≈ 800 KB. A cap small enough never to matter on a
    //   game server and large enough to hold a whole web shop's bulk grant
    //   without refusing anything. Past that it isn't a burst any more: at 2 s
    //   per write, 4,096 pending tasks take over two hours to drain, and
    //   whatever arrived after would be lost anyway — better to refuse now and
    //   SAY SO, which is what Conceder does.
    //
    //   256 `visto` within the 4,096. `visto` is cosmetic (it lets the admin
    //   type a name instead of a number) and it's the ONLY one that arrives in
    //   bursts from the game loop. Keeping it separate guarantees a flood of
    //   `visto` never takes the place of a `conceder`, which is the one worth
    //   money.
    constexpr size_t FILA_TETO       = 4096;
    constexpr size_t FILA_TETO_VISTO = 256;

    // ── growing backoff between open/reconnect attempts ─────────────────────
    //
    // 5 · 10 · 20 · 40 · 80 · 160 · 300 (cap), in seconds. The why of both
    // extremes is in Armazem.h, alongside the m_esperaSegundos field.
    constexpr int ESPERA_PRIMEIRA = 5;
    constexpr int ESPERA_TETO     = 300;

    // How long Abrir() holds the plugin-load thread waiting for the first
    // open. The why of the number — and the measured 120.5 s that motivated it
    // — is in Armazem::Abrir's comment, in Armazem.h.
    constexpr int ARRANQUE_ESPERA_MS = 15000;

    // How often the log repeats that the database is down. Repeating is
    // mandatory and isn't noise: a wrong password doesn't fix itself, and the
    // owner reads the log HOURS later, when a player complains — by which point
    // the startup line is buried under the game's own log.
    constexpr int64_t AVISO_A_CADA = 300;

    // Node matching. Returns the match's weight, or -1 if it doesn't match.
    //
    //   exact       "vip.kit.diario"  against "vip.kit.diario"  -> len+1
    //   wildcard    "vip.kit.*"       against "vip.kit.diario"  -> 8 (prefix)
    //   everything  "*"               against anything          -> 0
    //
    // The weight is the length of what matched, and an exact match gets +1 so
    // it always beats a wildcard of the same length. That's what makes "vip.*"
    // allow and "-vip.teleporte" deny only the teleport, with declaration order
    // not mattering.
    int32_t Casa(const std::string& padrao, const char* no, bool& curinga)
    {
        curinga = false;
        if (padrao.size() == 1 && padrao[0] == '*') { curinga = true; return 0; }
        if (padrao.size() >= 2 && padrao[padrao.size() - 1] == '*')
        {
            // accepts "vip.*" and also "vip*"
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
//  Tabela — a hash with no allocation, for the game loop's lookup
// ============================================================================
uint64_t Tabela::Hash(const char* s)
{
    // 64-bit FNV-1a. Chosen for being trivial to audit and having no state.
    uint64_t h = 1469598103934665603ULL;
    // ── the loop HAS a cap ──────────────────────────────────────────────────
    //
    // It used to be `while (*s)`, unbounded. A key with no terminator carried
    // this loop off the map, on the game's thread, outside the loader's SEH
    // guard — it killed the process, not just the plugin. See
    // ComprimentoLimitado, in Armazem.h.
    //
    // MAX_NO is the largest of the ABI's three caps (id 64, group 64, node
    // 128), and this Tabela indexes all three. No STORED key exceeds MAX_ID
    // (Inserir refuses what doesn't fit in Item::id), so stopping here changes
    // no lookup's result: a key longer than that simply isn't in the table and
    // still comes back as "not found".
    for (int i = 0; i < MAX_NO && s[i]; ++i)
    { h ^= static_cast<unsigned char>(s[i]); h *= 1099511628211ULL; }
    return h;
}

void Tabela::Reservar(size_t n)
{
    itens.clear();
    itens.reserve(n);
    // A maximum load factor of 50%: linear probing degrades fast above that.
    size_t cap = 16;
    while (cap < n * 2) cap <<= 1;
    balde.assign(cap, -1);
}

bool Tabela::Inserir(const char* chave, int32_t valor)
{
    if (!chave) return false;
    // It used to be `std::strlen(chave)`, which walks to the '\0' wherever it
    // may be. Here the cap is MAX_ID because Item::id is MAX_ID: a longer key
    // would be refused just below anyway, so measuring past that would only
    // serve to read memory that may not be ours.
    const int lenLim = ComprimentoLimitado(chave, MAX_ID);
    if (lenLim < 0) return false;
    const size_t len = static_cast<size_t>(lenLim);
    // A key that doesn't fit is REFUSED, never truncated. Truncating would
    // make two different players the same player — one would inherit the
    // other's VIP, silently. Refusing is ugly and visible; truncating is tidy
    // and wrong.
    if (len == 0 || len >= static_cast<size_t>(MAX_ID)) return false;
    if (balde.empty()) Reservar(16);
    if (itens.size() * 2 >= balde.size())
    {
        // rehash: keep the items and rebuild
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
//  lifecycle
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
//  AbrirBanco — from config.json to the first published snapshot
//
//  Called by startup AND by the writer thread (a retry). NEVER by the game
//  loop: with MySQL this waits on the network.
//
//  IT RE-READS THE CONFIG ON EVERY CALL, on purpose (INV-ARMAZEM-003). It's
//  what lets the owner fix the password, the port, the database name or the
//  "Database" line in the file and have the fix take effect on its own —
//  without restarting the game server, which costs 6 to 9 minutes with nobody
//  able to connect.
// ============================================================================
bool Armazem::AbrirBanco()
{
    // ── 1. which database it is: config.json decides ────────────────────────
    //
    // This comes BEFORE opening anything, and it's the only possible order: the
    // file says where to go. A broken config here is a REFUSAL, not "fall back
    // to the default" — see LerConfigBanco in Banco.cpp for why (whoever writes
    // "mysqll" and quietly falls through to sqlite writes the VIPs into a file
    // nobody looks at).
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

    // ── the database is installed BEFORE trying to open, and that's deliberate
    //
    // The only way Fechar() can interrupt an open in progress is for it to be
    // reachable. Without this, a shutdown requested during startup — which is
    // exactly when startup is slow — would wait out the whole open: 120 s
    // measured against a MySQL at 2 s per command.
    //
    // The price is that m_banco now exists in a "created and not opened" state.
    // It's paid here and only here: EVERY failure path below returns m_banco to
    // nullptr, so anyone reading m_banco can still take "exists" to mean "has
    // opened at least once".
    { std::lock_guard<std::mutex> g(m_mtxBanco); m_banco = std::move(novo); }

    if (!m_banco->Abrir())
    {
        // The medium's message already comes fully formed (closed port, a name
        // that doesn't resolve, wrong password, missing database). Passing it
        // through whole is right: shortening it here would throw away the only
        // actionable information.
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
        // An invalid config does NOT take Permission down: the database still
        // has the groups from the last time the JSON was good. Better to run
        // with yesterday's configuration and shout in the log than to have no
        // permissions at all because somebody missed a comma at 3am.
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
    // The guard became the THREAD, not the database: there's now a legitimate
    // state where Armazem is standing and the database hasn't opened yet, and
    // checking m_banco here would let Abrir be called twice in exactly that
    // state — two writer threads over the same queue.
    if (m_thread.joinable()) { Registrar("[permission] Abrir chamado duas vezes; ignorado"); return false; }
    if (!caminhoDb || !*caminhoDb) { Registrar("[permission] caminho do banco vazio"); return false; }

    m_caminhoDb   = caminhoDb;
    m_caminhoJson = caminhoJson ? caminhoJson : "";

    // ── the THREAD opens the database, and Abrir only waits for it — capped ─
    //
    // See Abrir's comment in Armazem.h for the measured number (120.5 s of
    // startup against a MySQL at 2 s per command) and for why 15 s.
    //
    // In the normal case — local sqlite, or a healthy MySQL — this finishes in
    // tens of milliseconds and the behaviour is identical to before: Abrir()
    // returns with Permission already up.
    m_rodando.store(true, std::memory_order_release);
    m_thread = std::thread(&Armazem::Trabalhar, this);

    for (int i = 0; i < ARRANQUE_ESPERA_MS / 5; ++i)
    {
        if (m_arranqueFeito.load(std::memory_order_acquire)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    if (!m_arranqueFeito.load(std::memory_order_acquire))
    {
        // Neither failed nor came up: it's taking a while. Saying so is what
        // separates "the server froze" from "their database is slow and the
        // plugin said so".
        Registrar("[permission] o banco esta demorando mais de %d s para abrir. NAO vou "
                  "segurar o arranque do servidor por causa disso: sigo abrindo em "
                  "segundo plano e o Permission entra sozinho quando terminar. Ate la "
                  "ele responde como AUSENTE.", ARRANQUE_ESPERA_MS / 1000);
        return true;
    }

    // The boxed warning about a database that didn't open does NOT come from
    // here: it comes from CuidarDaConexao, on the thread that found the
    // failure. If it came from here, a database that takes longer than
    // ARRANQUE_ESPERA_MS to fail (slow DNS, say) would fail without ever
    // printing the warning. A message the owner can't find is a message that
    // doesn't exist.
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

        // ── interrupt BEFORE waiting ───────────────────────────────────────
        //
        // The writer thread can be sitting inside a recv() against a MySQL that
        // accepted the connection and then went silent. Without this, the join
        // waits out the database's timeout — msOperar PER COMMAND, and a
        // rebuild is 7 queries: up to ~70 s of server hanging at shutdown with
        // the default timeouts. At that point
        // the owner does what anyone would and kills the process — and killing
        // Conan during shutdown loses the world save. A database problem must
        // not end in a lost save.
        //
        // Interromper() is safe from another thread by contract (Banco.h) and
        // it's final: from here on this database serves no purpose at all,
        // which is exactly what you want from a process that's dying.
        { std::lock_guard<std::mutex> g(m_mtxBanco); if (m_banco) m_banco->Interromper(); }

        if (m_thread.joinable()) m_thread.join();
    }
    m_bancoServe.store(false, std::memory_order_release);
    // Only now are snapshots freed: the writer thread has stopped, and in the
    // game Fechar happens as the process unloads.
    const Instantaneo* a = m_atual.exchange(nullptr, std::memory_order_acq_rel);
    delete a;
    for (const Instantaneo* v : m_aposentados) delete v;
    m_aposentados.clear();
    if (m_banco) { m_banco->Fechar(); m_banco.reset(); }
}

bool Armazem::AplicarEsquema()
{
    // One command at a time, on both media. SQLite would take the whole schema
    // in a single exec; this house's MySQL wouldn't, because
    // CLIENT_MULTI_STATEMENTS is off on purpose (MySqlCliente.h). Running them
    // one by one on both makes the path the same path — and a failure names
    // WHICH command failed, instead of "the schema failed".
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

    // A schema version HIGHER than the one this binary knows: refuse. Running
    // over a database from a future version is the shortest path to deleting
    // data you don't understand.
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
//  configuration: permission.json -> database
//
//  The rule governing this stretch: the JSON RULES the SHAPE (which groups
//  exist, what each one can do, who inherits from whom) and NEVER the
//  OWNERSHIP (who is VIP). Ownership lives only in the database, and nothing
//  here deletes ownership.
//
//  That's what makes "rename without losing what's stored" work: touching the
//  JSON rewrites group permissions freely; jogador_grupo is never touched.
//
//  WHAT CHANGED WITH TWO DATABASES
//  -------------------------------
//  This used to be a handful of big SQL statements, with json_each joining
//  straight against the real tables. json1 still reads the file (now in an
//  in-memory SQLite, see Banco.cpp), but the WRITING became command by command,
//  parameterised — which is the only way the same code serves both.
//
//  THE ORDER IS THE SAME, and it isn't arbitrary: renames BEFORE any upsert.
//  If the upsert ran first, it would create a NEW group under the new key, and
//  the old one — with every VIP inside it — would be orphaned. The symptom
//  would be exactly what the owner asked never to happen: I renamed it and lost
//  the data.
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

    // All or nothing. A half-applied config is the worst possible state: the
    // group exists and its permissions don't.
    if (!m_banco->Iniciar())
    { Registrar("[permission] nao consegui abrir transacao para a config: %s",
                m_banco->Erro()); return false; }

    bool bom = true;
    auto falhar = [&](const char* onde)
    {
        Registrar("[permission] config: %s: %s", onde, m_banco->Erro());
        bom = false;
    };

    // ── 1. explicit renames ("era") ─────────────────────────────────────────
    for (const ConfigGrupo& g : cfg.grupos)
    {
        if (!bom) break;
        if (g.era.empty() || g.era == g.chave) continue;

        // Store the old key as an ALIAS before swapping: it's what keeps the
        // third-party plugin compiled asking for "vip" correct.
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

    // ── 2. the groups ───────────────────────────────────────────────────────
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

    // ── 3. inheritance and permissions: the JSON is the truth, so rewrite ───
    //
    // Delete-and-rewrite ONLY these three tables. jogador_grupo and
    // jogador_permissao stay out — that's where ownership lives.
    if (bom && !m_banco->Executar(m_banco->S().grupo_herda_limpar))       falhar("limpar heranca");
    if (bom)
    {
        // Deduplicate here, and not with INSERT OR IGNORE / INSERT IGNORE:
        // MySQL's IGNORE also swallows foreign-key violations and data
        // truncation, and a freshly emptied table only receives a duplicate if
        // permission.json itself repeats the line. A repetition in the file is
        // a harmless editing mistake — it goes away, with a log line — and a
        // real error still shows up.
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
                // 0 rows = the parent doesn't exist (or is the child itself).
                // This used to be silent, and a mistyped "herda": ["deafult"]
                // became a group with no inheritance at all without a word in
                // the log.
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
                    // same group. The table's key is (group,node) and `nega`
                    // isn't part of it, so only one of the two can exist. The
                    // first wins — as it already did with INSERT OR IGNORE —
                    // but now the owner is told, instead of watching the
                    // denial disappear.
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

    // ── 4. a default group has to exist ─────────────────────────────────────
    //
    // With no default group, every player an admin never touched is a player
    // with NOTHING — and the whole server looks broken without a single error
    // message. Failing loud here is far better.
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

    // ── a COMMIT that fails NEEDS a ROLLBACK ────────────────────────────────
    //
    // Found while reviewing the change itself (§19), not by running it. On
    // sqlite, a COMMIT that fails — a full disk, a database locked by another
    // process — does NOT close the transaction: it stays OPEN. The next
    // Iniciar() then dies with "cannot start a transaction within a
    // transaction", and from there EVERY write the plugin makes fails because
    // of an error that happened once, minutes earlier. The symptom bears no
    // visible relation to the cause.
    if (bom && !m_banco->Confirmar())
    {
        Registrar("[permission] COMMIT da config falhou: %s", m_banco->Erro());
        bom = false;
        // ── DESFAZER É OBRIGATÓRIO AQUI ─────────────────────────────────────
        //
        // A COMMIT that fails does NOT close the transaction: it stays open.
        // And with an open transaction hanging around, EVERY subsequent write
        // fails — granting VIP, revoking, creating a group. The log would blame
        // whichever operation happened to be running, and the real cause (a
        // COMMIT that failed minutes earlier) would appear nowhere.
        //
        // A concrete scenario: the disk fills for a moment, the config's
        // COMMIT fails, the disk frees up — and Permission stays unusable until
        // the server restarts, answering "erro ao gravar" to everything.
        //
        // A ROLLBACK over an already-undone transaction is harmless; leaving
        // it open is not.
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

    // A read that fails ABORTS the rebuild before any publication
    // (INV-BANCO-003): the previous snapshot stays in force. With MySQL that
    // stopped being hypothetical — the connection can drop in the middle of the
    // third query — and the right behaviour is a server with old, correct
    // permissions, never with half of them.
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

    // The group keys, so "does this group exist?" can be answered without
    // going to the database. `porChave` was just built above and holds exactly
    // that.
    novo->idxGrupo.Reservar(porChave.size());
    for (const auto& par : porChave)
    {
        // A key that doesn't fit is a key no lookup would find anyway;
        // ignoring it silently here would create an invisible group, so the log
        // says so.
        if (!novo->idxGrupo.Inserir(par.first.c_str(), int32_t(par.second)) && m_log)
            m_log("[permission] chave de grupo longa demais para o indice; "
                  "'conceder' nesse grupo vai recusar");
    }

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

    // ── aliases, built straight into the table with no lookup allocation ────
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

    // ── flattening a group's inheritance ────────────────────────────────────
    //
    // Done ONCE, here, outside the loop. That's why the in-game read is a hash
    // lookup and not a graph traversal. The depth guard and the visited set
    // exist because circular inheritance ("a inherits from b, b inherits from
    // a") is an ordinary typo in a hand-edited file — and in an unguarded graph
    // that's infinite recursion inside the server's process, meaning a server
    // on the floor because of one comma.
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

    // ── the player who isn't in the database yet ────────────────────────────
    for (const auto& kv : grupos)
        if (kv.second.padrao)
        {
            achatar(kv.first, false, 0, novo->padrao.nos);
            novo->padrao.grupos.push_back(GrupoDoJogador{kv.second.chave, 0});
        }

    // ── the players ─────────────────────────────────────────────────────────
    //
    // Built into a map (allocating here is free: this runs on the writer
    // thread, not in the game loop) and then FLATTENED into a vector + Tabela,
    // which is what lookups use.
    std::unordered_map<std::string, JogadorResolvido> emMontagem;
    {
        // ── why the rows are COLLECTED before being processed ────────────────
        //
        // `achatar` is a lambda with captures (it needs the `grupos` map), and
        // the database interface takes a plain function pointer — on purpose: a
        // std::function on the read path would be a hidden allocation at a
        // boundary both media have to honour identically.
        //
        // Collecting first and processing afterwards costs a temporary vector
        // and solves that without touching the logic. Allocating HERE is free:
        // this runs on the writer thread, never in the game loop. And it has a
        // good side effect: the connection is busy only for as long as the read
        // takes, which matters when there's a MySQL on a slow network at the
        // other end.
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

    // individual exceptions: they beat any group, by how the weight is built
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

    // Every known player also has the default group's rights. Without this,
    // granting somebody VIP would REMOVE from them whatever the default group
    // gave — because they'd stop falling down the "unknown" path. A classic
    // silent defect: the admin promotes the player and the player loses
    // abilities.
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

    // ── flatten into vector + Tabela: this is the shape lookups read ────────
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

    // ── why the old one is NOT freed right now ──────────────────────────────
    //
    // A reader in the game loop may have grabbed the old pointer a nanosecond
    // before this swap and still be inside it. A `delete` here would be a
    // use-after-free inside the server's process — the defect this project
    // forbids above all others, because it doesn't give you an error: it gives
    // you a crash, later, somewhere else, with no trace.
    //
    // The swap already happened above: from here on, EVERY new reader sees the
    // new snapshot. So if the reader counter is zero at this instant, no reader
    // can be holding a retired one — and freeing is certainty, not an
    // estimate.
    //
    // The previous version counted PUBLICATIONS instead of readers, betting
    // that "two swaps take seconds". They take ~4 ms. The server crashed. See
    // Armazem::Leitura's comment, in Armazem.h, for the measurement and the
    // trace.
    //
    // A reader-writer lock would solve it too, at the cost of the game loop
    // possibly waiting on a writer. That isn't negotiable: the loop doesn't
    // wait. The reader counter gives the same guarantee without ever blocking a
    // reader.
    if (velho) m_aposentados.push_back(velho);

    if (m_leitores.load(std::memory_order_acquire) == 0)
    {
        for (const Instantaneo* v : m_aposentados) delete v;
        m_aposentados.clear();
    }
    else if (m_aposentados.size() > 64)
    {
        // A read stuck for a long time with writes coming in bursts. Nothing
        // is freed — piling up memory is bad, a use-after-free is unacceptable
        // — but the log has to say so, because 64 live retired snapshots means
        // some read isn't finishing, and that's a defect somewhere else.
        Registrar("[permission] ATENCAO: %zu instantaneos aposentados e %d leitor(es) "
                  "ativo(s). Nada foi liberado (de proposito). Se este numero cresce "
                  "sem parar, alguma leitura de permissao nao esta terminando.",
                  m_aposentados.size(), m_leitores.load(std::memory_order_relaxed));
    }
}

uint64_t Armazem::Geracao() const
{ const Instantaneo* a = Atual(); return a ? a->geracao : 0; }

// ============================================================================
//  reading — what runs in the game loop
// ============================================================================
int32_t Armazem::Tem(const char* jogador, const char* no) const
{
    const Leitura lida(*this);
    const Instantaneo* s = lida.get();
    if (!s) return NAO_SEI;
    // ABI input: it checks the TERMINATOR, not just null/empty. See
    // ComprimentoLimitado in Armazem.h for the defect and for what the defence
    // doesn't cover. Past this point, `no` can be walked by
    // `std::string::operator==` inside Casa() without taking the process with
    // it.
    if (ComprimentoLimitado(jogador, MAX_ID) < 0) return NAO_SEI;
    if (ComprimentoLimitado(no,      MAX_NO) < 0) return NAO_SEI;

    // node alias: rename a permission without invalidating a compiled plugin.
    // Returns a pointer into the snapshot — no copy, no allocation.
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
        // weight: individual >> match length >> denial breaks the tie
        const int32_t peso = (n.individual ? (1 << 24) : 0) + len * 2 + (n.nega ? 1 : 0);
        if (peso > melhorPeso) { melhorPeso = peso; melhorNega = n.nega; }
    }
    if (melhorPeso < 0) return NEGADO;      // nada casou: negado por omissão
    return melhorNega ? NEGADO : PERMITIDO;
}

// ── este grupo existe? ──────────────────────────────────────────────────────
//
// Queries the snapshot without touching the database: it's called from inside
// `Conceder`, which can come from the game loop.
//
// It resolves the ALIAS before looking. A plugin compiled when the group was
// called "vip" keeps saying "vip" after it becomes "patrono"; refusing there
// would break exactly what the alias mechanism exists to protect.
int32_t Armazem::GrupoExiste(const char* grupo) const
{
    if (ComprimentoLimitado(grupo, MAX_GRUPO) < 0) return NEGADO;
    const Leitura lida(*this);
    const Instantaneo* s = lida.get();
    if (!s) return NAO_SEI;
    return s->idxGrupo.Achar(s->GrupoAtual(grupo)) >= 0 ? PERMITIDO : NEGADO;
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

    // Grupos() allocates (it builds a std::string) and it's the ONLY lookup
    // that does. Acceptable because it exists for the chat command and for the
    // log — not for the game loop. Anyone calling this per tick is using it
    // wrong, and the documentation says so.
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
//  writing — a queue plus a thread of its own
// ============================================================================
// ── enfileirar com teto ──────────────────────────────────────────────────────
//
// Runs in the GAME LOOP. Everything here is O(1) and under the lock for
// microseconds: no walking the queue (see why at the m_vistosNaFila field, in
// Armazem.h).
//
// false = recusada por teto. Quem chamou traduz isso para o retorno certo; a
// the log message belongs to the writer thread, which is the one that can have
// a repetition brake without costing the tick anything.
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
    // ── the database is down: NAO_SEI, not PERMITIDO ────────────────────────
    //
    // THE GOOD-FAITH DEFECT this fixes: with the owner's MySQL down, the
    // previous version queued, returned PERMITIDO, and the task was refused
    // later on the writer thread, in the log only. The admin typed
    // `dar fulano vip`, read "pronto", and the player complained the next day.
    // The caller needs to know NOW that it won't be written — that's what lets
    // them answer "o banco esta fora, tente de novo" instead of lying.
    if (!m_bancoServe.load(std::memory_order_acquire)) return NAO_SEI;
    // `t.jogador = jogador` builds a std::string by walking to the '\0': the
    // SAME defect as on the lookup path, only on the write path. Check
    // first.
    if (ComprimentoLimitado(jogador, MAX_ID)    < 0) return NEGADO;
    if (ComprimentoLimitado(grupo,   MAX_GRUPO) < 0) return NEGADO;
    // `quem` is optional and only goes into the audit log: null becomes "",
    // but unterminated text is refused — queuing that would take the writer
    // thread down.
    if (quem && ComprimentoLimitado(quem, MAX_TEXTO) < 0) return NEGADO;

    // ── THE SAME GOOD-FAITH DEFECT, from the other cause ────────────────────
    //
    // The comment above fixes "the database is down". Its sibling was missing:
    // the group DOESN'T EXIST. The task went into the queue, the writer thread
    // found out, wrote "conceder ignorado: grupo 'x' nao existe" to the log —
    // and the caller had already been handed PERMITIDO.
    //
    // Seen happening on 2026-08-20: `grupo <jogador> naoexiste` through
    // ConanShop's queue answered "ok". The database did the right thing
    // (nothing was written); it's the ANSWER that lied. An admin who mistypes a
    // group name reads "ok" and walks away.
    //
    // The public header always promised `0 refused (group doesn't exist,
    // invalid id)`. Now it keeps that promise.
    //
    // NAO_SEI while the snapshot hasn't come up: there's no asserting the group
    // doesn't exist then, and NEGADO would swap one mistake for another.
    {
        const int32_t existe = GrupoExiste(grupo);
        if (existe == NEGADO)  return NEGADO;
        if (existe == NAO_SEI) return NAO_SEI;
    }

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
    // This is the path that arrives in bursts from the game loop (ConanPermId
    // -> VerJogador). With the database down, leaving here avoids even the cost
    // of building
    // as strings da tarefa, 60 vezes por segundo por jogador, para nada.
    if (!m_bancoServe.load(std::memory_order_acquire)) return NAO_SEI;
    if (ComprimentoLimitado(jogador, MAX_ID) < 0) return NEGADO;
    // The display name comes from the game (an FString already copied into a
    // buffer of ours), but VerJogador is public on Armazem and the test calls
    // it directly. Same cap.
    if (nome && ComprimentoLimitado(nome, MAX_TEXTO) < 0) return NEGADO;
    Tarefa t; t.tipo = Tarefa::VISTO;
    t.jogador = jogador; t.nome = nome ? nome : "";
    return Enfileirar(std::move(t)) ? PERMITIDO : NAO_SEI;
}

bool Armazem::Recarregar()
{
    if (!m_rodando.load(std::memory_order_acquire)) return false;
    // Reloading with the database down isn't the requester's mistake: it's
    // just too early.
    // Devolver false deixa quem pediu dizer isso, em vez de a recarga sumir.
    if (!m_bancoServe.load(std::memory_order_acquire)) return false;
    Tarefa t; t.tipo = Tarefa::RECARREGAR;
    return Enfileirar(std::move(t));
}

void Armazem::EsperarFila()
{
    // Only the test uses this. In the game nobody waits on a write — that's
    // the whole point of the design.
    for (int i = 0; i < 20000; ++i)
    {
        if (m_feitas.load(std::memory_order_acquire) >=
            m_enfileiradas.load(std::memory_order_acquire)) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// ============================================================================
//  ExecutarTarefa — one write task, in the form both databases accept
//
//  Returns false when the task was NOT applied. `mexeu` comes out true only
//  when something genuinely changed and the snapshot needs rebuilding.
// ============================================================================
bool Armazem::ExecutarTarefa(const Tarefa& t, bool& mexeu)
{
    mexeu = false;
    const Sql& S = m_banco->S();

    switch (t.tipo)
    {
    case Tarefa::CONCEDER:
    {
        // ── why the group's id is ASKED FOR, not inferred ───────────────────
        //
        // The previous version did `INSERT ... SELECT ... FROM grupo WHERE
        // chave=?` and concluded "the group doesn't exist" when the affected-row
        // count came back 0. On SQLite that works. On MySQL it does NOT: an
        // `ON DUPLICATE KEY UPDATE` that rewrites the row with the SAME values
        // returns 0 affected — indistinguishable from "I didn't find the
        // group".
        //
        // The symptom would be pure good faith: the admin grants VIP again to
        // somebody who already has it, and the log answers
        // "grupo 'vip' nao existe". They go hunting for a defect that isn't
        // there. Asking for the id first trades an inference for a fact, and it
        // holds the same on both databases.
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
            // Not a database failure: a wrong command. It returns true (the
            // task was handled) so the resend doesn't retry what could never
            // work.
            Registrar("[permission] conceder ignorado: grupo '%s' nao existe "
                      "(nem como chave nem como apelido)", t.grupo.c_str());
            return true;
        }

        // BEGIN: the three writes (player, link, audit row) either all count
        // or none do. If the server dies halfway, the database undoes the
        // fragment — there's no player created without a group and no audit row
        // recording what
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
            // The resend is MARKED in the audit log. Without this, an audit
            // finding two identical rows for the same grant becomes a mystery;
            // with it, it explains itself. See Trabalhar() for when a resend
            // happens.
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
            // The detail field APPENDS the resend marker instead of REPLACING
            // the group's name. The previous version swapped one for the other,
            // and a resent revocation's audit row came out without saying which
            // group was revoked — losing exactly the datum that makes the audit
            // log worth having ("who took whose VIP away?").
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
        // No rebuild: "seen" changes no permission at all. Rebuilding on every
        // login would mean paying for a whole snapshot for data that only lets
        // the admin read a name instead of a number.
        mexeu = false;
        return true;
    }

    case Tarefa::RECARREGAR:
        if (!m_caminhoJson.empty()) AplicarConfig(m_caminhoJson.c_str());
        // It rebuilds even if the config was refused: it's cheap, and handing
        // back a snapshot coherent with what's IN THE DATABASE is always
        // right.
        mexeu = true;
        return true;

    default:
        return true;
    }
}

// ============================================================================
//  CuidarDaConexao — the whole "the owner's database won't cooperate" policy
//
//  Runs on the writer thread, once per pass. Never in the game loop.
//
//  It handles both cases with the SAME mechanism, because to the server owner
//  they are the same problem ("my MySQL isn't answering"):
//     · never opened      — wrong password, database not created, no GRANT,
//                           MySQL still coming up, closed port, wrong host;
//     · opened and dropped — a database restart, a KILLed connection,
//                           wait_timeout, a network blip.
// ============================================================================
void Armazem::CuidarDaConexao()
{
    // Shutting down: don't start any opening at all. Without this, a Fechar()
    // asked for during startup would wait out the whole open — 120 s measured
    // against a MySQL at 2 s per command. (An open ALREADY IN FLIGHT is cut off
    // by another route: Fechar calls IBanco::Interromper.)
    if (!m_rodando.load(std::memory_order_acquire)) return;

    const bool temBanco = (m_banco != nullptr);
    const bool vivo     = temBanco && m_banco->Vivo();

    if (vivo && m_bancoServe.load(std::memory_order_acquire))
        return;                                   // está tudo bem; nada a fazer

    const int64_t agora = Agora();

    // It just dropped: log it right away, once, and reset the backoff so the
    // first retry is quick (a 3-second blip has to clear in 5 s, not in 5
    // minutes).
    //
    // What decides whether the line has already been written is
    // m_estavaServindo, NOT m_bancoServe: the drop is almost always discovered
    // INSIDE a task that failed, and that task has already knocked
    // m_bancoServe down so the game loop stops accepting writes. Using
    // m_bancoServe here, the common case — the only one that really happens —
    // passed in silence, and the owner only saw the periodic message five
    // minutes later, written as if they already knew.
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

    // Growing backoff. Stamped BEFORE trying, on purpose: the attempt itself
    // can take a while (up to MysqlTempoConectarMs), and stamping afterwards
    // would make the real wait "backoff + attempt", which is a different thing
    // from what the log says.
    m_esperaSegundos = m_esperaSegundos ? std::min(m_esperaSegundos * 2, ESPERA_TETO)
                                        : ESPERA_PRIMEIRA;
    m_proximaTentativa = agora + m_esperaSegundos;

    bool ok = false;
    // ── the cheap reconnect first, the full rebuild after ───────────────────
    //
    // Reconectar() reuses what was already read from the config and repeats
    // neither the schema nor the configuration: that's right for the common
    // case (the database blinked). But it does NOT re-read config.json, so a
    // corrected password would never take. After two failures in a row the
    // database is dropped and the next pass rebuilds everything from the file —
    // which is the path that lets the owner fix things without restarting the
    // game server.
    if (temBanco && m_falhasReconexao < 2)
    {
        ok = m_banco->Reconectar();
        if (ok)
        {
            // Reconnecting after a drop can mean we lost writes of our own, or
            // of another server pointed at the same database; rebuilding the
            // snapshot from what's in the database is always right.
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

        // "BACK" only if it had actually gone away. On the very first open
        // this is ordinary startup, and announcing a return that never happened
        // would send the owner hunting for a drop that never occurred — the
        // same kind of misleading message this task exists to get out of the
        // way.
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

    // ── the FIRST failure gets the boxed warning ────────────────────────────
    //
    // `!m_arranqueFeito` is exactly "this is the very first attempt" — the flag
    // is set by Trabalhar(), right after this function's first call. These are
    // the lines the server owner will read and paste into a forum, and they
    // say, in this order: what is NOT happening, why we didn't invent somewhere
    // else to write, and what will happen on its own.
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

    // ── the periodic warning ────────────────────────────────────────────────
    //
    // Repeating every 5 min is mandatory, not noise: a wrong password and a
    // missing GRANT don't fix themselves, and the owner reads the log HOURS
    // later — when a player complains. By then the startup line is buried under
    // the game's own log, and a message they can't find is a message that
    // doesn't exist.
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
    // ── the FIRST open happens here, before waiting on anything ─────────────
    //
    // This is what takes the database's cost out of the server's startup:
    // Abrir() only waits for this result, and for at most ARRANQUE_ESPERA_MS.
    // If the owner's database is slow, the server comes up and this thread
    // keeps working.
    CuidarDaConexao();
    m_arranqueFeito.store(true, std::memory_order_release);

    while (true)
    {
        Tarefa t;
        bool   temTarefa = false;
        {
            std::unique_lock<std::mutex> g(m_mtxFila);

            // How long to wait: 60 s in the good case (that's the clock for
            // sweeping expired entries). With the database down, at most until
            // the next attempt is due — otherwise recovery would be hostage to
            // somebody sending a command, and an idle server whose MySQL came
            // back would stay absent until the first `dar`.
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

        // Before any task: is the database up? This covers both cases (never
        // opened / dropped later) and it's the only place that decides.
        CuidarDaConexao();

        if (!temTarefa)
        {
            if (!m_rodando.load(std::memory_order_acquire)) return;
            // ── sweeping expired entries ────────────────────────────────────
            // Reads ALREADY ignore anything expired, so a VIP stops applying at
            // the exact moment it should, without depending on this sweep. It
            // exists only to keep the database from piling up junk and to keep
            // the log and `grupos` clean. Order matters: the guarantee first,
            // the tidying after.
            //
            // With the database down the sweep doesn't run — and that's right:
            // it's tidying, and tidying doesn't justify hammering a database
            // that's already down.
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
        // ── `tentou` is NOT decoration: without it the 15 s brake doesn't
        //    exist
        //
        // Found while reviewing the change itself (§19), before running it. The
        // resend below fired on `!ok && !Vivo()`. With the database down the
        // task NEVER EVEN GETS tried — and `ok` is still false and `Vivo()` is
        // still false, so the resend fired and called Reconectar() directly,
        // OUTSIDE the brake. With a queue of tasks and a dead MySQL that
        // becomes one connection attempt after another, each costing up to
        // MysqlTempoConectarMs — exactly the hammering of a dead database the
        // brake exists to prevent. A resend only makes sense for a task that
        // WAS tried and caught the drop mid-way.
        bool tentou = false;
        if (m_banco && m_banco->Vivo())
        {
            tentou = true;
            ok = ExecutarTarefa(t, mexeu);
            // The drop may have happened RIGHT NOW, in the middle of this
            // task. Publishing immediately stops the game loop from carrying on
            // accepting writes for up to 60 s (the time until this thread wakes
            // again) against a database that's already dead — every one of
            // those would become a "pronto" that never happened.
            if (!ok && !m_banco->Vivo())
                m_bancoServe.store(false, std::memory_order_release);
        }
        else
        {
            // ── why this is COUNTED and not printed ─────────────────────────
            //
            // One log line per refused task looks like diagnostics and is a
            // second way for the database to take the game down: `VerJogador`
            // arrives in bursts from the game loop (ConanPermId), and with
            // MySQL down that becomes hundreds of lines a second in
            // ConanApi.log. A full disk takes the Conan server down — and takes
            // the save with it. The total goes out in the periodic warning,
            // which has a 5-minute brake.
            ++m_recusadasDesdeAviso;
        }

        // ── resend: ONCE, and only when the CONNECTION was the reason ───────
        //
        // WHY RESENDING IS SAFE HERE, AND ONLY HERE
        // The three write tasks are idempotent by construction: conceder is an
        // upsert with the same values, revogar is a DELETE of a row that may
        // already be gone, visto is an upsert. Applying them twice gives the
        // same final state. What is NOT idempotent is the audit log, which is
        // append-only — so the resent row goes in MARKED (see ExecutarTarefa),
        // rather than the resend being hidden.
        //
        // WHY NOT ALWAYS RESEND: if the task failed on SQL, on data or on
        // configuration, repeating it just produces the same error. `Vivo()`
        // separates the two causes — it's the only question that tells "the
        // database went away" from "the command was wrong".
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
                // This attempt COUNTS towards the reconnection policy:
                // without that, the resend would be a side door that never
                // reaches "drop the database and re-read config.json", and a
                // corrected password would only take if, by luck, no task
                // happened to be queued when the drop occurred. Two doors to
                // the same decision drift apart.
                ++m_falhasReconexao;
            }
        }

        if (ok && mexeu) Reconstruir();
        m_feitas.fetch_add(1, std::memory_order_release);
    }
}

}   // namespace Perm
