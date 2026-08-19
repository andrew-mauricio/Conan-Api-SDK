// ============================================================================
//  ExemploOla — o menor plugin possível que ainda PROVA que a API funciona
//
//  Não é "hello world". Um hello world imprime um texto e não prova nada: ele
//  imprimiria igual se cada offset desta API estivesse errado. Este plugin
//  exercita os caminhos que a tabela oferece e IMPRIME O QUE LEU, para que
//  qualquer pessoa confira contra o que o servidor mostra:
//
//     1. reflexão de pé + custo medido -> número no log, não estimativa
//     2. achar classe por nome         -> tem de achar as do Conan, não só engine
//     3. ler membro por offset         -> valor tem de ser plausível, não lixo
//     4. hierarquia por nome           -> DescendeDe tem de bater com o esperado
//     5. chamar função por nome        -> tem de responder o que se sabe de cor
//     6. bitfield x função             -> duas fontes que não se conversam
//
//  Copie esta pasta para começar o seu.
//
//  ────────────────────────────────────────────────────────────────────────────
//  O MODELO MUDOU: TABELA DE FUNÇÕES, NADA NOSSO DENTRO DO SEU BINÁRIO
//  ────────────────────────────────────────────────────────────────────────────
//  Até 17/08/2026 este exemplo incluía `Conan/ConanSDK.h` e compilava 4.398
//  linhas do NOSSO motor para dentro do DLL dele. Agora o único arquivo nosso
//  que entra aqui é `Conan/ConanPluginApi.h` — 246 linhas de declaração pura —
//  e o plugin não linka biblioteca nenhuma nossa. Tudo se chama por `g_api->`.
//
//  Por que isso importa PARA QUEM COPIA ESTE EXEMPLO (o porquê longo está no
//  cabeçalho do ConanPluginApi.h): o seu compilador deixa de importar, porque
//  a fronteira virou C puro; e um conserto no nosso motor não obriga você a
//  recompilar, porque ele não mora mais dentro do seu binário.
//
//  COMPILAR
//     ./compilar.sh          (gera ExemploOla.dll)
//  INSTALAR
//     copiar para  <servidor>/ConanSandbox/Binaries/Win64/Conan-Api/Plugins/
//
//  A PASTA TEM HÍFEN: `Conan-Api`, nunca a grafia emendada.
//  Até 17/08/2026 esta linha mandava copiar para a pasta SEM hífen — e o
//  loader monta `<pasta do exe>\Conan-Api` + `\Plugins\*.dll`
//  (loader/ConanLoader.cpp), com a API derivando Config/Dados/Logs do MESMO
//  lugar (api/src/ConanApi.cpp, CaminhoRaiz). Quem seguia a instrução copiava
//  o DLL para uma pasta que o loader NUNCA varre: o plugin não carregava e não
//  havia uma linha de erro apontando a causa — o log só dizia "pasta de plugins
//  vazia ou ausente" citando o OUTRO caminho, que o dono nem sabia que existia.
//  Falha silenciosa no primeiro contato da comunidade com a API.
//  O script plugins/conferir-caminhos.sh existe para isso não voltar.
// ============================================================================
#include "Conan/ConanPluginApi.h"
#include <windows.h>
#include <stdint.h>

// A tabela chega uma vez, em ConanPluginCarregar, e vale pela vida inteira do
// processo: ela é um objeto estático do lado da API, nunca realocado. Guardar o
// ponteiro num global é o padrão — e é o ÚNICO estado que este plugin tem.
static const ConanApiTabela* g_api = nullptr;

