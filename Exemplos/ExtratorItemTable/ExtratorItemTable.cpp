// ExtratorItemTable — Conan's item catalogue, from the CANONICAL SOURCE.
//
// WHY THIS PLUGIN REPLACES ExtratorDeItens
// ----------------------------------------
// The first extractor swept the world's live objects looking for anything
// descending from GameItem, and read the TemplateId off the CDO. It didn't lie,
// but it answered the wrong question, and the result proved it: 27 items, ALL
// with TemplateId 0, in a world of 786,927 objects.
//
// The cause is that Unreal loads Blueprints on demand. What isn't loaded
// doesn't exist in the object array, and a freshly started server has loaded
// almost nothing. Sweeping later improves the number and never closes it: the
// list ends up depending on who opened which chest.
//
// The source of truth is elsewhere, and Andrew pointed at it on 2026-08-20:
//
//     in Conan, the "item ID" is the Template ID / Row Name of
//     /Game/Items/ItemTable. The row's ItemClass field is the main blueprint;
//     BuildingClass and VisualObject apply depending on the item.
//
// In other words: it isn't the object that has the ID — it's the TABLE, and the
// object is a FIELD of it. The table is a UDataTable, the server loads all of
// it so it can build any item, and it doesn't depend on anybody having opened
// anything.
//
// AN ITEM IS NOT A BLUEPRINT (the difference from ARK)
// ----------------------------------------------------
// In ARK "one blueprint = one item" holds, and the shop stores the blueprint's
// path. In Conan it does NOT: a simple resource like Stone (10001) has
// ItemClass pointing at the NATIVE class /Script/ConanSandbox.GameItem, the
// same one as hundreds of others. What tells Stone from Iron is the ID, not the
// class.
//
// The direct consequence for any shop: the selling key is the TemplateId, and
// NEVER the class path. A shop modelled on ARK's would sell "that class" and
// deliver the wrong item for the whole family of simple items — with no error,
// because the class exists and the delivery works.
//
// HOW THIS READS THE TABLE WITHOUT KNOWING THE ROW'S LAYOUT
// --------------------------------------------------------
// A DataTable row is a struct whose layout changes with every patch. Decoding a
// struct by offset is exactly the defect this project hunts: it works today,
// reads the neighbouring field after the next patch, and says nothing.
//
// It isn't necessary. Unreal itself exposes the table as TEXT:
//
//     GetDataTableRowNames(table)             -> the Row Names (= the TemplateIds)
//     GetDataTableColumnNames(table)          -> which columns exist
//     GetDataTableColumnAsString(table, col)  -> the whole column, already text
//
// Three calls, no layout constants. And the columns aren't chosen here: the
// plugin reads ALL of the ones the build offers. If Funcom adds one, it shows
// up in the file on its own; if they rename one, the old one drops off the list
// and the difference between two catalogues shows what changed.
#include "Conan/ConanPluginApi.h"
#include "Conan/ConanBase.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

static const ConanApiTabela* g_api = nullptr;

// ── the raw types that cross the boundary ───────────────────────────────────
//
// These aren't "guessed layouts": FName and FString are CORE Unreal types,
// stable for a decade, and the API itself already depends on them in NomeDeFName
// and LerTextoDoJogo. What this plugin NEVER does is bake in an offset of a
// GAMEPLAY field — those do move with every patch.
struct FNameCru   { int32_t indice; int32_t numero; };
struct FStringCru { void* dados; int32_t num; int32_t max; };

