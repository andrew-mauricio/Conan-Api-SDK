// ============================================================================
//  ConanPluginApi.h — A TABELA. É isto que um plugin recebe, e é só isto.
//
//  ┌──────────────────────────────────────────────────────────────────────┐
//  │  Este arquivo é AUTOSSUFICIENTE. Não inclua mais nada nosso.          │
//  │  Não há biblioteca para linkar, não há .cpp nosso para compilar.      │
//  └──────────────────────────────────────────────────────────────────────┘
//
//  ────────────────────────────────────────────────────────────────────────────
//  COMO FUNCIONA
//  ────────────────────────────────────────────────────────────────────────────
//  O carregador entra no processo do servidor, monta a reflexão, e chama o seu
//  plugin passando um ponteiro para uma tabela de funções:
//
//      static const ConanApiTabela* g_api = nullptr;
//
//      extern "C" ConanAcao AoFalar(ConanChamada* c)
//      {
//          return CONAN_CONTINUAR;
//      }
//
//      extern "C" __declspec(dllexport)
//      void ConanPluginCarregar(const ConanApiTabela* api)
//      {
//          if (!api || api->tamanho < sizeof(ConanApiTabela)) return;
//          g_api = api;
//          g_api->Log("meu plugin subiu");
//          g_api->HookProcessEvent("ServerSendChatMessage", AoFalar, nullptr, 100);
//      }
//
//  Este exemplo COMPILA como está — é extraído deste comentário e compilado a
//  cada montagem do pacote. A versão anterior chamava HookProcessEvent com dois
//  argumentos quando ela exige quatro: quem copiasse daqui, que é o primeiro
//  lugar onde um dev olha, batia num erro de compilação na primeira tentativa.
//
//  Você chama tudo através de `api->`. Não existe nenhuma implementação nossa
//  dentro do seu binário.
//
//  ────────────────────────────────────────────────────────────────────────────
//  POR QUE ASSIM, E NÃO COMPILANDO O FONTE JUNTO
//  ────────────────────────────────────────────────────────────────────────────
//  Três motivos, e os três importam para VOCÊ, não só para nós:
//
//  1. SEU COMPILADOR NÃO IMPORTA MAIS. Isto é C puro: `struct` de ponteiros de
//     função com convenção `__cdecl`. Visual Studio de qualquer versão, MinGW,
//     clang — todos concordam sobre isso. Antes, o SDK precisava ir com o fonte
//     porque biblioteca C++ não atravessa compilador: layout de `std::string` e
//     de vtable mudam entre MSVC e MinGW, e até entre versões do MSVC. Linkava,
//     rodava, e corrompia memória sem erro legível. Esse problema deixa de
//     existir aqui.
//
//  2. ATUALIZAR A API NÃO OBRIGA VOCÊ A RECOMPILAR. Enquanto campos novos forem
//     acrescentados **no fim** da tabela e `versao` subir, seu plugin compilado
//     hoje continua funcionando amanhã. Se o motor de hooks mudar por dentro,
//     você não fica sabendo — e é assim que tem de ser.
//
//  3. O MOTOR FICA DE UM LADO SÓ. O decodificador de instruções, a mesa de
//     hooks, os offsets desta build — nada disso entra no seu binário. Menos
//     código seu para dar errado, e nós podemos consertar um defeito do motor
//     sem pedir que 40 plugins sejam recompilados.
//
//  ────────────────────────────────────────────────────────────────────────────
//  COMPATIBILIDADE: SEMPRE CONFIRA `versao` E `tamanho`
//  ────────────────────────────────────────────────────────────────────────────
//  Um plugin compilado contra uma tabela MAIOR, rodando numa API mais velha,
//  leria ponteiro além do fim da struct — e chamaria lixo. Por isso:
//
//      if (!api || api->tamanho < sizeof(ConanApiTabela)) {
//          // API mais velha que este plugin. Use só o que existe, ou saia.
//      }
//
//  Nós nunca removemos nem reordenamos campo. Só acrescentamos no fim.
// ============================================================================
#ifndef CONAN_PLUGIN_API_H
#define CONAN_PLUGIN_API_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// A versão sobe quando campos são ACRESCENTADOS. Nunca quando algo muda de
// lugar — isso não acontece.
#define CONAN_API_VERSAO 6