// ── atalhos para ChamarFuncao ───────────────────────────────────────────────
//
// `ChamarFuncao` é deliberadamente crua: vetor de ponteiros para os valores e
// vetor de TAMANHOS. Não existe template `Call<T>` aqui, e é de propósito —
// template é C++, e C++ não atravessa a fronteira entre o seu compilador e o
// nosso (veja ConanPluginApi.h). O preço é escrever o tamanho na mão; o troco
// é que a API confere esse tamanho contra o que a reflexão diz do parâmetro e
// RECUSA a chamada quando não bate, em vez de corromper o bloco em silêncio.
// Estas duas funções existem só para o resto do arquivo ficar legível.
static int ChamarII(void* obj, const char* nome, int32_t a, int32_t b, int32_t* saida)
{
    const void*    args[2] = { &a, &b };
    const uint32_t tams[2] = { 4, 4 };          // IntProperty: 4 bytes cada
    return g_api->ChamarFuncao(obj, nome, args, tams, 2, saida, sizeof(*saida));
}

static int ChamarDD(void* obj, const char* nome, double a, double b, double* saida)
{
    const void*    args[2] = { &a, &b };
    const uint32_t tams[2] = { 8, 8 };          // DoubleProperty: 8 bytes cada
    return g_api->ChamarFuncao(obj, nome, args, tams, 2, saida, sizeof(*saida));
}

// Nome do objeto para um buffer nosso. `NomeDoObjeto` devolve o próprio buffer,
// então dá para usar direto no Log. std::string não cruza a fronteira C — por
// isso o plugin é quem fornece a memória e quem sabe o tamanho dela.
static const char* Nome(void* obj, char* buf, int tam)
{
    return g_api->NomeDoObjeto(obj, buf, tam);
}

static void Passo1_Reflexao()
{
    // ── o custo é MEDIDO, não estimado ──────────────────────────────────────
    //
    // Uma revisão adversarial mediu que decodificar nome custava 7,66 us por
    // objeto, porque cada nome disparava tres VirtualQuery (2.551 ns cada) sob
    // Wine. Com 300 mil objetos, uma varredura passava de 2 SEGUNDOS — e um
    // plugin que resolvesse identidade de jogador no laco do jogo travaria o
    // servidor, com o sintoma "esta lento", que nao aponta para a causa.
    //
    // Numero no log a cada arranque: comentario envelhece, medicao nao. O que
    // a medicao mostra e o CACHE: a 1a resolucao paga a varredura, a 2a nao.
    // Se um dia a 2a chamada custar o mesmo que a 1a, o cache morreu — e isso
    // aparece aqui antes de aparecer como "servidor lento".
    DWORD t0 = GetTickCount();
    void* cdo = g_api->GetDefaultObject("KismetMathLibrary");
    const DWORD d1 = GetTickCount() - t0;

    t0 = GetTickCount();
    cdo = g_api->GetDefaultObject("KismetMathLibrary");        // 2a vez: cache
    const DWORD d2 = GetTickCount() - t0;

    g_api->Log("[1] GetDefaultObject: 1a chamada %lu ms · 2a (cache) %lu ms  %s",
               d1, d2, cdo ? "" : "(NAO ACHOU)");

    // ── o que este passo NAO consegue mais medir, e por que ─────────────────
    //
    // A versao anterior imprimia `NumObjects()` (quantos objetos vivos ha no
    // GUObjectArray) e cronometrava `FindObjects("Actor", ...)`. Nenhuma das
    // duas existe na tabela: ela nao expoe VARREDURA do array de objetos, so
    // busca de UM objeto por nome (`FindObject`). Nao ha como substituir isso
    // por outra funcao da tabela — entao este exemplo deixou de medir, e diz
    // que deixou, em vez de imprimir um numero parecido que nao mede a mesma
    // coisa. Numero errado com cara de certo e pior do que numero nenhum.
    g_api->Log("    (contagem de objetos e varredura por classe nao existem na"
               " tabela — este passo mede so o custo da resolucao por nome)");
}

static void Passo2_Classes()
{
    // Achar classe da ENGINE não prova quase nada — toda API de UE acha Actor.
    // O que prova é achar as classes DO CONAN: elas só existem se o catálogo
    // veio da reflexão deste jogo.
    const char* alvos[] = { "ConanPlayerController", "ConanCharacter",
                            "ConanGameMode", "Actor", "PlayerController" };
    for (const char* n : alvos)
    {
        void* c = g_api->FindClass(n);
        g_api->Log("[2] FindClass(\"%s\") -> %s", n, c ? "achou" : "NAO ACHOU");
    }
}

