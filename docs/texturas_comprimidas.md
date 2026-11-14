# Texturas comprimidas

O motor guarda as texturas de material em formato de bloco (comprimido na GPU)
sempre que a placa sabe ler esse formato. Menos VRAM, carga quente mais curta
(o pacote já vem pronto do disco) e menos bytes lidos por quadro. A
decodificação é sempre do hardware; a CPU só codifica, uma vez, no cozimento.

## Ligar e desligar

Chave `texture_compression` de cada preset em `data/graphics.json`:

| valor  | o que faz |
|--------|-----------|
| `bc`   | padrão. BC onde a GPU tem (todo PC), senão RGBA8. É o comportamento de sempre. |
| `auto` | BC; sem BC, ASTC; sem ASTC, ETC2/EAC; sem nada, RGBA8. Acende a compressão no celular sem BC. |
| `astc` | só ASTC (RGBA8 se a GPU não tiver). |
| `etc2` | só ETC2/EAC (RGBA8 se a GPU não tiver). |
| `off`  | tudo em RGBA8, sem pacote (o caminho de uma GPU sem formato de bloco). |
| `raw`  | o mesmo cozimento dos outros modos, guardado em RGBA8: referência para medir a perda do codec. Não é para jogo. |

Opções (todas desligadas por padrão): `texture_albedo_bc7` (albedo opaco em BC7
em vez de BC1: dobra os bytes do albedo), `texture_alpha_bc7` (albedo com alfa em
BC7 em vez de BC3, mesmos bytes), `texture_astc_albedo_6x6` (albedo opaco em ASTC
6×6, 0,44 B/texel em vez de 1).

Variáveis de ambiente (A/B na bancada, sem editar o preset):

- `ERUPTION_TEXTURE_COMPRESSION=auto|off|bc|astc|etc2|raw` vence a chave do preset.
- `ERUPTION_NO_TEXTURE_BC=1` é o mesmo que `off`.
- `ERUPTION_TEST_TEXTURE_CAPS=astc,etc2,eac` restringe os formatos que o motor
  considera disponíveis (simula a GPU de um aparelho). Só restringe: nunca liga o
  que o driver não tem. Nomes: `bc`, `bc1`, `bc3`, `bc4`, `bc5`, `bc7`, `astc`,
  `astc4x4`, `astc6x6`, `etc2`, `etc2rgb`, `etc2rgba`, `eac`, `eacr11`, `eacrg11`, `none`.
- `ERUPTION_TEST_PRESET_SET=texture_albedo_bc7=true` liga uma opção num teste.

O log diz o que foi escolhido:

```
[TEXCOMP] formatos de bloco amostraveis: bc1,bc3,bc4,bc5,bc7
[TEXCOMP] modo bc -> familia bc: albedo BC1, albedo_alfa BC3, normal BC7, dados BC7 (pacote BC classico)
```

## Formato por tipo de textura

| classe | BC (PC) | ASTC | ETC2/EAC |
|--------|---------|------|----------|
| albedo opaco | BC1 (0,5 B/texel) | 4×4 (1 B) ou 6×6 com opção | ETC2 RGB (0,5 B) |
| albedo com alfa | BC3 (1 B) | 4×4 | ETC2 RGBA (1 B) |
| normal | BC7 (1 B) | 4×4 | ETC2 RGBA |
| dados de 4 canais (metal/rugosidade/altura/umidade) | BC7 | 4×4 | ETC2 RGBA |
| máscara de 1 canal | BC4 (0,5 B) | 4×4 | EAC R11 (0,5 B) |

O normal nunca vai para formato de 2 canais (BC5, EAC RG11): os normal maps do
motor guardam o z de propósito (não são unitários) e o alfa carrega dado do
material; reconstruir z mudaria a iluminação, e o BC5 custa o mesmo 1 B/texel
do BC7.

Textura que não tem formato na GPU fica em RGBA8: nada quebra, só ocupa mais.

## Cache

- Texturas embutidas no `.glb`: um pacote ao lado do modelo,
  `<modelo>.glb.<teto><s|n>.etexpack` no modo `bc`, e
  `<modelo>.glb.<teto><s|n>.<etiqueta>.etexpack` nos outros (a etiqueta é um
  hash dos formatos escolhidos). Trocar de modo não apaga nem reassa o pacote
  dos outros modos. O pacote se invalida sozinho quando o conteúdo do `.glb`,
  o teto de resolução (`texture_max_size`) ou os formatos mudam.
- Imagens soltas (PNG): `<imagem>.etex` no modo `bc`, `<imagem>.<etiqueta>.etex`
  nos outros, regravado quando a imagem fica mais nova que ele. Um `.etex` num
  formato que a GPU não lê é ignorado.
- O cozimento roda uma vez por mapa, preset e modo; a carga seguinte só lê.

## Qualidade e custo medidos

