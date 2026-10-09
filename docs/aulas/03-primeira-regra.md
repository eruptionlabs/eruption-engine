# Aula 3 — A primeira regra

**Objetivo:** quando o jogador chegar na porta, aparece uma mensagem e a porta se abre.

## 1. A área

1. **Adicionar > Área de gatilho** põe uma caixa no meio da cena.
2. Mova a caixa para a frente da porta (**W**) e acerte o tamanho (**R**).
3. No Inspetor, mude o **Nome** para `porta` e aperte Enter.

## 2. A folha de regras

1. Abra a aba **Regras** (no centro, ao lado de Cena).
2. Digite `casa` e clique em **+ Nova folha de regras**.
3. Clique em **+ Nova regra**.
4. Em **QUANDO / SE**, escolha *Quando o jogador entrar na área [porta]*.
5. Em **FAÇA**, clique em **+ Ação** e escolha *Mostrar a mensagem ["Bem-vindo!"] por
   [3] s*. Clique nos valores entre colchetes para mudá-los.
6. Mais uma ação: *Mover [a porta] em [(0, 30, 0)] durante [1] s* — escolha o modelo
   da porta na lista.

A regra se salva sozinha. Aperte **F5** e ande até a porta.

## 3. Condições

Uma regra pode ter condições: **+ Condição** depois do *Quando*. Exemplo:
*A hora está entre [18]h e [6]h* — a porta só abre à noite. **Inverter (NÃO)** troca
o sentido da condição, e **+ Senão** diz o que fazer quando ela não vale.

## 4. Variáveis

Variáveis guardam números e textos durante o jogo:

- *Somar [1] à variável [moedas]* numa área de moeda;
- *A variável [moedas] [>=] [10]* como condição para abrir o portão.

## Blocos

Ao lado de **Folha**, troque para **Blocos**: as mesmas regras viram blocos de
encaixar, que se arrastam da paleta e se encaixam embaixo do evento.