static void Passo3_Membros()
{
    // Ler membro de um objeto REAL. Se o offset estivesse errado, sairia lixo —
    // e lixo é visível: contagem negativa, número gigante, ponteiro absurdo.
    void* o = g_api->FindObject("ConanPlayerController");
    if (!o) { g_api->Log("[3] nenhum ConanPlayerController no mundo (servidor vazio?)"); return; }

    char buf[256];
    g_api->Log("[3] %s", Nome(o, buf, sizeof(buf)));

    // O offset vem do catálogo da reflexão (golden/, AConanPlayerController:
    // CachedFollowerCount é IntProperty em +0x930), NUNCA de chute. `LerMembro`
    // ainda passa o endereço pelo teste de legibilidade antes de tocar nele:
    // objeto do jogo pode estar na janela do coletor de lixo, e ler ponteiro
    // morto derruba o servidor inteiro.
    int32_t seguidores = 0;
    if (g_api->LerMembro(o, 0x930, &seguidores, sizeof(seguidores)))
        g_api->Log("    CachedFollowerCount (+0x930) = %d", seguidores);
    else
        g_api->Log("    CachedFollowerCount (+0x930): ILEGIVEL — objeto saiu de baixo de nos");
}

static void Passo4_Hierarquia()
{
    void* o = g_api->FindObject("ConanGameMode");
    if (!o) { g_api->Log("[4] ConanGameMode nao encontrado"); return; }

    char buf[256];
    g_api->Log("[4] GameMode: %s", Nome(o, buf, sizeof(buf)));

    // `DescendeDe` percorre a cadeia de superclasses pelo nome. O par
    // positivo/negativo é o que dá valor ao teste: uma implementação que
    // respondesse "sim" para tudo passaria na primeira linha e falharia na
    // segunda. Uma que respondesse "não" para tudo falha na primeira.
    g_api->Log("    DescendeDe(Actor)                 = %s",
               g_api->DescendeDe(o, "Actor") ? "sim" : "nao");
    g_api->Log("    DescendeDe(ConanPlayerController) = %s   %s",
               g_api->DescendeDe(o, "ConanPlayerController") ? "sim" : "nao",
               g_api->DescendeDe(o, "ConanPlayerController")
                 ? "<<< ERRADO: GameMode nao e PlayerController"
                 : "<<< CORRETO — o negativo separa de um 'sim' automatico");
}