Cena de demonstração, câmera fixa, contra o mesmo cozimento guardado em RGBA8
(`texture_compression=raw`, só para comparação: 4-8× a VRAM). "Invisível" na
bancada do motor = PSNR ≥ 40 dB, SSIM ≥ 0,995, LPIPS ≤ 0,01, FLIP ≤ 0,02.

- `bc` (padrão): PSNR 39-41 dB, SSIM 0,96-0,98 sem PBR — diferença visível
  na imagem de diferença, não a olho; textura de conteúdo 4,4× menor em VRAM
  (high: 697 → 160 MB), carga quente 6,5 → 4,0 s.
- `texture_albedo_bc7`: 43-44 dB, SSIM 0,99, por +39 MB no high.
- ASTC 4×4 (GPU Intel com ASTC): 46 dB, SSIM 0,993 sem PBR — o melhor dos
  formatos de bloco medidos. ASTC 6×6 no albedo e ETC2: perto do BC ou abaixo
  (ETC2 com PBR: 34,6 dB e a resposta do relevo à luz cai para 97,5%).

Velocidade: numa GPU dedicada (RTX 4060) o tempo de quadro com e sem
compressão fica dentro do ruído, em 720p e em 1440p. Numa GPU integrada
(Intel) a 720p o GBuffer cai ~4% com BC; com a RAM disputada por outro
processo a mediana do quadro cai 14-18% (ASTC/BC), com faixas sobrepostas. O
ganho certo é memória e carga; o de quadro aparece onde a banda é curta.

## GPU sem descriptor indexing: array de camadas em bloco

Sem descriptor indexing (GPU de celular com poucos samplers), cada textura de
conteúdo é copiada para uma camada de lado fixo (`texture_layer_size`, padrão
256) de um `sampler2DArray`. Em RGBA8 cada camada de 256 com mips pede 344 KB;
na cena de demonstração em high são 464 camadas, 163 MB, e cada passo de
crescimento do array pede o novo com o velho ainda vivo.

Chave `texture_layers_compressed` do preset (padrão `false`) ou
`ERUPTION_TEX_LAYER_COMPRESSED=1`: o array A passa a um formato de bloco de
1 B/texel, o da família de `texture_compression` sobre os formatos da GPU —
BC7 (família BC), ASTC 4×4 (ASTC) ou ETC2 RGBA (ETC2). Opaco e alfa dividem o
formato: o codificador trata cada camada como opaca ou com alfa. Um formato
opaco de 0,5 B/texel pediria um terceiro array e uma ligação nova no set 0;
o layout de ligações, a tabela por slot e o shader são os mesmos do RGBA8.

- Fonte já no formato do A, quadrada, com mips até 1×1 a partir do lado da
  camada: a camada é cópia de bloco (`vkCmdCopyImage`), sem decodificar nem
  codificar — sem perda em relação à fonte.
- Fonte ETC2 RGB (albedo opaco da família ETC2) no A em ETC2 RGBA: os blocos
  da fonte voltam para a CPU e sobem com um bloco de alfa 255 na frente de cada
  um — também sem perda e sem codificar.
- Outra fonte comprimida (ex.: albedo BC1 no array BC7) ou cor sRGB: a cópia
  de sempre desenha a camada num RGBA8 de passagem, que volta para a CPU, é
  codificado e sobe para o A. O resultado fica em cache em
  `.cache/eruption/layers/` pela soma do conteúdo da camada (troca a textura,
  troca a chave). `ERUPTION_TEX_LAYER_CACHE=0` desliga o cache (medida a frio).
- Dados lineares sem compressão (RGBA8, R8, RG8) e texturas sem filtro
  (sprites, pixel art) ficam no array B, em RGBA8, que também recebe o que não
  couber no A. Com o A em bloco o B cresce de 8 em 8 camadas.
- GPU sem o formato (sem cópia ou sem amostragem em array), modo `off`/`raw`,
  navegador ou `ERUPTION_DISABLE_LAYER_B`: o A fica em RGBA8, como antes.

Medido na cena de demonstração em high (464 camadas de 256): o array vai de
163,0 para 43,9 MB e cada camada pede 88 KB em vez de 344 KB (4060; 108 KB em
vez de 390 KB no Intel); o maior pedido durante o crescimento cai de 295 para
79 MB. Com ASTC 4×4 quase toda camada é cópia direta (455 de 457, 59 ms) e a
imagem fica invisível contra as camadas RGBA8 (50,7 dB, SSIM 0,996); ETC2 dá
42,2 dB e BC7 47,7 dB. O tempo de GPU por quadro não muda além do ruído: o
ganho é de memória.

