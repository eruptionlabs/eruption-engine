# Aula 2 — Objetos e personagens

**Objetivo:** pôr modelos e personagens no mapa e arrumar a cena.

## Modelos

1. No painel **Projeto**, ache um modelo `.glb` (as miniaturas aparecem sozinhas).
   Clique nele: o **Inspetor** mostra o modelo girando — arraste para girar.
2. Arraste o `.glb` do Projeto para a cena. Ele cai no chão onde você soltar.
3. Ferramentas: **W** move, **E** gira, **R** muda o tamanho. Segure **Ctrl** para
   encaixar nos passos da grade.
4. **End** põe o objeto no chão logo abaixo dele.

Vários de uma vez: segure **Ctrl** e clique, ou arraste no vazio da cena para
desenhar um retângulo. Mover, girar, apagar e duplicar (**Ctrl+D**) valem para o grupo.

## Sólido

No **Inspetor > Modelo**, marque **Sólido**: o jogador não atravessa a base do modelo
(o tronco de uma árvore, a parede de uma casa — a copa não conta).

## Prefab

Ajustou um modelo (giro, tamanho)? **Editar > Salvar como prefab...** guarda isso em
`assets/prefabs`. Arrastar o prefab põe outro igual; mudar o prefab muda todos.

## Personagens

1. Arraste um PNG de personagem do Projeto para a cena: vira um **personagem**.
2. Se o PNG é uma folha (várias poses em grade), clique com o botão direito nele e
   escolha **Importar como folha de sprites...**: diga quantas colunas e linhas tem,
   qual linha é cada direção e quais quadros são parado e andando. Salve.
3. O **Nome** do personagem (no Inspetor) é como as regras o encontram.

Salve a cena com **Ctrl+S**.
