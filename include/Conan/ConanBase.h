// ============================================================================
//  ConanBase.h — foundation of the Conan Exiles Enhanced plugin API
//
//  Conan Exiles Enhanced · Unreal Engine 5.6.1 (++exiles+release)
//
//  The only hand-written header. Everything else (ConanSDK.h, thousands of
//  classes) is generated from the server's live reflection data.
//
//  A NOTE ON IDENTIFIER NAMES
//  --------------------------
//  Identifiers in the ABI are Portuguese (ConanPluginCarregar, LerTextoDoJogo,
//  CONAN_CANCELAR). They are part of the published ABI, and renaming them would
//  break every plugin already compiled against it. All documentation is in
//  English; a glossary is in Docs/DEVELOPERS.md.
//
//  THE IDEA
//  --------
//  Funcom publishes neither an SDK nor PDBs for the dedicated server. But
//  Unreal loads its own reflection metadata into memory in order to function,
//  and that metadata is present in any running UE process. Reading it makes it
//  possible to reconstruct the catalogue of classes, members and functions
//  without depending on anyone.
//
//  MEMBER ACCESS
//  -------------
//  A member is not a struct field — it is read at a measured offset, through
//  FieldRef. Reproducing the game's struct field by field looks more elegant,
//  but a single padding mistake misaligns everything below it SILENTLY, and the
//  defect only surfaces as memory corruption at runtime. With FieldRef, a wrong
//  offset gets ONE field wrong — the error stays isolated and visible.
//
//  FUNCTION CALLS
//  --------------
//  By NAME, never by address. An address baked into a plugin is a time bomb: on
//  update day the plugin calls the wrong place and the server dies with no
//  readable error. A name costs one lookup on the first call (then cached) and
//  survives updates.
// ============================================================================
#pragma once

// THE TABLE. Until v2.4.0 this header did not know about it: the runtime spoke
// through internal helpers and the plugin through `api->`, two separate paths.
// v6 joins them, because that is what lets ConanSDK.h (thousands of classes
// with real signatures) work without linking a static library of ours — which
// was the actual reason it never shipped in the package the README advertised.
#include "ConanPluginApi.h"

// <atomic> because of the offset cache in the generated accessors: a
// `static int32_t` read and written by two threads is a formal data race, even
// when both write the same value — and ProcessEvent arrives from 34 threads on
// this build.
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>
#include <utility>

// ── opening a file without a warning on either compiler ─────────────────────
//
// MSVC marks `fopen` as unsafe (C4996) and warns on every include. The easy way
// out would be defining _CRT_SECURE_NO_WARNINGS, but that switches off ALL CRT
// security warnings in the project of whoever compiles a plugin — a decision
// that is not ours to make for them. And a warning that always appears is a
// warning nobody reads: before long the developer ignores the ones that matter
// too.
#include <cstdio>
#if defined(_MSC_VER)
  #define CONAN_FOPEN(fh, caminho, modo)  do { \
        FILE* _f = nullptr; \
        (fh) = (fopen_s(&_f, (caminho), (modo)) == 0) ? _f : nullptr; \
      } while (0)
#else
  #define CONAN_FOPEN(fh, caminho, modo)  do { (fh) = std::fopen((caminho), (modo)); } while (0)
#endif

// alloca mora em <malloc.h> no Windows (MSVC e MinGW) e em <alloca.h> no resto
#if defined(_WIN32)
  #include <malloc.h>
  #define CONAN_ALLOCA(n) _alloca(n)
#else
  #include <alloca.h>
  #define CONAN_ALLOCA(n) alloca(n)
#endif

// ── convenção de chamada ────────────────────────────────────────────────────
//
// No x64 do Windows existe UMA convenção nativa só. `__fastcall` não muda nada
// ali: o MinGW aceita em silêncio, o MSVC aceita mas AVISA que ignorou, e num
// projeto que a comunidade vai compilar, aviso do compilador é ruído que faz
// gente parar e perguntar se está errado. Deixar vazio diz a verdade.
#define CONAN_CALL

// Declaradas ANTES do namespace: se `class UClass*` aparecesse só dentro de
// ConanApi, o compilador criaria ConanApi::UClass e os headers gerados (que
// usam ::UClass) não enxergariam o mesmo tipo.
class UObject;
class UClass;
class UFunction;

namespace ConanApi
{
    // Declaradas ANTES de Call(), que as usa. Num template, nome que não depende
    // de parâmetro de template é resolvido na definição — declarar depois dá
    // erro, e o erro aponta para a linha do uso, não para a causa.
    void Log(const char* fmt, ...);

    // ── did the last call actually execute? ─────────────────────────────────
    //
    // `Call<R>` returns a value even when the function does not run — there is
    // no other way, the return type is R. But returning zero silently would be
    // the very defect the sentinel exists to kill. Whoever needs the distinction
    // asks:
    //
    //     bool v = actor->GetActorEnableCollision();
    //     if (!ConanApi::UltimaChamadaExecutou()) { /* absence, not an answer */ }
    //
    // It answers `false` on ALL paths where the returned value is an absence:
    //   · the object is null;
    //   · the function does not exist on that class (wrong name, or it
    //     disappeared in a game update) — this is the most common case, and it
    //     was the one left out: the `return` from a missing function left
    //     without touching the flag, which starts out `true`, and the plugin
    //     received zero along with confirmation that it was an answer;
    //   · an argument did not fit the parameter (the call was not made);
    //   · a typed return was requested that the function has nowhere to put;
    //   · ProcessEvent filtered the call (the 0xCD sentinel, further down).
    //
    // It is per thread: the server has 34+ threads, and a global here would let
    // one thread read another's result.
    bool UltimaChamadaExecutou();
    void MarcarExecucao(bool ok);

    // ── o que a reflexão precisa saber de uma função para poder chamá-la ────
    //
    // Resolver isso custa caminhar a hierarquia de classes e a cadeia de
    // propriedades. Feito UMA vez por (classe, nome) e guardado — chamada em
    // laço de jogo não pode pagar busca.
    // Quantos parâmetros cabem na ficha. Quem preenche deriva o teto de
    // `sizeof(Offset)/sizeof(Offset[0])`, então este número manda sozinho.
    constexpr int MAX_PARMS = 24;

    struct FuncInfo
    {
        void*    Function;          // a UFunction
        uint16_t ParmsSize;         // tamanho do bloco de parâmetros
        uint8_t  NumParms;          // quantos parâmetros, retorno incluído
        uint8_t  NumEntrada;        // quantos são de ENTRADA (sem o retorno)
        uint16_t Offset[MAX_PARMS]; // offset de cada parâmetro dentro do bloco
        uint16_t OffsetRetorno;     // 0xFFFF quando a função não devolve nada

        // ── SIZE of each parameter (reflection's ElementSize) ───────────────
        //
        // WHY THESE TWO FIELDS HAD TO EXIST
        // ----------------------------------
        // The function record only carried the OFFSET. With that, `Empacotar`
        // wrote sizeof(T) of the plugin's argument at the parameter's offset and
        // stopped there — never asking how many bytes the parameter actually is.
        // The defect that opens is told in full in `EspacoNoBloco()`, just
        // below.
        //
        // 0 = whoever filled this record did NOT measure the size. That is not
        // an error and disables no guard: validation falls back to the limit
        // derived from offsets, which is conservative and never rejects a
        // correct call. With the measured size the same guard becomes exact and
        // also starts catching the inverse case — an argument SMALLER than the
        // slot, which corrupts nothing and delivers a wrong number (a 4-byte
        // `float` in an 8-byte DoubleProperty becomes a denormal: 3.5f read as a
        // double gives 5.336073e-315). There are 1,088 DoubleProperty input
        // parameters on this build, and ExemploOla documents that case because
        // it has been through it.
        //
        // They sit at the END of the struct on purpose: a new field in the
        // middle would shift Offset[] and OffsetRetorno for anything compiled
        // earlier. At the end of the struct, a mismatch moves no existing field.
        uint16_t Tamanho[MAX_PARMS];
        uint16_t TamanhoRetorno;
    };

    void*            ModuleBase();
    UClass*          FindClass(const char* nome);
    const FuncInfo*  ResolveFunction(void* obj, const char* nome);
    void             InvokeRaw(void* obj, void* function, void* parms);

    // ── achar objetos vivos ─────────────────────────────────────────────────
    //
    // Sem isto os headers são só um mapa: o plugin sabe ler qualquer membro de
    // qualquer classe e não tem de onde tirar o primeiro ponteiro. É por aqui
    // que um plugin começa — pega o GameMode, a lista de PlayerControllers, os
    // Actors de um tipo — e daí navega pelo resto.
    //
    // Varrer o GUObjectArray custa: são 1,5 milhão de objetos num servidor
    // cheio. Chame no arranque ou de tempos em tempos, nunca a cada tick.
    int      NumObjects();
    UObject* GetObjectByIndex(int i);

    // primeiro objeto da classe (ou de uma filha dela, com incluirFilhas)
    UObject* FindObject(const char* nomeClasse, bool incluirFilhas = true);

    // todos eles; devolve quantos couberam em `saida`
    int      FindObjects(const char* nomeClasse, UObject** saida, int max,
                         bool incluirFilhas = true);

    // o objeto padrão da classe (CDO) — existe mesmo sem instância no mundo
    UObject* GetDefaultObject(const char* nomeClasse);

    // ── ferramentas de baixo nível ──────────────────────────────────────────
    // Expostas porque plugin de DESCOBERTA precisa delas: decodificar um FName
    // que a API ainda não modela, ou conferir se um ponteiro é seguro antes de
    // seguir. Plugin comum não precisa tocar nisto.
    std::string NomeDeFName(int32_t indice);

    // Acha o FName (índice + Number) de uma função com este nome, em qualquer
    // classe. Serve para VALIDAR o nome antes de registrar um hook: nome escrito
    // errado produz um hook que nunca dispara, e é o defeito mais silencioso que
    // existe neste terreno.
    bool AcharFNameDeFuncao(const char* nome, int32_t* indice, int32_t* numero);

    // ── versão C de Call<>, para a tabela que o plugin recebe ───────────────
    //
    // Mesma coisa que Call<>, com argumentos em vetor em vez de template: o
    // plugin é C e não instancia template nosso. Confere o tamanho de cada
    // argumento contra o parâmetro real e RECUSA quando não bate.
    bool ChamarPorTabela(void* obj, const char* nome,
                         const void** args, const uint32_t* tams, int nargs,
                         void* retorno, uint32_t tamRetorno);

    // Mesma coisa, com os slots de SAIDA. Declarada aqui e definida no
    // ConanApi.cpp: e' o motor. O plugin nao chama isto direto — chega nela
    // pela tabela (ChamarFuncaoEx), que e' o que permite ao ConanSDK.h
    // funcionar sem linkar biblioteca nossa.
    bool ChamarPorTabelaEx(void* obj, const char* nome,
                           const void** args, const uint32_t* tams, int nargs,
                           const ConanSaida* saidas, int nsaidas,
                           void* retorno, uint32_t tamRetorno);
    bool        Legivel(const void* p, size_t n);

