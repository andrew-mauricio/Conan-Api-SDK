// ============================================================================
//  ConanPermission.h — the Permission plugin's public interface
//
//  Conan Exiles Enhanced · C++ plugin API
//
//  THIS IS THE ONLY FILE A THIRD-PARTY PLUGIN NEEDS.
//  There is no library to link. No .lib, no .a, no import library. Copy this
//  header, include it, use it. That is not laziness — it is the central design
//  decision, and the three reasons are below.
//
//  A NOTE ON IDENTIFIER NAMES
//  --------------------------
//  The identifiers here are Portuguese (ConanPermTem, id_do_controller,
//  se_ausente). They are part of the published ABI and renaming them would
//  break every plugin already compiled against it. Rough guide: Tem = Has,
//  Grupos = Groups, se_ausente = if_absent, conceder = grant, revogar = revoke.
//
//  ----------------------------------------------------------------------------
//  WHY YOU DO NOT LINK AGAINST PERMISSION
//  ----------------------------------------------------------------------------
//
//  1. A PLUGIN THAT LINKS WILL NOT LOAD WITHOUT ITS TARGET.
//     An import library writes a static dependency into the PE. If the server
//     owner did not install Permission, Windows refuses LoadLibrary on the
//     third-party plugin with "module not found" and the loader records a
//     failure. A VIP plugin has to DEGRADE (answer "I do not know"), not cease
//     to exist. With runtime resolution, Permission's absence is a return
//     value, not a load error.
//
//  2. std::string AND std::vector DO NOT CROSS A DLL BOUNDARY.
//     MSVC and MinGW have incompatible layouts for std::string (different SSO
//     sizes), std::vector and std::shared_ptr; and each allocates on ITS OWN
//     heap. Passing a std::string from the plugin to a Permission built with
//     the other compiler produces no link error: it produces silent memory
//     corruption, or a free() on the wrong heap — which is the WORSE defect,
//     the one that looks like success. It even breaks between two MSVC versions
//     with different _ITERATOR_DEBUG_LEVEL. So: C types only here. const char*
//     goes in, the caller's char* comes out, fixed-width integers for the rest.
//     Whoever allocates, frees.
//
//  3. UPDATING PERMISSION MUST NOT BREAK WHAT IS ALREADY COMPILED.
//     The function table below is a POD struct with `tamanho` and `abi` at the
//     start. A new field goes ONLY AT THE END; nothing is reordered or removed,
//     ever. A plugin compiled today keeps calling the same offsets tomorrow,
//     and a new plugin discovers through `tamanho` whether the older Permission
//     that is installed has the field it wants to use. Size check plus ABI
//     check: both, because either one alone lies.
//
//  ----------------------------------------------------------------------------
//  HOW TO USE IT
//  ----------------------------------------------------------------------------
//
//      #include "Conan/ConanPermission.h"
//
//      // inside your plugin, with the PlayerController's UObject* in hand:
//      char id[CONAN_PERM_MAX_ID];
//      if (ConanPermIdDoController(pc, id, sizeof(id)) > 0)
//      {
//          // 3rd argument: what to answer if Permission is NOT installed.
//          // You choose — and you are required to choose.
//          if (ConanPermTem(id, "myplugin.kit.daily", /*if_absent=*/0) == 1)
//              GiveKit(pc);
//      }
//
//  THREE ANSWERS, NEVER TWO. Every query returns:
//      1  = allowed
//      0  = denied
//     -1  = I DO NOT KNOW (Permission absent, still starting, or empty id)
//
//  A plugin that treats "I do not know" as "denied" on its own is being wrong
//  on purpose; one that treats it as "allowed" opens the server up. That is why
//  the convenience functions require the fallback value explicitly in the
//  argument: there is no silent default.
// ============================================================================
#ifndef CONAN_PERMISSION_H
#define CONAN_PERMISSION_H

#include <stdint.h>
#include <string.h>

#if !defined(_WIN32)
#  error "Permission only exists on Conan's Windows dedicated server (which runs under Wine on Linux)."
#endif
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

// ── ABI version ─────────────────────────────────────────────────────────────
//
// Increments ONLY when an existing field changes meaning — which should never
// happen. Appending a field at the end does NOT bump the ABI: that is what
// `tamanho` is for. Keeping the two separate is what allows adding a function
// without invalidating an already-compiled plugin.
#define CONAN_PERM_ABI 1