static void Passo5_ChamarFuncao()
{
    // ── A PROVA DE QUE ProcessEvent FUNCIONA ────────────────────────────────
    //
    // Os passos 1 a 4 leem. Este ESCREVE parâmetros, chama código do jogo e
    // confere o resultado — e é o único que não pode mentir.
    //
    // As funções escolhidas são da biblioteca de matemática de Blueprint: puras,
    // sem efeito colateral nenhum, e com resposta que se sabe de cor.
    // Se os resultados baterem, TUDO isto está certo ao mesmo tempo:
    //
    //    · a UFunction foi achada percorrendo a hierarquia pelo nome;
    //    · os dois parâmetros foram escritos nos offsets que a reflexão deu;
    //    · ProcessEvent estava mesmo no índice 79 da vtable;
    //    · o retorno foi lido no offset certo do bloco de parâmetros;
    //    · e a travessia plugin -> tabela -> motor não embaralhou nada.
    //
    // Qualquer um desses errado e o resultado não é 12 — é lixo, ou zero, ou o
    // servidor cai. Não existe como passar por acidente.
    void* mat = g_api->GetDefaultObject("KismetMathLibrary");
    if (!mat) { g_api->Log("[5] KismetMathLibrary nao encontrada"); return; }

    // ── PRIMEIRO: um teste que NÃO é comutativo ─────────────────────────────
    //
    // Isto conserta um furo real na prova anterior. `Add_IntInt(7,5)`,
    // `Multiply_IntInt(6,7)` e `FMax(3.5, 9.25)` são todas COMUTATIVAS: dariam
    // o mesmo resultado com os parâmetros trocados de lugar. Elas provam que a
    // chamada acontece e que o retorno é lido — mas NÃO provam que A foi para o
    // offset de A e B para o de B.
    //
    // Subtração não perdoa: se a ordem inverter, 10-3=7 vira 3-10=-7. Um sinal
    // trocado é um discriminante de um bit, impossível de confundir. E no
    // modelo de tabela isso passou a cobrir mais chão: a ordem agora depende
    // também de `args[]` chegar do outro lado da fronteira C na mesma ordem.
    int32_t sub = 0;
    ChamarII(mat, "Subtract_IntInt", 10, 3, &sub);
    g_api->Log("[5] Subtract_IntInt(10, 3) = %d   %s", sub,
        sub == 7  ? "<<< CORRETO — a ORDEM dos parametros esta certa"
      : sub == -7 ? "<<< ERRADO: parametros INVERTIDOS (deu 3-10)"
                  : "<<< ERRADO (esperado 7)");

    // Divisão idem, e com resto: 100/7 = 14 (inteiro). Invertido daria 0.
    int32_t div = 0;
    ChamarII(mat, "Divide_IntInt", 100, 7, &div);
    g_api->Log("[5] Divide_IntInt(100, 7) = %d   %s", div,
        div == 14 ? "<<< CORRETO" : div == 0 ? "<<< ERRADO: invertido" : "<<< ERRADO (esperado 14)");

    int32_t r = 0;
    ChamarII(mat, "Add_IntInt", 7, 5, &r);
    g_api->Log("[5] Add_IntInt(7, 5) = %d   %s", r,
        r == 12 ? "<<< CORRETO — chamada por reflexao FUNCIONA"
                : "<<< ERRADO (esperado 12)");

    int32_t m = 0;
    ChamarII(mat, "Multiply_IntInt", 6, 7, &m);
    g_api->Log("[5] Multiply_IntInt(6, 7) = %d   %s", m,
        m == 42 ? "<<< CORRETO" : "<<< ERRADO (esperado 42)");

    // negativo e zero pegam erro de sinal e de offset que positivo pequeno esconde
    int32_t n = 0;
    ChamarII(mat, "Add_IntInt", -100, 37, &n);
    g_api->Log("[5] Add_IntInt(-100, 37) = %d   %s", n,
        n == -63 ? "<<< CORRETO" : "<<< ERRADO (esperado -63)");

    // ── O TAMANHO DO ARGUMENTO TEM DE SER O TAMANHO DA REFLEXÃO ─────────────
    //
    // Esta prova ATESTAVA COMO CORRETA uma chamada errada, até 17/08/2026. A
    // linha era, no modelo antigo:
    //
    //     float fm = mat->Call<float>("FMax", 3.5f, 9.25f);   // ERRADO
    //
    // e o log do servidor imprimia `FMax(3.5, 9.25) = 9.2500  <<< CORRETO`.
    //
    // A reflexão DESTA build (golden/funcoes.json, KismetMathLibrary::FMax)
    // diz que a função é toda em DoubleProperty, parmssize 24:
    //     A  off=0 size=8 · B  off=8 size=8 · ReturnValue off=16 size=8
    //
    // O template copiava `sizeof(T)` bytes no offset do parâmetro. Com `3.5f`
    // isso são 4 bytes num slot de 8: a metade alta fica zerada e o jogo recebe
    // um DENORMAL. Conferido byte a byte, com a mesma aritmética:
    //     3.5f  no slot de double -> 5,336073e-315
    //     9.25f no slot de double -> 5,394356e-315
    //
    // O 9.25 saía no log por ACIDENTE ARITMÉTICO: com a metade alta zerada, a
    // ordem desses denormais espelha a ordem dos padrões de bits dos floats
    // POSITIVOS. FMax escolhia o operando certo pelo motivo errado, e o retorno
    // era lido de volta nos 4 bytes baixos. Os três casos que a prova antiga
    // escolheu — 3.5/9.25, 2.0/1.0, 100.0/0.5 — eram todos positivos: passavam
    // todos, por acaso, e o sentinela 0xCD não pega nada disso (a função
    // EXECUTOU; ela só recebeu outros números).
    //
    // No modelo de tabela o tamanho deixou de ser deduzido do tipo do C++ e
    // passou a ser DECLARADO (`tams[]`), e a API o confere contra a reflexão —
    // veja o teste do guarda logo abaixo. Mas declarar continua sendo você quem
    // faz: `tams` = o que a reflexão diz, sempre. Aqui, 8 e 8.
    double fm = 0.0;
    ChamarDD(mat, "FMax", 3.5, 9.25, &fm);
    g_api->Log("[5] FMax(3.5, 9.25) = %.6f  %s", fm,
        (fm > 9.2499 && fm < 9.2501) ? "<<< CORRETO" : "<<< ERRADO (esperado 9.25)");

    // ── o caso que SEPARA — sem ele esta prova volta a aprovar o errado ──────
    //
    // Mesmo papel do Subtract_IntInt lá em cima: um discriminante de um bit. Com
    // 8 bytes dá 3.5; com 4 bytes escritos num slot de 8 daria -1.0. Não há
    // como os dois caminhos concordarem aqui.
    double fneg = 0.0;
    ChamarDD(mat, "FMax", 3.5, -1.0, &fneg);
    g_api->Log("[5] FMax(3.5, -1.0) = %.6f  %s", fneg,
        (fneg > 3.4999  && fneg < 3.5001)  ? "<<< CORRETO — o tamanho declarado casa com a reflexao (8 bytes)"
      : (fneg > -1.0001 && fneg < -0.9999) ? "<<< ERRADO: veio -1.0 — argumento de 4 bytes num slot de 8"
                                           : "<<< ERRADO (esperado 3.5)");

    // Negativo dos dois lados: pega o mesmo defeito por outro caminho, e pega
    // também inversão de ordem. Certo = -2.5; com 4 bytes num slot de 8 vinha
    // -7.25; com A e B trocados também daria -7.25 — por isso o de cima, que
    // separa as duas causas, vem primeiro.
    double fneg2 = 0.0;
    ChamarDD(mat, "FMax", -2.5, -7.25, &fneg2);
    g_api->Log("[5] FMax(-2.5, -7.25) = %.6f  %s", fneg2,
        (fneg2 > -2.5001 && fneg2 < -2.4999) ? "<<< CORRETO"
                                             : "<<< ERRADO (esperado -2.5)");

    // ── e agora o GUARDA, que é o que o modelo novo acrescentou ─────────────
    //
    // A chamada abaixo é o defeito de 17/08 escrito de propósito: 4 bytes onde
    // a reflexão pede 8. No modelo antigo isso rodava e mentia. Aqui tem de ser
    // RECUSADA, com o motivo no log e o retorno intocado — e é isso que esta
    // linha prova. Um plugin que copie este exemplo herda a proteção, não o
    // defeito.
    //
    // É seguro: nada é executado quando a chamada é recusada. A linha
    // "RECUSADO" que vai aparecer no log LOGO ABAIXO é esperada.
    g_api->Log("[5] proposital: FMax com tams=4 (a proxima linha RECUSADO e esperada)");
    const float  fa = 3.5f, fb = -1.0f;
    const void*    args[2] = { &fa, &fb };
    const uint32_t tams[2] = { 4, 4 };          // ERRADO de proposito
    double lixo = 12345.0;
    const int passou = g_api->ChamarFuncao(mat, "FMax", args, tams, 2, &lixo, sizeof(lixo));
    g_api->Log("    ChamarFuncao devolveu %d, retorno %s   %s",
        passou, (lixo == 12345.0) ? "intocado" : "SOBRESCRITO",
        (!passou && lixo == 12345.0)
          ? "<<< CORRETO — a API recusou o tamanho errado em vez de corromper"
          : "<<< ERRADO: a chamada de tamanho errado NAO foi barrada");
}