// A game FString is UTF-16 with a count; `num` includes the terminator. This
// converts to UTF-8 so the file comes out readable with accented names — the
// catalogue has "Poção", "Espada de Aço", and swapping accents for '?' would
// ruin precisely the field the shop owner will show the player.
static void TextoDeFString(const FStringCru& s, std::string& saida)
{
    saida.clear();
    if (!s.dados || s.num <= 1) return;
    if (!g_api->Legivel(s.dados, size_t(s.num) * 2)) return;

    const uint16_t* u = static_cast<const uint16_t*>(s.dados);
    for (int i = 0; i < s.num - 1; ++i)
    {
        uint32_t c = u[i];
        if (!c) break;
        // A surrogate pair (emoji and the like): join the two halves into one
        // code point.
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s.num - 1)
        {
            const uint32_t baixo = u[i + 1];
            if (baixo >= 0xDC00 && baixo <= 0xDFFF)
            {
                c = 0x10000 + ((c - 0xD800) << 10) + (baixo - 0xDC00);
                ++i;
            }
        }
        if (c < 0x80) saida += char(c);
        else if (c < 0x800)
        {
            saida += char(0xC0 | (c >> 6));
            saida += char(0x80 | (c & 0x3F));
        }
        else if (c < 0x10000)
        {
            saida += char(0xE0 | (c >> 12));
            saida += char(0x80 | ((c >> 6) & 0x3F));
            saida += char(0x80 | (c & 0x3F));
        }
        else
        {
            saida += char(0xF0 | (c >> 18));
            saida += char(0x80 | ((c >> 12) & 0x3F));
            saida += char(0x80 | ((c >> 6) & 0x3F));
            saida += char(0x80 | (c & 0x3F));
        }
    }
}

// ── NSLOCTEXT: unwrap it and keep the text ──────────────────────────────────
//
// FText columns don't come out as text: they come out as the expression that
// rebuilds them. The stone's name arrives like this:
//
//     NSLOCTEXT("", "ItemTable_10001_Name", "Stone")
//
// The first two fields are the namespace and the translation key; the third is
// the text. Handing back the raw expression would force EVERY consumer of the
// catalogue to write this same unwrapping — and anyone who forgot would show
// `NSLOCTEXT("", "ItemTable_10001_Name", "Stone")` in the shop, for the player
// to read.
//
// If the format doesn't match, it returns the original string instead of an
// empty one: losing the name would be worse than showing it wrapped.
static void DesembrulharNsloctext(std::string& s)
{
    static const char PREFIXO[] = "NSLOCTEXT(";
    if (s.compare(0, sizeof(PREFIXO) - 1, PREFIXO) != 0) return;
    if (s.empty() || s.back() != ')') return;

    // The text is the contents of the LAST quoted string before the closing
    // ')'. Walking backwards avoids tripping over a comma or bracket inside the
    // name itself ("Espada (curta)").
    size_t fim = s.rfind('"');
    if (fim == std::string::npos || fim == 0) return;

    // Walk back to the opening quote, respecting an escaped \".
    size_t i = fim;
    while (i > 0)
    {
        --i;
        if (s[i] != '"') continue;
        // count the backslashes immediately before: even = a real quote
        size_t barras = 0, j = i;
        while (j > 0 && s[j - 1] == '\\') { ++barras; --j; }
        if (barras % 2 == 0) break;
    }
    if (i >= fim) return;

    std::string texto = s.substr(i + 1, fim - i - 1);

    // Undo the escaping serialisation put in.
    std::string saida;
    saida.reserve(texto.size());
    for (size_t k = 0; k < texto.size(); ++k)
    {
        if (texto[k] == '\\' && k + 1 < texto.size()) { saida += texto[++k]; continue; }
        saida += texto[k];
    }
    s.swap(saida);
}

static void EscreverTextoJson(FILE* f, const char* s)
{
    std::fputc('"', f);
    for (; s && *s; ++s)
    {
        if (*s == '"' || *s == '\\') { std::fputc('\\', f); std::fputc(*s, f); }
        else if ((unsigned char)*s < 0x20) std::fprintf(f, "\\u%04x", (unsigned char)*s);
        else std::fputc(*s, f);
    }
    std::fputc('"', f);
}

// CSV the way a spreadsheet understands it: quotes doubled inside, the whole
// field in quotes. Without that, a name with a comma ("Espada, longa") splits
// the row in two and shifts EVERY column after it — silently.
static void EscreverTextoCsv(FILE* f, const std::string& s)
{
    std::fputc('"', f);
    for (char c : s)
    {
        if (c == '"') std::fputc('"', f);
        std::fputc(c, f);
    }
    std::fputc('"', f);
}