// Buffer sizes. Fixed on purpose: a fixed-size buffer owned by the caller is
// the only way to return text across a DLL boundary without agreeing on an
// allocator.
#define CONAN_PERM_MAX_ID     64    // platform id: 17 digits today, ample slack
#define CONAN_PERM_MAX_GRUPO  64    // nome de grupo
#define CONAN_PERM_MAX_NO    128    // nó de permissão, ex. "vip.kit.diario"

// nome do arquivo, para quem quiser procurar o módulo por conta própria
#define CONAN_PERM_DLL       "ConanPermission.dll"
#define CONAN_PERM_FABRICA   "ConanPermissionObterApi"

// Um header que a comunidade inclui não pode cuspir aviso. As conveniências
// abaixo são `static` num header: quem usar duas e ignorar as outras oito
// receberia oito -Wunused-function do GCC. O atributo cala isso sem esconder
// aviso de verdade nenhum.
#if defined(__GNUC__) || defined(__clang__)
#  define CONAN_PERM_TALVEZ __attribute__((unused))
#else
#  define CONAN_PERM_TALVEZ
#endif

// ── a tabela de funções ─────────────────────────────────────────────────────
//
// POD puro: nenhum método virtual, nenhuma herança, nenhum tipo de C++.
// Uma vtable de C++ também "funcionaria", e é uma armadilha: a ordem que o
// compilador dá aos slots virtuais não é garantida pelo padrão, e MSVC e MinGW
// divergem em herança múltipla. Ponteiro de função em struct é definido pela
// ABI da plataforma, não pelo compilador.
typedef struct ConanPermApi
{
    // ── cabeçalho: SEMPRE os quatro primeiros, SEMPRE nesta ordem ───────────
    uint32_t tamanho;        // sizeof(ConanPermApi) como o PERMISSION compilou
    uint32_t abi;            // CONAN_PERM_ABI que ele implementa
    uint32_t versao;         // 10203 == 1.2.3; só para log e diagnóstico
    uint32_t reservado;      // 0 — mantém o alinhamento de 8 dos ponteiros

    // ── o que TODA string desta ABI tem de cumprir ─────────────────────────
    //
    // São `const char*` sem parâmetro de comprimento — é ABI de C, e é o preço
    // de não passar std::string por fronteira de DLL (ver o item 2 lá em cima).
    // Então o contrato tem de estar escrito:
    //
    //   · `jogador` termina em '\0' dentro de CONAN_PERM_MAX_ID bytes;
    //   · `grupo`   termina em '\0' dentro de CONAN_PERM_MAX_GRUPO bytes;
    //   · `no`      termina em '\0' dentro de CONAN_PERM_MAX_NO bytes;
    //   · `quem`    termina em '\0' dentro de 256 bytes.
    //
    // O Permission CONFERE isso e recusa (-1 / 0) o que não cumprir. Até
    // 17/08/2026 ele conferia só nulo e vazio, e depois entregava a string a
    // código que caminha até o '\0' — `Tabela::Hash`, a comparação de nó em
    // `Casa`, a construção do std::string da fila de escrita. Uma string sem
    // terminador levava esse laço para fora do mapa, na thread do jogo, FORA da
    // guarda SEH do loader: derrubava o servidor inteiro, não só o plugin. Não
    // precisava de plugin malicioso — bastava um buffer mal montado.
    //
    // O QUE A CONFERÊNCIA **NÃO** COBRE, dito aqui e não escondido:
    //   · ponteiro inválido já no primeiro byte (para ler um `const char*` é
    //     preciso dereferenciá-lo — não há defesa possível);
    //   · string sem terminador que comece a menos de N bytes do fim de uma
    //     região mapeada: a leitura limitada ainda cruza a borda.
    // Fechar os dois exigiria VirtualQuery por chamada (2.551 ns medidos sob
    // Wine, contra 145 ns da consulta inteira — 17x no laço do jogo) ou
    // variantes com comprimento na ABI. Nenhuma das duas foi feita, e por isso
    // está escrito em vez de prometido.

    // ── consulta (é o que roda no laço do jogo) ─────────────────────────────
    //
    // Responde de um instantâneo em memória, montado fora do laço. NENHUMA
    // destas funções abre arquivo, toca no SQLite, aloca memória ou pega
    // trava que um escritor possa estar segurando. Custo: um hash e, no pior
    // caso, alguns compares de curinga.
    //
    // Devolvem 1 permitido · 0 negado · -1 não sei.
    int32_t (*tem)      (const char* jogador, const char* no);
    int32_t (*no_grupo) (const char* jogador, const char* grupo);

    // Quando o VIP vence, em segundos desde 1970 (UTC).
    //   > 0  vence nessa hora
    //     0  pertence e nunca vence
    //    -1  não pertence / não sei  (use no_grupo() para separar os dois)
    int64_t (*expira_em)(const char* jogador, const char* grupo);

    // Grupos do jogador, um por linha, terminado em '\0'. Devolve o tamanho
    // que a saída PRECISARIA ter (podendo ser maior que `tam` — aí truncou),
    // ou negativo em erro. Convenção de snprintf, que todo mundo já conhece.
    int32_t (*grupos)   (const char* jogador, char* saida, int32_t tam);

    // ── identidade ──────────────────────────────────────────────────────────
    //
    // Traduz um UObject* de ConanPlayerController (ou de ConanPlayerState,
    // ou de ConanCharacter) na chave que o Permission usa.
    //
    // Isto está na ABI de PROPÓSITO: nenhum plugin de terceiro deveria
    // reimplementar a extração da identidade. O caminho tem armadilhas
    // (função de reflexão que devolve FString e vaza o buffer se chamada em
    // laço; campo que existe só depois do login completar) e é o Permission
    // que paga esse preço, uma vez, com cache. Se amanhã a Funcom trocar o
    // campo, muda AQUI e todos os plugins continuam certos.
    //
    // Devolve o comprimento do id (>0), 0 se o objeto ainda não tem
    // identidade (jogador conectando), ou negativo em erro.
    int32_t (*id_do_controller)(void* objetoDoJogo, char* saida, int32_t tam);

    // ── escrita (NUNCA no laço do jogo) ─────────────────────────────────────
    //
    // Enfileira para a thread escritora e volta na hora. `expira_em` em
    // segundos desde 1970, ou 0 para nunca. `quem` vai para o diário de
    // auditoria — quem deu VIP para quem, e quando.
    // Devolve 1 aceito · 0 recusado (grupo inexistente, id inválido) · -1 não sei.
    int32_t (*conceder)(const char* jogador, const char* grupo,
                        int64_t expira_em, const char* quem);
    int32_t (*revogar) (const char* jogador, const char* grupo, const char* quem);

    // Relê permission.json e o banco. Devolve 1 se releu, 0 se falhou.
    int32_t (*recarregar)(void);

    // ── ACRESCENTE CAMPO NOVO AQUI, NO FIM, E SÓ AQUI ───────────────────────
    // Quem acrescentar: suba `versao`, NÃO suba `abi`, e o `tamanho` cresce
    // sozinho. Plugin antigo nem percebe; plugin novo confere com
    // ConanPermTemCampo() antes de chamar.
} ConanPermApi;

