// ============================================================================
//  ConanBase.h — fundação da API de plugins do Conan Exiles Enhanced
//
//  Conan Exiles Enhanced · Unreal Engine 5.6.1 (++exiles+release) · 24383534
//
//  Único header escrito à mão. Todo o resto (ConanSDK.h, 4.705 classes) é
//  gerado a partir da reflexão viva do servidor por tools/gerar_headers.py.
//
//  A IDEIA
//  -------
//  A Funcom não publica SDK nem PDB do servidor. Mas a Unreal carrega a própria
//  reflexão em memória para funcionar — GUObjectArray e FNamePool existem em
//  todo processo UE vivo. Lendo essas duas estruturas dá para reconstruir o
//  catálogo de classes, membros e funções sem depender de ninguém.
//
//  ACESSO A MEMBRO
//  ---------------
//  Membro não é campo de struct — é lido no offset medido, via FieldRef.
//  Reproduzir o struct do jogo campo a campo parece mais elegante, mas um único
//  erro de padding desalinha tudo dali para baixo EM SILÊNCIO, e o defeito só
//  aparece como corrupção de memória em runtime. Com FieldRef, offset errado
//  erra UM campo — o erro fica isolado e visível.
//
//  CHAMADA DE FUNÇÃO
//  -----------------
//  Por NOME, nunca por endereço. Endereço gravado no plugin vira bomba-relógio:
//  no dia da atualização o plugin chama o lugar errado e o servidor morre sem
//  erro legível. O nome custa uma busca na primeira chamada (com cache) e
//  sobrevive a atualizações.
// ============================================================================
#pragma once

// A TABELA. Ate a v2.4.0 este header nao a conhecia: o motor falava por
// InvokeRaw e o plugin por `api->`, dois caminhos separados. A v6 junta os
// dois, porque e' o que permite ao ConanSDK.h (8.287 classes com assinatura
// de verdade) funcionar sem linkar a libconanapi.a — que era o motivo real de
// ele nunca ter entrado no pacote que o README anunciava.
#include "ConanPluginApi.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>
#include <utility>

