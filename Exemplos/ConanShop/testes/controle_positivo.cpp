// controle_positivo — proves that teste_pontos KNOWS HOW TO FAIL.
//
// WHY THIS EXISTS
// ---------------
// teste_pontos passed. On its own that doesn't prove the debit is atomic — it
// proves the test found no problem, which is a different thing. A test with a
// badly built race (too few threads, weak contention, a database serialising by
// luck) passes on both implementations alike and hands you a pass that means
// nothing.
//
// This program re-runs the SAME race against the NAIVE implementation —
// ArkShop's, with the balance check outside the UPDATE:
//
//     if (balance >= price)                      <- read
//         UPDATE carteira SET pontos = pontos-?  <- spend, without repeating
//                                                   the condition
//
// If the test is capable of finding the defect, then here it MUST fail: more
// than 20 purchases go through, or the balance ends up negative. If it passes
// on both, the test is blind and the other one's pass is worth nothing.
//
// This file does NOT go into the plugin. It's the instrument that calibrates
// the instrument.
#include "terceiros/sqlite3/sqlite3.h"

#include <cstdio>
#include <string>
#include <thread>
#include <vector>
#include <atomic>
#include <mutex>
#include <chrono>

namespace
{
    sqlite3*    g_db = nullptr;
    // The database's path, so each thread opens its OWN connection.
    std::string g_caminho;
    std::mutex g_trava;

    // Exactly ArkShop's pattern: read the balance, decide in C++, and only
    // then send the UPDATE — which does NOT repeat the condition.
    //
    // ── WHY THERE'S NO LOCK HERE ────────────────────────────────────────────
    //
    // The first version of this file wrapped every access in a std::mutex,
    // imitating Pontos.cpp's lock. With it, the NAIVE version PASSED the test —
    // 20 out of 20, balance zero — and the positive control FAILED, accusing
    // the test of being blind.
    //
    // The diagnosis was right, and the cause was the instrument itself: a lock
    // that serialises read-then-spend erases exactly the gap we're trying to
    // measure. Testing with it tests whether the mutex works, not whether the
    // SQL protects anything.
    //
    // Without a lock, each thread is an INDEPENDENT client of the database —
    // which is exactly the real case that matters: two game servers pointed at
    // the SAME MySQL, each with its own process, with no mutex between them.
    // That's the case the condition lives inside the UPDATE for.
    bool DebitarIngenuo(sqlite3* db, const char* jogador, int64_t quanto)
    {
        int64_t saldo = 0;
        {
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(db, "SELECT pontos FROM carteira WHERE jogador=?1;",
                                   -1, &st, nullptr) != SQLITE_OK) return false;
            sqlite3_bind_text(st, 1, jogador, -1, SQLITE_TRANSIENT);
            if (sqlite3_step(st) == SQLITE_ROW) saldo = sqlite3_column_int64(st, 0);
            sqlite3_finalize(st);
        }

        if (saldo < quanto) return false;      // <- the check, OUTSIDE the UPDATE

        // The pause isn't a trick to force the defect: it stands for what any
        // shop does between deciding and charging — building the message,
        // checking a permission, finding the character. In ArkShop that stretch
        // calls GetPoints, formats an FString and queries Permissions. Here
        // it's an instant, and that's already enough.
        std::this_thread::sleep_for(std::chrono::microseconds(50));

        {
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(db,
                    "UPDATE carteira SET pontos = pontos - ?1 WHERE jogador=?2;",
                    -1, &st, nullptr) != SQLITE_OK) return false;
            sqlite3_bind_int64(st, 1, quanto);
            sqlite3_bind_text (st, 2, jogador, -1, SQLITE_TRANSIENT);
            int rc;
            // SQLITE_BUSY isn't a refusal for lack of balance: it's the
            // database being busy. Retrying is what any client does, and it's
            // what keeps the comparison fair.
            for (int tent = 0; (rc = sqlite3_step(st)) == SQLITE_BUSY && tent < 100; ++tent)
                sqlite3_reset(st);
            sqlite3_finalize(st);
            return rc == SQLITE_DONE;
        }
    }

    // OURS: the same race, the same pause, everything the same — only the SQL
    // changes. The balance condition goes INSIDE the UPDATE, and the database
    // decides, once, under its own lock. There's nothing to read first, so
    // there's no gap between reading and spending.
    bool DebitarComCondicao(sqlite3* db, const char* jogador, int64_t quanto)
    {
        // The same pause as the naive version, at the same point in the
        // timeline: without it the comparison would be between a tight race
        // and a loose one.
        std::this_thread::sleep_for(std::chrono::microseconds(50));

        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db,
                "UPDATE carteira SET pontos = pontos - ?1 "
                "WHERE jogador=?2 AND pontos >= ?1;",
                -1, &st, nullptr) != SQLITE_OK) return false;
        sqlite3_bind_int64(st, 1, quanto);
        sqlite3_bind_text (st, 2, jogador, -1, SQLITE_TRANSIENT);
        int rc;
        for (int tent = 0; (rc = sqlite3_step(st)) == SQLITE_BUSY && tent < 100; ++tent)
            sqlite3_reset(st);
        sqlite3_finalize(st);
        if (rc != SQLITE_DONE) return false;

        // "It changed a row" is the database's answer to "was there a
        // balance?".
        return sqlite3_changes(db) == 1;
    }
}

