# Aula 5 — Personagens que andam e falam

**Objetivo:** um guarda que vai até o jogador, fala e faz uma animação.

## Andar

*[guarda] anda até [posição] em [segundos] s*: o guarda acha o caminho sozinho,
desviando de objetos sólidos e do que não se anda (a grade de caminhada da aba
**Caminhada** da ferramenta Terreno). Com 0 s ele anda no passo normal.

## Falar

*[guarda] diz ["Alto lá!"] por [3] s* mostra um balão em cima da cabeça.
*O jogador diz [...]* faz o mesmo com o jogador.

## Animações

No importador de sprites, **Animações extras** acrescenta animações com nome
(`atacar`, `acenar`...): primeira linha, coluna, quantos quadros, tempo de cada um e
se repete. Depois, nas regras: *[guarda] faz a animação [acenar]*. Sem repetir, ela
toca uma vez e volta ao parado.

## Juntando

Ponha uma **área de gatilho** chamada `guarita` em volta do guarda
(**Adicionar > Área de gatilho**, como na [aula 3](03-primeira-regra.md)):

*Quando o jogador entrar na área [guarita]* →
*[guarda] diz ["Bom dia, viajante."] por [3] s* → *[guarda] faz a animação [acenar]*.