static void Passo6_BitfieldERetornoZero()
{
    // ── DUAS CORREÇÕES PROVADAS DE UMA VEZ, POR FONTES QUE NÃO SE CONVERSAM ──
    //
    // 1. BITFIELD: `bActorEnableCollision` é o bit 0x02 do byte 0x6D (109) do
    //    Actor. No MESMO byte moram `bActorIsBeingDestroyed` (0x04) e
    //    `bAsyncPhysicsTickEnabled` (0x80). Ler o byte inteiro como bool
    //    respondia "true" quando qualquer um dos vizinhos estava ligado — por
    //    isso a tabela expõe `LerBit(obj, offset, mascara)` e não só LerMembro.
    //
    // 2. RETORNO NO OFFSET 0: `GetActorEnableCollision()` não tem argumentos,
    //    então o valor de retorno mora no offset 0 do bloco de parâmetros. A
    //    versão anterior exigia offset > 0 para reconhecer um retorno, e
    //    devolvia `false` para TODA função sem argumentos — inclusive
    //    IsAdmin(). Sem erro nenhum.
    //
    // O teste: para cada Actor que se consiga alcançar, o bit e a função têm de
    // concordar. Duas rotas independentes até o mesmo fato — leitura direta de
    // memória de um lado, execução de código do jogo do outro.
    //
    // ── O QUE ESTE PASSO PERDEU AO MIGRAR, E POR QUE NÃO FOI DISFARÇADO ─────
    //
    // A versão anterior varria 400 Actors com `FindObjects("Actor", ...)` e
    // exigia VARIAÇÃO no conjunto — se todos tivessem o mesmo valor, dois
    // códigos errados que sempre respondem "false" também passariam. E usava
    // `UltimaChamadaExecutou()` para separar "a função respondeu false" de "a
    // chamada foi FILTRADA pelo ProcessEvent" (template de Blueprint, CDO,
    // Actor não inicializado) — ausência de resultado não é resultado.
    //
    // NENHUMA DAS DUAS existe na tabela. Sem varredura, o conjunto aqui são os
    // poucos Actors alcançáveis por nome; sem o sinal de execução, um "false"
    // pode ser resposta ou pode ser filtro. Consequência honesta: este passo
    // deixou de ser conclusivo e passou a ser observacional. Ele imprime o que
    // viu e diz que não separa — em vez de manter o veredito antigo em cima de
    // uma base que encolheu, que é exatamente como um teste vira carimbo.
    static const char* const alvos[] = {
        "ConanPlayerController", "ConanCharacter", "ConanGameMode",
        "PlayerController", "WorldSettings", "GameStateBase", "Actor"
    };

    void* vistos[16];
    int nvistos = 0;
    int concorda = 0, discorda = 0, ligados = 0, desligados = 0;
    char buf[256];

    for (const char* alvo : alvos)
    {
        void* o = g_api->FindObject(alvo);
        if (!o) continue;
        // O offset 0x6D só faz sentido em AActor. Sem esta linha, um objeto que
        // não é Actor faria ler um byte qualquer e o placar contaria lixo.
        if (!g_api->DescendeDe(o, "Actor")) continue;

        // FindObject por nomes diferentes pode devolver o MESMO objeto (o
        // primeiro ConanPlayerController também é um PlayerController). Contar
        // duas vezes o mesmo Actor infla o placar sem acrescentar evidência.
        bool repetido = false;
        for (int i = 0; i < nvistos; ++i) if (vistos[i] == o) { repetido = true; break; }
        if (repetido) continue;
        if (nvistos < 16) vistos[nvistos++] = o;

        const int antes = g_api->LerBit(o, 0x6D, 0x02);          // bit 0x02 do byte 109

        uint8_t ret = 0;
        const int chamou = g_api->ChamarFuncao(o, "GetActorEnableCollision",
                                               nullptr, nullptr, 0, &ret, sizeof(ret));
        if (!chamou)
        {
            g_api->Log("    %s: GetActorEnableCollision nao resolveu", Nome(o, buf, sizeof(buf)));
            continue;
        }
        const int fn = ret ? 1 : 0;

        antes ? ++ligados : ++desligados;
        if (antes == fn) ++concorda;
        else
        {
            ++discorda;
            // Identificar QUEM discorda. Estatística agregada não diz o que
            // fazer com "1 em 7"; o nome do objeto diz.
            g_api->Log("    DIVERGE: %s  · bit=%s funcao=%s",
                       Nome(o, buf, sizeof(buf)), antes ? "true" : "false",
                       fn ? "true" : "false");
        }
    }

    g_api->Log("[6] %d Actors alcancaveis por nome: bActorEnableCollision"
               "(bit 0x02 do byte 0x6D) x GetActorEnableCollision(retorno @0)",
               nvistos);
    g_api->Log("    concordam: %d   divergem: %d   ·   %d ligados, %d desligados",
               concorda, discorda, ligados, desligados);

    if (nvistos == 0)
        g_api->Log("    <<< SEM DADOS: nenhum Actor alcancavel por nome (servidor vazio?)");
    else if (discorda > 0)
        g_api->Log("    <<< DIVERGENCIA: as duas fontes discordam. Pode ser defeito, pode ser"
                   " chamada filtrada pelo ProcessEvent — a tabela nao expoe o sinal que"
                   " separa os dois casos. Investigue pelo nome impresso acima.");
    else if (ligados == 0 || desligados == 0)
        g_api->Log("    <<< INCONCLUSIVO: sem variacao no conjunto, o teste nao separa"
                   " codigo certo de codigo que responde sempre a mesma coisa");
    else
        g_api->Log("    <<< CONSISTENTE — bitfield com mascara E retorno no offset 0"
                   " concordam, COM variacao no conjunto");
}

