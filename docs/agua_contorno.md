# Aguas de contorno: varias aguas num mapa, cada uma com o seu nivel

Um mapa tem o plano d'agua do terreno (o mar, um lago grande: um nivel so',
vindo dos dados do terreno) e, alem dele, quantas aguas de contorno o `.env`
quiser: canais, fontes, lagos em outra altura. Cada uma e' um poligono no
plano XZ com a propria altura, desenhado com a agua completa (ondas, espuma de
margem, cor por profundidade, refracao, causticas, reflexo) e tratado pelo
chao, pelo caminhar e pelos scripts como uma agua de verdade naquela altura.

Um mapa sem nenhuma agua de contorno desenha e se comporta exatamente como
antes.

## Formato no `.env`

Dentro da lista `liquids` (a mesma das lavas), uma entrada com `"kind":
"water"` e `polygon` (ou `polygons`):

```json
{
  "liquids": [
    {
      "name": "canal_norte",
      "kind": "water",
      "polygon": [[100, 12.0, 40], [220, 12.0, 40], [220, 11.5, 70], [100, 11.5, 70]],
      "shore": "swash_foam",
      "deepColor": [0.02, 0.10, 0.12],
      "transparency": 0.6
    }
  ]
}
```

| Campo | Tipo | Significado |
|---|---|---|
| `name` | texto | Nome para `world.liquids()` e `world.set_liquid_level`. |
| `kind` | `"water"` | Sem `polygon`, a entrada `water` continua sendo so' um registro (o plano do terreno ja' cobre). |
| `polygon` | `[[x, y, z], ...]` | Contorno simples, sem buraco, em unidades de mundo. Cada ponto leva a **sua altura** `y` (um canal que desce e' um poligono so', mais alto numa ponta). Um ponto `[x, z]` fica na altura `level`. |
| `polygons` | lista de `polygon` | Varios pedacos da mesma agua (uma margem de verdade e' so' a que nao e' dividida por dois pedacos). |
| `level` | numero | Nivel da agua para scripts e consultas. Sem ele, e' a media das alturas dos pontos. Ao subir (`set_liquid_level`) a superficie inteira anda a diferenca. |
| `shore` | `"swash"` / `"swash_foam"` | Beira no chao desta agua: a lamina da onda sobe a areia (`swash`) e com espuma na frente (`swash_foam`). Ausente: so' o contato (espuma pela diferenca de profundidade). |

Regras do contorno:

- A orientacao (horario ou anti-horario) nao importa: o motor corrige. O
  contorno nao pode se cruzar (as orelhas que nao saem sao descartadas com
  aviso no log) e nao tem buraco.
- A malha e' subdividida pela maior aresta (raio / 24 u, no minimo 1 u) e a
  altura e' interpolada entre os pontos; ate' 32 superficies de contorno
  (lavas contam juntas) por mapa, e a beira no chao (lamina, areia molhada)
  vale para as 7 primeiras.
- O contorno vale a partir do chao: onde o chao fica acima da agua nao ha'
  agua visivel, so' o que o poligono cobre e esta' acima do chao.

### Aparencia por superficie

Todos opcionais; ausente = o do menu de agua (`data/water_config.json`) e do
estilo do preset (`water_style` em `data/graphics.json`). Cores em linear
(0..1), as mesmas do menu.

| Campo | Faixa | Efeito |
|---|---|---|
| `deepColor`, `shallowColor` | `[r, g, b]` | Cor da agua funda e da rasa. No estilo diorama (`water_hd2d`) so' a funda conta: ela e' a cor do espalhamento (o mesmo que `scatterColor`, que ganha se os dois vierem); `shallowColor` so' age fora do diorama. |
| `scatterColor` | `[r, g, b]` | Diorama: cor do espalhamento no fundo (`water_style.deep_color`). |
| `clarity` | 0.1 a 200 | Diorama: unidades ate' o vermelho cair a 1/e dentro d'agua (agua turva = pequeno). |
| `shoreFoam` | 0 a 8 | Diorama: largura da espuma de margem. |
| `transparency` | 0 a 1 | 0 opaco, 1 transparente. |
| `refraction` | 0 a 0.5 | Forca da distorcao da refracao. |
| `reflectivity`, `roughness` | 0 a 1 | Reflexo do ceu e rugosidade (brilho do sol). |
| `waveAmplitude`, `waveFrequency`, `waveSpeed` | 0..4, 0.01..8, 0..8 | Ondas. No estilo diorama as ondas ficam so' na normal (a amplitude do vertice e' zerada, como no mar). |
| `caustics` | 0 a 4 | Intensidade das causticas no fundo (0 desliga). |
| `foam` | `true` / `false` | Espuma de contato (personagens, objetos). |

O clima (chuva, tempestade) agita e escurece cada agua de contorno como a do
mapa; `deepColor` e os outros valores valem antes dessa conta.

## Scripts

- `world.liquids()` lista tambem as aguas de contorno (`kind = "water"`,
  `level` = o nivel de agora, cada uma o seu).
- `world.liquid_at(x, z)` devolve a agua que cobre o ponto, com o nivel
  **daquela** superficie (um canal acima do mar da' o nivel do canal).
- `world.set_liquid_level(agua, nivel)` move uma agua de contorno (indice
  1-based ou nome) durante o jogo: a superficie, `liquid_at`, o caminhar e os
  passos acompanham no mesmo quadro. Falso para o plano do terreno.
- Passos (`characters.on_step`, `in_water`) e as ondinhas dos passos usam a
  agua sob o pe'.
- Navegacao: agua funda (mais de um degrau sobre o chao) bloqueia quem nao nada,
  como a do mar; `avoid_liquid: "water"` do `world.path` desvia dela.

## Reflexo

O reflexo planar (SSPR e o passe espelhado do preset) custa um passe cada e
existe **um so' por mapa**, num unico nivel: o da maior agua (area em XZ),
seja o plano do terreno ou uma de contorno. As outras aguas refletem pela
marcha de raio em espaco de tela, com o ceu (e a sonda de ceu) de fundo; a
diferenca fica nos objetos refletidos, que ali nao entram fora da tela.

A chave `water_contour_planar` do preset (`data/graphics.json`) deixa uma
agua de contorno ser a dona do plano (`true`, `medium` e `high`); com `false`
(`low`) o plano e' sempre o do terreno e as de contorno marcham. Os presets
`mobile_*` nao ligam o diorama (`water_hd2d`): nao tem plano planar e nenhum
passe extra.

## Limites

- O navegador (WebGL2) ainda nao desenha as aguas de contorno.
- Os controles de posicao e rotacao do menu de agua so' mexem em lava.
- O contorno e' reto: onde o poligono corta um terreno inclinado a agua termina
  numa aresta vertical, sem margem esculpida. Para uma margem natural, cave o
  chao (ou use o poligono so' onde o chao ja' forma a bacia).
- O nivel do plano do terreno (o mar) vem do `.ter` ou de `environment.water`
  da cena; a chave `water` do `.env` nao o muda num mapa com `.ter`.
- Os campos de aparencia sao lidos por `parseWaterLook` (`src/formats/LiquidLook.cpp`),
  coberto por `tests/test_liquid_look.cpp`.
