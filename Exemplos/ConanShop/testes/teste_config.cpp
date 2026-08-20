// teste_config — config.json is the plugin's MOST EXPOSED path.
//
// WHY THIS TEST EXISTS
// --------------------
// Everything else in the plugin runs from what this file read. And it gets
// edited by hand, late at night, by somebody with a full server — which is
// exactly when a comma goes missing.
//
// The plugin's promise is "a broken config does NOT replace a good one". An
// untested promise is just a promise. Here it gets exercised with genuinely bad
// files.
//
// The most important case is number 3: valid JSON, INVALID items. It's the only
// one where a careless plugin comes up "successfully" and goes on charging
// points without delivering anything — because zero items is a syntactically
// valid state.
#include "../Config.h"

#include <cstdio>
#include <cstring>
#include <string>

static int g_falhas = 0;

static void Conferir(bool ok, const char* oque, const std::string& detalhe = "")
{
    std::printf("  [%s] %s%s%s\n", ok ? "ok" : " X", oque,
                detalhe.empty() ? "" : " — ", detalhe.c_str());
    if (!ok) ++g_falhas;
}

static std::string Escrever(const std::string& pasta, const char* nome, const char* conteudo)
{
    const std::string caminho = pasta + "\\" + nome;
    FILE* f = std::fopen(caminho.c_str(), "wb");
    if (f) { std::fputs(conteudo, f); std::fclose(f); }
    return caminho;
}