    // "Dá para ESCREVER aqui?" — pergunta diferente de Legivel(), e a diferença
    // derruba servidor: PAGE_READONLY passa em Legivel e falha no memcpy.
    bool        Gravavel(void* p, size_t n);

    // Falar com o jogador. Monta a FString com memória DO JOGO (o jogo aloca,
    // nós sobrescrevemos), que é o que impede a queda ao ProcessEvent destruir
    // o bloco de parâmetros.
    bool        MensagemParaTodos(const char* texto);
    bool        MensagemParaJogador(const char* nomeDoJogador, const char* texto);
    // Na TELA do jogador (não no chat). `playerController` é o dele.
    bool        MensagemNaTela(void* playerController, const char* texto, float segundos);

    // ── onde o plugin guarda as coisas dele ─────────────────────────────────
    //
    // Todo plugin precisa de um lugar para configuração e dados, e "o diretório
    // atual" não serve: o cwd do processo do jogo não é garantido e pode mudar.
    // Estes caminhos são derivados do endereço do EXECUTÁVEL, que é fixo.
    //
    // A árvore é única e fica ao lado do executável:
    //
    //     <Win64>/Conan-Api/Plugins/     as DLLs
    //     <Win64>/Conan-Api/Config/      um arquivo por plugin
    //     <Win64>/Conan-Api/Dados/       bancos e estado
    //     <Win64>/Conan-Api/Logs/        registros
    //
    // A pasta é criada se não existir. Plugin que falha por falta de pasta é
    // plugin que falha na instalação de metade das pessoas.
    const char* CaminhoRaiz();                       // <Win64>/Conan-Api
    const char* CaminhoConfig(const char* plugin);   // .../Config/<plugin>.json
    // ── caminhos: UMA PASTA POR PLUGIN ──────────────────────────────────────
    //
    // Cada plugin mora em Conan-Api\\Plugins\\<SeuPlugin>\\ e guarda tudo ali:
    //
    //     CaminhoConfig("SeuPlugin")                 -> .../Plugins/SeuPlugin/config.json
    //     CaminhoDados("SeuPlugin", "banco.db")      -> .../Plugins/SeuPlugin/banco.db
    //
    // O nome que você passa é o da PASTA. Se ela não existir, cai no esquema
    // antigo (Config\ e Dados\ globais) em vez de devolver caminho inexistente —
    // instalação antiga continua funcionando.
    const char* CaminhoConfig(const char* plugin);
    const char* CaminhoDados(const char* plugin, const char* arquivo);
    const char* CaminhoDados(const char* arquivo);   // antiga: .../Dados/<arquivo>

    // ── registro de plugin ──────────────────────────────────────────────────
    // O loader chama isto; o plugin não precisa saber como foi carregado.
    void     Log(const char* fmt, ...);
    bool     Pronta();          // as âncoras conferiram? só então é seguro usar

    // ── how much may be written starting at an offset in the block ──────────
    //
    // THE DEFECT THIS FIXES
    // ---------------------
    // `Empacotar` used to copy sizeof(T) from the plugin's argument into the
    // parameter's offset without looking at the parameter's size — and the
    // comment right above it PROMISED the opposite ("never corrupt the server's
    // stack"). The buffer has exactly ParmsSize bytes, from an alloca.
    //
    // `ActorComponent::SetComponentTickInterval` has parmssize=4 and a single
    // 4-byte FloatProperty at offset 0. The most natural line in C++ — a literal
    // without the `f` suffix:
    //
    //     comp->Call<void>("SetComponentTickInterval", 0.5);
    //
    // wrote 8 bytes into an alloca of 4. Reproduced before the fix, under
    // AddressSanitizer (native g++, the same header):
    //
    //     ERROR: AddressSanitizer: dynamic-stack-buffer-overflow
    //     WRITE of size 8 ... ConanApi::Empacotar<double> ... ConanBase.h:193
    //
    // There are 1,468 functions on this build with parmssize=4 and a single
    // input parameter — every one of them is that trigger. If the call comes
    // from inside a hook callback, the corrupted stack is the game thread's.
    //
    // THE LIMIT, AND WHY IT DOES NOT WAIT FOR ANYONE TO MEASURE ANYTHING
    // -------------------------------------------------------------------
    // Parameters sit side by side in a single block. The ceiling for one
    // starting at `off` is the next offset ABOVE it — another parameter's, or
    // the return value's — and, failing both, the end of the block. The
    // function record already knows this today.
    //
    // Writing past the END OF THE BLOCK corrupts the caller's stack. Writing
    // past the end of the PARAMETER corrupts the neighbouring parameter: the
    // function runs, with an argument nobody asked for, and the symptom appears
    // far from the cause.
    //
    // CALIBRATION (against the catalogue: the 42,767 input parameters of the
    // 22,913 functions with inputs on this build, comparing the derived limit
    // against the ElementSize reflection reports):
    //     limit == real size ................................. 35,184 (82.27%)
    //     limit  > real size (alignment slack) ................ 7,583 (17.73%)
    //     limit  < real size ....................................... 0
    // The zero on that last line is what decides it: the limit NEVER falls below
    // the real size, so this guard rejects no correct call. In the 17.73% with
    // slack it is conservative — what closes that gap is `Tamanho[]`, once
    // measured. And it does reject the defect: among the 10,877 input parameters
    // whose limit is under 8 bytes, a `double` literal does not get through.
    inline uint32_t EspacoNoBloco(const FuncInfo* fi, uint32_t off)
    {
        uint32_t fim = fi->ParmsSize;
        const int n = (fi->NumEntrada < MAX_PARMS) ? int(fi->NumEntrada) : MAX_PARMS;
        for (int j = 0; j < n; ++j)
        {
            const uint32_t o = fi->Offset[j];
            if (o > off && o < fim) fim = o;
        }
        if (fi->OffsetRetorno != 0xFFFF)
        {
            const uint32_t r = fi->OffsetRetorno;
            if (r > off && r < fim) fim = r;
        }
        return (fim > off) ? (fim - off) : 0u;
    }

    // ── empacotar os argumentos no bloco de parâmetros ──────────────────────
    //
    // Devolve `false` quando ALGUM argumento não coube. Quem chama aborta a
    // chamada inteira: empacotar o que deu e chamar assim mesmo seria trocar
    // corrupção de pilha por um argumento zerado — a função rodaria e devolveria
    // um resultado plausível e falso, que é o defeito que esta API existe para
    // matar.
    // Monta uma FString com memoria do JOGO. 16 bytes: {wchar_t* Data; int32
    // Num; int32 Max}. Ver o comentario em ConanApi.cpp — o plugin nunca pode
    // montar isso sozinho (I-2).
    bool CriarTextoDoJogo(const char* texto, void* destino16Bytes);

    // ── TEXTO COMO ARGUMENTO ────────────────────────────────────────────────
    //
    // O que o plugin escreve:      ator->SetNome(ConanApi::Texto("Andrew"));
    // O que chega ao jogo:         uma FString que o JOGO alocou.
    //
    // O tempo de vida e' o da expressao da chamada, que basta: o jogo copia ou
    // consome durante o ProcessEvent, e destroi o bloco ao retornar.
    // FText: o mesmo principio, um passo a mais. Ver ConanApi.cpp.
    bool CriarTextoRicoDoJogo(const char* texto, void* destino16Bytes);

    // ── TEXTO RICO (FText) COMO ARGUMENTO ───────────────────────────────────
    //
    //     pc->ClientShowMessageBox(ConanApi::TextoRico("Titulo"),
    //                              ConanApi::TextoRico("Mensagem"));
    //
    // FText e' o que as funcoes de INTERFACE do Conan pedem — notificacao,
    // caixa de mensagem, rotulo. Sem isto, 838 parametros ficavam sem tipo.
    struct TextoRico
    {
        unsigned char bruto[16];
        bool valido;
        explicit TextoRico(const char* s) : bruto{}, valido(false)
        { valido = CriarTextoRicoDoJogo(s, bruto); }
    };

    struct Texto
    {
        unsigned char bruto[16];
        bool valido;
        explicit Texto(const char* s) : bruto{}, valido(false)
        { valido = CriarTextoDoJogo(s, bruto); }
    };

    // ── FName AS AN ARGUMENT ────────────────────────────────────────────────
    //
    //     character->SpawnTemplateItem(10001, ConanApi::Nome("shop"), 10, ...)
    //
    // WHY THIS HAD TO EXIST
    // ---------------------
    // An FName is a reference to an entry in the process's name pool — two
    // int32s, and neither can be invented: they are the index of the text
    // INSIDE that run's pool. Building {0,0} sends "None"; building an arbitrary
    // number sends whatever name sits at that position, which exists, is valid,
    // and is NOT what was asked for. The game accepts it and does something
    // else, without an error.
    //
    // Until 2026-08-20 the SDK had no such bridge, and the consequence was
    // large: every function taking an FName parameter was out of a plugin's
    // reach — including SpawnTemplateItem, which is how you hand an item to a
    // player, and AddItemTemplate, which is how you put an item into an
    // inventory. A shop was impossible to write, and the reason showed up
    // nowhere.
    //
    // WHY THROUGH THE GAME'S FUNCTION, RATHER THAN READING THE POOL
    // -------------------------------------------------------------
    // Scanning the name pool for the text would also yield the index, at the
    // cost of walking hundreds of thousands of entries with arithmetic over the
    // pool's internal layout — a layout Epic has already changed between Unreal
    // versions. Conv_StringToName is the same bridge Blueprint uses, has a
    // public signature, and does what FName genuinely requires: if the name does
    // not exist in the pool yet, it CREATES it. A pool search would answer "not
    // found" for a new name — which is precisely the case of a plugin inventing
    // a context of its own.
    //
    // The constructor is defined further down, once `Call` exists.
    struct Nome
    {
        unsigned char bruto[8];   // sizeof(FName): ComparisonIndex + Number
        bool valido;
        explicit Nome(const char* s);
    };

    // ── TEXTO COMO SAIDA: ForaTexto ─────────────────────────────────────────
    //
    // POR QUE E' UM TIPO SEPARADO DO Fora<T>
    // --------------------------------------
    // Fora<T> copia sizeof(T) bytes do slot para o destino. Isso esta certo para
    // int, float, FVector — e ERRADO para FString: copiar os 16 bytes daria ao
    // plugin um ponteiro para memoria do JOGO, que o ProcessEvent destroi ao
    // retornar. O plugin ficaria com ponteiro pendurado, e leria memoria
    // liberada — pior que nao ter a saida.
    //
    // O que este tipo faz: DECODIFICA. Le a FString do slot enquanto ela ainda
    // vale, converte UTF-16 para o buffer do plugin, e nada do jogo atravessa a
    // fronteira. Foram 238 funcoes desta build que sairam sem assinatura por
    // causa disso.
    //
    //     char nome[128];
    //     obj->GetStringAttribute(..., ConanApi::ParaForaTexto(nome, sizeof(nome)));
    // Decodifica a FString que esta NESTE slot para um buffer do plugin.
    // Declarada aqui e implementada no ConanApi.cpp, que e' onde vive a leitura
    // validada de memoria do jogo.
    // Refaz a parte da conferencia de build que ficou pendente no arranque.
    // Chamada pelo carregador DEPOIS de o mundo montar — no arranque as
    // UFunction nativas ainda nao existem, e o I-4 sairia "4 de 6" para sempre.
    bool ReconferirBuild();

