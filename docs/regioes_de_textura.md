# Regiões por textura e luzes noturnas

Um mapa de IDs desenhado em cima de uma textura do mapa (como os ID maps do
Substance): cada região diz como aquele pedaço da textura se comporta. Vale
para qualquer mapa de um `.glb`, sem refazer o pacote `.tuf`: o motor lê o
arquivo da cena do projeto e aplica por cima do que veio no pacote. Sem região,
o mapa carrega exatamente como antes.

## Onde fica

No mesmo arquivo de cena que o editor já grava com Ctrl+S:
`<projeto>/assets/scenes/<mapa>.scene.luau`. Os traços de pincel, borracha e
varinha são PNGs de 1 canal ao lado dele (`<mapa>.reg_<i>_<j>.png`), citados
pelo arquivo.

```lua
return {
    texture_regions = {
        { image = "nome_da_imagem_do_gltf", enabled = true, regions = {
            { type = "window", shape = "rect", pts = {0.1, 0.1, 0.4, 0.5},
              color = {1, 0.72, 0.36}, intensity = 1, glow = "dark",
              on_hour = 18, off_hour = 6 },
            { type = "lamp", shape = "poly", pts = {u, v, u, v, ...},
              glow = "light", light_radius = 40, light_intensity = 3,
              flicker = 0.3, flicker_speed = 8, glow_radius = 4, glow_strength = 0.6 },
            { type = "sand", shape = "bitmap", bitmap = "mapa.reg_0_2.png" },
        } },
    },
    emissive_off_nodes = { "no_sem_brilho" },
    lights = { [3] = { ..., on_hour = 19, off_hour = 5, flicker = 0.2, flicker_speed = 6,
                       glow_radius = 4, glow_strength = 0.6 } },
}
```

- `image`: o nome da imagem de cor do glTF (a aba lista os nomes do mapa).
- `pts`/UV em 0..1, v de cima para baixo. `shape`: `poly` (u,v,u,v,...),
  `rect` (u0,v0,u1,v1), `ellipse` (centro u,v e raios u,v), `all` (a textura
  inteira) ou `bitmap` (PNG de cobertura).
- `enabled = false` na imagem apaga a emissiva que veio do pacote para ela.
- `emissive_off_nodes`: nós do glb cuja emissiva fica apagada (no editor vale na
  hora).
- Luzes da cena (`lights`): `on_hour`/`off_hour` (hora do dia, 0..24; atravessa
  a meia-noite) acendem a luz por horário em vez de pela curva do sol, e
  `flicker` (0..1) com `flicker_speed` (Hz) a faz cintilar. `glow_radius` (unidades)
  com `glow_strength`: as superfícies até esse raio da luz acendem com a cor dela
  (sem os dois, nada muda). O Inspector da luz tem todos esses campos.
- Região `lamp` aceita os mesmos `glow_radius`/`glow_strength` (vão para as luzes
  dos postes).

## Tipos de região

| `type` | O que faz |
|---|---|
| `window` | janela com luz: acende de noite (cor, intensidade, modo, horário) |
| `lamp` | o mesmo brilho, mais uma luz pontual no mundo (raio, intensidade, cintilação) no centro das superfícies que usam a região (uma luz por célula de 6 unidades, no máximo 256) |
| `sand`, `snow` | superfície: `characters.on_step` entrega `ground.surface` ao script, que escolhe pegada ou poeira (`world.footprint`, `world.dust`) |

A lista é extensível: um tipo que o motor não conhece é guardado e devolvido
no arquivo, sem efeito.

Modo de brilho (`glow`), com a luminância da textura (a mesma regra de
`tools/emissive_windows.py`): `dark` escuro brilha (1,1 - lum, piso 0,15),
`light` claro brilha (vidro de lampião), `paper` papel claro (piso 0,1) e
`uniform`. Regiões se somam (máximo); o resultado passa por um desfoque leve e
vira a textura emissiva da imagem, que o motor acende pela curva das luzes
artificiais (some de dia). A cintilação vale para a luz de mundo e para a
textura: o shader de modelos multiplica a emissiva da imagem por um fator por
quadro (uma tabela slot/fator no UBO do quadro; a textura não é refeita). Uma
imagem tem um fator só, o da região acesa de maior `flicker`. `intensity`
multiplica a cor e é cortada em 1 (8 bits).

Brilho do objeto (`glow_radius`, `glow_strength`): o shader de modelos soma ao
emissivo a cor da luz vezes o albedo da superfície, com queda quadrática até o
raio; vale para as 8 luzes acesas mais perto da câmera e, sem nenhuma, o laço
não roda.

## No editor

Ferramenta **Terrain**, aba **Regions**:

1. Escolha a textura: **clique numa superfície no viewport** (a imagem sob o
   cursor abre e a região que cobre o ponto fica selecionada; uma cruz marca o
   ponto na textura) ou use a lista. Um `*` marca imagens sem pixels no editor
   (precisam recarregar o mapa).
2. Ferramentas: Select, Polygon, Rectangle, Ellipse, Brush, Eraser, Magnetic
   lasso, Magic wand; **Ctrl+A** faz a textura inteira virar uma região.
   - Polígono e laço: clique para pôr pontos; Enter, duplo clique ou clicar no
     primeiro ponto fecha; Backspace tira um ponto; botão direito (polígono)
     também.
   - Pincel/borracha editam a região selecionada pixel a pixel (uma região
     vetorial vira bitmap ao ser tocada); sem região selecionada, o pincel
     cria uma nova.
   - Laço magnético: live-wire (Mortensen e Barrett, 1995), custo = gradiente de
     Sobel invertido + cruzamento por zero do laplaciano + direção do gradiente,
     Dijkstra a partir do último ponto, na CPU, numa janela de 160 px.
   - Varinha: tolerância por canal; "Contiguous only" liga o preenchimento a
     partir do clique, desligado pega todo pixel parecido.
3. Escolha o tipo da região e edite as propriedades (cor, intensidade, modo,
   horário, cintilação, luz). Desfazer e refazer são os do editor.
4. O slider **Time of day** vale para tudo (regiões e luzes noturnas); a
   textura emissiva é refeita na hora, sem recarregar o mapa. "Preview glow"
   mostra só o brilho sobre a textura.
5. **Ctrl+S** (ou o botão Save) grava o arquivo e os PNGs.

Luz de mundo: cada região `lamp` cria as luzes dos postes; mudar cor, raio,
intensidade, horário ou cintilação vale na hora, e uma região `lamp` nova (ou
alterada) acha os postes na hora: o editor guarda, por imagem, o centro de UV e
do mundo dos triângulos (até 400 mil) em vez de depender da malha da carga.
"Add world light with these values" põe uma luz avulsa no viewport.

Um nó do glb (aba, "Nodes of the map") desmarcado fica sem emissiva na hora:
no editor cada (imagem, nó) tem um slot próprio que aponta para a emissiva da
imagem ou para uma textura preta, então ligar/desligar troca só o descritor.
Uma imagem que já vinha com emissiva no pacote passa pela textura do motor
(que parte da emissiva do pacote) e aceita região na hora; com uma região de
brilho, as regiões substituem a emissiva do pacote.

## Limites conhecidos

- Horário por região vale para a textura emissiva (refeita quando muda quem
  está aceso).
- Imagem cuja textura não tem pixels no editor (não vem embutida no glb) só
  aceita região depois de recarregar o mapa.
- A cintilação da textura é um fator por imagem; até 64 slots cintilando ao
  mesmo tempo (o editor usa um slot por nó).