int main(int argc, char** argv)
{
    const std::string pasta = (argc > 1) ? argv[1] : ".";

    std::printf("\n== 1. config bom ==\n");
    const char* BOM = R"({
      "banco":  { "tipo": "local" },
      "pontos": { "minutos": 7, "somar": true, "grupos": { "default": 3, "vip": 9 } },
      "loja":   { "itens_por_pagina": 5, "usar_tela": false },
      "comandos": { "loja": "!loja", "comprar": "!c" },
      "mensagens": { "seus_pontos": "tem {0} pontos" },
      "itens": {
        "pedra": { "nome": "Pedra", "categoria": "recurso",
                   "template_id": 10001, "quantidade": 100, "preco": 10 },
        "ferro": { "nome": "Ferro", "template_id": 11001, "preco": 25 }
      }
    })";
    Shop::Config c;
    std::string erro;
    bool ok = Shop::LerConfig(Escrever(pasta, "bom.json", BOM).c_str(), c, erro);
    Conferir(ok, "leu o arquivo bom", erro);
    if (ok)
    {
        Conferir(c.itens.size() == 2, "dois itens", std::to_string(c.itens.size()));
        Conferir(c.pontosMinutos == 7, "minutos = 7");
        Conferir(c.pontosSomar,        "somar = true");
        Conferir(c.itensPorPagina == 5,"itens por pagina = 5");
        Conferir(!c.usarTela,          "usar_tela = false");
        Conferir(c.cmdLoja == "!loja", "comando trocado", c.cmdLoja);
        // What was NOT written in the file has to fall back to the default,
        // not to empty.
        Conferir(c.cmdPontos == "!pontos", "comando ausente virou o padrao", c.cmdPontos);
        Conferir(c.pontosPorGrupo.size() == 2, "dois grupos de pontos");
        const Shop::Item* p = c.Achar("pedra");
        Conferir(p && p->templateId == 10001, "achou pedra pelo id de chat");
        const Shop::Item* fe = c.Achar("ferro");
        Conferir(fe && fe->quantidade == 1, "quantidade ausente virou 1");
        Conferir(fe && fe->categoria == "outros", "categoria ausente virou 'outros'");
    }

    std::printf("\n== 2. JSON quebrado (virgula a mais) ==\n");
    const char* QUEBRADO = R"({ "itens": { "pedra": { "template_id": 1, "preco": 1 }, } })";
    Shop::Config antes = c;
    erro.clear();
    ok = Shop::LerConfig(Escrever(pasta, "quebrado.json", QUEBRADO).c_str(), c, erro);
    Conferir(!ok, "RECUSOU o arquivo quebrado");
    Conferir(!erro.empty(), "e disse por que", erro);
    Conferir(c.itens.size() == antes.itens.size(),
             "a configuracao ANTERIOR continua intacta",
             std::to_string(c.itens.size()) + " item(ns)");

    std::printf("\n== 3. JSON VALIDO, itens invalidos (o caso perigoso) ==\n");
    // Syntactically perfect. If the plugin accepted it, it would come up with
    // zero items and a shop that charges and delivers nothing — with no error
    // anywhere at all.
    const char* SEM_ID = R"({
      "itens": {
        "fantasma": { "nome": "Sem id",   "preco": 10 },
        "zero":     { "nome": "Id zero",  "template_id": 0, "preco": 10 },
        "negativo": { "nome": "Preco -1", "template_id": 5, "preco": -1 }
      }
    })";
    erro.clear();
    ok = Shop::LerConfig(Escrever(pasta, "semid.json", SEM_ID).c_str(), c, erro);
    Conferir(!ok, "RECUSOU: nenhum item entregavel");
    Conferir(erro.find("template_id") != std::string::npos,
             "e o motivo aponta o campo certo", erro);
    Conferir(c.itens.size() == antes.itens.size(), "a anterior continua valendo");

    std::printf("\n== 4. banco.tipo errado de digitacao ==\n");
    const char* MYSQLL = R"({ "banco": {"tipo":"mysqll"},
      "itens": { "p": { "template_id": 1, "preco": 1 } } })";
    erro.clear();
    ok = Shop::LerConfig(Escrever(pasta, "mysqll.json", MYSQLL).c_str(), c, erro);
    Conferir(!ok, "RECUSOU \"mysqll\" em vez de cair no local calado");
    Conferir(erro.find("mysqll") != std::string::npos, "e mostrou o que foi digitado", erro);

    std::printf("\n== 5. dois comandos com o mesmo texto ==\n");
    const char* IGUAIS = R"({ "comandos": {"loja":"!x","pontos":"!x"},
      "itens": { "p": { "template_id": 1, "preco": 1 } } })";
    erro.clear();
    ok = Shop::LerConfig(Escrever(pasta, "iguais.json", IGUAIS).c_str(), c, erro);
    Conferir(!ok, "RECUSOU comandos duplicados (o segundo nunca rodaria)");

    std::printf("\n== 6. arquivo que nao existe ==\n");
    erro.clear();
    ok = Shop::LerConfig((pasta + "\\nao_existe_mesmo.json").c_str(), c, erro);
    Conferir(!ok, "RECUSOU arquivo ausente");
    Conferir(!erro.empty(), "e disse qual", erro);

    std::printf("\n== 7. item ruim no meio de bons: sobra o que presta ==\n");
    // A wrong id on item 3 must not take the shop down — but the owner HAS to
    // know that what they wrote didn't go in whole.
    const char* MISTO = R"({
      "itens": {
        "bom1": { "nome":"A", "template_id": 10001, "preco": 5 },
        "ruim": { "nome":"B", "preco": 5 },
        "bom2": { "nome":"C", "template_id": 10002, "preco": 5 },
        "ch@ve invalida": { "nome":"D", "template_id": 3, "preco": 5 }
      }
    })";
    erro.clear();
    ok = Shop::LerConfig(Escrever(pasta, "misto.json", MISTO).c_str(), c, erro);
    Conferir(ok, "ACEITOU: ha itens bons", erro);
    if (ok)
    {
        Conferir(c.itens.size() == 2, "ficou so' com os dois bons",
                 std::to_string(c.itens.size()));
        Conferir(c.msg.count("_aviso_itens_recusados") > 0,
                 "e AVISOU quantos foram recusados",
                 c.msg.count("_aviso_itens_recusados") ? c.msg["_aviso_itens_recusados"] : "");
    }

    std::printf("\n== 8. limites são aparados, nao aceitos ==\n");
    const char* LIMITES = R"({
      "pontos": { "minutos": 5 },
      "loja":   { "itens_por_pagina": 9999 },
      "itens":  { "p": { "template_id": 1, "preco": 1 } } })";
    erro.clear();
    ok = Shop::LerConfig(Escrever(pasta, "limites.json", LIMITES).c_str(), c, erro);
    Conferir(ok, "leu", erro);
    Conferir(c.itensPorPagina <= 40, "itens_por_pagina foi aparado para caber na tela",
             std::to_string(c.itensPorPagina));

    const char* MINUTOS_ZERO = R"({ "pontos": {"minutos": 0},
      "itens": { "p": { "template_id": 1, "preco": 1 } } })";
    erro.clear();
    ok = Shop::LerConfig(Escrever(pasta, "min0.json", MINUTOS_ZERO).c_str(), c, erro);
    Conferir(!ok, "RECUSOU minutos = 0 (creditaria em laco)");

    std::printf("\n%s  (%d falha(s))\n", g_falhas ? "REPROVADO" : "APROVADO", g_falhas);
    return g_falhas ? 1 : 0;
}
