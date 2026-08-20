// Config — implementation. The reasoning behind the decisions is in Config.h.
#include "Config.h"

#include "terceiros/sqlite3/sqlite3.h"

#include <cstdio>
#include <cstring>
#include <cctype>

namespace Shop
{
namespace
{
    // ── the JSON, parsed by json1 in an in-memory SQLite ────────────────────
    class Json
    {
    public:
        ~Json() { if (m_db) sqlite3_close(m_db); }

        bool Abrir(std::string& erro)
        {
            if (sqlite3_open_v2(":memory:", &m_db,
                                SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK)
            {
                erro = "nao consegui abrir o SQLite de memoria para ler o JSON";
                if (m_db) { sqlite3_close(m_db); m_db = nullptr; }
                return false;
            }
            return true;
        }

        // json_valid BEFORE any extraction. Without it, a file with one comma
        // too many returns NULL for every field and the shop comes up empty,
        // "successfully".
        bool Valido(const std::string& t)
        {
            sqlite3_stmt* st = nullptr;
            int ok = 0;
            if (sqlite3_prepare_v2(m_db, "SELECT json_valid(?1);", -1, &st, nullptr) == SQLITE_OK)
            {
                sqlite3_bind_text(st, 1, t.c_str(), -1, SQLITE_STATIC);
                if (sqlite3_step(st) == SQLITE_ROW) ok = sqlite3_column_int(st, 0);
            }
            sqlite3_finalize(st);
            return ok != 0;
        }

        // The text at a path. `achou` tells "field missing" apart from "field
        // empty" — both would arrive as "" and they mean different things.
        std::string Texto(const std::string& t, const char* caminho, bool* achou = nullptr)
        {
            if (achou) *achou = false;
            sqlite3_stmt* st = nullptr;
            std::string r;
            if (sqlite3_prepare_v2(m_db, "SELECT json_extract(?1, ?2);", -1, &st, nullptr) == SQLITE_OK)
            {
                sqlite3_bind_text(st, 1, t.c_str(), -1, SQLITE_STATIC);
                sqlite3_bind_text(st, 2, caminho, -1, SQLITE_STATIC);
                if (sqlite3_step(st) == SQLITE_ROW &&
                    sqlite3_column_type(st, 0) != SQLITE_NULL)
                {
                    const unsigned char* v = sqlite3_column_text(st, 0);
                    if (v) { r = reinterpret_cast<const char*>(v); if (achou) *achou = true; }
                }
            }
            sqlite3_finalize(st);
            return r;
        }

        int64_t Inteiro(const std::string& t, const char* caminho, int64_t padrao)
        {
            bool achou = false;
            const std::string v = Texto(t, caminho, &achou);
            if (!achou || v.empty()) return padrao;
            char* fim = nullptr;
            const long long n = std::strtoll(v.c_str(), &fim, 10);
            return (fim && *fim == 0) ? int64_t(n) : padrao;
        }

        bool Booleano(const std::string& t, const char* caminho, bool padrao)
        {
            bool achou = false;
            const std::string v = Texto(t, caminho, &achou);
            if (!achou || v.empty()) return padrao;
            if (v == "1" || v == "true")  return true;
            if (v == "0" || v == "false") return false;
            return padrao;
        }

        // Walks an object, calling `fn` per key. That's json_each, which only
        // exists in SQLite: another reason the JSON is parsed by it.
        bool CadaChave(const std::string& t, const char* caminho,
                       void (*fn)(const char* chave, const char* valor, void* ctx),
                       void* ctx, std::string& erro)
        {
            sqlite3_stmt* st = nullptr;
            const char* sql = "SELECT key, value FROM json_each(?1, ?2);";
            if (sqlite3_prepare_v2(m_db, sql, -1, &st, nullptr) != SQLITE_OK)
            { erro = sqlite3_errmsg(m_db); return false; }
            sqlite3_bind_text(st, 1, t.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_text(st, 2, caminho, -1, SQLITE_STATIC);
            int rc;
            while ((rc = sqlite3_step(st)) == SQLITE_ROW)
            {
                const unsigned char* k = sqlite3_column_text(st, 0);
                const unsigned char* v = sqlite3_column_text(st, 1);
                if (k) fn(reinterpret_cast<const char*>(k),
                          v ? reinterpret_cast<const char*>(v) : "", ctx);
            }
            sqlite3_finalize(st);
            if (rc != SQLITE_DONE) { erro = sqlite3_errmsg(m_db); return false; }
            return true;
        }

    private:
        sqlite3* m_db = nullptr;
    };

    bool LerArquivo(const char* caminho, std::string& texto, std::string& erro)
    {
        FILE* f = std::fopen(caminho, "rb");
        if (!f)
        {
            erro = std::string("nao consegui abrir ") + caminho +
                   ". Confira se o arquivo existe e se o servidor pode le-lo.";
            return false;
        }
        std::fseek(f, 0, SEEK_END);
        const long n = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (n < 0 || n > 32 * 1024 * 1024)
        {
            std::fclose(f);
            erro = "o config.json tem tamanho invalido (vazio ou acima de 32 MB)";
            return false;
        }
        texto.resize(size_t(n));
        const size_t lidos = n ? std::fread(&texto[0], 1, size_t(n), f) : 0;
        std::fclose(f);
        if (lidos != size_t(n)) { erro = "a leitura do config.json parou no meio"; return false; }
        return true;
    }

    // An item's key becomes a chat command. Refusing here is better than
    // accepting it and having the player never manage to type it.
    bool ChaveUsavel(const std::string& k, std::string& porque)
    {
        if (k.empty()) { porque = "chave vazia"; return false; }
        if (k.size() > 32) { porque = "chave com mais de 32 caracteres"; return false; }
        for (unsigned char c : k)
        {
            if (std::isalnum(c) || c == '_' || c == '-') continue;
            porque = "a chave so' pode ter letras, numeros, '-' e '_' "
                     "(e o que o jogador digita no chat)";
            return false;
        }
        return true;
    }

    struct CtxItens
    {
        Json*        j;
        Config*      cfg;
        std::string* erro;
        int          recusados;
    };
}  // namespace

const Item* Config::Achar(const std::string& chave) const
{
    for (const Item& i : itens) if (i.chave == chave) return &i;
    return nullptr;
}

const std::string& Config::Msg(const char* chave, const char* padrao) const
{
    auto it = msg.find(chave);
    if (it != msg.end() && !it->second.empty()) return it->second;
    // The default lives here, in a static per key: a message missing from the
    // file must not turn into an empty line in the player's chat.
    static std::map<std::string, std::string> padroes;
    auto p = padroes.emplace(chave, padrao);
    return p.first->second;
}

bool LerConfig(const char* caminho, Config& destino, std::string& erro)
{
    std::string texto;
    if (!LerArquivo(caminho, texto, erro)) return false;

    Json j;
    if (!j.Abrir(erro)) return false;

    if (!j.Valido(texto))
    {
        erro = "o config.json nao e' JSON valido. O erro mais comum e' virgula a "
               "mais antes de } ou ], ou aspas faltando. NADA foi alterado.";
        return false;
    }

    // Monta numa copia. So' vira a configuracao viva se chegar inteira ao fim.
    Config c;

    // ── banco ───────────────────────────────────────────────────────────────
    {
        bool achou = false;
        std::string tipo = j.Texto(texto, "$.banco.tipo", &achou);
        for (char& ch : tipo) ch = char(std::tolower((unsigned char)ch));
        if (!achou || tipo.empty()) tipo = "local";

        if (tipo == "local" || tipo == "sqlite")      c.banco.mysql = false;
        else if (tipo == "mysql" || tipo == "mariadb") c.banco.mysql = true;
        else
        {
            // A deliberate refusal: falling back to local after the owner
            // typed "mysqll" would send the points to a file they will never
            // go looking for.
            erro = "banco.tipo = \"" + tipo + "\" nao existe. Use \"local\" ou \"mysql\".";
            return false;
        }
        c.banco.host    = j.Texto(texto, "$.banco.mysql_host");
        if (c.banco.host.empty()) c.banco.host = "127.0.0.1";
        c.banco.porta   = int(j.Inteiro(texto, "$.banco.mysql_porta", 3306));
        c.banco.usuario = j.Texto(texto, "$.banco.mysql_usuario");
        c.banco.senha   = j.Texto(texto, "$.banco.mysql_senha");
        c.banco.banco   = j.Texto(texto, "$.banco.mysql_banco");
        c.banco.prazoConectarMs = int(j.Inteiro(texto, "$.banco.mysql_prazo_conectar_ms", 5000));
        c.banco.prazoOperarMs   = int(j.Inteiro(texto, "$.banco.mysql_prazo_operar_ms", 10000));
        c.banco.caminhoLocal    = j.Texto(texto, "$.banco.caminho_local");
    }

    // ── points ──────────────────────────────────────────────────────────────
    c.pontosLigados = j.Booleano(texto, "$.pontos.ligado", true);
    c.pontosMinutos = int(j.Inteiro(texto, "$.pontos.minutos", 5));
    c.pontosSomar   = j.Booleano(texto, "$.pontos.somar", false);
    c.pontosAvisar  = j.Booleano(texto, "$.pontos.avisar_no_chat", true);

    if (c.pontosMinutos < 1 || c.pontosMinutos > 1440)
    {
        erro = "pontos.minutos precisa ficar entre 1 e 1440 (um dia). "
               "Valor lido: " + std::to_string(c.pontosMinutos);
        return false;
    }

    {
        struct Ctx { Config* c; } ctx{ &c };
        std::string e;
        j.CadaChave(texto, "$.pontos.grupos",
            [](const char* k, const char* v, void* p)
            {
                if (!k || k[0] == '_') return;      // _leia_isto e afins
                Ctx* x = static_cast<Ctx*>(p);
                char* fim = nullptr;
                const long long n = std::strtoll(v ? v : "0", &fim, 10);
                if (fim && *fim == 0) x->c->pontosPorGrupo[k] = int64_t(n);
            }, &ctx, e);
    }

    // ── shop / commands / permissions ───────────────────────────────────────
    c.itensPorPagina = int(j.Inteiro(texto, "$.loja.itens_por_pagina", 12));
    if (c.itensPorPagina < 1)  c.itensPorPagina = 1;
    if (c.itensPorPagina > 40) c.itensPorPagina = 40;   // above this the screen cuts off
    c.usarTela     = j.Booleano(texto, "$.loja.usar_tela", true);
    c.tituloDaTela = j.Texto(texto, "$.loja.titulo_da_tela");
    if (c.tituloDaTela.empty()) c.tituloDaTela = "Loja";
    c.contexto     = j.Texto(texto, "$.loja.contexto");
    if (c.contexto.empty()) c.contexto = "ConanShop";

    auto cmd = [&](const char* caminho, const char* padrao)
    {
        std::string v = j.Texto(texto, caminho);
        return v.empty() ? std::string(padrao) : v;
    };
    c.cmdLoja       = cmd("$.comandos.loja",       "!shop");
    c.cmdComprar    = cmd("$.comandos.comprar",    "!comprar");
    c.cmdPontos     = cmd("$.comandos.pontos",     "!pontos");
    c.cmdAjuda      = cmd("$.comandos.ajuda",      "!shopajuda");
    c.cmdRecarregar = cmd("$.comandos.recarregar", "!shopreload");
    c.cmdDar        = cmd("$.comandos.dar",        "!shopdar");

    // Two identical commands: the first always answers and the second never
    // runs, and the owner would spend the night wondering why "!pontos doesn't
    // work".
    {
        const std::string* todos[] = { &c.cmdLoja, &c.cmdComprar, &c.cmdPontos,
                                       &c.cmdAjuda, &c.cmdRecarregar, &c.cmdDar };
        const int N = 6;
        for (int a = 0; a < N; ++a)
            for (int b = a + 1; b < N; ++b)
                if (*todos[a] == *todos[b])
                {
                    erro = "dois comandos com o mesmo texto: \"" + *todos[a] +
                           "\". Um deles nunca seria atendido.";
                    return false;
                }
    }

    c.permAdmin   = j.Texto(texto, "$.permissoes.admin");
    if (c.permAdmin.empty()) c.permAdmin = "shop.admin";
    c.permComprar = j.Texto(texto, "$.permissoes.comprar");

    // ── mensagens ───────────────────────────────────────────────────────────
    {
        struct Ctx { Config* c; } ctx{ &c };
        std::string e;
        j.CadaChave(texto, "$.mensagens",
            [](const char* k, const char* v, void* p)
            {
                if (!k || k[0] == '_') return;
                static_cast<Ctx*>(p)->c->msg[k] = v ? v : "";
            }, &ctx, e);
    }

    // ── items ───────────────────────────────────────────────────────────────
    //
    // Every item is validated. A bad item does NOT invalidate the whole file: it
    // is left out and counted, because a wrong id on item 40 shouldn't take the
    // shop down. But if NO item passes, then it is a refusal: a shop with no
    // items is a shop that charges and delivers nothing.
    {
        CtxItens ctx{ &j, &c, &erro, 0 };
        std::string e;
        // json_each returns each item's VALUE as JSON text; each object is then
        // re-read with json_extract over that text.
        struct Passagem
        {
            static void Uma(const char* chave, const char* valorJson, void* p)
            {
                CtxItens* x = static_cast<CtxItens*>(p);
                if (!chave || chave[0] == '_') return;   // _leia_isto

                std::string porque;
                if (!ChaveUsavel(chave, porque)) { ++x->recusados; return; }

                const std::string v = valorJson ? valorJson : "";
                if (v.empty() || v[0] != '{') { ++x->recusados; return; }

                Item it;
                it.chave      = chave;
                it.nome       = x->j->Texto(v, "$.nome");
                it.categoria  = x->j->Texto(v, "$.categoria");
                it.permissao  = x->j->Texto(v, "$.permissao");
                it.templateId = int32_t(x->j->Inteiro(v, "$.template_id", 0));
                it.quantidade = int32_t(x->j->Inteiro(v, "$.quantidade", 1));
                it.preco      = x->j->Inteiro(v, "$.preco", -1);

                // template_id 0 is what a missing field produces, and
                // delivering item 0 delivers nothing: the player pays and
                // receives nothing. Refusing is the only honest answer.
                if (it.templateId <= 0)   { ++x->recusados; return; }
                if (it.quantidade <= 0)   { ++x->recusados; return; }
                if (it.preco < 0)         { ++x->recusados; return; }
                if (it.nome.empty())      it.nome = it.chave;
                if (it.categoria.empty()) it.categoria = "outros";

                // A repeated key: the second one could never be bought.
                for (const Item& j2 : x->cfg->itens)
                    if (j2.chave == it.chave) { ++x->recusados; return; }

                x->cfg->itens.push_back(it);
            }
        };
        j.CadaChave(texto, "$.itens", &Passagem::Uma, &ctx, e);

        if (c.itens.empty())
        {
            erro = "nenhum item valido em \"itens\". Cada item precisa de "
                   "template_id maior que zero e preco. NADA foi alterado.";
            return false;
        }
        if (ctx.recusados > 0)
        {
            // Not an error, but it can't pass silently: the owner needs to know
            // that what they wrote didn't go in whole.
            c.msg["_aviso_itens_recusados"] = std::to_string(ctx.recusados);
        }
    }

    destino = std::move(c);
    return true;
}

}  // namespace Shop
