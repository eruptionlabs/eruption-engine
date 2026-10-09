# Aula 4 — Som e música

**Objetivo:** música de fundo e um som quando o jogador pisa numa área.

1. Ponha os arquivos de som (`.ogg`, `.wav`, `.mp3` ou `.flac`) em `assets/sounds`.
   Clicando num deles no Projeto, o Inspetor tem **Play** para ouvir.
2. Na folha de regras:
   - *Quando o jogo começar* → *Tocar a música [assets/sounds/tema.ogg] no volume [0.6]*.
     Trocar de música faz uma passagem suave entre as duas.
   - *Quando o jogador entrar na área [ponte]* → *Tocar o som [assets/sounds/passos.wav]
     em [a ponte], volume [1]* — o som sai do lugar: mais baixo longe, do lado certo.
   - *Parar a música* para o silêncio de uma caverna.

O som sai junto quando o jogo é exportado.