    // Arma os hooks que foram PEDIDOS antes de a reflexao existir. Chamada uma
    // vez, no instante em que o mundo monta — e' o que faz um comando responder
    // no primeiro segundo em vez de depois de o carregador ativar os plugins.
    void DrenarPendentes();

    // ── MEMBRO POR NOME, RESOLVIDO EM RUNTIME ───────────────────────────────
    //
    // Devolve o offset do membro nesta build, ou -1 se nao achar. Sobe a
    // hierarquia e guarda em cache por (classe, nome).
    //
    // POR QUE ISTO IMPORTA MAIS DO QUE PARECE: com resolucao por nome, a
    // superficie que uma atualizacao do jogo quebra deixa de ser 36.210 offsets
    // gravados e passa a ser CINCO ponteiros (GUObjectArray, FNamePool,
    // ProcessEvent, ProcessInternal e o layout da FField). O resto e' derivado.
    //
    // -1 NUNCA e' confundido com offset 0, que e' legitimo.
    int32_t OffsetDoMembro(void* objeto, const char* nome,
                           uint32_t* tamanho = nullptr, uint64_t* flags = nullptr);

    // 1 replicado · 0 nao · -1 nao sei. Os tres estados importam: -1 NAO e
    // "pode escrever". Ver o comentario em ConanApi.cpp.
    int  EhReplicadoPorNome(void* objeto, const char* nome);
    int  EhReplicadoPorOffset(void* objeto, uint32_t offset);
    bool NomeDoMembroNoOffset(void* objeto, uint32_t offset, char* saida, int tam);

    int TextoDeSlot(const void* slot16Bytes, char* saida, int tam);

    // FText do slot -> char*, passando pelo Conv_TextToString do jogo. Ver
    // ConanApi.cpp: FText nao guarda caracteres, guarda referencia contada.
    int TextoRicoDeSlot(const void* slot16Bytes, char* saida, int tam);

    // Saida de FText. Separado do ForaTexto porque a decodificacao passa por
    // uma chamada a mais no jogo — e confundir os dois daria texto vazio sem
    // dizer por que.
    struct ForaTextoRico
    {
        char* destino;
        int   tam;
        ForaTextoRico(char* d, int t) : destino(d), tam(t) { if (d && t > 0) d[0] = 0; }
    };
    inline ForaTextoRico ParaForaTextoRico(char* d, int t) { return ForaTextoRico(d, t); }

    struct ForaTexto
    {
        char* destino;
        int   tam;
        ForaTexto(char* d, int t) : destino(d), tam(t) { if (d && t > 0) d[0] = 0; }
    };
    inline ForaTexto ParaForaTexto(char* d, int t) { return ForaTexto(d, t); }

    // ── ENTRADA E SAIDA AO MESMO TEMPO: EntreSai<T> ─────────────────────────
    //
    // POR QUE ISTO PRECISOU EXISTIR
    // ------------------------------
    // 4.777 parametros desta build tem CPF_OutParm E CPF_ReferenceParm: o
    // `UPARAM(ref)` da Unreal. Eles chegam por referencia JA PREENCHIDOS pelo
    // chamador, e a funcao pode escrever de volta.
    //
    // Ate 19/08/2026 eles eram classificados como saida PURA — e a consequencia
    // era silenciosa: em BoxOverlapActors, o filtro de tipos e a lista de atores
    // a ignorar sao desses. Tratados como saida, o plugin nao tinha como
    // passa-los, a busca rodava sem filtro nenhum, e o resultado parecia
    // legitimo. Nenhum erro, nenhum log.
    //
    // EntreSai<T> escreve o valor do plugin no slot ANTES da chamada e copia de
    // volta DEPOIS. E' o unico jeito de honrar os dois lados.
    template<typename T>
    struct EntreSai
    {
        T* valor;
        explicit EntreSai(T& v) : valor(&v) {}
    };
    template<typename T> inline EntreSai<T> ParaEntreSai(T& v) { return EntreSai<T>(v); }

    // ── LISTA COMO SAIDA: ForaLista<T> ──────────────────────────────────────
    //
    // POR QUE E' MAIS UM TIPO, E NAO O Fora<T>
    // -----------------------------------------
    // Um TArray no bloco de parametros e' um FScriptArray: {void* Data; int Num;
    // int Max}. Copiar esses 16 bytes daria ao plugin o PONTEIRO do jogo — e o
    // ProcessEvent libera esse buffer ao retornar. O plugin ficaria lendo memoria
    // liberada, que e' pior que nao ter a saida.
    //
    // Aqui se COPIAM OS ELEMENTOS para o buffer do plugin enquanto o array ainda
    // vale. Nada do jogo atravessa a fronteira, e o tempo de vida passa a ser do
    // plugin.
    //
    // Eram 1.653 parametros de saida e 541 de retorno sem assinatura por causa
    // disto — funcoes como BoxOverlapActors, que devolve TArray<AActor*> e e'
    // exatamente o que um plugin de area/evento precisa.
    //
    //     AActor* achados[32]; int n = 0;
    //     lib->BoxOverlapActors(..., ConanApi::ParaForaLista(achados, 32, n));
    template<typename T>
    struct ForaLista
    {
        T*   destino;
        int  capacidade;
        int* quantos;      // recebe quantos couberam de verdade
        ForaLista(T* d, int cap, int& n) : destino(d), capacidade(cap), quantos(&n)
        { n = 0; }
    };
    template<typename T>
    inline ForaLista<T> ParaForaLista(T* d, int cap, int& n)
    { return ForaLista<T>(d, cap, n); }

    // ── PARAMETRO DE SAIDA: Fora<T> ─────────────────────────────────────────
    //
    // POR QUE ISTO EXISTE
    // -------------------
    // 7,985 functions on this build came out of ConanSDK.h as generic templates
    // — no signature, no types, no parameter names — for ONE reason only: they
    // had an OUTPUT parameter. Measured on 2026-08-19, and it was the LARGEST
    // group, bigger than all the type problems combined.
    //
    // "Output" in Unreal is neither a pointer nor a reference in the block: it
    // is an ORDINARY slot of the parameter block that the function WRITES
    // instead of reading. The caller has to read that slot after the function
    // runs. The old Call built the block, called, and discarded the whole block
    // — the output value was written and thrown away.
    //
    // Fora<T> marks "this argument is an output": Empacotar records the
    // destination address and the slot offset, and Call copies it back AFTER
    // the invocation.
    //
    // WHY A TYPE, AND NOT JUST PASSING T&
    // ------------------------------------
    // Because T& is indistinguishable from T at packing time, and guessing
    // would be the worst path: copying back an INPUT parameter would overwrite
    // the plugin's variable with garbage from the block. The type makes the
    // intent explicit, and the generated header emits it only where reflection
    // says CPF_OutParm.
    template<typename T>
    struct Fora
    {
        T* destino;
        explicit Fora(T& d) : destino(&d) {}
    };

    // ── O RETORNO QUE NAO CABE NUM RETORNO ──────────────────────────────────
    //
    // Funcao que devolve FString/FText/TArray nao pode ter isso como valor de
    // retorno em C++: sao 16 bytes com ponteiro do jogo dentro, que morre
    // quando o ProcessEvent destroi o bloco. Devolve-los seria entregar um
    // ponteiro pendurado — e o sintoma apareceria longe daqui.
    //
    // Sao 1.125 funcoes desta build (365 FString + 219 FText + 541 TArray).
    // Todas ficavam genericas por causa disso.
    //
    // A saida e' a mesma dos parametros de saida: decodificar na janela em que
    // o dado vale. Estes envelopes marcam "este destino e' o RETORNO", e o
    // motor traduz para OffsetRetorno.
    struct RetornoTexto
    {
        char* destino; int tam;
        RetornoTexto(char* d, int t) : destino(d), tam(t) { if (d && t > 0) d[0] = 0; }
    };
    inline RetornoTexto ParaRetornoTexto(char* d, int t) { return RetornoTexto(d, t); }

    struct RetornoTextoRico
    {
        char* destino; int tam;
        RetornoTextoRico(char* d, int t) : destino(d), tam(t) { if (d && t > 0) d[0] = 0; }
    };
    inline RetornoTextoRico ParaRetornoTextoRico(char* d, int t)
    { return RetornoTextoRico(d, t); }

    template<typename T>
    struct RetornoLista
    {
        T* destino; int capacidade; int* quantos;
        RetornoLista(T* d, int cap, int& n) : destino(d), capacidade(cap), quantos(&n)
        { n = 0; }
    };
    template<typename T>
    inline RetornoLista<T> ParaRetornoLista(T* d, int cap, int& n)
    { return RetornoLista<T>(d, cap, n); }

    // ── UPARAM(ref) DE TEXTO: entra E volta no mesmo buffer ─────────────────
    //
    // Sao 1.399 funcoes desta build — o maior grupo de genericas depois que os
    // motivos passaram a ser precisos. Antes ficavam escondidas sob o rotulo
    // "tem N parametros de SAIDA", que dizia que HA saida e nao POR QUE ela
    // nao podia ser expressa.
    //
    // As duas metades ja' existiam separadas:
    //   `Texto`     monta uma FString com memoria do JOGO a partir de char*
    //   `ForaTexto` decodifica um slot de FString de volta para char*
    //
    // Um UPARAM(ref) de FString e' exatamente as duas, no MESMO slot: escreve
    // antes, le' depois. O envelope abaixo so' junta o par — nao ha mecanismo
    // novo, e por isso nao ha risco novo de posse.
    struct EntreSaiTexto
    {
        Texto entrada;      // a FString do jogo, ja' montada
        char* destino;
        int   tam;
        EntreSaiTexto(char* buf, int t)
            : entrada(buf ? buf : ""), destino(buf), tam(t) {}
    };
    inline EntreSaiTexto ParaEntreSaiTexto(char* buf, int tam)
    { return EntreSaiTexto(buf, tam); }

    // O mesmo par, para FText: TextoRico monta pelo Conv_StringToText do jogo,
    // e a volta passa pelo Conv_TextToString. Sao 193 funcoes.
    struct EntreSaiTextoRico
    {
        TextoRico entrada;
        char*     destino;
        int       tam;
        EntreSaiTextoRico(char* buf, int t)
            : entrada(buf ? buf : ""), destino(buf), tam(t) {}
    };
    inline EntreSaiTextoRico ParaEntreSaiTextoRico(char* buf, int tam)
    { return EntreSaiTextoRico(buf, tam); }
    template<typename T> inline Fora<T> ParaFora(T& d) { return Fora<T>(d); }

