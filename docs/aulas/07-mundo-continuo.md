# Aula 7 — Mundo contínuo

**Objetivo:** dois mapas emendados; o jogador passa de um para o outro sem tela de
carregamento.

1. Gere dois mapas com a mesma receita e semente, um com **Origem no mundo** 0,0 e o
   outro com 1000,0 (para mapas de tamanho 1000). Assim os morros, a pintura e os
   rios batem na emenda.
2. Abra o primeiro. Selecione **Ambiente** na Hierarquia; no Inspetor, em
   **Mapas vizinhos**, escolha o segundo mapa em **Emendar** e clique em **Leste**.
3. Salve (**Ctrl+S**). Ande até a borda: o vizinho aparece antes de você chegar (é
   carregado em segundo plano), com objetos, personagens, áreas e água dele.

Detalhes:

- A emenda vale nos dois sentidos: abrindo o segundo mapa, o primeiro aparece a oeste.
- Vizinho de vizinho também entra (dá para fazer uma fileira ou uma grade de mapas).
- Longe, o vizinho sai de cena; depois de um tempo longe, devolve a memória e é lido
  de novo se o jogador voltar.
- No editor, o vizinho aparece travado (só contexto). Para editar, abra o mapa dele.