// A fábrica exportada pelo ConanPermission.dll. Recebe a ABI que o CHAMADOR
// conhece; devolve NULL se não puder atender (ABI futura, ou o plugin ainda
// não terminou de subir). Passar a ABI do chamador — e não só ler a do
// callee — deixa o Permission recusar explicitamente em vez de entregar uma
// tabela que o outro lado vai interpretar errado.
typedef const ConanPermApi* (*ConanPermFabrica)(uint32_t abiDoChamador);

// ── descoberta em runtime ───────────────────────────────────────────────────
//
// Estático e inline: cada plugin fica com a sua cópia, nada é compartilhado,
// nada precisa ser linkado.
CONAN_PERM_TALVEZ static const ConanPermApi* ConanPermObter(void)
{
    static const ConanPermApi* g_api  = 0;
    static DWORD               g_prox = 0;   // quando tentar de novo

    if (g_api) return g_api;   // sucesso se guarda para sempre

    // ── POR QUE A FALHA **NÃO** É GUARDADA PARA SEMPRE ──────────────────────
    //
    // Esta é a lição que a própria ConanApi::Inicializar() já pagou: ela
    // marcava "falhou" e desistia definitivamente, e o carregamento inteiro
    // morreu no teste real — porque a primeira consulta acontece antes de o
    // alvo existir.
    //
    // Aqui o mesmo defeito tem uma causa concreta e garantida: o ConanLoader
    // carrega as DLLs na ordem que o FindFirstFile devolve, que NÃO é
    // especificada. Um plugin de VIP pode muito bem rodar o seu
    // ConanPluginCarregar() ANTES de o ConanPermission.dll existir no
    // processo. Se a ausência ficasse gravada, esse plugin ficaria cego pelo
    // resto da vida do servidor — e o dono veria "VIP não funciona" sem uma
    // linha de erro em lugar nenhum.
    //
    // Também não se pode tentar a cada chamada: consulta em laço de jogo
    // chamaria GetModuleHandle 60 vezes por segundo para sempre. Então:
    // reconsulta no máximo uma vez a cada 3 segundos.
    const DWORD agora = GetTickCount();
    if (g_prox && (int32_t)(agora - g_prox) < 0) return 0;
    g_prox = agora + 3000;

    // GetModuleHandle primeiro: se o loader já carregou o Permission, é ele
    // que se usa. LoadLibrary só como segunda tentativa — e por caminho
    // absoluto derivado do executável, nunca relativo: o CWD do servidor não
    // é a pasta do binário, e caminho relativo aqui viraria busca em DLL
    // search path, que é justamente o vetor que o próprio loader usa para
    // entrar. Não se convida esse risco para dentro.
    HMODULE m = GetModuleHandleA(CONAN_PERM_DLL);
    if (!m)
    {
        // ── a pasta oficial é Conan-Api; a antiga fica por compatibilidade ──
        //
        // O loader monta `<Win64>\Conan-Api\Plugins\*.dll` (ConanLoader.cpp) e a
        // distribuição é `distribuicao/Conan-Api/`. Então a primeira tentativa é
        // essa, e é a que vai valer sempre.
        //
        // A pasta SEM hífen continua sendo tentada em segundo lugar de
        // propósito: houve uma fase em que o loader procurava lá, e alguém pode
        // ter uma instalação daquela época. Tentar as duas custa um
        // LoadLibrary que falha; NÃO tentar custaria um servidor onde o VIP
        // simplesmente não funciona, sem erro nenhum para investigar.
        //
        // Esta é a ÚNICA menção legítima à pasta antiga em todo plugins/: é
        // busca por compatibilidade, não instrução de instalação. Por isso leva
        // o selo abaixo, que plugins/conferir-caminhos.sh usa para não a
        // confundir com o defeito real (três arquivos mandando o dono INSTALAR
        // na pasta errada, e o plugin nunca carregando, sem erro no log).
        // A PRIMEIRA é a de hoje: cada plugin numa pasta própria, então o
        // Permission mora em Plugins\Permission\. As duas seguintes são
        // instalações de fases anteriores — DLL solta em Plugins\, e antes
        // disso a pasta sem hífen. Tentar as três custa dois LoadLibrary que
        // falham; não tentar custaria um servidor onde o VIP simplesmente não
        // funciona, sem erro nenhum para investigar.
        //
        // Estas são as ÚNICAS menções legítimas às pastas antigas em todo
        // plugins/: é busca por compatibilidade, não instrução de instalação.
        // Por isso levam o selo abaixo, que plugins/conferir-caminhos.sh usa
        // para não as confundir com o defeito real (arquivos mandando o dono
        // INSTALAR na pasta errada, e o plugin nunca carregando, calado).
        static const char* const pastas[] = { "\\Conan-Api\\Plugins\\Permission\\",
                                              "\\Conan-Api\\Plugins\\",          /* PASTA-ANTIGA-DE-PROPOSITO */
                                              "\\ConanApi\\Plugins\\" };         /* PASTA-ANTIGA-DE-PROPOSITO */
        char raiz[MAX_PATH];
        DWORD n = GetModuleFileNameA(0, raiz, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) return 0;
        {
            char* barra = strrchr(raiz, '\\');
            if (!barra) return 0;
            *barra = 0;
        }
        {
            int i;
            for (i = 0; i < 3 && !m; ++i)
            {
                char caminho[MAX_PATH];
                if (strlen(raiz) + strlen(pastas[i]) + sizeof(CONAN_PERM_DLL) > MAX_PATH)
                    continue;
                strcpy(caminho, raiz);
                strcat(caminho, pastas[i]);
                strcat(caminho, CONAN_PERM_DLL);
                m = LoadLibraryA(caminho);
            }
        }
        if (!m) return 0;
    }

    {
        ConanPermFabrica f = (ConanPermFabrica)(void*)
                             GetProcAddress(m, CONAN_PERM_FABRICA);
        const ConanPermApi* a = f ? f(CONAN_PERM_ABI) : 0;

        // Conferir o que voltou antes de confiar. Um Permission com ABI
        // diferente, ou com struct menor que o cabeçalho, é tratado como
        // ausente — melhor cego e sabendo do que chamando ponteiro que não
        // existe. `tamanho` menor que os 4 uint32 do cabeçalho é lixo.
        if (!a || a->abi != CONAN_PERM_ABI || a->tamanho < 16) return 0;
        g_api = a;
    }
    return g_api;
}