    // Onde copiar de volta, e quanto. Preenchido pelo Empacotar, consumido pelo
    // Call. MAX_PARMS entradas bastam: nao ha como haver mais saidas que
    // parametros.
    struct SaidasPendentes
    {
        void*    destino[MAX_PARMS];
        uint32_t offset[MAX_PARMS];
        uint32_t tam[MAX_PARMS];
        // tam == TAM_TEXTO marca "decodifique a FString deste slot em vez de
        // copiar bytes". Cabe no mesmo array de proposito: uma lista so para
        // percorrer significa uma lista so para esquecer de percorrer.
        int      capTexto[MAX_PARMS];
        // Para ForaLista: onde escrever a contagem e o tamanho de cada elemento.
        int*     contagem[MAX_PARMS];
        uint32_t tamElem[MAX_PARMS];
        int      n;
    };
    static const uint32_t TAM_TEXTO      = 0xFFFFFFFFu;
    static const uint32_t TAM_TEXTO_RICO = 0xFFFFFFFEu;
    static const uint32_t TAM_LISTA      = 0xFFFFFFFDu;

    inline bool Empacotar(const FuncInfo*, const char*, uint8_t*, int) { return true; }

    template<typename T, typename... R>
    inline bool Empacotar(const FuncInfo* fi, const char* nome, uint8_t* buf, int i,
                          T&& v, R&&... resto)
    {
        bool ok = true;
        // Argumento a mais do que a função aceita é ignorado em vez de escrever
        // fora do bloco: um plugin da comunidade com a assinatura errada tem de
        // falhar em silêncio, nunca corromper a pilha do servidor.
        if (i < int(fi->NumEntrada) && i < MAX_PARMS)
        {
            using U = typename std::decay<T>::type;
            const uint32_t off    = fi->Offset[i];
            const uint32_t limite = EspacoNoBloco(fi, off);
            const uint32_t medido = fi->Tamanho[i];      // 0 = não medido

            // Duas reprovações, e nenhuma delas confia na outra:
            //  · não CABE  — escreveria fora do parâmetro (ou fora do bloco);
            //  · não BATE  — cabe, mas o tamanho medido diz que é outro tipo.
            const bool cabe = (sizeof(U) <= limite);
            const bool bate = (medido == 0) || (sizeof(U) == medido);
            if (!cabe || !bate)
            {
                Log("Call(\"%s\"): argumento %d tem %u bytes e o parametro %d "
                    "aceita %u (offset %u, bloco de %u; tamanho %s). A chamada "
                    "NAO foi feita. Causa mais comum: literal sem sufixo — 0.5 e' "
                    "double de 8 bytes, 0.5f e' float de 4. Use o tipo da "
                    "reflexao, que e' o que o ConanSDK.h ja usa.",
                    nome ? nome : "(sem nome)", i, unsigned(sizeof(U)), i,
                    unsigned(medido ? medido : limite), unsigned(off),
                    unsigned(fi->ParmsSize),
                    medido ? "medido na reflexao" : "deduzido do offset seguinte");
                ok = false;
            }
            else
            {
                U tmp = v;
                std::memcpy(buf + off, &tmp, sizeof(U));
            }
        }
        // O resto é conferido SEMPRE, mesmo depois de uma reprovação: quem errou
        // dois argumentos tem de ver os dois no log, não descobrir o segundo
        // depois de consertar o primeiro.
        const bool restoOk = Empacotar(fi, nome, buf, i + 1, std::forward<R>(resto)...);
        return ok && restoOk;
    }

    // ── a chamada, com retorno tipado opcional ─────────────────────────────
    //
    //     pc->UnbanPlayer(id);                    // sem retorno
    //     float v = ator->GetDistanceTo<float>(o) // com retorno
    //
    // ── EMPACOTAR COM SAIDA ─────────────────────────────────────────────────
    //
    // Espelha o Empacotar acima, com uma diferenca: quando o argumento e'
    // Fora<T>, o slot NAO recebe valor (a funcao e' quem escreve nele) e o
    // destino do plugin fica anotado para a copia de volta.
    //
    // Duplicar a recursao em vez de acrescentar um parametro ao Empacotar
    // original e deliberado: aquele e' o caminho quente, usado por todo plugin
    // ja compilado, e mexer nele por causa de um recurso novo trocaria um
    // ganho por um risco em codigo que ja funciona.
    inline bool EmpacotarS(const FuncInfo*, const char*, uint8_t*, int,
                           SaidasPendentes*) { return true; }

    // Texto como argumento: os 16 bytes da FString que o JOGO alocou vao para o
    // slot. O tamanho e conferido contra a reflexao como qualquer outro — se o
    // parametro nao for FString, a chamada e recusada em vez de escrever 16
    // bytes num slot de 4.
    // FText no slot — mesma conferencia de tamanho do Texto.
    template<typename... R>
    inline bool Empacotar(const FuncInfo* fi, const char* nome, uint8_t* buf,
                          int i, const TextoRico& t, R&&... resto)
    {
        bool ok = true;
        if (i < int(fi->NumEntrada) && i < MAX_PARMS)
        {
            const uint32_t off    = fi->Offset[i];
            const uint32_t limite = EspacoNoBloco(fi, off);
            const uint32_t medido = fi->Tamanho[i];
            if (!t.valido)
            {
                Log("Call(\"%s\"): nao consegui pedir o FText ao jogo para o "
                    "argumento %d. A chamada NAO foi feita.",
                    nome ? nome : "(sem nome)", i);
                ok = false;
            }
            else if (sizeof(t.bruto) > limite || (medido != 0 && medido != sizeof(t.bruto)))
            {
                Log("Call(\"%s\"): argumento %d e' texto rico (16 bytes de FText) "
                    "e o parametro aceita %u. A chamada NAO foi feita.",
                    nome ? nome : "(sem nome)", i, unsigned(medido ? medido : limite));
                ok = false;
            }
            else
                std::memcpy(buf + off, t.bruto, sizeof(t.bruto));
        }
        const bool r = Empacotar(fi, nome, buf, i + 1, std::forward<R>(resto)...);
        return ok && r;
    }

    template<typename... R>
    inline bool Empacotar(const FuncInfo* fi, const char* nome, uint8_t* buf,
                          int i, const Texto& t, R&&... resto)
    {
        bool ok = true;
        if (i < int(fi->NumEntrada) && i < MAX_PARMS)
        {
            const uint32_t off    = fi->Offset[i];
            const uint32_t limite = EspacoNoBloco(fi, off);
            const uint32_t medido = fi->Tamanho[i];
            if (!t.valido)
            {
                Log("Call(\"%s\"): nao consegui pedir a FString ao jogo para o "
                    "argumento %d. A chamada NAO foi feita.",
                    nome ? nome : "(sem nome)", i);
                ok = false;
            }
            else if (sizeof(t.bruto) > limite || (medido != 0 && medido != sizeof(t.bruto)))
            {
                Log("Call(\"%s\"): argumento %d e' texto (16 bytes de FString) e o "
                    "parametro aceita %u. A chamada NAO foi feita.",
                    nome ? nome : "(sem nome)", i,
                    unsigned(medido ? medido : limite));
                ok = false;
            }
            else
                std::memcpy(buf + off, t.bruto, sizeof(t.bruto));
        }
        const bool r = Empacotar(fi, nome, buf, i + 1, std::forward<R>(resto)...);
        return ok && r;
    }

    template<typename T, typename... R>
    inline bool EmpacotarS(const FuncInfo* fi, const char* nome, uint8_t* buf,
                           int i, SaidasPendentes* sp, EntreSai<T> v, R&&... resto)
    {
        // Escreve como ENTRADA (reusa o Empacotar, que ja confere tamanho)...
        bool ok = Empacotar(fi, nome, buf, i, *v.valor);
        // ...e anota para copiar de volta como SAIDA.
        if (ok && i < int(fi->NumEntrada) && i < MAX_PARMS && sp->n < MAX_PARMS)
        {
            sp->destino[sp->n]  = v.valor;
            sp->offset[sp->n]   = fi->Offset[i];
            sp->tam[sp->n]      = uint32_t(sizeof(T));
            sp->contagem[sp->n] = nullptr;
            ++sp->n;
        }
        const bool r = EmpacotarS(fi, nome, buf, i + 1, sp, std::forward<R>(resto)...);
        return ok && r;
    }

    template<typename T, typename... R>
    inline bool EmpacotarS(const FuncInfo* fi, const char* nome, uint8_t* buf,
                           int i, SaidasPendentes* sp, ForaLista<T> v, R&&... resto)
    {
        bool ok = true;
        if (i < int(fi->NumEntrada) && i < MAX_PARMS)
        {
            const uint32_t off    = fi->Offset[i];
            const uint32_t limite = EspacoNoBloco(fi, off);
            const uint32_t medido = fi->Tamanho[i];
            // FScriptArray sao 16 bytes. Se o slot nao tem 16, nao e' TArray.
            if (limite < 16 || (medido != 0 && medido != 16))
            {
                Log("Call(\"%s\"): parametro de saida %d nao e' TArray (aceita %u "
                    "bytes; TArray tem 16). A chamada NAO foi feita.",
                    nome ? nome : "(sem nome)", i, unsigned(medido ? medido : limite));
                ok = false;
            }
            else if (!v.destino || v.capacidade <= 0 || !v.quantos)
            {
                Log("Call(\"%s\"): saida de lista %d sem buffer de destino.",
                    nome ? nome : "(sem nome)", i);
                ok = false;
            }
            else if (sp->n < MAX_PARMS)
            {
                sp->destino[sp->n]  = v.destino;
                sp->offset[sp->n]   = off;
                sp->tam[sp->n]      = TAM_LISTA;
                sp->capTexto[sp->n] = v.capacidade;
                sp->contagem[sp->n] = v.quantos;
                sp->tamElem[sp->n]  = uint32_t(sizeof(T));
                ++sp->n;
            }
        }
        const bool r = EmpacotarS(fi, nome, buf, i + 1, sp, std::forward<R>(resto)...);
        return ok && r;
    }

    template<typename... R>
    inline bool EmpacotarS(const FuncInfo* fi, const char* nome, uint8_t* buf,
                           int i, SaidasPendentes* sp, ForaTextoRico v, R&&... resto)
    {
        bool ok = true;
        if (i < int(fi->NumEntrada) && i < MAX_PARMS)
        {
            const uint32_t off    = fi->Offset[i];
            const uint32_t limite = EspacoNoBloco(fi, off);
            const uint32_t medido = fi->Tamanho[i];
            if (limite < 16 || (medido != 0 && medido != 16))
            {
                Log("Call(\"%s\"): parametro de saida %d nao e' FText (aceita %u "
                    "bytes; FText tem 16). A chamada NAO foi feita.",
                    nome ? nome : "(sem nome)", i, unsigned(medido ? medido : limite));
                ok = false;
            }
            else if (!v.destino || v.tam <= 0)
            {
                Log("Call(\"%s\"): saida de texto rico %d sem buffer.",
                    nome ? nome : "(sem nome)", i);
                ok = false;
            }
            else if (sp->n < MAX_PARMS)
            {
                sp->destino[sp->n]  = v.destino;
                sp->offset[sp->n]   = off;
                sp->tam[sp->n]      = TAM_TEXTO_RICO;
                sp->capTexto[sp->n] = v.tam;
                ++sp->n;
            }
        }
        const bool r = EmpacotarS(fi, nome, buf, i + 1, sp, std::forward<R>(resto)...);
        return ok && r;
    }

