// Texto (text) — the two functions that handle what the player types and what
// they read.
//
// WHY THEY LIVE IN THEIR OWN HEADER
// ----------------------------------
// They used to sit inside ConanShop.cpp as `static`, and therefore out of reach
// of any test. Both are small and look obvious, which is exactly the profile of
// code nobody tests and that breaks silently:
//
//   · `Prefixo` decides which command answers. If it lets `!shop` match
//     `!shopreload`, a reload turns into a shop listing and the owner never
//     works out why the config won't update.
//   · `Formatar` builds what the player reads. If it gets it wrong the player
//     sees "You have {0} points" — ugly, but worse: it must NOT become printf,
//     because the text comes from the owner's file, and a "%s" typed there by
//     accident would turn into an arbitrary memory read in here.
//
// They're `inline` in a header so the plugin and the tests compile exactly the
// same code, not a similar copy.
#pragma once

#include <string>
#include <vector>
#include <cctype>
#include <cstdlib>

namespace Shop
{
    // Replaces {0}, {1}, {2}… with the values, in order. Anything that isn't a
    // number in braces is left alone: `{name}` is the owner's text, not a
    // placeholder.
    inline std::string Formatar(const std::string& modelo,
                                const std::vector<std::string>& valores)
    {
        std::string r;
        r.reserve(modelo.size() + 32);
        for (size_t i = 0; i < modelo.size(); ++i)
        {
            if (modelo[i] != '{') { r += modelo[i]; continue; }
            const size_t fim = modelo.find('}', i);
            if (fim == std::string::npos) { r += modelo[i]; continue; }

            const std::string dentro = modelo.substr(i + 1, fim - i - 1);
            bool numero = !dentro.empty();
            for (char c : dentro)
                if (!std::isdigit(static_cast<unsigned char>(c))) { numero = false; break; }
            if (!numero) { r += modelo[i]; continue; }

            const size_t n = static_cast<size_t>(std::atoi(dentro.c_str()));
            // An index past what was passed becomes empty, not "{7}" and not
            // garbage: a message from the owner citing a value that doesn't
            // exist is their mistake, and showing the raw key to the player
            // helps nobody.
            if (n < valores.size()) r += valores[n];
            i = fim;
        }
        return r;
    }

    // Does `texto` start with the command `cmd`? Returns the rest, trimmed.
    //
    // THE RULE THAT KEEPS THIS FROM BEING A `starts_with`: what follows the
    // command must be a SPACE or the end of the line. Without that, `!shop`
    // would match `!shopreload` and `!shopdar`, and the shortest command would
    // swallow all the others — silently, because a shop listing is a plausible
    // answer.
    inline bool Prefixo(const std::string& texto, const std::string& cmd,
                        std::string& resto)
    {
        if (cmd.empty() || texto.size() < cmd.size()) return false;
        if (texto.compare(0, cmd.size(), cmd) != 0) return false;
        if (texto.size() > cmd.size() && texto[cmd.size()] != ' ') return false;

        resto = (texto.size() > cmd.size()) ? texto.substr(cmd.size() + 1)
                                            : std::string();
        while (!resto.empty() && (resto.front() == ' ' || resto.front() == '\t'))
            resto.erase(resto.begin());
        while (!resto.empty() && (resto.back() == ' ' || resto.back() == '\t' ||
                                  resto.back() == '\r' || resto.back() == '\n'))
            resto.pop_back();
        return true;
    }
}