// ── o contrato ──────────────────────────────────────────────────────────────
//
// O carregador procura esta função pelo nome e passa a tabela. Sem ela, a DLL
// é carregada e nada acontece.
extern "C" __declspec(dllexport)
void ConanPluginCarregar(const ConanApiTabela* api)
{
    // ── esta conferência não é formalidade ──────────────────────────────────
    //
    // Se este plugin for compilado contra uma tabela MAIOR do que a que o
    // servidor tem (API mais velha do que o plugin), todo campo depois do fim
    // da struct real é memória de outra coisa — e chamar um "ponteiro de
    // função" lido dali derruba o servidor num lugar sem nenhuma relação com a
    // causa. `tamanho` é o único jeito de saber isso ANTES de tocar em algo.
    //
    // Não dá para logar o motivo: `Log` é justamente um dos campos em que não
    // se pode confiar aqui. Sair calado é o certo.
    if (!api || api->tamanho < sizeof(ConanApiTabela)) return;
    g_api = api;

    g_api->Log("======================================================");
    g_api->Log(" ExemploOla — API do Conan Exiles Enhanced (tabela v%u, %u bytes)",
               api->versao, api->tamanho);
    g_api->Log("======================================================");

    if (!g_api->Pronta())
    {
        g_api->Log("ABORTADO: reflexao indisponivel. O jogo atualizou? Regere a API.");
        return;
    }

    Passo1_Reflexao();
    Passo2_Classes();
    Passo3_Membros();
    Passo4_Hierarquia();
    Passo5_ChamarFuncao();
    Passo6_BitfieldERetornoZero();
    g_api->Log("== fim ==");
}

// Opcional. Chamada quando o servidor desce de forma limpa. Este plugin não
// registra hook nem thread, então não há nada para desfazer — mas soltar o
// ponteiro deixa explícito que a tabela não vale mais depois daqui.
extern "C" __declspec(dllexport)
void ConanPluginDescarregar(void)
{
    if (g_api) g_api->Log("ExemploOla: descarregando.");
    g_api = nullptr;
}