    template<typename... R>
    inline bool EmpacotarS(const FuncInfo* fi, const char* nome, uint8_t* buf,
                           int i, SaidasPendentes* sp, ForaTexto v, R&&... resto)
    {
        bool ok = true;
        if (i < int(fi->NumEntrada) && i < MAX_PARMS)
        {
            const uint32_t off    = fi->Offset[i];
            const uint32_t limite = EspacoNoBloco(fi, off);
            const uint32_t medido = fi->Tamanho[i];
            // FString sao 16 bytes. Se o parametro nao tem 16, nao e' FString, e
            // decodificar dali leria um ponteiro que nao esta ali.
            if (limite < 16 || (medido != 0 && medido != 16))
            {
                Log("Call(\"%s\"): parametro de saida %d nao e' FString (aceita "
                    "%u bytes; FString tem 16). A chamada NAO foi feita.",
                    nome ? nome : "(sem nome)", i,
                    unsigned(medido ? medido : limite));
                ok = false;
            }
            else if (!v.destino || v.tam <= 0)
            {
                Log("Call(\"%s\"): saida de texto %d sem buffer de destino.",
                    nome ? nome : "(sem nome)", i);
                ok = false;
            }
            else if (sp->n < MAX_PARMS)
            {
                sp->destino[sp->n]  = v.destino;
                sp->offset[sp->n]   = off;
                sp->tam[sp->n]      = TAM_TEXTO;
                sp->capTexto[sp->n] = v.tam;
                ++sp->n;
            }
        }
        const bool r = EmpacotarS(fi, nome, buf, i + 1, sp, std::forward<R>(resto)...);
        return ok && r;
    }

    template<typename T, typename... R>
    inline bool EmpacotarS(const FuncInfo* fi, const char* nome, uint8_t* buf,
                           int i, SaidasPendentes* sp, Fora<T> v, R&&... resto)
    {
        bool ok = true;
        if (i < int(fi->NumEntrada) && i < MAX_PARMS)
        {
            const uint32_t off    = fi->Offset[i];
            const uint32_t limite = EspacoNoBloco(fi, off);
            const uint32_t medido = fi->Tamanho[i];

            // A MESMA dupla conferencia da entrada, e pela mesma razao: se o
            // tamanho nao bate, copiar de volta escreveria por cima da pilha do
            // plugin. Saida errada corrompe a variavel de QUEM CHAMOU — e o
            // sintoma aparece longe daqui.
            if (sizeof(T) > limite || (medido != 0 && sizeof(T) != medido))
            {
                Log("Call(\"%s\"): parametro de SAIDA %d espera %u bytes e o "
                    "destino tem %u. A chamada NAO foi feita — copiar de volta "
                    "com tamanho errado corromperia a memoria do plugin.",
                    nome ? nome : "(sem nome)", i,
                    unsigned(medido ? medido : limite), unsigned(sizeof(T)));
                ok = false;
            }
            else if (sp->n < MAX_PARMS)
            {
                sp->destino[sp->n] = v.destino;
                sp->offset[sp->n]  = off;
                sp->tam[sp->n]     = uint32_t(sizeof(T));
                ++sp->n;
            }
        }
        const bool r = EmpacotarS(fi, nome, buf, i + 1, sp, std::forward<R>(resto)...);
        return ok && r;
    }

    template<typename T, typename... R>
    inline bool EmpacotarS(const FuncInfo* fi, const char* nome, uint8_t* buf,
                           int i, SaidasPendentes* sp, T&& v, R&&... resto)
    {
        const bool ok = Empacotar(fi, nome, buf, i, std::forward<T>(v));
        const bool r  = EmpacotarS(fi, nome, buf, i + 1, sp, std::forward<R>(resto)...);
        return ok && r;
    }

    // ── A CHAMADA COM SAIDA ─────────────────────────────────────────────────
    //
    // Identica ao Call, mais o passo que faltava: depois do InvokeRaw, os slots
    // marcados como Fora<T> sao copiados para o destino do plugin.
    //
    // A copia acontece SO se a funcao executou de verdade. Se o ProcessEvent
    // filtrou a chamada (CDO, template de Blueprint, Actor nao inicializado), o
    // bloco continua com o que estava — copiar dali entregaria lixo com cara de
    // resposta, que e' o defeito que o sentinela existe para impedir.
    // ── A CHAMADA COM SAIDA ─────────────────────────────────────────────────
    //
    // Identica ao Call, mais o passo que faltava: depois do InvokeRaw, os slots
    // marcados como Fora<T> sao copiados para o destino do plugin.
    //
    // A copia acontece SO se a funcao executou de verdade. Se o ProcessEvent
    // filtrou a chamada (CDO, template de Blueprint, Actor nao inicializado), o
    // bloco continua com o que estava — copiar dali entregaria lixo com cara de
    // resposta, que e' o defeito que o sentinela existe para impedir.

// ═══════════════════════════════════════════════════════════════════════════
//  DOIS CAMINHOS PARA A MESMA CHAMADA, E POR QUE PRECISAM SER DOIS
//
//  MOTOR (CONAN_MOTOR definido): usa InvokeRaw e o FuncInfo direto. E' quem
//  tem o mapa da funcao — offsets de cada slot, tamanhos medidos, offset do
//  retorno — e por isso e' quem valida.
//
//  PLUGIN (o padrao): nao tem InvokeRaw, nao tem FuncInfo, e NAO PODE ter: se
//  tivesse, precisaria linkar a libconanapi.a, e o modelo de tabela morre
//  junto com a promessa "seu compilador nao importa". Aqui as chamadas viram
//  ChamarFuncao/ChamarFuncaoEx da tabela: o plugin entrega ponteiros e
//  tamanhos, o motor confere contra o FuncInfo real e recusa o que nao bate.
//
//  ISTO E' O QUE DESTRAVA O ConanSDK.h. Ate a v2.4.0 o header de 8.287 classes
//  emitia `ConanApi::Call<>`, que so' existia no motor — entao o SDK que o
//  README anunciava nao podia ir no pacote. Com o caminho de plugin, ele vai.
// ═══════════════════════════════════════════════════════════════════════════
#ifdef CONAN_MOTOR

    template<typename R = void, typename... A>
    inline R CallSaida(void* obj, const char* nome, A&&... args)
    {
        const FuncInfo* fi = obj ? ResolveFunction(obj, nome) : nullptr;
        if (!fi)
        {
            MarcarExecucao(false);
            Log("CallSaida(\"%s\"): %s. Nada foi chamado.",
                nome ? nome : "(sem nome)",
                obj ? "funcao inexistente nesta classe nem nas maes"
                    : "o objeto e nulo");
            return R();
        }
        const uint32_t n = fi->ParmsSize ? fi->ParmsSize : 1;
        uint8_t* buf = static_cast<uint8_t*>(CONAN_ALLOCA(n));
        std::memset(buf, 0, n);

        SaidasPendentes sp{}; sp.n = 0;
        if (!EmpacotarS(fi, nome, buf, 0, &sp, std::forward<A>(args)...))
        {
            MarcarExecucao(false);
            return R();
        }

        MarcarExecucao(false);
        InvokeRaw(obj, fi->Function, buf);
        MarcarExecucao(true);

        for (int k = 0; k < sp.n; ++k)
        {
            if (!sp.destino[k]) continue;
            if (sp.tam[k] == TAM_LISTA)
            {
                // COPIA OS ELEMENTOS, nunca os 16 bytes do FScriptArray: o
                // ponteiro de dentro dele e' do jogo e morre no retorno.
                if (sp.offset[k] + 16 <= n)
                {
                    const uint8_t* a = buf + sp.offset[k];
                    void* dados; int num;
                    std::memcpy(&dados, a, sizeof(dados));
                    std::memcpy(&num, a + 8, sizeof(num));
                    int cabe = (num < sp.capTexto[k]) ? num : sp.capTexto[k];
                    if (cabe < 0) cabe = 0;
                    if (dados && cabe > 0 &&
                        Legivel(dados, size_t(cabe) * sp.tamElem[k]))
                        std::memcpy(sp.destino[k], dados, size_t(cabe) * sp.tamElem[k]);
                    else cabe = 0;
                    if (sp.contagem[k]) *sp.contagem[k] = cabe;
                }
                continue;
            }
            if (sp.tam[k] == TAM_TEXTO_RICO)
            {
                if (sp.offset[k] + 16 <= n)
                    TextoRicoDeSlot(buf + sp.offset[k],
                                    static_cast<char*>(sp.destino[k]), sp.capTexto[k]);
                continue;
            }
            if (sp.tam[k] == TAM_TEXTO)
            {
                // FString: DECODIFICA enquanto ela ainda vale. Copiar os 16
                // bytes daria ao plugin um ponteiro para memoria que o
                // ProcessEvent destroi ao retornar.
                if (sp.offset[k] + 16 <= n)
                    TextoDeSlot(buf + sp.offset[k],
                                static_cast<char*>(sp.destino[k]), sp.capTexto[k]);
                continue;
            }
            if (sp.offset[k] + sp.tam[k] <= n)
                std::memcpy(sp.destino[k], buf + sp.offset[k], sp.tam[k]);
        }

        if (fi->OffsetRetorno != 0xFFFF)
        {
            using Rt = typename std::conditional<std::is_void<R>::value, char, R>::type;
            if (fi->OffsetRetorno + sizeof(Rt) <= n)
            {
                Rt r{};
                std::memcpy(&r, buf + fi->OffsetRetorno, sizeof(Rt));
                return static_cast<R>(r);
            }
        }
        return R();
    }