// ── abrir arquivo sem aviso em nenhum dos dois compiladores ─────────────────
//
// O MSVC marca `fopen` como inseguro (C4996) e emite aviso em toda inclusão. A
// saída fácil seria definir _CRT_SECURE_NO_WARNINGS, mas isso desliga TODOS os
// avisos de segurança da CRT no projeto de quem for compilar plugin — decisão que
// não é nossa de tomar por eles. E aviso que aparece sempre é aviso que ninguém
// lê: em pouco tempo o dev ignora também o que importa.
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

    // ── a última chamada realmente executou? ────────────────────────────────
    //
    // `Call<R>` devolve um valor mesmo quando a função não roda — não há outro
    // jeito, o tipo de retorno é R. Mas devolver zero calado seria o defeito que
    // o sentinela existe para matar. Quem precisa da distinção pergunta:
    //
    //     bool v = ator->GetActorEnableCollision();
    //     if (!ConanApi::UltimaChamadaExecutou()) { /* ausência, não resposta */ }
    //
    // Responde `false` em TODOS os caminhos em que o valor devolvido é ausência:
    //   · o objeto é nulo;
    //   · a função não existe nessa classe (nome errado, ou sumiu numa
    //     atualização do jogo) — este é o caso mais comum, e era o que ficava
    //     de fora: o `return` da função inexistente saía sem tocar na flag, que
    //     nasce `true`, e o plugin recebia zero com a confirmação de que era
    //     resposta;
    //   · algum argumento não coube no parâmetro (a chamada não foi feita);
    //   · pediram um retorno tipado que a função não tem onde devolver;
    //   · o ProcessEvent filtrou a chamada (o sentinela 0xCD, mais abaixo).
    //
    // É por thread: o servidor tem 34+ threads, e uma global aqui faria uma
    // thread ler o resultado da outra.
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

        // ── TAMANHO de cada parâmetro (o ElementSize da reflexão) ───────────
        //
        // POR QUE ESTES DOIS CAMPOS PRECISARAM EXISTIR
        // --------------------------------------------
        // A ficha só carregava OFFSET. Com isso `Empacotar` gravava sizeof(T) do
        // argumento do plugin no offset do parâmetro e pronto — sem nunca
        // perguntar de quantos bytes é o parâmetro. O defeito que isso abre está
        // contado inteiro em `EspacoNoBloco()`, logo abaixo.
        //
        // 0 = quem preencheu esta ficha NÃO mediu o tamanho. Não é erro e não
        // desliga guarda nenhuma: a validação cai no limite derivado dos
        // offsets, que é conservador e nunca reprova chamada correta. Com o
        // tamanho medido a mesma guarda fica exata e passa a pegar também o
        // caso inverso — argumento MENOR que o slot, que não corrompe nada e
        // entrega um número errado (`float` de 4 bytes num DoubleProperty de 8
        // vira denormal: 3.5f lido como double dá 5,336073e-315). São 1.088
        // parâmetros de entrada DoubleProperty nesta build, e o ExemploOla
        // documenta esse caso porque ele já passou por lá.
        //
        // Ficam no FIM da struct de propósito: `ResolveFunction` mora na
        // biblioteca (libconanapi.a) e `Call` é inline dentro do plugin. Campo
        // novo no meio deslocaria Offset[] e OffsetRetorno para uma biblioteca
        // compilada antes — o `montar-distribuicao.sh` reconstrói a
        // libconanapi.a ANTES de compilar qualquer plugin, e é isso que mantém
        // header e biblioteca em passo; no fim da struct, um descompasso não
        // move nenhum campo antigo de lugar.
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

    // ── quanto dá para escrever a partir de um offset do bloco ──────────────
    //
    // O DEFEITO QUE ISTO CONSERTA
    // ---------------------------
    // `Empacotar` copiava sizeof(T) do argumento do plugin para o offset do
    // parâmetro sem olhar o tamanho do parâmetro — e o comentário logo acima
    // dela PROMETIA o contrário ("nunca corromper a pilha do servidor"). O
    // buffer tem exatamente ParmsSize bytes, vindos de um alloca.
    //
    // `ActorComponent::SetComponentTickInterval` tem parmssize=4 e um único
    // FloatProperty de 4 bytes no offset 0. A linha mais natural que existe em
    // C++ — literal sem o sufixo `f`:
    //
    //     comp->Call<void>("SetComponentTickInterval", 0.5);
    //
    // gravava 8 bytes num alloca de 4. Reproduzido antes do conserto, com
    // AddressSanitizer (g++ nativo, o mesmo header):
    //
    //     ERROR: AddressSanitizer: dynamic-stack-buffer-overflow
    //     WRITE of size 8 ... ConanApi::Empacotar<double> ... ConanBase.h:193
    //
    // São 1.468 funções desta build com parmssize=4 e um único parâmetro de
    // entrada — cada uma é esse gatilho. Se a chamada partir de dentro de um
    // callback de hook, a pilha corrompida é a da thread do jogo.
    //
    // O LIMITE, E POR QUE ELE NÃO ESPERA NINGUÉM MEDIR NADA
    // -----------------------------------------------------
    // Os parâmetros ficam lado a lado num bloco só. O teto de quem começa em
    // `off` é o próximo offset ACIMA dele — de outro parâmetro ou do retorno —
    // e, na falta dos dois, o fim do bloco. A ficha já sabe isso hoje.
    //
    // Escrever além do FIM DO BLOCO corrompe a pilha de quem chamou. Escrever
    // além do fim do PARÂMETRO corrompe o parâmetro vizinho: a função roda, com
    // um argumento que ninguém pediu, e o sintoma aparece longe da causa.
    //
    // CALIBRAÇÃO (golden/funcoes.json — os 42.767 parâmetros de entrada das
    // 22.913 funções com entrada desta build, comparando o limite derivado
    // contra o ElementSize que a reflexão informa):
    //     limite == tamanho real .............................. 35.184 (82,27%)
    //     limite  > tamanho real (folga de alinhamento) ........ 7.583 (17,73%)
    //     limite  < tamanho real ................................... 0
    // O zero da última linha é o que decide: o limite NUNCA fica abaixo do
    // tamanho real, então esta guarda não reprova nenhuma chamada correta. Nos
    // 17,73% de folga ela é conservadora — quem fecha a folga é `Tamanho[]`,
    // quando estiver medido. E ela reprova o defeito: nos 10.877 parâmetros de
    // entrada com limite menor que 8 bytes, um literal `double` não passa.
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
    // 7.985 funcoes desta build sairam no ConanSDK.h como template generico
    // — sem assinatura, sem tipo, sem nome de parametro — por UM motivo so:
    // tinham parametro de SAIDA. Medido em 19/08/2026, e era o MAIOR grupo,
    // maior que todos os problemas de tipo somados.
    //
    // "Saida" na Unreal nao e ponteiro nem referencia no bloco: e um slot
    // COMUM do bloco de parametros que a funcao ESCREVE em vez de ler. Quem
    // chama precisa ler aquele slot depois que a funcao roda. O Call antigo
    // montava o bloco, chamava e descartava o bloco inteiro — o valor de saida
    // era escrito e jogado fora.
    //
    // Fora<T> marca "este argumento e' saida": o Empacotar guarda o endereco do
    // destino e o offset do slot, e o Call copia de volta DEPOIS do InvokeRaw.
    //
    // POR QUE UM TIPO, E NAO SO PASSAR T&
    // ------------------------------------
    // Porque T& e' indistinguivel de T no empacotamento, e adivinhar seria o
    // pior caminho: copiar de volta um parametro de ENTRADA sobrescreveria a
    // variavel do plugin com lixo do bloco. O tipo torna a intencao explicita,
    // e o header gerado o emite so onde a reflexao diz CPF_OutParm.
    template<typename T>
    struct Fora
    {
        T* destino;
        explicit Fora(T& d) : destino(&d) {}
    };
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

        // ── SENTINELA NO RETORNO — e o defeito que ele revela ────────────────
        //
        // Nem toda chamada executa. `AActor::ProcessEvent` FILTRA: objeto que é
        // template de Blueprint, CDO, ou Actor ainda não inicializado não
        // despacha função nenhuma. Ela simplesmente não roda.
        //
        // Sem sentinela, o bloco de parâmetros continua zerado e o retorno sai
        // `false` / `0` / ponteiro nulo — indistinguível de um resultado
        // legítimo. Foi assim que apareceu: num teste de 400 Actors, 399
        // concordaram com a leitura direta do bitfield e UM discordou —
        // `FS_AnchorField_GenericEx_C AnchorField_GEN_VARIABLE_...`, um
        // objeto-template. O bit dizia `true`, a função dizia `false`, e a função
        // nunca tinha rodado.
        //
        // 0xCD é o padrão histórico de "memória não inicializada". Se ele
        // sobrevive à chamada, ninguém escreveu ali — a função não executou, e
        // isso vai para o log em vez de virar um valor plausível.
        // `sizeof(R)` não existe para R = void, então o tamanho vem de um alias
        // que troca void por char. Sem isso o compilador avisa em TODA chamada
        // sem retorno — e aviso que aparece sempre é aviso que ninguém lê.
        //
        // O ESPAÇO DO RETORNO SAI DA MESMA CONTA DOS ARGUMENTOS
        // ------------------------------------------------------
        // Antes o único teto era o fim do bloco (`OffsetRetorno + sizeof(R) <=
        // ParmsSize`). Só que o retorno nem sempre é o último parâmetro: são
        // 1.572 das 17.436 funções com retorno desta build em que existe
        // parâmetro de entrada em offset MAIOR que o do retorno. Nessas, um R
        // grande demais fazia o `memset(0xCD)` passar por cima de um argumento
        // JÁ empacotado — a função recebia lixo de propósito, que é exatamente o
        // que o comentário abaixo diz que não se faz. `EspacoNoBloco` para no
        // vizinho, não no fim do bloco.
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