// ── finding the table ───────────────────────────────────────────────────────
//
// THE FIRST VERSION OF THIS BIT WAS WRONG, and it's worth telling because the
// mistake is easy to repeat — any SDK developer falls into it. It called:
//
//     g_api->FindObject("/Game/Items/ItemTable.ItemTable")
//
// The name suggests "find the object with this name". That isn't what the
// function does: FindObject takes a CLASS name and returns the first INSTANCE
// of it. Given an asset path, it looked for a class called
// "/Game/Items/ItemTable.ItemTable", didn't find one, and returned null —
// answering correctly the question it was asked, which wasn't the intended one.
// The log said "NAO ACHEI a tabela" when the truth was "I asked wrong".
//
// The right way is to ask for every DataTable instance and pick by name. And
// when the one we want doesn't turn up, this code LISTS what does exist:
// "didn't find it" without the list sends the owner looking in the wrong place,
// and on a modded server the table can have another name.
static void* AcharTabela(char* comoAchou, int tamComoAchou)
{
    static void* tabelas[8192];
    const int n = g_api->FindObjects("DataTable", tabelas, 8192, /*incluirFilhas=*/1);
    if (n <= 0)
    {
        g_api->Log("[itemtable] nenhuma DataTable no mundo. O servidor terminou de carregar?");
        return nullptr;
    }
    g_api->Log("[itemtable] %d DataTable(s) carregadas no mundo.", n);

    char nome[256];
    void* achada = nullptr;

    for (int i = 0; i < n; ++i)
    {
        nome[0] = 0;
        if (!g_api->NomeDoObjeto(tabelas[i], nome, sizeof(nome))) continue;
        // The DataTable class's own CDO isn't a data table.
        if (std::strncmp(nome, "Default__", 9) == 0) continue;

        if (std::strcmp(nome, "ItemTable") == 0)
        {
            achada = tabelas[i];
            char completo[512] = {0};
            g_api->NomeCompletoDoObjeto(tabelas[i], completo, sizeof(completo));
            std::snprintf(comoAchou, size_t(tamComoAchou), "%s",
                          completo[0] ? completo : nome);
            g_api->Log("[itemtable] ACHEI: %s", comoAchou);
            break;
        }
    }

    if (!achada)
    {
        // The list IS the diagnosis. Without it, "didn't find it" is a dead end.
        g_api->Log("[itemtable] NAO achei uma DataTable chamada \"ItemTable\".");
        g_api->Log("[itemtable] Estas sao as que existem agora (ate' 60):");
        int mostradas = 0;
        for (int i = 0; i < n && mostradas < 60; ++i)
        {
            nome[0] = 0;
            if (!g_api->NomeDoObjeto(tabelas[i], nome, sizeof(nome))) continue;
            if (std::strncmp(nome, "Default__", 9) == 0) continue;
            g_api->Log("[itemtable]    %s", nome);
            ++mostradas;
        }
    }
    return achada;
}

