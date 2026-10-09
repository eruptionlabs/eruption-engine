# Aula 9 — Exportar o jogo

1. **Arquivo > Exportar jogo...**
2. Marque os mapas do jogo (o primeiro é onde o jogo começa; mapas vizinhos vão
   junto sozinhos) e escolha a pasta.
3. **Exportar.** O editor compila um programa do jogo sem o editor (bem menor) e
   copia só o que os mapas usam: modelos, texturas, sprites, sons, prefabs, scripts
   (já compilados) e os retoques de terreno.

Na pasta exportada, `run.sh` abre o jogo. Ela pode ir para outra máquina Linux com
Vulkan; o programa roda em qualquer processador com AVX2 (PCs de 2013 para cá).

O jogo exportado abre limpo: sem os painéis de depuração do motor. HUD e minimapa
aparecem se o jogo pedir (`ui.show_hud(true)`, `ui.show_minimap(true)`).