int main(int argc, char** argv)
{
    const std::string caminho = (argc > 1) ? argv[1] : "controle_positivo.db";
    g_caminho = caminho;
    std::remove(caminho.c_str());
    std::remove((caminho + "-wal").c_str());
    std::remove((caminho + "-shm").c_str());

    if (sqlite3_open_v2(caminho.c_str(), &g_db,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK)
    { std::printf("  X nao abriu o banco\n"); return 2; }
    sqlite3_busy_timeout(g_db, 5000);
    sqlite3_exec(g_db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    // No CHECK on purpose: here we WANT to see the negative balance show up.
    sqlite3_exec(g_db, "CREATE TABLE carteira (jogador TEXT PRIMARY KEY, pontos INTEGER NOT NULL DEFAULT 0);",
                 nullptr, nullptr, nullptr);
    sqlite3_exec(g_db, "INSERT INTO carteira (jogador,pontos) VALUES ('corrida',200);",
                 nullptr, nullptr, nullptr);

    std::printf("== a MESMA corrida, com as DUAS implementacoes, SEM TRAVA ==\n");
    std::printf("   200 pontos, 8 threads x 50 compras de 10 -> so' 20 podem passar.\n");
    std::printf("   Sem mutex dos dois lados: o que proteger, protege pelo SQL.\n\n");

    auto correr = [&](bool ingenuo)
    {
        sqlite3_exec(g_db, "UPDATE carteira SET pontos=200 WHERE jogador='corrida';",
                     nullptr, nullptr, nullptr);
        std::atomic<int> ok{0};
        std::vector<std::thread> ths;
        for (int t = 0; t < 8; ++t)
            ths.emplace_back([&, ingenuo]
            {
                // ── ONE CONNECTION PER THREAD ───────────────────────────────
                //
                // The comment at the top of this function always said "each
                // thread is an INDEPENDENT client of the database". The code
                // wasn't doing that: all eight shared `g_db`.
                //
                // That matters because of ONE line, `sqlite3_changes(db)`,
                // which answers "how many rows did THIS CONNECTION's LAST
                // operation change". With a shared connection, thread A could
                // read the `changes` thread B had just produced — and count as
                // a success an UPDATE that wasn't its own.
                //
                // The symptom was INTERMITTENT and misleading: on 2026-08-20
                // the suite failed 1 run in ~10, always here, with
                //     passaram=19   saldo=0
                // which is a contradiction — 19 purchases of 10 against 200
                // would leave a balance of 10, and a balance of 0 means 20 went
                // through. The DATABASE was right; it was the test's counter
                // that got lost. An instrument that's wrong sometimes fails
                // good code and teaches you to ignore it.
                //
                // With one connection per thread, `changes` goes back to
                // answering about what THIS thread did — and the test starts
                // measuring what it says it measures, which is also the real
                // case: two game servers, two processes, two connections, no
                // mutex between them.
                sqlite3* db = nullptr;
                if (sqlite3_open_v2(g_caminho.c_str(), &db,
                                    SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) return;
                sqlite3_busy_timeout(db, 5000);
                for (int i = 0; i < 50; ++i)
                    if (ingenuo ? DebitarIngenuo(db, "corrida", 10)
                                : DebitarComCondicao(db, "corrida", 10)) ++ok;
                sqlite3_close(db);
            });
        for (auto& t : ths) t.join();

        int64_t saldo = 0;
        sqlite3_stmt* st = nullptr;
        sqlite3_prepare_v2(g_db, "SELECT pontos FROM carteira WHERE jogador='corrida';",
                           -1, &st, nullptr);
        if (st && sqlite3_step(st) == SQLITE_ROW) saldo = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
        return std::make_pair(ok.load(), saldo);
    };

    const auto ingenuo = correr(true);
    std::printf("   INGENUA  (checagem fora do UPDATE, como o ArkShop):\n");
    std::printf("      passaram=%d   saldo=%lld\n\n", ingenuo.first, (long long)ingenuo.second);

    const auto nossa = correr(false);
    std::printf("   NOSSA    (condicao DENTRO do UPDATE):\n");
    std::printf("      passaram=%d   saldo=%lld\n\n", nossa.first, (long long)nossa.second);

    sqlite3_close(g_db);

    const bool ingenuaVazou = (ingenuo.first > 20) || (ingenuo.second < 0);
    const bool nossaSegurou = (nossa.first == 20) && (nossa.second == 0);

    if (!ingenuaVazou)
    {
        std::printf("REPROVADO como controle positivo: nem a versao INGENUA vazou.\n");
        std::printf("   A corrida esta fraca demais para provar qualquer coisa.\n");
        std::printf("   Aumente as threads/iteracoes antes de acreditar no outro teste.\n");
        return 1;
    }
    if (!nossaSegurou)
    {
        std::printf("REPROVADO: a NOSSA versao tambem vazou, sem a trava.\n");
        std::printf("   Entao quem estava segurando era o mutex, nao o SQL — e num\n");
        std::printf("   MySQL compartilhado entre dois servidores nao ha mutex nenhum.\n");
        return 1;
    }

    std::printf("APROVADO.\n");
    std::printf("   A ingenua deixou passar %d compra(s) a mais e o saldo foi a %lld.\n",
                ingenuo.first - 20, (long long)ingenuo.second);
    std::printf("   A nossa segurou em 20 e zero SEM TRAVA — logo quem protege e' a\n");
    std::printf("   condicao dentro do UPDATE, e a protecao vale tambem entre PROCESSOS\n");
    std::printf("   diferentes: dois servidores no mesmo MySQL.\n");
    return 0;
}
