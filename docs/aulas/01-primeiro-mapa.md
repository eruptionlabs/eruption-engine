# Aula 1 — Primeiro mapa

**Objetivo:** gerar um mapa com morros, um rio, uma estrada e um lago, e andar nele.

## 1. Uma receita

1. **Arquivo > Gerar mapa...** abre o gerador.
2. À esquerda, digite `minha_ilha` e clique em **+ Nova receita**. A receita é um
   arquivo pequeno (`assets/mapgen/minha_ilha.mapgen.luau`) com uma lista de
   **etapas** que rodam de cima para baixo.
3. A receita nova já vem com algumas etapas. A prévia, à direita, se atualiza sozinha
   a cada mudança.

## 2. Mexendo nas etapas

Cada etapa tem uma caixinha para ligar e desligar, setas para mudar a ordem e um
**x** para tirar. Clique no nome para abrir os parâmetros.

- **Morros (ruído):** aumente *height* para morros mais altos.
- **Ilha:** abaixa as bordas, deixando a terra no meio.
- **Erosão (chuva):** a chuva cava vales; mais *drops*, mais vales.
- **Nível da água:** tudo abaixo vira lago e mar.

Use **+ Etapa** para acrescentar:

- **Rio** — desce do ponto mais alto até o lago. Em *layer*, escolha uma textura de
  areia (selecione o PNG no painel Projeto e clique em *Usar o arquivo selecionado
  no Projeto*).
- **Estrada** — liga dois pontos (frações do mapa: 0 é uma borda, 1 a outra) pelo
  caminho mais plano, aplainando e pintando.
- **Pintar camada** — pinta uma textura por inclinação ou altura (ex.: pedra nas
  encostas acima de 30 graus).

> Prefere arrastar? Troque **Pilha** por **Blocos**: as etapas viram blocos de
> encaixar, que se arrastam da paleta para a receita.

## 3. Gerar e andar

1. Dê um nome em **Nome do mapa** e clique em **Gerar mapa**. A barra mostra o
   andamento; no fim, o mapa abre sozinho.
2. Aperte **F5** para jogar. Ande com **W A S D**.
3. Aperte **F5** de novo para parar.

## Desafio

Faça duas receitas iguais com **Origem no mundo** diferente (0,0 e 1000,0). Os morros
batem na emenda; a [aula 7](07-mundo-continuo.md) junta os dois mapas num mundo só.