static void Extrair(void*)
{
    char comoAchou[512] = {0};
    void* tabela = AcharTabela(comoAchou, sizeof(comoAchou));
    if (!tabela)
    {
        g_api->Log("[itemtable] Se a lista acima estiver vazia ou curta, rode com o servidor");
        g_api->Log("[itemtable] JA CARREGADO: a tabela entra na carga do mundo, nao no");
        g_api->Log("[itemtable] arranque do processo.");
        return;
    }

    void* biblioteca = g_api->GetDefaultObject("DataTableFunctionLibrary");
    if (!biblioteca)
    {
        g_api->Log("[itemtable] sem DataTableFunctionLibrary nesta build — nao ha' como ler.");
        return;
    }

    // ── 1. the Row Names: they are the TemplateIds ──────────────────────────
    std::vector<FNameCru> linhas(65536u);
    int nLinhas = 0;
    ConanApi::CallSaida(biblioteca, "GetDataTableRowNames", tabela,
                        ConanApi::ParaForaLista(linhas.data(), 65536, nLinhas));

    if (nLinhas <= 0)
    {
        g_api->Log("[itemtable] a tabela existe mas devolveu ZERO linhas.");
        g_api->Log("[itemtable] Resultado zero e' hipotese, nao conclusao — pode ser tabela");
        g_api->Log("[itemtable] ainda nao populada. Tente com o mundo carregado.");
        return;
    }
    g_api->Log("[itemtable] %d linha(s) — este e' o numero de itens do jogo.", nLinhas);

    std::vector<std::string> ids((size_t)nLinhas);
    char buf[256];
    for (int i = 0; i < nLinhas; ++i)
    {
        buf[0] = 0;
        g_api->NomeDeFName(linhas[i].indice, buf, sizeof(buf));
        ids[size_t(i)] = buf;
    }
    linhas.clear();
    linhas.shrink_to_fit();

    // ── 2. ALL of this build's columns ──────────────────────────────────────
    //
    // No column list baked in here. Whatever the build has, goes in.
    std::vector<FNameCru> colunas(512u);
    int nColunas = 0;
    ConanApi::CallSaida(biblioteca, "GetDataTableColumnNames", tabela,
                        ConanApi::ParaForaLista(colunas.data(), 512, nColunas));

    g_api->Log("[itemtable] %d coluna(s) nesta build.", nColunas);

    std::vector<std::string>               nomeColuna;
    std::vector<std::vector<std::string> > valores;

    // One column at a time: 65,000 FStrings are ~1 MB, and reading all 20 at
    // once would be 20 MB sitting idle. The buffer is reused; what's kept is
    // just the text.
    std::vector<FStringCru> coluna((size_t)nLinhas);

    for (int c = 0; c < nColunas; ++c)
    {
        buf[0] = 0;
        g_api->NomeDeFName(colunas[size_t(c)].indice, buf, sizeof(buf));
        const std::string nome = buf;
        if (nome.empty()) continue;

        int quantos = 0;
        ConanApi::CallSaida(
            biblioteca, "GetDataTableColumnAsString",
            tabela, ConanApi::Nome(nome.c_str()),
            ConanApi::ParaRetornoLista(coluna.data(), nLinhas, quantos));

        // UltimaChamadaExecutou tells "the function ran and the column is
        // empty" apart from "the function doesn't exist on this build".
        // Without asking, both cases would arrive here as `quantos == 0` and
        // the log would blame the innocent one.
        if (!g_api->UltimaChamadaExecutou() || quantos <= 0)
        {
            // A column that returns no text isn't a defect: nested structs
            // and arrays don't become strings. It's said in the log and left
            // out of the file — rather than becoming a column of blanks that
            // looks like "item with no value".
            g_api->Log("[itemtable]    %-28s (nao exportavel como texto — fora do arquivo)",
                       nome.c_str());
            continue;
        }

        std::vector<std::string> textos((size_t)nLinhas);
        for (int i = 0; i < quantos && i < nLinhas; ++i)
        {
            TextoDeFString(coluna[size_t(i)], textos[size_t(i)]);
            DesembrulharNsloctext(textos[size_t(i)]);
        }

        g_api->Log("[itemtable]    %-28s ok (%d valores)", nome.c_str(), quantos);
        nomeColuna.push_back(nome);
        valores.push_back(std::move(textos));
    }

    if (nomeColuna.empty())
    {
        g_api->Log("[itemtable] NENHUMA coluna exportavel. O arquivo teria so' os IDs;");
        g_api->Log("[itemtable] nao vou gravar catalogo que nao serve para vender nada.");
        return;
    }

    // ── 3. writing: JSON for a program, CSV for a spreadsheet ───────────────
    const char* saidaJson = g_api->CaminhoDados("ExtratorItemTable", "itemtable-conan.json");
    FILE* f = std::fopen(saidaJson, "wb");
    if (!f) { g_api->Log("[itemtable] nao consegui escrever em %s", saidaJson); return; }

    std::fprintf(f, "{\n");
    std::fprintf(f, "  \"gerado_por\": \"ExtratorItemTable (plugin da Conan-Api)\",\n");
    std::fprintf(f, "  \"fonte\": \"/Game/Items/ItemTable — a DataTable canonica da Funcom\",\n");
    std::fprintf(f, "  \"o_que_e_o_id\": \"o Row Name da tabela. E' ele que entrega o item, NAO a classe:\",\n");
    std::fprintf(f, "  \"por_que\": \"itens simples compartilham a classe nativa GameItem; so' o ID distingue Pedra de Ferro\",\n");
    std::fprintf(f, "  \"build_do_jogo\": \"%s\",\n", CONAN_BUILD_DO_JOGO);
    std::fprintf(f, "  \"achada_por\": ");  EscreverTextoJson(f, comoAchou);
    std::fprintf(f, ",\n  \"colunas\": [");
    for (size_t i = 0; i < nomeColuna.size(); ++i)
    {
        if (i) std::fprintf(f, ", ");
        EscreverTextoJson(f, nomeColuna[i].c_str());
    }
    std::fprintf(f, "],\n  \"itens\": [\n");

    for (int i = 0; i < nLinhas; ++i)
    {
        if (i) std::fprintf(f, ",\n");
        std::fprintf(f, "    {\"id\": ");
        EscreverTextoJson(f, ids[size_t(i)].c_str());
        for (size_t c = 0; c < nomeColuna.size(); ++c)
        {
            const std::string& v = valores[c][size_t(i)];
            if (v.empty()) continue;            // empty field means no key
            std::fprintf(f, ", ");
            EscreverTextoJson(f, nomeColuna[c].c_str());
            std::fprintf(f, ": ");
            EscreverTextoJson(f, v.c_str());
        }
        std::fprintf(f, "}");
    }
    std::fprintf(f, "\n  ],\n  \"total\": %d\n}\n", nLinhas);
    std::fclose(f);
    g_api->Log("[itemtable] JSON: %s", saidaJson);

    const char* saidaCsv = g_api->CaminhoDados("ExtratorItemTable", "itemtable-conan.csv");
    f = std::fopen(saidaCsv, "wb");
    if (!f) { g_api->Log("[itemtable] nao consegui escrever o CSV"); return; }

    // A BOM: without it Excel opens UTF-8 as latin-1 and every accented name
    // turns to mojibake. The shop owner edits this in a spreadsheet; the BOM is
    // what makes the spreadsheet get it right on its own.
    std::fputs("\xEF\xBB\xBF", f);
    std::fputs("ID", f);
    for (const std::string& n : nomeColuna) { std::fputc(',', f); EscreverTextoCsv(f, n); }
    std::fputc('\n', f);

    for (int i = 0; i < nLinhas; ++i)
    {
        EscreverTextoCsv(f, ids[size_t(i)]);
        for (size_t c = 0; c < nomeColuna.size(); ++c)
        {
            std::fputc(',', f);
            EscreverTextoCsv(f, valores[c][size_t(i)]);
        }
        std::fputc('\n', f);
    }
    std::fclose(f);

    g_api->Log("[itemtable] CSV : %s", saidaCsv);
    g_api->Log("[itemtable] %d itens x %d colunas.", nLinhas, (int)nomeColuna.size());
}

