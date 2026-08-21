// teste_texto — command routing and message assembly.
//
// THE CASE THIS TEST EXISTS TO CATCH
// ----------------------------------
// `!shop` swallowing `!shopreload`. If `Prefixo` were a "starts with", the
// shortest command would answer all the others — and the symptom would be the
// owner typing !shopreload, watching the SHOP LIST appear, and concluding the
// config doesn't reload. No error anywhere at all.
//
// The order in which the plugin tests the commands matters too, and case 3
// reproduces that order exactly as the router does it.
#include "../Texto.h"

#include <cstdio>
#include <string>
#include <vector>

static int g_falhas = 0;

static void Conferir(bool ok, const std::string& oque, const std::string& detalhe = "")
{
    std::printf("  [%s] %s%s%s\n", ok ? "ok" : " X", oque.c_str(),
                detalhe.empty() ? "" : " — ", detalhe.c_str());
    if (!ok) ++g_falhas;
}

int main()
{
    std::printf("\n== 1. Prefixo: casar o comando exato ==\n");
    std::string r;
    Conferir(Shop::Prefixo("!shop", "!shop", r) && r.empty(),
             "\"!shop\" casa com !shop, resto vazio");
    Conferir(Shop::Prefixo("!shop 2", "!shop", r) && r == "2",
             "\"!shop 2\" casa, resto = 2", r);
    Conferir(Shop::Prefixo("!shop   3  ", "!shop", r) && r == "3",
             "espacos sobrando sao aparados", "[" + r + "]");

    std::printf("\n== 2. Prefixo: NAO engolir o comando mais longo ==\n");
    Conferir(!Shop::Prefixo("!shopreload", "!shop", r),
             "\"!shopreload\" NAO casa com !shop");
    Conferir(!Shop::Prefixo("!shopdar Fulano 10", "!shop", r),
             "\"!shopdar ...\" NAO casa com !shop");
    Conferir(!Shop::Prefixo("!shopajuda", "!shop", r),
             "\"!shopajuda\" NAO casa com !shop");
    Conferir(Shop::Prefixo("!shopreload", "!shopreload", r) && r.empty(),
             "mas casa com o proprio !shopreload");
    Conferir(!Shop::Prefixo("!comprar", "!comprarx", r),
             "texto menor que o comando nao casa");
    Conferir(!Shop::Prefixo("!sho", "!shop", r),
             "prefixo incompleto nao casa");
    Conferir(!Shop::Prefixo("olha o !shop ai", "!shop", r),
             "comando no MEIO da frase nao casa");

    std::printf("\n== 3. a ORDEM do roteador, como o plugin faz ==\n");
    // The plugin tests in this order: recarregar, dar, comprar, pontos, ajuda,
    // loja. If the order were wrong, "!shopreload" would fall into "!shop".
    auto rotear = [](const std::string& msg) -> std::string
    {
        std::string resto;
        if (Shop::Prefixo(msg, "!shopreload", resto)) return "recarregar";
        if (Shop::Prefixo(msg, "!shopdar",    resto)) return "dar";
        if (Shop::Prefixo(msg, "!comprar",    resto)) return "comprar";
        if (Shop::Prefixo(msg, "!pontos",     resto)) return "pontos";
        if (Shop::Prefixo(msg, "!shopajuda",  resto)) return "ajuda";
        if (Shop::Prefixo(msg, "!shop",       resto)) return "loja";
        return "(nenhum)";
    };
    struct Caso { const char* digitou; const char* esperado; };
    const Caso CASOS[] = {
        { "!shop",                  "loja"       },
        { "!shop 3",                "loja"       },
        { "!shopreload",            "recarregar" },
        { "!shopdar Indio#7 500",   "dar"        },
        { "!shopajuda",             "ajuda"      },
        { "!comprar pedra",         "comprar"    },
        { "!comprar pedra 3",       "comprar"    },
        { "!pontos",                "pontos"     },
        { "!shoploja",              "(nenhum)"   },   // looks like one, matches none
        { "!banir alguem",          "(nenhum)"   },
        { "oi pessoal",             "(nenhum)"   },
    };
    for (const Caso& c : CASOS)
    {
        const std::string veio = rotear(c.digitou);
        Conferir(veio == c.esperado,
                 std::string("\"") + c.digitou + "\" -> " + c.esperado,
                 veio == c.esperado ? "" : "veio \"" + veio + "\"");
    }

    std::printf("\n== 4. Formatar ==\n");
    Conferir(Shop::Formatar("tem {0} pontos", {"42"}) == "tem 42 pontos",
             "troca {0}", Shop::Formatar("tem {0} pontos", {"42"}));
    Conferir(Shop::Formatar("{0} x{1} por {2}", {"Pedra","100","10"}) == "Pedra x100 por 10",
             "troca varios, na ordem");
    Conferir(Shop::Formatar("{1} antes de {0}", {"A","B"}) == "B antes de A",
             "respeita o indice, nao a ordem de aparicao");
    Conferir(Shop::Formatar("sem placeholder", {"x"}) == "sem placeholder",
             "texto sem chave passa inteiro");
    Conferir(Shop::Formatar("valor {9}", {"a"}) == "valor ",
             "indice alem do que veio vira vazio, nao a chave crua",
             "[" + Shop::Formatar("valor {9}", {"a"}) + "]");
    Conferir(Shop::Formatar("{nome} do dono", {"x"}) == "{nome} do dono",
             "chave NAO numerica fica como esta (e texto do dono)");
    Conferir(Shop::Formatar("chave { aberta", {"x"}) == "chave { aberta",
             "chave sem fechar nao quebra");
    Conferir(Shop::Formatar("", {"x"}).empty(), "texto vazio devolve vazio");
    Conferir(Shop::Formatar("tem {0}", {}) == "tem ",
             "sem valores, o placeholder some");

    std::printf("\n== 5. o que NAO pode acontecer: formato do dono nao vira printf ==\n");
    // If this went through printf, a "%s" typed into config.json would read
    // arbitrary server memory. Here it's ordinary text.
    const std::string perigoso = "saldo %s %d %n {0}";
    const std::string saiu = Shop::Formatar(perigoso, {"7"});
    Conferir(saiu == "saldo %s %d %n 7",
             "%s, %d e %n do config sao TEXTO, nao formato", saiu);

    std::printf("\n%s  (%d falha(s))\n", g_falhas ? "REPROVADO" : "APROVADO", g_falhas);
    return g_falhas ? 1 : 0;
}
