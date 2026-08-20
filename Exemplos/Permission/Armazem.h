// ============================================================================
//  Armazem.h — Permission's data core
//
//  IT DELIBERATELY DOES NOT DEPEND ON ConanApi.
//  Not on the game's <windows.h>, not on reflection, not on UObject. Only
//  SQLite and the standard library. That isn't architectural purity: it's what
//  lets this same code compile into a test .exe and RUN the test under Wine,
//  with no Conan server at all. A core that only runs inside the game is a core
//  that can only be tested in production — and the rule here is the opposite:
//  compiling proves nothing, only running proves.
//
//  THE SPLIT THAT GOVERNS EVERYTHING
//  ---------------------------------
//  READING happens in the game loop. It may not open a file, may not allocate,
//  may not take a lock a writer holds, may not touch SQLite. It answers from an
//  in-memory SNAPSHOT, already resolved (group inheritance flattened),
//  published by an atomic pointer swap.
//
//  WRITING happens outside the loop, on a thread of its own, and ends in a
//  database COMMIT. Whoever calls `Conceder` returns immediately: the task was
//  queued. After the commit, a new snapshot is built and published. No reader
//  ever waits on any writer, at any moment.
//
//  TWO DATABASES, ONE PIECE OF LOGIC (2026-08-18)
//  ----------------------------------------------
//  The "database" stopped being SQLite directly and became Perm::IBanco
//  (Banco.h): BancoSqlite (the default, a local file in WAL) or BancoMysql (the
//  owner points at a MySQL in config.json). The decision — WHAT data to store,
//  how to flatten inheritance, how to weigh a node match — doesn't know which
//  of the two is underneath.
//
//  And the split above got MORE important, not less: with MySQL, "writing
//  outside the loop" stopped being a matter of taste and became the only thing
//  standing between a slow database and a frozen game server. No permission
//  query touches the database — not even the local SQLite.
//
//  ╔══════════════════════════════════════════════════════════════════════════╗
//  ║  WHAT HAPPENS WHEN THE OWNER'S DATABASE WON'T COOPERATE (2026-08-18)     ║
//  ║                                                                          ║
//  ║  This runs on OTHER PEOPLE'S servers, configured by people who don't     ║
//  ║  write code. MySQL on the wrong host, a closed port, a changed           ║
//  ║  password, a database that doesn't exist, no GRANT, dying in the middle  ║
//  ║  of the night, 2 s per query — these aren't hypotheticals, they're a     ║
//  ║  normal week for anyone hosting a server.                                ║
//  ║                                                                          ║
//  ║  INV-ARMAZEM-001 · THE GAME SERVER MUST NOT CRASH OR FREEZE BECAUSE OF   ║
//  ║      THE DATABASE. A MySQL that's down is the database's problem. Full   ║
//  ║      stop. The layers: reads only from the snapshot · writes on their    ║
//  ║      own thread · a capped queue · a timeout on every network operation  ║
//  ║      · a shutdown that interrupts a stuck operation.                     ║
//  ║                                                                          ║
//  ║  INV-ARMAZEM-002 · DATABASE UNAVAILABLE = PERMISSION ABSENT, NEVER       ║
//  ║      PERMISSION LYING. While the database chosen in config.json has      ║
//  ║      never opened, ConanPermissionObterApi returns nullptr: to a caller  ║
//  ║      that's the same as the plugin not being installed, and              ║
//  ║      ConanPermission.h already makes every plugin declare its own        ║
//  ║      `se_ausente`. No falling back to the local SQLite (INV-BANCO-006,   ║
//  ║      in Banco.h): writing VIP somewhere else creates two irreconcilable  ║
//  ║      truths.                                                             ║
//  ║                                                                          ║
//  ║  INV-ARMAZEM-003 · A FAILURE AT STARTUP IS NOT FINAL. The plugin stays   ║
//  ║      LOADED and tries again in the background, with growing backoff and  ║
//  ║      RE-READING config.json on every attempt. When the owner fixes the   ║
//  ║      password, creates the database, issues the GRANT, or the MySQL      ║
//  ║      container finishes coming up, Permission joins on its own.          ║
//  ║                                                                          ║
//  ║      WHY THIS ISN'T A LUXURY: in this game, every server restart opens   ║
//  ║      a 6-to-9-minute window in which NOBODY can connect (measured). Ask  ║
//  ║      "restart the server" as the price of a corrected password and       ║
//  ║      that's far too expensive for a typo — and the most common case of   ║
//  ║      all (docker compose bringing the game up before MySQL) fixes        ║
//  ║      itself in seconds.                                                  ║
//  ║                                                                          ║
//  ║  INV-ARMAZEM-004 · A DATABASE THAT'S DOWN DOESN'T TURN INTO UNBOUNDED    ║
//  ║      MEMORY OR DISK. The write queue is capped and the failure log has   ║
//  ║      a brake. Without that, a slow database takes the server down by     ║
//  ║      consumption — the database's problem becoming the game's problem    ║
//  ║      by another route.                                                   ║
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
    // Every query has THREE answers. See ConanPermission.h for why.
    enum Resposta : int32_t { NAO_SEI = -1, NEGADO = 0, PERMITIDO = 1 };

    // Must match CONAN_PERM_MAX_ID from the public header. Repeated here
    // instead of included because this file deliberately doesn't depend on the
    // public header (the test compiles without it); Permission.cpp holds the
    // static_assert that guarantees the two copies haven't drifted — two copies
    // of the same truth drift if nobody checks.
    constexpr int MAX_ID = 64;

    // The other two caps, for the same reason and with the same static_assert
    // in Permission.cpp: CONAN_PERM_MAX_GRUPO and CONAN_PERM_MAX_NO.
    constexpr int MAX_GRUPO = 64;
    constexpr int MAX_NO    = 128;

    // Free text that only goes into the audit log (who granted it, the
    // player's display name). It's a key to nothing; the cap exists only so
    // there's no ABI input without one.
    constexpr int MAX_TEXTO = 256;

    // ── ABI input: a `const char*` with NO length ────────────────────────────
    //
    // THE DEFECT THIS FIXES
    // The ABI is C and passes only the pointer (ConanPermission.h). Every input
    // checked `!s || !*s` — which catches a null pointer and an empty string —
    // and THEN handed the string to code that walks to the '\0':
    // `Tabela::Hash` (`while (*s)`), `std::string::operator==(const char*)` in
    // `Casa`, and `t.jogador = jogador` in `Conceder`. A badly written plugin
    // (it needn't be malicious) passing bytes with no terminator carried that
    // loop off the map, and the fault happens on the game's thread, OUTSIDE the
    // loader's SEH guard: it kills the process, not just the plugin.
    //
    // The defence is the cap. Every stored key is shorter than the cap (Inserir
    // refuses what doesn't fit), so stopping the read there changes no answer
    // for valid input — and it turns "read until I find a zero, wherever it may
    // be" into "read at most N bytes and refuse".
    //
    // What this does **NOT** cover, and can't without changing the ABI:
    //   · an invalid pointer at the very first byte. To read anything from a
    //     `const char*` you have to dereference it; there's no defence here;
    //   · an unterminated string starting less than N bytes from the end of a
    //     mapped region — the bounded read still crosses the edge.
    // Covering both would take a VirtualQuery per call (2,551 ns measured under
    // Wine, against 145 ns for the whole query: 17x more expensive in the game
    // loop) or length-carrying variants in the ABI. It's written down rather
    // than promised.
    //
    // Returns the length when there's a '\0' within `teto` bytes; -1 when there
    // isn't (and then nobody touches the rest), when the pointer is null, or
    // when the string is empty — empty was never a valid key here.
    inline int ComprimentoLimitado(const char* s, int teto)
    {
        if (!s || teto <= 0) return -1;
        for (int i = 0; i < teto; ++i)
            if (!s[i]) return i > 0 ? i : -1;
        return -1;
    }

    // ── a hash table with no allocation on lookup ────────────────────────────
    //
    // It exists because `std::unordered_map<std::string,...>::find(const char*)`
    // CONSTRUCTS a std::string on every lookup, and a platform id is 17
    // characters — past libstdc++'s 15-char SSO, so off to malloc. Measured
    // under Wine: 145 ns per lookup, with a malloc/free inside. malloc in the
    // game loop, 60 times a second per player, to answer a question that is
    // just a string comparison.
    //
    // Open addressing, key stored inline (no pointer out), linear probing. Zero
    // allocation, zero indirection.
    struct Tabela
    {
        struct Item { char id[MAX_ID]; int32_t valor; };
        std::vector<int32_t> balde;      // -1 = empty; otherwise an index into `itens`
        std::vector<Item>    itens;

        void    Reservar(size_t n);
        bool    Inserir(const char* chave, int32_t valor);  // false if the key doesn't fit
        int32_t Achar(const char* chave) const;             // -1 if not found
        static uint64_t Hash(const char* s);
    };

    // ── a permission node already resolved for one player ────────────────────
    struct NoResolvido
    {
        std::string padrao;      // "vip.kit.diario", "vip.*" or "*"
        int32_t     casaLen;     // the match's weight; see Decidir()
        bool        curinga;
        bool        nega;        // an explicit denial ("-vip.teleporte")
        bool        individual;  // came from jogador_permissao, not from a group
        int64_t     expira;      // 0 = never; inherits the earliest in the chain
    };

    struct GrupoDoJogador
    {
        std::string chave;
        int64_t     expira;      // 0 = never
    };

    struct JogadorResolvido
    {
        std::vector<NoResolvido>    nos;
        std::vector<GrupoDoJogador> grupos;
    };

    // ── the published snapshot ───────────────────────────────────────────────
    //
    // Immutable once published. You never modify a live snapshot: you build
    // another and swap the pointer. A reader that already grabbed the old
    // pointer keeps reading coherent, stale data for a few milliseconds — and
    // coherent stale data is infinitely better than data being modified
    // underneath it.
    struct Instantaneo
    {
        std::vector<JogadorResolvido> jogadores;
        Tabela                        idxJogador;      // id -> index into `jogadores`

        // Someone who never showed up in the database has rights too: those of
        // whichever group(s) are marked as default. Without this, every
        // player's first login would be a player with nothing — and the
        // third-party plugin would see "denied" where the configuration says
        // "allowed".
        JogadorResolvido padrao;

        // ── the groups that EXIST ───────────────────────────────────────────
        //
        // Keys only; the value is the id, which nobody here uses. It serves a
        // question Armazem couldn't answer without going to the database: *does
        // this group exist?*
        //
        // It arrived on 2026-08-20 because of a good-faith defect. `Conceder`
        // queued without looking, and validation only happened on the writer
        // thread, which logged "conceder ignorado: grupo 'x' nao existe" and
        // carried on. The caller got PERMITIDO — because PERMITIDO meant "I
        // accepted it into the queue", not "I wrote it". The public header,
        // though, promises `0 refused (group doesn't exist, invalid id)`, and
        // that was never the return value. An admin would grant VIP on a
        // mistyped group name, read "ok" and walk away.
        Tabela                   idxGrupo;

        // group alias -> current key. It's what keeps a RENAME from breaking an
        // already-compiled plugin that says "vip".
        Tabela                   idxApelidoGrupo;
        std::vector<std::string> apelidoGrupoPara;
        // permission-node alias -> current node
        Tabela                   idxApelidoNo;
        std::vector<std::string> apelidoNoPara;

        uint64_t geracao = 0;

        // Translates alias -> current with no allocation. Returns `chave`
        // itself when there's no alias, and a pointer into the snapshot when
        // there is — the snapshot is immutable and outlives the query.
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

        // Opens (or creates) the database, applies the schema, reads the
        // configuration JSON, builds the first snapshot and starts the writer
        // thread. Returns false WITHOUT bringing anything up if any step fails
        // — a half-standing Permission is worse than an absent one, because the
        // third-party plugin believes the "denied" answer is the truth.
        //
        // `caminhoDb` is still whatever ConanApi handed over
        // (CaminhoDados("Permission","permission.db")). It's the DEFAULT: if
        // config.json carries "DbPathOverride", the config's path wins; if it
        // carries "Database": "mysql", the path isn't used at all. The
        // signature didn't change on purpose — Permission.cpp needn't know
        // MySQL exists.
        //
        // WHERE THIS IS CALLED: at plugin load, NOT in the game loop. With
        // MySQL this call waits on the network: up to MysqlTempoConectarMs
        // (5 s by default) to connect, plus the schema. That's server startup
        // time, not tick time — and it's bounded on purpose.
        //
        // ── HOW LONG THIS HOLDS UP STARTUP, AND WHY IT HAS A CAP ────────────
        //
        // MEASURED, not estimated: against a MySQL at 2 s per command (a real
        // slow proxy, testes/teste_banco.cpp, scenario 8), a full startup is 61
        // commands — connection, schema, configuration and first snapshot — and
        // it took **120.5 seconds**. ConanLoader loads plugins in sequence, so
        // those two minutes belonged to the whole server, and the owner watched
        // it "hang" at boot without knowing why.
        //
        // The arithmetic can't shrink: the work is the work and the network is
        // what it is. What you can choose is WHERE you wait. So: opening
        // happens on the writer thread, and Abrir() waits for it for at most
        // 15 s. Past that, Abrir() returns and startup CONTINUES in the
        // background; Permission stays absent until it finishes, and joins on
        // its own when it does. No other plugin sits stuck waiting on the
        // database of whoever chose MySQL.
        //
        // 15 s was chosen because it is: (a) generous for any healthy MySQL,
        // local or remote — the normal case finishes in tens of milliseconds
        // and NOTHING changes for it; (b) below the point where an operator
        // starts thinking it has frozen.
        //
        // ── WHAT `false` MEANS HERE, AND WHAT CHANGED (2026-08-18) ───────────
        //
        // BEFORE: any database stumble (wrong password, MySQL still coming up,
        // database not created, no GRANT) returned false, and Permission stayed
        // DEAD UNTIL SOMEBODY RESTARTED THE GAME SERVER. A restart costs 6-9
        // minutes with nobody able to connect. A typo in the password cost
        // that, and the most common cause of all — the MySQL container
        // finishing its startup 20 s after the game — cost it for nothing.
        //
        // NOW: `false` only comes out when there's NO WAY TO EVEN TRY (empty
        // path, Abrir called twice). A database that didn't open returns TRUE:
        // the plugin stays loaded, in the ABSENT state (Pronto() == false, and
        // in that state ConanPermissionObterApi returns nullptr — see
        // INV-ARMAZEM-002), and the writer thread retries in the background
        // with growing backoff, re-reading config.json on every attempt. When
        // the database answers, Permission joins on its own, with nothing
        // restarted.
        //
        // What does NOT happen under any circumstances: using a database other
        // than the one config.json said to use (INV-BANCO-006).
        bool Abrir(const char* caminhoDb, const char* caminhoJson, FnLog log);

        // Tells the writer thread to leave and WAITS for it. Before waiting it
        // interrupts the in-flight database operation (IBanco::Interromper) —
        // without that, a MySQL that accepted the connection and then went
        // silent holds up the server's shutdown by up to msOperar per
        // command.
        void Fechar();

        // ── the game loop: only this runs there ─────────────────────────────
        int32_t Tem(const char* jogador, const char* no) const;
        int32_t NoGrupo(const char* jogador, const char* grupo) const;

        // 1 exists · 0 doesn't · -1 don't know (snapshot not published yet).
        // Accepts aliases: asking by the old key of a renamed group gets
        // "exists", which is the truth that matters to whoever is granting.
        int32_t GrupoExiste(const char* grupo) const;
        int64_t ExpiraEm(const char* jogador, const char* grupo) const;
        int32_t Grupos(const char* jogador, char* saida, int32_t tam) const;

        // ── outside the loop ────────────────────────────────────────────────
        //
        // All three return PERMITIDO when the task was ACCEPTED for writing
        // (not when it has been written — the writer thread does that), and
        // NAO_SEI when Armazem knows in advance it won't write:
        //
        //   · the database isn't serving right now (never opened, or dropped).
        //     Saying "ok" to a command that won't happen is the most expensive
        //     good-faith defect here: the admin types `dar fulano vip`, reads
        //     "done", and the player complains tomorrow. With NAO_SEI, the
        //     caller can say right away that the database is down;
        //   · the queue is at its cap (INV-ARMAZEM-004). Also NAO_SEI:
        //     refusing is honest, growing without limit kills the game's
        //     process.
        int32_t Conceder(const char* jogador, const char* grupo,
                         int64_t expiraEm, const char* quem);
        int32_t Revogar(const char* jogador, const char* grupo, const char* quem);
        int32_t VerJogador(const char* jogador, const char* nome);  // records "seen"
        bool    Recarregar();

        // Is the database chosen in config.json answering RIGHT NOW? An
        // atomic, cheap read, callable from any thread. Only the writer thread
        // ever writes it.
        bool BancoServindo() const { return m_bancoServe.load(std::memory_order_acquire); }

        // For the test only: waits for the write queue to drain. It does NOT
        // exist for the game — in the game nobody waits on a write.
        void EsperarFila();

        uint64_t Geracao() const;
        bool     Pronto() const { return m_atual.load(std::memory_order_acquire) != nullptr; }

    private:
        struct Tarefa
        {
            enum Tipo { CONCEDER, REVOGAR, VISTO, RECARREGAR, SAIR } tipo;
            std::string jogador, grupo, quem, nome;
            int64_t     expira = 0;
            // Only a resend after a dropped connection sets this. It serves to
            // (a) resend at most ONCE and (b) mark the audit-log row, so that
            // an audit finding two identical rows has a written explanation
            // instead of turning into a mystery. See Trabalhar().
            bool        reenvio = false;
        };

        void  Trabalhar();                       // the writer thread's body
        bool  ExecutarTarefa(const Tarefa& t, bool& mexeu);

        // Builds the IBanco from config.json, opens it, applies the schema and
        // the configuration, and publishes the first snapshot. It RE-READS the
        // config on every call, on purpose (INV-ARMAZEM-003): that's what makes
        // a corrected password, a corrected port and a corrected "Database"
        // take effect without restarting the game. Called by startup and by the
        // writer thread — never by the loop.
        bool  AbrirBanco();

        // Called by the writer thread on every pass. Decides whether it's time
        // to try opening or reconnecting, applies the growing backoff, and
        // maintains m_bancoServe. The whole "the database won't cooperate"
        // policy lives here.
        void  CuidarDaConexao();

        // Queues, applying the caps. false = refused (queue full). It doesn't
        // check whether the database is alive: the caller does that, because
        // the right message depends on which command it was.
        bool  Enfileirar(Tarefa&& t);

        bool  AplicarEsquema();
        bool  AplicarConfig(const char* caminhoJson);
        bool  Reconstruir();                     // database -> a new snapshot
        void  Publicar(Instantaneo* novo);
        void  Registrar(const char* fmt, ...) const;
        const Instantaneo* Atual() const { return m_atual.load(std::memory_order_acquire); }

        // ── a guarded read: the snapshot can't be freed underneath ───────────
        //
        // THE DEFECT THIS FIXES (the server WAS CRASHING)
        // -----------------------------------------------
        // The previous version freed a snapshot that survived TWO publications,
        // arguing that "two swaps take seconds at minimum". Measured under
        // Wine: a publication takes ~4 ms. A retired snapshot's lifetime window
        // was ~8 ms — less than a scheduling quantum, and the premise was wrong
        // by a factor of ~250.
        //
        // The consequence, reproduced 3 times out of 3: an admin granting VIP
        // in bulk (a web shop, a loop of /perm dar) while any plugin reads a
        // permission in the game loop →
        //
        //   wine: Unhandled page fault on read access to 00007FFFFF0654DC
        //   =>0 strcmp(str1=*** invalid address ***, str2="76561198000010000")
        //
        // An access violation ON THE GAME'S THREAD, which is exactly where the
        // plugin guard does NOT reach. Server on the floor, with no trace
        // anywhere near the cause.
        //
        // The fix isn't raising the number of publications: any number would be
        // a guess about how long a reader takes. It's COUNTING the readers. A
        // retired snapshot is only freed when there's no active reader at all —
        // and then it's certainty, not an estimate.
        //
        // The ordering that makes this correct: Publicar swaps m_atual FIRST.
        // After that, every new reader sees the new snapshot. If the counter is
        // zero at that moment, no reader can be holding the old one.
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

        // THE MEDIUM. What decides which one it is: "Database" in config.json.
        // Only the writer thread and startup touch this — see INV-BANCO-001.
        //
        // SWAPPING the pointer is guarded by m_mtxBanco, and only that. Using
        // it needs no lock because only one thread uses it; the lock exists
        // because Fechar(), from ANOTHER thread, has to call Interromper() on
        // whichever database is there — and without the lock that races the
        // swap.
        std::unique_ptr<IBanco> m_banco;
        mutable std::mutex      m_mtxBanco;

        FnLog       m_log = nullptr;
        std::string m_caminhoDb, m_caminhoJson;

        // ── the "the database won't cooperate" policy ───────────────────────
        //
        // The chosen database is answering RIGHT NOW. Written only by the
        // writer thread (and by Abrir, before that thread exists); read by the
        // game loop in Conceder/Revogar/VerJogador, which is why it's atomic.
        std::atomic<bool> m_bancoServe{false};

        // Has the FIRST open attempt finished (successfully or not)? It exists
        // only so Abrir() can tell whether the silence means "it failed" or
        // "I'm still opening" — and the two need different log messages.
        std::atomic<bool> m_arranqueFeito{false};

        // GROWING backoff between attempts, in seconds: 5, 10, 20, 40, 80,
        // 160, 300 (cap). Zero = everything's fine, no attempt pending.
        //
        // WHY GROWING, and not the fixed 15 s it used to be: both ends matter
        // and they pull opposite ways. A 3-second network blip has to clear
        // fast (hence the short first step); a MySQL that's been off for 6
        // hours can't take one attempt every 15 s all night — that's 1,440
        // connections against a dead database, 1,440 log lines, and not one of
        // them was going to work. The 300 s cap exists for the other end: the
        // owner who just fixed the password can't wait more than 5 minutes for
        // the fix to take.
        int64_t     m_proximaTentativa   = 0;   // absolute clock, in seconds
        int         m_esperaSegundos     = 0;
        int         m_falhasReconexao    = 0;   // 2 in a row => rebuild from config

        // The last state the writer thread ANNOUNCED in the log. Only it
        // touches this, so it needn't be atomic — and that's exactly why it
        // exists separately from m_bancoServe: that one is the state published
        // to the game loop and can be knocked down from inside a failed task;
        // this one decides whether the "the connection dropped" line has
        // already been written. Merging the two made a drop discovered
        // mid-task pass in silence.
        bool        m_estavaServindo    = false;

        int64_t     m_ultimoAvisoConexao = 0;
        // Counters since the last warning. They exist so the periodic warning
        // can say HOW MUCH was lost, instead of repeating "the database is
        // down" with no size to it — that's the difference between the owner
        // knowing they lost 3 commands and assuming they lost the night.
        uint64_t    m_recusadasDesdeAviso   = 0;
        uint64_t    m_descartadasDesdeAviso = 0;

        std::atomic<const Instantaneo*> m_atual{nullptr};
        // Retired snapshots. They aren't freed right away: a reader may have
        // the pointer in hand. They're kept and freed once no reader is active
        // (see the Leitura class above). It costs a few KB and removes the only
        // way this design could take the server down.
        std::vector<const Instantaneo*> m_aposentados;

        // Readers ACTIVE right now. mutable because the read is const from the
        // point of view of whoever queries a permission — and logically it is:
        // nothing changes.
        mutable std::atomic<int> m_leitores{0};

        mutable std::mutex      m_mtxFila;
        std::condition_variable m_cvFila;
        std::deque<Tarefa>      m_fila;
        // How many VISTO tasks are in the queue right now. A counter, not a
        // sweep of the queue: what enqueues is the game loop, and walking
        // thousands of tasks with the lock in hand would be exactly putting the
        // database's cost inside the tick.
        size_t                  m_vistosNaFila = 0;
        std::thread             m_thread;
        std::atomic<bool>       m_rodando{false};
        std::atomic<uint64_t>   m_feitas{0};
        std::atomic<uint64_t>   m_enfileiradas{0};
    };
}
