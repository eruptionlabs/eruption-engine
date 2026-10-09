# Aula 6 — Salvar o jogo e trocar de mapa

## Salvar

- *Quando apertar a tecla [G]* → *Salvar o jogo no espaço [1]*.
- *Quando o jogo começar* + condição *Existe um jogo salvo no espaço [1]* →
  *Carregar o jogo do espaço [1]*.

O jogo salvo guarda o mapa, onde o jogador está, as variáveis, a hora, o clima e a
música. Fica na pasta de dados do jogador (no Linux, `~/.local/share/<jogo>/saves`).

## Portas para outro mapa

Uma área na entrada da caverna:
*Quando o jogador entrar na área [caverna]* → *Ir para o mapa ["caverna"]*.

O jogo troca de mapa e põe o jogador no início do mapa novo. No editor isso também
funciona no Play: o mapa novo abre e o jogo continua, com as mesmas variáveis.