Diagnóstico: o log diz o formato e a memória pedida por camada
(`Texture layers: array A BC7 com 487 camadas: 41,9 MB pedidos, 88,0 KB por
camada`), quantas camadas foram cópia direta, quantas vieram do cache e quantas
foram codificadas; a telemetria tem `sys.layers.kb_per_layer`, `block`,
`block_direct`, `block_cache_hits`, `block_encoded` e `block_ms`.
`ERUPTION_TEX_LAYER_PSNR=1` mede cada camada codificada contra o RGBA8 que a
cópia desenhou; `ERUPTION_TEX_LAYER_DIRECT=0` força tudo pelo caminho que
codifica (A/B).

## Streaming de mips por distancia

Para placa com pouca memoria de video (`src/renderer/TextureStreamer.*`).
Liga com `texture_streaming` nos presets de desktop; no celular (caminho de
camadas, sem descriptor indexing) fica desligado.

- **So' age quando falta memoria.** O orcamento e' `texture_budget_mb` ou,
  com 0, metade do que o heap local da GPU ainda deixa usar (minimo 64 MB).
  Enquanto as texturas do mapa (pacote embutido e o que ja' foi carregado)
  cabem, tudo sobe inteiro, como sem streaming: mesma imagem, mesma memoria.
- **Quando nao cabe**, cada textura comprimida sobe so' a cauda da piramide
  (lado ate' `texture_stream_base`, 128 px). A cada 4 quadros o streamer
  estima, por textura, o mip mais fino que a tela mostra (densidade de UV da
  malha contra pixels por unidade na distancia da caixa da textura, menos
  `texture_stream_mip_bias` de folga) e, se a soma passa do orcamento, tira o
  mip de cima de quem menos perde. Sobe a imagem nova sem travar o quadro
  (cerca, ate' `texture_stream_upload_mb` por quadro) e troca o slot bindless;
  a imagem antiga morre depois dos quadros em voo.
- **Sem salto seco ao ficar nitida.** A imagem nova entra mostrando o mesmo
  mip que ja' estava na tela e desce um mip a cada `texture_stream_ramp_frames`
  quadros (6 = ~0,1 s por mip a 60 fps), por views com `baseMipLevel` = k, sem
  mexer em shader. Na chegada ao mapa a troca e' direta: cai nos quadros 2-3,
  sob o fade do warp e a mudanca da tela inteira na partida (com rampa ali, os
  degraus apareciam sozinhos nos quadros 7-19: ate' +15% de pixels com
  |dY| > 8 num quadro, contra +0,9% da troca direta). Ficar mais borrada
  (orcamento apertado ao chegar um vizinho) continua sendo troca direta.
- Custo quando ligado: uma copia da textura em RAM (de onde os mips sobem)
  e a cauda duplicada na VRAM (~1/64 do total).

Medido em 45 mapas de teste (1920x1080, preset high, RTX 4090):

| | streaming ligou | VRAM (media onde ligou) | imagem |
|---|---|---|---|
| orcamento automatico (24 GB) | 0 de 45 | igual | igual |
| orcamento 512 MB | 17 de 45 | -61 MB (ate' -289) | perto do ruido entre execucoes |
| orcamento 128 MB | 38 de 45 | -227 MB (ate' -999) | borra de perto nos mapas grandes |

Carga do mapa: mesma mediana com e sem streaming (1,45 / 1,37 / 1,42 s em
8 mapas grandes, 3 execucoes intercaladas). Validacao Vulkan sem erro novo.

## RDO (opcional)

`texture_rdo_lambda` > 0 passa os blocos BC1/BC3/BC7 pela transformada de
reducao de entropia do bc7enc_rdo: o pacote fica do mesmo tamanho bruto, mas
o zstd encolhe mais. O pacote leva a etiqueta da politica (nao mistura com o
classico). No mapa de demonstracao, zstd -19: 66,5 MB sem RDO, 55,3 MB com 1
(-17%, imagem 40-42 dB), 47,1 MB com 3 (-29%, 34-37 dB, ja' visivel).

## Limites

- A primeira carga de um mapa num modo novo paga o cozimento (na cena de
  demonstração, teto 1024, CPU de 14 threads: BC 7,8 s, ETC2 4,4 s, ASTC 4×4
  15,0 s). Num celular a CPU é mais lenta; meça antes de ligar `auto` num preset
  de celular.
- O array de camadas em bloco (seção acima) é desligado por padrão; sem ele, o
  caminho sem bindless guarda as camadas em RGBA8.
- Quadros de sprite e as poucas imagens geradas em tempo de execução ficam em
  RGBA8.
- O censo por formato sai no relatório de fluxo (`ERUPTION_TEST_FLUXO_REPORT=<quadro>`,
  linhas `[TEXCOMP] censo`) e na telemetria (`bw.tex_mb`).

## Codificadores

BC: bc7enc_rdo/rgbcx. ASTC: astc-encoder 5.2.0 (Apache-2.0, `third_party/astcenc`).
ETC2/EAC: etcpak (BSD-3, `third_party/etcpak`). Nenhum usa rede; a licença e a
versão estão no `NOTICE`. Teste: `ctest -R TextureCodec` (política e ida-e-volta
de cada formato).