// O Permission está instalado e de pé?
CONAN_PERM_TALVEZ static int ConanPermDisponivel(void) { return ConanPermObter() != 0; }

// Versão do plugin instalado (10203 == 1.2.3), ou 0.
CONAN_PERM_TALVEZ static uint32_t ConanPermVersao(void)
{ const ConanPermApi* a = ConanPermObter(); return a ? a->versao : 0u; }

// O Permission instalado é novo o bastante para ter o campo em `offset_do_fim`?
// Use com offsetof() + sizeof() do campo que você quer chamar. Existe para o
// dia em que a tabela crescer: plugin novo contra Permission velho.
CONAN_PERM_TALVEZ static int ConanPermTemCampo(uint32_t bytesNecessarios)
{ const ConanPermApi* a = ConanPermObter(); return a && a->tamanho >= bytesNecessarios; }

// ── conveniências ───────────────────────────────────────────────────────────
//
// Todas exigem `se_ausente` explícito. Não há sobrecarga sem esse argumento de
// propósito: o autor do plugin TEM de decidir o que acontece quando o
// Permission não está instalado, e a decisão fica escrita na chamada, visível
// em code review, em vez de escondida num padrão.
CONAN_PERM_TALVEZ static int32_t ConanPermTem(const char* jogador, const char* no, int32_t se_ausente)
{
    const ConanPermApi* a = ConanPermObter();
    if (!a || !a->tem || !jogador || !no || !jogador[0]) return se_ausente;
    {
        int32_t r = a->tem(jogador, no);
        return (r == 1 || r == 0) ? r : se_ausente;
    }
}

