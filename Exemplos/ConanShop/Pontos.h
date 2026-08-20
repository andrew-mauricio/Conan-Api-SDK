// Pontos (points) — each player's wallet, and the only thing in this plugin
// that HAS to survive a shutdown.
//
// WHAT THIS FILE PROTECTS
// -----------------------
// Points are money. The two things that must not happen are:
//
//   1. a player spending what they don't have (negative balance)
//   2. a player spending the same point twice (two simultaneous purchases
//      reading the same balance)
//
// ArkShop, which this plugin takes as its reference, guards both in C++:
//
//     if (points >= price && Points::SpendPoints(price, eos_id))
//
// and the SQL underneath is `UPDATE ... SET Points = Points - ? WHERE EosId = ?`.
// The check comes BEFORE, in the process; the UPDATE doesn't repeat the
// condition. Another purchase by the same player fits between the GetPoints and
// the UPDATE — two clients, or a double click — and both go through. The balance
// goes negative and nobody sees it.
//
// Here the condition lives INSIDE the UPDATE:
//
//     UPDATE carteira SET pontos = pontos - ?  WHERE jogador = ? AND pontos >= ?
//
// The database decides, once, under its own lock. If no row changed, there was
// no balance — and "changed nothing" is an answer, not an assumption: it's what
// `Mudancas()` returns. Two simultaneous purchases: the first changes 1 row, the
// second changes 0 and is refused. There's no window between reading and
// spending because nothing is read before spending.
#pragma once

#include <cstdint>
#include <string>

namespace Shop
{
    // How this module answers its caller: no exception crossing the game's
    // boundary, and the reason kept separate from the result.
    enum class Gasto
    {
        Ok,             // debited
        SemSaldo,       // not enough — the UPDATE changed nothing
        Erro,           // the database failed; NOTHING was debited
    };

    struct ConfigBanco
    {
        bool        mysql = false;
        std::string caminhoLocal;      // already resolved
        std::string host = "127.0.0.1";
        int         porta = 3306;
        std::string usuario, senha, banco;
        int         prazoConectarMs = 5000;
        int         prazoOperarMs   = 10000;
    };

    // Where this module writes what went wrong. Without it, a database failure
    // dies inside the plugin and the server owner only sees "the shop isn't
    // answering", with no line saying why.
    void DefinirLog(void (*log)(const char*));

    // Opens (and creates the tables on first run). `erro` receives the reason
    // when this returns false: the server owner reads it in the log and needs to
    // know what to do, not a code.
    bool Abrir(const ConfigBanco& cfg, std::string& erro);
    void Fechar();

    // Balance. -1 when the database didn't answer: the caller MUST distinguish
    // "has zero" from "don't know", because telling someone with 500 points that
    // they have 0 is worse than telling them the shop is down.
    int64_t Saldo(const std::string& jogador);

    // Credits. Returns false if the database failed (nothing was credited).
    bool Creditar(const std::string& jogador, int64_t quanto, const char* motivo);

    // Debits IF there's balance. The database decides, in a single statement.
    Gasto Debitar(const std::string& jogador, int64_t quanto, const char* motivo);

    // For when delivery fails after the debit. It's an ordinary credit; it has
    // its own name so the ledger shows it as a refund rather than a reward.
    bool Devolver(const std::string& jogador, int64_t quanto, const char* motivo);
}