// ── o que um hook recebe ────────────────────────────────────────────────────
//
// `Parms` é o bloco de parâmetros da função interceptada, no layout que a
// reflexão descreve. `Obj` é o objeto dono da chamada.
typedef struct ConanChamada
{
    void*       Obj;         // o UObject que recebeu a chamada
    void*       Func;        // a UFunction
    void*       Parms;       // bloco de parâmetros (pode ser NULL)
    uint32_t    ParmsSize;   // tamanho do bloco, em bytes
    // O FName da função, JÁ RESOLVIDO. Comparar dois int32 é o teste O(1) que
    // permite a um hook curinga separar "nome novo" de ruído sem decodificar
    // texto a 6.300 chamadas por segundo. Sem isto, um plugin de descoberta não
    // tem sobre o que indexar.
    int32_t     NomeIndice;  // FName.ComparisonIndex
    int32_t     NomeNumero;  // FName.Number  (Foo_1 é Number=2)
} ConanChamada;

// O que o seu callback devolve.
typedef enum ConanAcao
{
    CONAN_CONTINUAR = 0,   // deixa a função original rodar
    CONAN_CANCELAR  = 1    // engole a chamada: o jogo não executa
} ConanAcao;

typedef ConanAcao (*ConanFnAntes)(ConanChamada* c);
typedef void      (*ConanFnDepois)(ConanChamada* c);
typedef void      (*ConanFnTarefa)(void* contexto);

// ── por que um hook por endereço pode ser RECUSADO ──────────────────────────
//
// Recusa não é erro nosso: é a API se negando a fazer algo que corromperia o
// servidor. O motivo vem sempre no log, em português.
typedef enum ConanRecusa
{
    CONAN_OK = 0,
    CONAN_RECUSA_ENDERECO_INVALIDO,
    CONAN_RECUSA_BYTES_INESPERADOS,
    CONAN_RECUSA_SEM_JANELA_ATOMICA,
    CONAN_RECUSA_INSTRUCAO_DESCONHECIDA,
    CONAN_RECUSA_SALTO_CAI_NO_PATCH,
    CONAN_RECUSA_FORA_DA_PDATA,
    CONAN_RECUSA_JA_HOOKADA,
    CONAN_RECUSA_SEM_ARENA,
    CONAN_RECUSA_API_NAO_PRONTA
} ConanRecusa;

// ============================================================================
//  A TABELA
//
//  Ordem NUNCA muda. Campos novos entram no fim, e `versao` sobe.
// ============================================================================
typedef enum ConanTipoSaida
{
    CONAN_SAIDA_POD = 0,   // copia os bytes do slot
    CONAN_SAIDA_TEXTO,     // FString: DECODIFICA. Copiar os 16 bytes daria
                           // ao plugin um ponteiro que o ProcessEvent
                           // destroi ao retornar.
    CONAN_SAIDA_TEXTO_RICO,// FText, pelo mesmo motivo
    CONAN_SAIDA_LISTA      // TArray: copia os ELEMENTOS, nunca o cabecalho
} ConanTipoSaida;

typedef struct ConanSaida
{
    int      indice;       // qual parametro, na ordem da reflexao
    void*    destino;      // onde escrever
    uint32_t tipo;         // ConanTipoSaida
    uint32_t capacidade;   // POD: bytes · TEXTO: capacidade do char* ·
                           // LISTA: quantos elementos cabem
    uint32_t tamElemento;  // so' LISTA
    int*     contagem;     // so' LISTA: recebe quantos couberam (pode ser NULL)
} ConanSaida;