CONAN_PERM_TALVEZ static int32_t ConanPermNoGrupo(const char* jogador, const char* grupo, int32_t se_ausente)
{
    const ConanPermApi* a = ConanPermObter();
    if (!a || !a->no_grupo || !jogador || !grupo || !jogador[0]) return se_ausente;
    {
        int32_t r = a->no_grupo(jogador, grupo);
        return (r == 1 || r == 0) ? r : se_ausente;
    }
}

CONAN_PERM_TALVEZ static int64_t ConanPermExpiraEm(const char* jogador, const char* grupo)
{
    const ConanPermApi* a = ConanPermObter();
    if (!a || !a->expira_em || !jogador || !grupo) return -1;
    return a->expira_em(jogador, grupo);
}

CONAN_PERM_TALVEZ static int32_t ConanPermGrupos(const char* jogador, char* saida, int32_t tam)
{
    const ConanPermApi* a = ConanPermObter();
    if (saida && tam > 0) saida[0] = 0;
    if (!a || !a->grupos || !jogador) return -1;
    return a->grupos(jogador, saida, tam);
}

// O atalho que a maioria dos plugins vai usar: do objeto do jogo para o id.
CONAN_PERM_TALVEZ static int32_t ConanPermIdDoController(void* objetoDoJogo, char* saida, int32_t tam)
{
    const ConanPermApi* a = ConanPermObter();
    if (saida && tam > 0) saida[0] = 0;
    if (!a || !a->id_do_controller || !objetoDoJogo) return -1;
    return a->id_do_controller(objetoDoJogo, saida, tam);
}

CONAN_PERM_TALVEZ static int32_t ConanPermConceder(const char* jogador, const char* grupo,
                                 int64_t expira_em, const char* quem)
{
    const ConanPermApi* a = ConanPermObter();
    if (!a || !a->conceder || !jogador || !grupo) return -1;
    return a->conceder(jogador, grupo, expira_em, quem ? quem : "");
}

CONAN_PERM_TALVEZ static int32_t ConanPermRevogar(const char* jogador, const char* grupo, const char* quem)
{
    const ConanPermApi* a = ConanPermObter();
    if (!a || !a->revogar || !jogador || !grupo) return -1;
    return a->revogar(jogador, grupo, quem ? quem : "");
}

#ifdef __cplusplus
}   // extern "C"
#endif
#endif  // CONAN_PERMISSION_H