    template<typename R = void, typename... A>
    inline R Call(void* obj, const char* nome, A&&... args)
    {
        const FuncInfo* fi = obj ? ResolveFunction(obj, nome) : nullptr;
        if (!fi)
        {
            // Função inexistente devolve valor zerado em vez de lixo da pilha.
            // Silêncio é ruim, mas lixo tipado é pior: vira bug em outro lugar.
            //
            // E O SENTINELA TEM DE CAIR AQUI
            // ------------------------------
            // Este `return` saía sem tocar no sentinela, e a flag é thread_local
            // inicializada em `true`. Resultado: `UltimaChamadaExecutou()`
            // respondia TRUE depois de uma chamada que nem existiu — o furo
            // ficava justamente no caso mais comum de não-execução (nome
            // escrito errado, função que sumiu numa atualização do jogo, objeto
            // nulo), não no caso raro do ProcessEvent filtrando.
            //
            // É o mesmo cenário que ConanApi.cpp descreve como já ocorrido neste
            // projeto: um plugin de permissão perguntando `IsAdmin` por um nome
            // que mudou trata todo administrador como jogador comum — e o
            // sentinela, que existe exatamente para desmentir isso, confirmava
            // que o `false` era resposta legítima.
            MarcarExecucao(false);
            Log("Call(\"%s\"): %s. Nada foi chamado — o valor devolvido e zero por "
                "ausencia, nao por resultado, e UltimaChamadaExecutou() responde "
                "false.",
                nome ? nome : "(sem nome)",
                obj ? "esta funcao nao existe nesta classe nem nas maes dela"
                    : "o objeto e nulo");
            return R();
        }
        const uint32_t n = fi->ParmsSize ? fi->ParmsSize : 1;
        uint8_t* buf = static_cast<uint8_t*>(CONAN_ALLOCA(n));
        std::memset(buf, 0, n);
        if (!Empacotar(fi, nome, buf, 0, std::forward<A>(args)...))
        {
            // Algum argumento não coube no parâmetro. `Empacotar` já disse no
            // log qual e por quê; aqui a chamada simplesmente não acontece.
            MarcarExecucao(false);
            return R();
        }

        // ── SENTINEL IN THE RETURN SLOT — and the defect it reveals ──────────
        //
        // Not every call executes. `AActor::ProcessEvent` FILTERS: an object
        // that is a Blueprint template, a CDO, or an Actor not yet initialised
        // dispatches no function at all. It simply does not run.
        //
        // Without a sentinel, the parameter block stays zeroed and the return
        // comes out `false` / `0` / null pointer — indistinguishable from a
        // legitimate result. That is how this surfaced: in a test over 400
        // Actors, 399 agreed with a direct bitfield read and ONE disagreed —
        // `FS_AnchorField_GenericEx_C AnchorField_GEN_VARIABLE_...`, a template
        // object. The bit said `true`, the function said `false`, and the
        // function had never run.
        //
        // 0xCD is the historical pattern for "uninitialised memory". If it
        // survives the call, nobody wrote there — the function did not execute,
        // and that goes to the log instead of becoming a plausible value.
        // `sizeof(R)` does not exist for R = void, so the size comes from an
        // alias that swaps void for char. Without it the compiler warns on EVERY
        // call with no return — and a warning that always appears is a warning
        // nobody reads.
        //
        // THE RETURN SLOT'S SPACE COMES FROM THE SAME CALCULATION AS ARGUMENTS
        // ---------------------------------------------------------------------
        // The only ceiling used to be the end of the block (`OffsetRetorno +
        // sizeof(R) <= ParmsSize`). But the return value is not always the last
        // parameter: on this build there are 1,572 of the 17,436 functions with
        // a return where an input parameter sits at a HIGHER offset than the
        // return. In those, an oversized R made the `memset(0xCD)` run over an
        // argument that was ALREADY packed — the function received deliberate
        // garbage, which is exactly what the comment below says must not happen.
        // `EspacoNoBloco` stops at the neighbour, not at the end of the block.
        using RSeguro = typename std::conditional<std::is_void<R>::value, char, R>::type;
        bool temRetorno = false;
        if (fi->OffsetRetorno != 0xFFFF)
        {
            const uint32_t limiteRet = EspacoNoBloco(fi, fi->OffsetRetorno);
            const uint32_t medidoRet = fi->TamanhoRetorno;   // 0 = não medido
            temRetorno = (sizeof(RSeguro) <= limiteRet) &&
                         (medidoRet == 0 || sizeof(RSeguro) == medidoRet);
        }
        if constexpr (!std::is_void<R>::value)
        {
            // Só a região DO RETORNO recebe o sentinela. Marcar os parâmetros de
            // entrada faria a função do jogo ler lixo de propósito.
            if (temRetorno) std::memset(buf + fi->OffsetRetorno, 0xCD, sizeof(R));
        }
        MarcarExecucao(true);

        InvokeRaw(obj, fi->Function, buf);

        if constexpr (!std::is_void<R>::value)
        {
            R r{};
            if (!temRetorno)
            {
                // Pediram um R tipado de uma função que não tem onde devolvê-lo
                // — ou não devolve nada, ou devolve de outro tamanho. O zero que
                // sai daqui é ausência, não resposta, e o sentinela precisa
                // dizer isso: era mais um caminho que devolvia `false`/`0` com
                // `UltimaChamadaExecutou()` respondendo true.
                MarcarExecucao(false);
                Log("Call(\"%s\"): pediram retorno de %u bytes e a funcao nao tem "
                    "onde devolver isso (offset do retorno %d, bloco de %u). O "
                    "valor devolvido e zero por ausencia, nao por resultado. "
                    "Confira o tipo do retorno na reflexao — o ConanSDK.h ja traz "
                    "a assinatura certa.",
                    nome ? nome : "(sem nome)", unsigned(sizeof(R)),
                    (fi->OffsetRetorno == 0xFFFF) ? -1 : int(fi->OffsetRetorno),
                    unsigned(fi->ParmsSize));
                return r;
            }

            uint8_t sentinela[sizeof(R)];
            std::memset(sentinela, 0xCD, sizeof(R));
            if (std::memcmp(buf + fi->OffsetRetorno, sentinela, sizeof(R)) == 0)
            {
                MarcarExecucao(false);
                // Ninguém escreveu no retorno. Falha ALTA: quem chamou fica
                // sabendo, em vez de receber zero achando que é resposta.
                Log("Call(\"%s\"): a funcao NAO executou (objeto template/CDO ou "
                    "Actor nao inicializado — ProcessEvent filtrou). "
                    "O valor devolvido e zero por falta de resposta, nao por "
                    "resultado.", nome);
                return r;
            }
            std::memcpy(&r, buf + fi->OffsetRetorno, sizeof(R));
            return r;
        }
    }

#else   // ── PLUGIN: tudo pela tabela, sem linkar nada ─────────────────────

    // ── DE ONDE VEM A TABELA, DO LADO DO PLUGIN ────────────────────────────
    //
    // No motor, `TabelaDoPlugin()` mora no ConanTabela.cpp. O plugin nao tem
    // esse .cpp — nem deve ter: e' justamente o que ele NAO linka. O que ele
    // tem e' o ponteiro que o carregador entrega em ConanPluginCarregar.
    //
    // Entao aqui a funcao vira inline sobre uma variavel que o proprio plugin
    // preenche, com UMA linha:
    //
    //     void ConanPluginCarregar(const ConanApiTabela* api) {
    //         ConanApi::UsarTabela(api);      <- esta
    //         ...
    //     }
    //
    // Sem ela, todo `Call` do ConanSDK.h vira no-op silencioso. Por isso
    // `Call` avisa no log a primeira vez que for chamado sem tabela: o pior
    // resultado possivel aqui e' "nao aconteceu nada e ninguem disse nada".
    inline const ConanApiTabela*& TabelaMutavel()
    {
        static const ConanApiTabela* t = nullptr;
        return t;
    }
    inline const ConanApiTabela* TabelaDoPlugin() { return TabelaMutavel(); }
    inline void UsarTabela(const ConanApiTabela* t) { TabelaMutavel() = t; }

    // ── AS FUNCOES DO MOTOR QUE O HEADER USA, ROTEADAS PELA TABELA ─────────
    //
    // `Texto`, `TextoRico` e os leitores de slot sao DECLARADOS la' em cima e
    // usados por structs definidas antes deste ponto — no motor eles vem da
    // libconanapi.a. Dentro de um plugin nao ha biblioteca, e a definicao tem
    // de ser esta: um desvio para a tabela.
    //
    // Sem isto, incluir o ConanSDK.h num plugin linka ate' a primeira `Texto`
    // e morre com "undefined reference to ConanApi::CriarTextoDoJogo" — que foi
    // exatamente o que aconteceu ao escrever a prova.
    inline bool CriarTextoDoJogo(const char* texto, void* destino16Bytes)
    {
        const ConanApiTabela* t = TabelaDoPlugin();
        return t && t->CriarTextoDoJogo && t->CriarTextoDoJogo(texto, destino16Bytes) != 0;
    }
    inline bool CriarTextoRicoDoJogo(const char* texto, void* destino16Bytes)
    {
        const ConanApiTabela* t = TabelaDoPlugin();
        return t && t->CriarTextoRicoDoJogo &&
               t->CriarTextoRicoDoJogo(texto, destino16Bytes) != 0;
    }
    // Estes dois so' sao chamados pelo caminho de motor (dentro do CallSaida
    // que usa InvokeRaw). No plugin, quem decodifica o slot e' o motor, do lado
    // de la' da tabela — mas as definicoes precisam existir para o header
    // compilar inteiro.
    inline int TextoDeSlot(const void*, char* saida, int tam)
    { if (saida && tam > 0) saida[0] = 0; return 0; }
    inline int TextoRicoDeSlot(const void*, char* saida, int tam)
    { if (saida && tam > 0) saida[0] = 0; return 0; }

    // `Log` do plugin vai para o mesmo arquivo, pela tabela.
    inline void Log(const char* fmt, ...)
    {
        const ConanApiTabela* t = TabelaDoPlugin();
        if (!t || !t->Log) return;
        // Sem vsnprintf aqui: a tabela recebe o formato e os argumentos ja'
        // expandidos seria outra ABI. Uma linha sem formatacao basta para o
        // punhado de avisos que o header emite.
        t->Log("%s", fmt);
    }

    // Chamado quando o plugin esqueceu o UsarTabela(). Nao ha para onde
    // escrever (o Log tambem vem da tabela), entao sobra a saida padrao do
    // processo — que no servidor cai no log do jogo. Melhor isso do que o
    // plugin inteiro nao fazer nada em silencio.
    inline void AvisarSemTabela(const char* nome)
    {
        static bool avisou = false;
        if (avisou) return;
        avisou = true;
        std::fprintf(stderr,
            "[Conan] Call(\"%s\") sem tabela: o plugin nao chamou "
            "ConanApi::UsarTabela(api) no ConanPluginCarregar. "
            "Nenhuma chamada do ConanSDK.h vai funcionar.\n",
            nome ? nome : "(sem nome)");
    }

    // Cada argumento e' classificado em tempo de compilacao: entrada comum,
    // texto (que ja' vem como 16 bytes do jogo), ou um dos descritores de
    // saida. A ORDEM e' a da reflexao — o indice do argumento e' o indice do
    // parametro, e reordenar aqui escreveria cada valor no slot do vizinho.
    // AS POSICOES SAO AS DA REFLEXAO, E ISSO NAO E' DETALHE
    // ------------------------------------------------------
    // O `FuncInfo` do motor indexa TODOS os parametros — entradas e saidas na
    // mesma numeracao. A primeira versao disto numerava so' as entradas, e num
    // `Split(entrada, entrada, SAIDA, SAIDA, entrada, entrada)` o quinto
    // argumento (1 byte) foi conferido contra o slot do terceiro (16 bytes).
    // O motor recusou — corretamente, e essa recusa e' o unico motivo de o
    // defeito ter aparecido em vez de escrever 1 byte no meio de uma FString.
    //
    // Entao `ent[i]` e' o parametro i, e slot de saida entra como `nullptr`:
    // buraco explicito, que o motor pula.
    struct ColetaArgs
    {
        const void*  ent[MAX_PARMS];
        uint32_t     tam[MAX_PARMS];
        int          nent = 0;        // = total de parametros vistos
        ConanSaida   sai[MAX_PARMS];
        int          nsai = 0;
        int          indice = 0;      // posicao na lista de parametros