// ── PASSAR TEXTO PARA O JOGO: NÃO É POSSÍVEL por este caminho ───────────────
//
// Havia uma classe `TextoParaOJogo` aqui, que montava uma `FString` apontando
// para um buffer nosso. Ela foi REMOVIDA porque **derruba o servidor**, e o
// motivo é estrutural, não um bug a consertar.
//
// O TESTE E O RESULTADO
// --------------------
// `ConvertToAbsolutePath("teste-api-xyz")` chamado por reflexão, com a FString
// montada assim. O log do plugin morre exatamente nessa linha:
//
//     [prova] === 1. passar FString PARA o jogo ===
//                                                   <- e o processo termina
//
// POR QUE, E POR QUE NÃO TEM CONSERTO SIMPLES
// -------------------------------------------
// `ProcessEvent` DESTRÓI o bloco de parâmetros quando a função retorna: ele
// percorre a lista de destrutores das propriedades (`DestructorLink`) e chama o
// destrutor de cada uma. O destrutor de `FString` chama `FMemory::Free(Data)` —
// o alocador DO JOGO — sobre um ponteiro que veio da nossa pilha.
//
// Não é o jogo "lendo errado": é o jogo fazendo o certo com memória que não é
// dele. Qualquer buffer nosso passado como FString por reflexão vai para o
// `FMemory::Free`.
//
// O caminho correto é alocar com o alocador do jogo (`GMalloc`/`FMemory::Malloc`),
// e isso exige achar o alocador nesta build — **não medido**. Enquanto não estiver,
// a API não oferece a ferramenta, porque oferecê-la seria entregar uma arma
// apontada para o servidor de quem instalar.
//
// O QUE FAZER ENQUANTO ISSO
// -------------------------
// Para o plugin se comunicar: `ConanApi::Log()` (arquivo), ou hookar uma função
// que JÁ recebe texto do jogo e alterar o que passa por ela — aí a FString é do
// jogo, com memória do jogo, e ninguém libera nada indevido.
//
// Este comentário fica porque o erro é fácil de repetir: a classe compilava,
// parecia certa, e o teste levou 90 segundos para derrubar o servidor.

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
// A versão anterior gerava `FieldRef<bool>` para cada um, todos no mesmo
// offset. O resultado é o pior defeito que este projeto reconhece: ler
// `bOnlyRelevantToOwner` respondia `true` porque `bHidden` estava ligado.
// Não dava erro, não dava lixo — dava um booleano plausível e errado.
//
// Eram 1.908 membros em 401 classes assim: Actor 33, PrimitiveComponent 63,
// CharacterMovementComponent 56, Material 87.
//
// E escrever era pior: `bHidden() = true` gravava 0x01 no byte inteiro e
// DESLIGAVA os outros seis de uma vez.
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