// It answers a file trigger, for the same reason as the old extractor: reading
// the whole table is expensive for a rare event, and the owner decides WHEN.
static void VerificarPedido(void*)
{
    const char* raiz = g_api->CaminhoRaiz();
    if (!raiz) return;
    char caminho[512];
    std::snprintf(caminho, sizeof(caminho), "%s\\EXTRAIR-ITEMTABLE", raiz);

    FILE* t = std::fopen(caminho, "rb");
    if (!t) return;
    std::fclose(t);
    std::remove(caminho);

    g_api->Log("[itemtable] EXTRAIR-ITEMTABLE pedido — lendo a tabela.");
    Extrair(nullptr);
}

extern "C" __declspec(dllexport)
void ConanPluginCarregar(const ConanApiTabela* api)
{
    if (!api || api->tamanho < sizeof(ConanApiTabela)) return;
    g_api = api;
    ConanApi::UsarTabela(api);

    g_api->Log("=====================================================");
    g_api->Log(" ExtratorItemTable — o catalogo, da fonte canonica");
    g_api->Log("=====================================================");
    g_api->Log("   para extrair: crie o arquivo Conan-Api/EXTRAIR-ITEMTABLE");
    g_api->Log("   ao contrario do extrator antigo, este NAO depende de quem");
    g_api->Log("   abriu qual bau: le' a /Game/Items/ItemTable inteira.");

    g_api->AgendarNaThreadDoJogo(VerificarPedido, 20, nullptr, 1);
}

extern "C" __declspec(dllexport)
void ConanPluginDescarregar(void) {}