        ColetaArgs()
        {
            for (int i = 0; i < int(MAX_PARMS); ++i) { ent[i] = nullptr; tam[i] = 0; }
        }
        void Entrada(const void* p, uint32_t t)
        {
            if (indice < int(MAX_PARMS)) { ent[indice] = p; tam[indice] = t; }
            if (indice + 1 > nent) nent = indice + 1;
        }
        void Buraco()   // o slot existe, mas quem escreve nele e' o jogo
        {
            if (indice + 1 > nent) nent = indice + 1;
        }
    };

    // ── entrada comum ──────────────────────────────────────────────────────
    template<typename T>
    inline void ColetaUm(ColetaArgs& c, const T& v)
    {
        c.Entrada(&v, uint32_t(sizeof(T)));
        ++c.indice;
    }
    // Texto ja' e' uma FString que o JOGO alocou: passam-se os 16 bytes dela.
    inline void ColetaUm(ColetaArgs& c, const Texto& v)
    {
        c.Entrada(v.bruto, 16);
        ++c.indice;
    }
    inline void ColetaUm(ColetaArgs& c, const TextoRico& v)
    {
        c.Entrada(v.bruto, 16);
        ++c.indice;
    }
    // O FName ja' resolvido pelo pool do jogo: 8 bytes, sem posse — o pool e'
    // dono do texto e vive o processo inteiro, entao nada aqui pende depois que
    // a chamada retorna (ao contrario da FString, que o ProcessEvent destroi).
    inline void ColetaUm(ColetaArgs& c, const Nome& v)
    {
        c.Entrada(v.bruto, 8);
        ++c.indice;
    }

    // ── saidas ─────────────────────────────────────────────────────────────
    template<typename T>
    inline void ColetaUm(ColetaArgs& c, const Fora<T>& v)
    {
        if (c.nsai < int(MAX_PARMS))
        { c.sai[c.nsai] = ConanSaida{ c.indice, v.destino, CONAN_SAIDA_POD,
                                      uint32_t(sizeof(T)), 0, nullptr }; ++c.nsai; }
        c.Buraco();
        ++c.indice;
    }
    inline void ColetaUm(ColetaArgs& c, const ForaTexto& v)
    {
        if (c.nsai < int(MAX_PARMS))
        { c.sai[c.nsai] = ConanSaida{ c.indice, v.destino, CONAN_SAIDA_TEXTO,
                                      uint32_t(v.tam), 0, nullptr }; ++c.nsai; }
        c.Buraco();
        ++c.indice;
    }
    inline void ColetaUm(ColetaArgs& c, const ForaTextoRico& v)
    {
        if (c.nsai < int(MAX_PARMS))
        { c.sai[c.nsai] = ConanSaida{ c.indice, v.destino, CONAN_SAIDA_TEXTO_RICO,
                                      uint32_t(v.tam), 0, nullptr }; ++c.nsai; }
        c.Buraco();
        ++c.indice;
    }
    template<typename T>
    inline void ColetaUm(ColetaArgs& c, const ForaLista<T>& v)
    {
        if (c.nsai < int(MAX_PARMS))
        { c.sai[c.nsai] = ConanSaida{ c.indice, v.destino, CONAN_SAIDA_LISTA,
                                      uint32_t(v.capacidade), uint32_t(sizeof(T)),
                                      v.quantos }; ++c.nsai; }
        c.Buraco();
        ++c.indice;
    }
    // UPARAM(ref): escreve ANTES e le' DEPOIS. Entra nas duas listas, com o
    // mesmo indice — que e' exatamente o que o slot faz no bloco do jogo.
    template<typename T>
    inline void ColetaUm(ColetaArgs& c, const EntreSai<T>& v)
    {
        c.Entrada(v.valor, uint32_t(sizeof(T)));
        if (c.nsai < int(MAX_PARMS))
        { c.sai[c.nsai] = ConanSaida{ c.indice, v.valor, CONAN_SAIDA_POD,
                                      uint32_t(sizeof(T)), 0, nullptr }; ++c.nsai; }
        c.Buraco();
        ++c.indice;
    }

    inline void ColetaUm(ColetaArgs& c, const EntreSaiTextoRico& v)
    {
        c.Entrada(v.entrada.bruto, 16);
        if (c.nsai < int(MAX_PARMS))
        { c.sai[c.nsai] = ConanSaida{ c.indice, v.destino, CONAN_SAIDA_TEXTO_RICO,
                                      uint32_t(v.tam), 0, nullptr }; ++c.nsai; }
        ++c.indice;
    }

    inline void ColetaUm(ColetaArgs& c, const EntreSaiTexto& v)
    {
        c.Entrada(v.entrada.bruto, 16);
        if (c.nsai < int(MAX_PARMS))
        { c.sai[c.nsai] = ConanSaida{ c.indice, v.destino, CONAN_SAIDA_TEXTO,
                                      uint32_t(v.tam), 0, nullptr }; ++c.nsai; }
        ++c.indice;
    }

    // Retorno: indice -1, e NAO consome posicao de parametro.
    inline void ColetaUm(ColetaArgs& c, const RetornoTexto& v)
    {
        if (c.nsai < int(MAX_PARMS))
        { c.sai[c.nsai] = ConanSaida{ -1, v.destino, CONAN_SAIDA_TEXTO,
                                      uint32_t(v.tam), 0, nullptr }; ++c.nsai; }
    }
    inline void ColetaUm(ColetaArgs& c, const RetornoTextoRico& v)
    {
        if (c.nsai < int(MAX_PARMS))
        { c.sai[c.nsai] = ConanSaida{ -1, v.destino, CONAN_SAIDA_TEXTO_RICO,
                                      uint32_t(v.tam), 0, nullptr }; ++c.nsai; }
    }
    template<typename T>
    inline void ColetaUm(ColetaArgs& c, const RetornoLista<T>& v)
    {
        if (c.nsai < int(MAX_PARMS))
        { c.sai[c.nsai] = ConanSaida{ -1, v.destino, CONAN_SAIDA_LISTA,
                                      uint32_t(v.capacidade), uint32_t(sizeof(T)),
                                      v.quantos }; ++c.nsai; }
    }

    template<typename... A>
    inline void Coleta(ColetaArgs& c, A&&... args)
    { (void)c; (void)std::initializer_list<int>{ (ColetaUm(c, args), 0)..., 0 }; }

    // ── as duas chamadas ───────────────────────────────────────────────────
    //
    // A tabela pode ser mais VELHA que este header: um plugin compilado contra
    // a v6 pode rodar numa API v5, que nao tem ChamarFuncaoEx. Conferir o
    // `tamanho` antes de usar o ponteiro e' o que separa "nao da'" de ler
    // memoria alem do fim da struct e chamar lixo.
    inline bool TabelaTem(size_t bytesAte)
    {
        const ConanApiTabela* t = TabelaDoPlugin();
        return t && t->tamanho >= bytesAte;
    }

    template<typename R = void, typename... A>
    inline R Call(void* obj, const char* nome, A&&... args)
    {
        ColetaArgs c;
        Coleta(c, args...);
        const ConanApiTabela* t = TabelaDoPlugin();
        using RSeguro = typename std::conditional<std::is_void<R>::value, char, R>::type;
        RSeguro r{};
        if (!t) { AvisarSemTabela(nome); if constexpr (!std::is_void<R>::value) return r; else return; }
        if (t->ChamarFuncao)
            t->ChamarFuncao(obj, nome, c.ent, c.tam, c.nent,
                            std::is_void<R>::value ? nullptr : &r,
                            std::is_void<R>::value ? 0u : uint32_t(sizeof(RSeguro)));
        if constexpr (!std::is_void<R>::value) return r;
        else                                    return;
    }

    template<typename R = void, typename... A>
    inline R CallSaida(void* obj, const char* nome, A&&... args)
    {
        ColetaArgs c;
        Coleta(c, args...);
        const ConanApiTabela* t = TabelaDoPlugin();
        using RSeguro = typename std::conditional<std::is_void<R>::value, char, R>::type;
        RSeguro r{};
        if (TabelaTem(offsetof(ConanApiTabela, ChamarFuncaoEx) + sizeof(void*)) &&
            t->ChamarFuncaoEx)
        {
            t->ChamarFuncaoEx(obj, nome, c.ent, c.tam, c.nent,
                              c.sai, c.nsai,
                              std::is_void<R>::value ? nullptr : &r,
                              std::is_void<R>::value ? 0u : uint32_t(sizeof(RSeguro)));
        }
        else if (t && t->Log)
        {
            // Nao inventar sucesso: sem ChamarFuncaoEx os slots de saida ficam
            // com o valor que ja' tinham, e devolver como se tivessem sido
            // preenchidos e' o defeito que esta API passou a noite perseguindo.
            t->Log("CallSaida(\"%s\"): esta API e' anterior a v6 e nao sabe "
                   "devolver parametro de saida. Nada foi chamado.", nome);
        }
        if constexpr (!std::is_void<R>::value) return r;
        else                                    return;
    }

#endif  // CONAN_MOTOR

    // ── Nome: o construtor, agora que `Call` existe ─────────────────────────
    //
    // Vale para os dois caminhos (motor e plugin) porque `Call` existe nos dois
    // — no motor pelo InvokeRaw, no plugin pela tabela. Uma implementacao so',
    // e nenhum "#ifdef" para as duas divergirem depois.
    //
    // MEMORIZA porque o pool nao esquece: resolvido uma vez, aquele texto tem
    // aquele indice ate' o processo morrer. Sem isto, uma loja que entrega item
    // pagaria um ProcessEvent por entrega so' para redescobrir o mesmo numero.
    // O cache e' pequeno de proposito: nomes de contexto sao poucos e fixos
    // (o plugin os escreve no codigo); cheio, ainda funciona, so' deixa de
    // memorizar — nunca devolve errado.
    inline Nome::Nome(const char* s) : bruto{}, valido(false)
    {
        if (!s || !*s) return;

        struct Entrada { char texto[64]; unsigned char bruto[8]; };
        static Entrada memo[64];
        static int nMemo = 0;

        for (int i = 0; i < nMemo; ++i)
        {
            if (std::strcmp(memo[i].texto, s) != 0) continue;
            std::memcpy(bruto, memo[i].bruto, 8);
            valido = true;
            return;
        }

        // O CDO da biblioteca vem por caminhos diferentes dos dois lados da
        // fronteira, e este e' o unico ponto do envelope onde isso aparece: o
        // motor chama a funcao direto, o plugin so' tem a tabela. As duas
        // linhas fazem a MESMA coisa — nao ha regra divergindo aqui, so' a
        // porta de entrada.
#ifdef CONAN_MOTOR
        void* lib = static_cast<void*>(GetDefaultObject("KismetStringLibrary"));
#else
        const ConanApiTabela* tb = TabelaDoPlugin();
        if (!tb || !tb->GetDefaultObject) return;
        void* lib = tb->GetDefaultObject("KismetStringLibrary");
#endif
        if (!lib) return;

        // Conv_StringToName(InString: FString) -> FName. A FString de entrada e'
        // montada pelo jogo (Texto), e o retorno sao os 8 bytes do FName.
        Texto entrada(s);
        if (!entrada.valido) return;

        struct { unsigned char b[8]; } r{};
        r = Call<decltype(r)>(lib, "Conv_StringToName", entrada);

        // Um FName {0,0} e' "None" — resposta legitima para a string "None" e
        // sinal de falha para qualquer outra. Recusar aqui evita que o plugin
        // mande "None" achando que mandou "loja".
        const bool ehNone = (r.b[0]|r.b[1]|r.b[2]|r.b[3]|r.b[4]|r.b[5]|r.b[6]|r.b[7]) == 0;
        if (ehNone && std::strcmp(s, "None") != 0) return;

        std::memcpy(bruto, r.b, 8);
        valido = true;

        if (nMemo < 64 && std::strlen(s) < sizeof(memo[0].texto))
        {
            std::strcpy(memo[nMemo].texto, s);
            std::memcpy(memo[nMemo].bruto, bruto, 8);
            ++nMemo;
        }
    }

