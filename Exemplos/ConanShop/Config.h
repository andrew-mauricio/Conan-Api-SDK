// Config — the config.json, read and VALIDATED before it takes effect.
//
// THE RULE THIS FILE EXISTS TO GUARANTEE
// ---------------------------------------
// A configuration only replaces the previous one if it is WHOLE and correct.
// Never halfway.
//
// The failure mode this avoids is concrete: the owner edits the config at 9pm,
// misses a comma, runs !shopreload, and the shop sits empty through peak hours —
// with no visible error, because "0 items" is a valid state. Here a broken
// config is REFUSED and the previous one stays live; the owner reads the reason
// in chat and fixes it with the shop still running.
//
// WHY THE JSON IS PARSED BY SQLITE
// ---------------------------------
// Same reason as Permission: SQLite's json1 is already in the process, and
// writing a JSON parser by hand to read a file the owner edits by hand is new
// code on the plugin's most exposed path. json_valid() before anything else:
// extracting from broken JSON returns NULL silently, and an empty configuration
// wearing the face of a loaded one is worse than an error.
#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <map>

#include "Pontos.h"

namespace Shop
{
    struct Item
    {
        std::string chave;        // what the player types: !comprar stone
        std::string nome;         // what they read in the list
        std::string categoria;
        std::string permissao;    // empty = anyone may buy
        int32_t     templateId = 0;
        int32_t     quantidade = 1;
        int64_t     preco = 0;
    };

    struct Config
    {
        ConfigBanco banco;

        // points
        bool     pontosLigados = true;
        int      pontosMinutos = 5;
        bool     pontosSomar   = false;
        bool     pontosAvisar  = true;
        std::map<std::string, int64_t> pontosPorGrupo;

        // shop
        int         itensPorPagina = 12;
        bool        usarTela = true;
        std::string tituloDaTela = "Loja";
        std::string contexto = "ConanShop";

        // commands
        std::string cmdLoja = "!shop", cmdComprar = "!comprar", cmdPontos = "!pontos",
                    cmdAjuda = "!shopajuda", cmdRecarregar = "!shopreload",
                    cmdDar = "!shopdar";

        // permissions
        std::string permAdmin = "shop.admin", permComprar;

        // messages, by key
        std::map<std::string, std::string> msg;

        // items, in file order (the list order is the one the owner wrote)
        std::vector<Item> itens;

        const Item* Achar(const std::string& chave) const;
        const std::string& Msg(const char* chave, const char* padrao) const;
    };

    // Reads and VALIDATES. Returns false without touching `destino` if anything
    // is wrong, and then `erro` carries a sentence the server owner can act on,
    // not a code.
    bool LerConfig(const char* caminho, Config& destino, std::string& erro);
}
