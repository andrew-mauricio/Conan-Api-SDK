# O que está sob qual licença

Este repositório — o **SDK** — está sob **MIT**. Isso vale para tudo que há
aqui: os headers, o `ConanSDK.h`, os exemplos e o Permission.

**Na prática, para você:** compile, altere, publique, **venda**. Sem pedir
autorização, sem pagar nada, sem dividir nada. O plugin é seu e a licença dele
é escolha sua.

## E o loader?

O **[Conan-Api](../../../Conan-Api)** — o carregador, o motor e os binários que
rodam no servidor — está sob **licença própria**, e ela é restritiva num ponto:
a API não pode ser revendida, re-hospedada nem incluída em pacote comercial.

Isso não te afeta enquanto você escreve plugins. Nenhuma linha do motor entra no
seu binário: você fala com uma tabela de ponteiros de função, e é por isso que o
header pode ser MIT sem contaminar nada.

## Por que a divisão existe

O que você constrói é seu. A fundação fica com quem a mantém, para que exista
**uma** API com um caminho de atualização quando o jogo muda — em vez de cinco
cópias divergentes que ninguém consegue acompanhar.

O link deste repositório é livre: divulgue e indexe onde quiser.
