# Publicar um plugin — o que a gente pede, e por quê

*Tradução. O documento principal é o [PUBLISHING-A-PLUGIN.md](PUBLISHING-A-PLUGIN.md), em inglês.*

Não existe processo burocrático aqui. Mas tem três coisas que a gente pede, e
cada uma nasceu de um problema real.

## 1. Publique o código-fonte junto

Seu plugin roda **dentro do processo do servidor de outra pessoa**, com os mesmos
poderes que o servidor tem: ele alcança a memória inteira, os dados de identidade
dos jogadores, e qualquer arquivo que o servidor alcança.

Não existe sandbox. Não vai existir — é o que "plugin nativo" significa em
qualquer jogo, e fingir o contrário seria pior do que dizer a verdade.

Então a única proteção real que o dono de servidor tem é **poder ler o que vai
instalar**, ou confiar em quem escreveu. Todos os exemplos deste SDK vão com
fonte, e a gente espera o mesmo de quem publica.

Isso não te impede de cobrar pelo plugin. Impede de pedir que estranhos rodem
código que ninguém pode ver.

## 2. Teste num servidor de verdade antes

Compilar não é funcionar. Este projeto aprendeu isso caro, várias vezes:

- uma prova imprimia `CORRETO` para uma chamada errada, porque todos os valores
  do teste eram positivos e o erro se cancelava;
- um hook registrado com nome inexistente subia, nunca disparava, e ninguém
  descobria — o log dizia que estava tudo bem;
- um plugin gravava 9 MB por boot na pasta errada, e só apareceu quando alguém
  foi procurar por quê.

Suba um servidor local, entre com um personagem, e use o recurso. Nenhum teste
unitário substitui isso.

## 3. Ponha um `PluginInfo.json`

```json
{
  "FullName":      "Meu Plugin",
  "Description":   "O que ele faz, em uma linha",
  "Version":       "1.0.0",
  "MinApiVersion": 2,
  "Dependencies":  ["Permission"]
}
```

Não é formalidade. O `MinApiVersion` faz o carregador **recusar** seu plugin
numa API velha demais, em vez de deixá-lo rodar lendo estrutura que mudou de
tamanho. E quando alguém pedir ajuda num fórum, a primeira pergunta ("qual
versão você tem?") já está respondida no log do servidor.

---

## Erros que a gente vê com frequência

**Guardar arquivo em caminho relativo.** `fopen("dados.db", "w")` grava onde o
**servidor** está, não onde seu plugin está. Use
`api->CaminhoDados("SeuPlugin", "dados.db")`.

**Perguntar ao Permission durante o carregamento.** Nesse instante ele pode
ainda não ter subido, e você conclui que ninguém o instalou. Pergunte quando o
jogador usar o recurso.

**Fazer trabalho no `DllMain`.** Ali o Windows segura uma trava global e quase
qualquer chamada trava o processo inteiro. Faça tudo no `ConanPluginCarregar`.

**Passar `float` onde o jogo espera `double`.** A API recusa e diz — mas é bom
saber antes: **293 funções** desta build corrompem a pilha por esse caminho.

**Chamar a API de outra thread sem cuidado.** Se seu plugin tem thread própria
para I/O, use `api->AgendarNaThreadDoJogo` para tocar em objeto do jogo. Tocar
direto de outra thread derruba o servidor.

---

## Achou um defeito na API?

Abra uma issue com: o que você fez, o que esperava, o que aconteceu, e o trecho
do `Conan-Api/Logs/ConanApi.log`.

Se derrubou o servidor, o log dos últimos segundos vale mais que qualquer
descrição.