typedef struct ConanApiTabela
{
    // ── cabeçalho: confira isto antes de usar o resto ───────────────────────
    uint32_t versao;    // CONAN_API_VERSAO de quem preencheu
    uint32_t tamanho;   // sizeof(ConanApiTabela) de quem preencheu

    // ── diagnóstico ─────────────────────────────────────────────────────────
    //
    // Escreve em Conan-Api/Logs/ConanApi.log, com data e rotação. É o seu
    // canal para falar com o dono do servidor — e o único, porque mandar texto
    // de volta para o jogo derruba o servidor (veja a documentação).
    void (*Log)(const char* fmt, ...);

    // A reflexão está de pé? Se devolver 0, o jogo provavelmente atualizou e a
    // API se recusou a trabalhar com offsets que não conferem. Não insista.
    int (*Pronta)(void);

    // ── memória: leia antes de tocar ────────────────────────────────────────
    //
    // Diz se [p, p+tam) está MAPEADO e legível.
    //
    // ── O QUE ELE NÃO RESPONDE, E ISSO IMPORTA MUITO ────────────────────────
    //
    // `Legivel` responde "esta memória está mapeada", NÃO "este objeto está
    // vivo". A diferença derruba servidor, e é a armadilha mais fácil de cair
    // nesta API:
    //
    //   · você guarda um ponteiro de UObject entre um hook e outro;
    //   · o coletor de lixo do jogo destrói o objeto e reaproveita o endereço;
    //   · `Legivel` continua devolvendo 1, porque a página segue mapeada;
    //   · você lê campos de um objeto que virou outra coisa, e age sobre lixo.
    //
    // Não existe primitiva barata para "está vivo" — a Unreal resolve isso com
    // ponteiros fracos que a reflexão não expõe. O que dá para fazer, e é o que
    // recomendamos:
    //
    //   NÃO guarde ponteiro de objeto do jogo entre chamadas. Pegue-o de novo
    //   dentro de cada hook (`c->Obj`) ou por FindObject/FindObjects na hora
    //   em que for usar. É mais barato do que parece e não tem esta classe de
    //   defeito.
    //
    // Use `Legivel` para o que ele serve: recusar ponteiro obviamente inválido
    // antes de tocar nele. Já evitou queda real aqui — uma leitura 256 KB além
    // do fim de um pool derrubava o servidor inteiro.
    int (*Legivel)(const void* p, size_t tam);

    // ── reflexão ────────────────────────────────────────────────────────────
    void* (*FindClass)(const char* nome);          // UClass* ou NULL
    void* (*FindObject)(const char* nome);         // UObject* ou NULL
    void* (*GetDefaultObject)(const char* nomeClasse);  // o CDO, por nome
    int   (*DescendeDe)(void* obj, const char* nomeClasse);
    const char* (*NomeDoObjeto)(void* obj, char* saida, int tam);

    // ── membros por offset ──────────────────────────────────────────────────
    //
    // O offset vem do catálogo da reflexão (Ferramentas/), não de chute.
    int (*LerMembro)(void* obj, uint32_t offset, void* saida, uint32_t tam);
    int (*EscreverMembro)(void* obj, uint32_t offset, const void* valor, uint32_t tam);
    // bitfield: 7 bools cabem num byte, e ler o byte inteiro devolve lixo
    int (*LerBit)(void* obj, uint32_t offset, uint8_t mascara);
    int (*EscreverBit)(void* obj, uint32_t offset, uint8_t mascara, int valor);

    // ── chamar função do jogo por reflexão ──────────────────────────────────
    //
    // `args` é um vetor de ponteiros para os valores, e `tams` os tamanhos de
    // cada um. A API confere cada tamanho contra o parâmetro real e RECUSA se
    // não bater — passar 4 bytes onde o jogo espera 8 escreve lixo no resto, e
    // passar 8 onde cabem 4 estoura o bloco. Devolve 1 se executou.
    int (*ChamarFuncao)(void* obj, const char* nomeFuncao,
                        const void** args, const uint32_t* tams, int nargs,
                        void* retorno, uint32_t tamRetorno);

    // ── hooks por nome: o caminho normal ────────────────────────────────────
    //
    // Filtra por índice de FName antes de despachar, então um hook em
    // "ServerSendChatMessage" não custa nada quando o jogo executa outra coisa.
    // Devolve o id (0 = falhou; o motivo vai no log).
    uint32_t (*HookProcessEvent)(const char* nomeFuncao,
                                 ConanFnAntes antes, ConanFnDepois depois,
                                 int prioridade);
    // Só remove hook SEU: a API guarda qual módulo registrou cada um.
    int (*RemoverHook)(uint32_t id);

    // ── hooks por endereço ──────────────────────────────────────────────────
    ConanRecusa (*HookFuncao)(void* endereco, void* nova, void** original);
    ConanRecusa (*HookVirtual)(void* classe, int indice, void* nova, void** original);
    // Toda execução de Blueprint, inclusive a que ProcessEvent não vê.
    // Medido: ~6.300/s em servidor no ar. Seu detour DEVE chamar o original.
    ConanRecusa (*HookExecucaoDeBlueprint)(void* nova, void** original);
    const char* (*TextoRecusa)(ConanRecusa r);

    // ── rodar na thread do jogo ─────────────────────────────────────────────
    //
    // Tocar objeto do jogo de outra thread derruba o servidor. Se o seu plugin
    // tem thread própria, agende por aqui.
    // A tarefa roda na THREAD DO JOGO — e agora isso é garantido, não provável.
    //
    // Como: o motor conta as chamadas de ProcessEvent por thread e elege a
    // dominante (medido neste servidor: 19.981 de 20.000, ou 99,9%). Antes a
    // tarefa rodava na thread que por acaso passasse pelo funil; dava certo
    // quase sempre, pela mesma dominância — mas "quase sempre" não é garantia,
    // e tocar no mundo pela thread errada corrompe estado devagar e sem erro.
    //
    // Enquanto a eleição não conclui (segundos), NENHUMA tarefa roda, e o log
    // diz isso uma vez. Atrasar a primeira execução custa segundos; executá-la
    // na thread errada custa o servidor de outra pessoa.
    uint32_t (*AgendarNaThreadDoJogo)(ConanFnTarefa tarefa, uint32_t segundos,
                                      void* contexto, int repetir);
    int      (*CancelarAgendamento)(uint32_t id);

    // ── caminhos: tudo dentro da SUA pasta ──────────────────────────────────
    //
    // Seu plugin mora em Conan-Api/Plugins/<SuaPasta>/ e guarda tudo lá.
    // Passe o nome da SUA PASTA.
    const char* (*CaminhoConfig)(const char* suaPasta);
    const char* (*CaminhoDados)(const char* suaPasta, const char* arquivo);
    const char* (*CaminhoRaiz)(void);

    // ── ler/escrever parâmetros dentro de um hook ───────────────────────────
    int (*LerParm)(ConanChamada* c, int indice, void* saida, uint32_t tam);
    int (*EscreverParm)(ConanChamada* c, int indice, const void* valor, uint32_t tam);
    int (*LerRetorno)(ConanChamada* c, void* saida, uint32_t tam);
    int (*DefinirRetorno)(ConanChamada* c, const void* valor, uint32_t tam);

    // Texto que JÁ É do jogo (FString em bloco de parâmetros) para char*.
    // Só leitura: montar FString nossa e passar ao jogo derruba o servidor.
    int (*LerTextoDoJogo)(const void* base, uint32_t offset, char* saida, int tam);

    // ── diagnóstico do próprio motor ────────────────────────────────────────
    void (*EstatisticaHooks)(uint64_t* total, uint64_t* despachadas);

    // ═══════════════════════════════════════════════════════════════════════
    //  ACRESCENTADOS NA VERSÃO 2
    //
    //  Estes vieram de migrar os nossos próprios exemplos para a tabela e
    //  descobrir que ela não bastava. Vale registrar o que a ausência custava,
    //  porque é o tipo de buraco que só aparece quando alguém tenta usar:
    //
    //    · sem NumObjects/GetObjectByIndex, o Cartógrafo não enumera NADA — e
    //      é dele que sai o catálogo inteiro da reflexão;
    //    · sem FindObjects (plural), contar jogadores era impossível:
    //      FindObject devolve o PRIMEIRO, e com dois jogadores no servidor um
    //      plugin enxergaria um só. Não é a mesma pergunta com outro nome;
    //    · sem NomeDeFName, um extrator não lê FField/FFieldClass/UEnum, que
    //      não são UObject e guardam o nome em outro offset. Usar
    //      NomeDoObjeto neles devolveria texto plausível e ERRADO;
    //    · sem HookProcessEventTudo, o Gravador de Eventos simplesmente não
    //      existe: ele serve para DESCOBRIR quais funções o jogo chama, e por
    //      isso não tem nome para passar a HookProcessEvent.
    // ═══════════════════════════════════════════════════════════════════════

    // ── varrer o mundo ──────────────────────────────────────────────────────
    //
    // É por aqui que um plugin de descoberta começa. `NumObjects` é o total de
    // entradas do GUObjectArray; `GetObjectByIndex` devolve a i-ésima (pode ser
    // NULL: há buracos no array, e isso é normal, não erro).
    int   (*NumObjects)(void);
    void* (*GetObjectByIndex)(int indice);

    // Todos os objetos de uma classe. Devolve quantos couberam em `saida`.
    // `incluirFilhas` != 0 traz também as subclasses — para PlayerState, é o
    // que faz aparecerem os jogadores de verdade.
    int (*FindObjects)(const char* nomeClasse, void** saida, int max,
                       int incluirFilhas);

    // ── FName cru ───────────────────────────────────────────────────────────
    //
    // Texto de um FName lido da memória, por índice. Necessário para tudo que
    // NÃO é UObject: FField (nome em +0x20), FFieldClass (+0x00) e os pares
    // <FName,int64> de UEnum. Devolve o número de caracteres escritos.
    int (*NomeDeFName)(int32_t indice, char* saida, int tam);

    // Nome COMPLETO (com classe e pacote). `NomeDoObjeto` devolve o curto, que
    // não distingue duas instâncias da mesma classe.
    const char* (*NomeCompletoDoObjeto)(void* obj, char* saida, int tam);

    // ── a chamada EXECUTOU mesmo? ───────────────────────────────────────────
    //
    // `ChamarFuncao` devolve 1 também quando o jogo FILTROU a chamada (template
    // de Blueprint, CDO, Actor não inicializado), e aí o retorno vem do bloco
    // zerado. Sem este sinal não dá para separar "a função respondeu false" de
    // "a função não rodou" — e os dois viram o mesmo `false` no seu plugin.
    int (*UltimaChamadaExecutou)(void);

    // ── o CURINGA: hook em TODA função ──────────────────────────────────────
    //
    // Não filtra por nome: dispara para tudo que passa pelo funil. Serve para
    // DESCOBRIR o que o jogo chama, quando você ainda não sabe o nome.
    //
    // CUSTA CARO e tem prazo de validade de propósito: `segundos` desliga
    // sozinho. Ligado no arranque do servidor, ele impediu o mundo de terminar
    // de carregar — travou em 4,35 GB em vez de 8,7 GB. Ligue com o servidor
    // já de pé, colha o que precisa, e deixe expirar.
    uint32_t (*HookProcessEventTudo)(ConanFnAntes antes, uint32_t segundos);

    // ═══════════════════════════════════════════════════════════════════════
    //  ACRESCENTADOS NA VERSÃO 3 — FALAR COM O JOGADOR
    //
    //  Até aqui, todo plugin era MUDO: dava para ler o chat, cancelar comando e
    //  escrever no log, mas não para responder. Uma loja não dizia "você
    //  comprou", um teleporte não dizia "pronto", um menu de ajuda era
    //  impossível.
    //
    //  A causa era estrutural e está medida: montar uma FString apontando para
    //  buffer SEU e passá-la ao jogo derruba o servidor. O jogo destrói o bloco
    //  de parâmetros ao retornar e chama o alocador DELE sobre memória SUA.
    //
    //  Resolvido pedindo ao próprio jogo que aloque, e sobrescrevendo o
    //  conteúdo. Você não precisa saber disso — passe `const char*` e pronto.
    // ═══════════════════════════════════════════════════════════════════════

    // Anuncia para TODOS os jogadores conectados. Devolve 1 se a chamada
    // chegou ao jogo.
    //
    //     g_api->MensagemParaTodos("O servidor reinicia em 5 minutos.");
    int (*MensagemParaTodos)(const char* texto);

    // Fala com UM jogador, pelo nome que ele usa no jogo. O nome vem, por
    // exemplo, do campo `userName` do chat (veja Docs/EVENTOS.md).
    //
    //     g_api->MensagemParaJogador(nome, "Kit entregue. Volte em 24h.");
    //
    // Devolve 0 se o jogador não estiver conectado — trate isso, porque ele
    // pode ter saído entre o comando e a resposta.
    int (*MensagemParaJogador)(const char* nomeDoJogador, const char* texto);

    // Escreve na TELA do jogador, não no chat. `playerController` é o dele —
    // você o tem no hook de login, ou por FindObjects("ConanPlayerController").
    // `segundos` é quanto tempo o texto fica; 0 usa o padrão (5 s).
    //
    //     g_api->MensagemNaTela(pc, "Bem-vindo ao servidor!", 8.0f);
    //
    // As funções de HUD do Conan recebem FText (tipo bem mais complexo, com
    // tabela de localização). Esta usa ClientMessage, que faz o serviço com
    // FString e é a rota clássica da Unreal para o servidor escrever na tela.
    int (*MensagemNaTela)(void* playerController, const char* texto, float segundos);

    // ── v4: TEXTO COM MEMORIA DO JOGO ───────────────────────────────────────
    //
    // Preenchem 16 bytes com uma FString / FText que o JOGO alocou. O plugin
    // passa esses 16 bytes como argumento de qualquer funcao que peca FString
    // ou FText, e nao precisa saber o layout de nenhuma das duas.
    //
    // POR QUE VOCE NAO PODE MONTAR ISSO SOZINHO
    // ------------------------------------------
    // O ProcessEvent destroi o bloco de parametros ao retornar e chama o
    // alocador DO JOGO sobre o ponteiro que estiver la. Se for memoria do seu
    // plugin, o servidor cai — esta medido neste projeto, nao e' teoria.
    //
    // Devolvem 1 se deu certo, 0 se nao. Zero significa que a chamada seguinte
    // NAO deve ser feita: passar 16 bytes zerados como FString e' entregar
    // ponteiro nulo ao jogo.
    //
    //     unsigned char t[16];
    //     if (api->CriarTextoDoJogo("Bem-vindo!", t))
    //         api->ChamarFuncao(pc, "AlgumaCoisa", ...);
    int (*CriarTextoDoJogo)(const char* texto, void* destino16Bytes);
    int (*CriarTextoRicoDoJogo)(const char* texto, void* destino16Bytes);

    // ── v5: O MEMBRO PELO NOME, E SE ELE E' REPLICADO ────────────────────────
    //
    // OffsetDoMembro devolve o offset NESTA build, resolvido pela reflexão, ou
    // -1 se não achar. Prefira isto a gravar o número no seu plugin: offset
    // gravado vira bomba-relógio no dia em que o jogo atualizar, e o sintoma é
    // ler lixo achando que leu o jogo.
    //
    //     const int32_t off = api->OffsetDoMembro(ator, "RelativeLocation");
    //     if (off >= 0) api->LerMembro(ator, off, &pos, sizeof(pos));
    //
    // EhReplicado responde 1 · 0 · -1. O -1 NÃO é "pode escrever": é "não sei".
    //
    // POR QUE VOCÊ DEVE PERGUNTAR ANTES DE ESCREVER
    // ----------------------------------------------
    // Escrever direto num campo que o jogo replica funciona no servidor e o
    // cliente não vê. Em campo de posição é pior: a predição do cliente corrige
    // de volta, e o sintoma é rubber-banding que ninguém liga a um plugin.
    //
    // Quando existir uma função do jogo que faça a mudança, chame-a: ela
    // percorre o caminho que já replica. Escrever no campo fura esse caminho.
    // 1.222 dos 36.210 membros desta build são replicados — entre eles
    // SceneComponent::RelativeLocation.
    int32_t (*OffsetDoMembro)(void* objeto, const char* nome);
    int     (*EhReplicado)(void* objeto, uint32_t offset);
    int     (*NomeDoMembro)(void* objeto, uint32_t offset, char* saida, int tam);

    // ── v6: CHAMAR COM PARAMETRO DE SAIDA, SEM LINKAR NADA ──────────────────
    //
    // POR QUE ISTO EXISTE
    // -------------------
    // O `ConanSDK.h` — 8.287 classes do jogo com assinatura de verdade — nao ia
    // no pacote, e o README o anunciava. A causa: ele emite `ConanApi::Call<>` e
    // `ConanApi::CallSaida<>`, que chamam `InvokeRaw`/`ResolveFunction`, e essas
    // moram na libconanapi.a. Ou seja: usar o SDK exigia linkar a nossa
    // biblioteca — exatamente o que o modelo de tabela existe para evitar, e o
    // contrario do que a capa promete ("seu compilador nao importa").
    //
    // `ChamarFuncao` (v1) ja' cobria ENTRADA e retorno. Faltava o que o jogo faz
    // em 6.157 das assinaturas: escrever num slot de SAIDA. Sem isto, um terco
    // do SDK nao tinha como funcionar por tabela.
    //
    // Os quatro tipos de saida nao sao capricho — cada um tem um jeito so' de
    // ser lido em seguranca:

    // Como ChamarFuncao, mais os slots de saida. Devolve 1 se executou.
    // As saidas sao copiadas DEPOIS da chamada e ANTES de o jogo destruir o
    // bloco de parametros — que e' a unica janela em que elas valem.
    int (*ChamarFuncaoEx)(void* obj, const char* nomeFuncao,
                          const void** args, const uint32_t* tams, int nargs,
                          const ConanSaida* saidas, int nsaidas,
                          void* retorno, uint32_t tamRetorno);

    // Campos novos entram AQUI, no fim, e `versao` sobe. Nunca no meio.
} ConanApiTabela;

// ============================================================================
//  O CONTRATO DO SEU PLUGIN
//
//  Exporte esta função. O carregador a procura por nome; sem ela, sua DLL é
//  carregada e nada acontece.
//
//      extern "C" __declspec(dllexport)
//      void ConanPluginCarregar(const ConanApiTabela* api);
//
//  Opcional, chamada quando o servidor desce de forma limpa:
//
//      extern "C" __declspec(dllexport)
//      void ConanPluginDescarregar(void);
//
//  NÃO faça trabalho no DllMain: ali o loader do Windows segura uma trava, e
//  chamar quase qualquer coisa trava o processo. Faça tudo no Carregar.
// ============================================================================
// Lado NOSSO: quem monta a tabela. Um plugin não chama isto — ele recebe o
// ponteiro pronto em ConanPluginCarregar.
#ifdef __cplusplus
}  // extern "C" — a linha abaixo é C++ de propósito, é interna nossa
namespace ConanApi { const ConanApiTabela* TabelaDoPlugin(); }
extern "C" {
#endif

typedef void (*ConanFnCarregar)(const ConanApiTabela* api);
typedef void (*ConanFnDescarregar)(void);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif // CONAN_PLUGIN_API_H