    // as âncoras desta build; regeradas pelo dump a cada atualização do jogo
// ── A BUILD DO JOGO, EM UM LUGAR SÓ ─────────────────────────────────────────
//
// O número aparecia escrito à mão na mensagem de log. Quando a build mudou de
// 24383534 para 24784646, os RVAs foram atualizados e a mensagem continuou
// dizendo o número velho — o log afirmando uma coisa e a API rodando outra.
//
// É o I-10 outra vez: numa madrugada, essa linha é o que alguém usa para saber
// para qual build a API foi feita. Dizer o número errado manda a pessoa
// investigar o lugar errado.
#define CONAN_BUILD_DO_JOGO "24784646"


    // ProcessEvent é virtual: chamamos pela vtable, nunca pelo endereço fixo.
    // O índice foi eleito por um teste que aprovou 1 função entre 1.550.
    constexpr int PROCESSEVENT_VTABLE_INDEX = 79;
}

// ── tipos mínimos da engine, só o que os acessores precisam ─────────────────
struct FName            { int32_t ComparisonIndex; int32_t Number; };
struct FString          { void* Data; int32_t Num; int32_t Max; };
// FText mede 16, não 8: medido na reflexão (ElementSize da TextProperty). O
// layout interno não foi medido, então são bytes opacos — inventar campos com o
// tamanho certo daria uma struct plausível e falsa.
struct FText            { uint8_t _opaco[16]; };
struct FScriptArray     { void* Data; int32_t Num; int32_t Max; };
// FScriptMap e FScriptSet medem 80 cada (medido), não 32. A conta errada fazia
// todo struct que contivesse um TMap fechar com tamanho menor que o real — e o
// campo seguinte cair no lugar errado, em silêncio.
struct FScriptMap       { uint8_t _opaco[80]; };
struct FScriptSet       { uint8_t _opaco[80]; };
struct FWeakObjectPtr   { int32_t ObjectIndex; int32_t ObjectSerialNumber; };
struct FSoftObjectPtr   { uint8_t _opaco[40]; };   // medido: 40, não 24
struct FScriptDelegate  { FWeakObjectPtr Object; FName FunctionName; };
struct FMulticastScriptDelegate { FScriptArray Invocations; };
struct FScriptInterface { void* Object; void* Interface; };
struct FVector          { double X, Y, Z; };     // UE5 usa double por padrão
struct FRotator         { double Pitch, Yaw, Roll; };
struct FVector2D        { double X, Y; };

// ── PASSING TEXT TO THE GAME: NOT POSSIBLE by this route ────────────────────
//
// There used to be a `TextoParaOJogo` class here, which built an `FString`
// pointing at a buffer of ours. It was REMOVED because it **crashes the
// server**, and the reason is structural, not a bug to be fixed.
//
// THE TEST AND THE RESULT
// -----------------------
// `ConvertToAbsolutePath("test-api-xyz")` called through reflection, with the
// FString built that way. The plugin's log dies on exactly that line:
//
//     [prova] === 1. passar FString PARA o jogo ===
//                                                   <- and the process ends
//
// WHY, AND WHY THERE IS NO SIMPLE FIX
// ------------------------------------
// `ProcessEvent` DESTROYS the parameter block when the function returns: it
// walks the properties' destructor list (`DestructorLink`) and calls each
// destructor. `FString`'s destructor calls `FMemory::Free(Data)` — THE GAME'S
// allocator — on a pointer that came from our stack.
//
// It is not the game "reading it wrong": it is the game doing the right thing
// with memory that is not its own. Any buffer of ours passed as an FString
// through reflection ends up in `FMemory::Free`.
//
// The correct path is allocating with the game's allocator
// (`GMalloc`/`FMemory::Malloc`), and that requires locating the allocator on
// this build — **not measured**. Until it is, the API does not offer the tool,
// because offering it would be handing over a weapon pointed at the server of
// whoever installs it.
//
// WHAT TO DO IN THE MEANTIME
// --------------------------
// For a plugin to communicate: `ConanApi::Log()` (to file), or hook a function
// that ALREADY receives text from the game and alter what passes through it —
// there the FString is the game's, with the game's memory, and nobody frees
// anything improperly.
//
// This comment stays because the mistake is easy to repeat: the class compiled,
// looked right, and the test took 90 seconds to bring the server down.

// ── ler texto que veio DO jogo ──────────────────────────────────────────────
// Declarada dentro de ConanApi, e aqui — depois de FString existir. A definição
// mora no namespace; declarar no escopo global daria erro de símbolo ausente que
// só aparece no link, longe da causa.
namespace ConanApi { std::string TextoDoJogo(const FString& s); }

// ── referência a um membro, resolvida por offset ────────────────────────────
template<typename T>
struct FieldRef
{
    void*     base;
    uintptr_t offset;

    T*   ptr()  const { return reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(base) + offset); }
    T&   Get()  const { return *ptr(); }
    void Set(const T& v) const { *ptr() = v; }

    operator T&() const { return Get(); }
    T& operator=(const T& v) const { Set(v); return Get(); }
    T* operator->() const { return ptr(); }
};

// ── referência a um BIT dentro de um byte compartilhado ─────────────────────
//
// POR QUE ISTO EXISTE, E O DEFEITO QUE ELE CONSERTA
// -------------------------------------------------
// Vários booleanos da Unreal compartilham o MESMO byte — são bitfield. Em
// `Actor`, SETE bools moram no offset 104, distinguidos só pela máscara:
//
//     bNetTemporary          0x68  máscara 0x01
//     bOnlyRelevantToOwner   0x68  máscara 0x04
//     bAlwaysRelevant        0x68  máscara 0x08
//     bReplicateMovement     0x68  máscara 0x10
//     bCallPreReplication    0x68  máscara 0x20
//     ...                    0x68  máscara 0x40
//     bHidden                0x68  máscara 0x80
//
// The previous version generated a `FieldRef<bool>` for each of them, all at
// the same offset. The result is the worst class of defect this project
// recognises: reading `bOnlyRelevantToOwner` answered `true` because `bHidden`
// was set. No error, no garbage — a plausible and wrong boolean.
//
// There were 1,908 members across 401 classes like that: Actor 33,
// PrimitiveComponent 63, CharacterMovementComponent 56, Material 87.
//
// And writing was worse: `bHidden() = true` wrote 0x01 over the whole byte and
// CLEARED the other six at once.
struct BitRef
{
    void*     base;
    uintptr_t offset;
    uint8_t   mascara;

    uint8_t* byte() const
    { return reinterpret_cast<uint8_t*>(base) + offset; }

    bool Get() const { return (*byte() & mascara) != 0; }

    void Set(bool v) const
    {
        uint8_t* p = byte();
        // Lê-modifica-escreve preservando os vizinhos. Não é atômico de
        // propósito: tornar atômico daria a impressão de que dá para escrever
        // bitfield do jogo de qualquer thread, e não dá — quem escreve num byte
        // que o jogo também escreve precisa saber o que está fazendo.
        *p = v ? uint8_t(*p | mascara) : uint8_t(*p & ~mascara);
    }

    operator bool() const { return Get(); }
    bool operator=(bool v) const { Set(v); return v; }
};

// ── raiz de tudo ────────────────────────────────────────────────────────────
class UObject
{
public:
    // offsets medidos na reflexão viva; confirmados por um segundo caminho ao
    // desmontar a vtable, que lê +0x08 ObjectFlags, +0x0C InternalIndex,
    // +0x10 ClassPrivate, +0x18 NamePrivate e +0x20 Outer.
    FieldRef<UClass*> ClassPrivate() { return { this, 0x10 }; }
    FieldRef<FName>   NamePrivate()  { return { this, 0x18 }; }
    FieldRef<UObject*> OuterPrivate(){ return { this, 0x20 }; }

    std::string GetName();
    std::string GetFullName();
    bool        IsA(UClass* c);

    template<typename R = void, typename... A>
    R Call(const char* nome, A&&... a)
    { return ConanApi::Call<R>(this, nome, std::forward<A>(a)...); }

    // Faltava o par: funcao com parametro de SAIDA sao 6.157 das 36.757 desta
    // build, e sem este atalho o dev tinha de descer para ConanApi::CallSaida
    // com o `this` na mao — justo no caso mais facil de errar.
    template<typename R = void, typename... A>
    R CallSaida(const char* nome, A&&... a)
    { return ConanApi::CallSaida<R>(this, nome, std::forward<A>(a)...); }
};

class UField : public UObject
{
public:
    FieldRef<UObject*> Next() { return { this, 0x28 }; }
};

class UStruct : public UField
{
public:
    FieldRef<UClass*>  SuperStruct()     { return { this, 0x40 }; }  // 299/300
    FieldRef<UObject*> Children()        { return { this, 0x48 }; }  // UFunction
    FieldRef<void*>    ChildProperties() { return { this, 0x70 }; }  // FProperty
};

class UClass : public UStruct
{
public:
    // StaticClass da própria UClass: útil para testar se um objeto É uma classe
    static UClass* StaticClass() { return ConanApi::FindClass("Class"); }
};

class UFunction : public UStruct
{
public:
    static UClass* StaticClass() { return ConanApi::FindClass("Function"); }
    FieldRef<uint32_t> FunctionFlags() { return { this, 0xB0 }; }  // FUNC_Native = 0x400
    FieldRef<uint8_t>  NumParms()      { return { this, 0xB4 }; }  // 100% × reflexão
    FieldRef<uint16_t> ParmsSize()     { return { this, 0xB6 }; }  // 96,9% × reflexão
    FieldRef<void*>    Func()          { return { this, 0xD8 }; }  // ponteiro nativo
};
