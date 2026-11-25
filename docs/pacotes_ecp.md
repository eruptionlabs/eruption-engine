# Pacotes .ecp num projeto de jogo

Um projeto pode guardar o conteudo (mapas, texturas) em pacotes `.ecp`
(formato em `src/formats/EcpReader.hpp`, leitor com zstd) em vez de pastas
soltas. O motor abre o mapa direto de dentro do pacote, sem extrair.

## Declarar os pacotes

No `project.eruption`, a chave opcional `packs` (lista), na ordem de
sobreposicao: **o ultimo pacote que cita um caminho vence**.

```json
{
  "name": "Meu Jogo",
  "startup_map": "cidade",
  "packs": ["packs/base.ecp", "packs/cidades.ecp", "packs/hires/"]
}
```

- Caminho relativo vale a partir da pasta do projeto; absoluto passa como esta'.
- Uma pasta na lista vale por todos os `.ecp` dentro dela, em ordem alfabetica.
- Sem a chave `packs`, o motor varre `packs/*.ecp` em ordem alfabetica e,
  depois, `packs/<subpasta>/*.ecp` (assim `packs/hires/` vem por cima).
- Um pacote que nao existe da' aviso no log; um que nao abre desliga todos.

## O que o pacote guarda

Os mesmos caminhos que o projeto teria em disco, relativos a' pasta do projeto.
Um projeto pode ter so' `project.eruption` + `packs/`:

```
assets/external/<mapa>/<mapa>.glb   (.ter, .nav, .env, textures/<hash>.png.png)
assets/data/texture/**, assets/pbr/base/**   (texturas de modelo, _normal/_mrahw)
assets/scenes/<mapa>.scene.luau, .scatter.luau, .height.bin, .walk.png, mascaras de terreno
assets/prefabs/*.prefab.luau, assets/models/*.glb
assets/scripts/**.luau              (require, subpastas)
assets/sprites/*.png|.spr, *_normal.png|.spr, *.sheet.json
assets/fonts/ui.ttf|.otf, ui_extra.*, ui.json
assets/luts/*.png|.cube             (caminho citado em "lut" do look)
assets/effects/*.vfx.json, interfaces .css
data/*.json do projeto (graphics, postprocess, skybox_config, diorama,
  char_config, hud/minimap, climate/<mapa>.json), assets/data/pbr_materials.json
resources.ini
```

Maiusculas e `\` x `/` nao importam (o pacote normaliza). O pacote guarda
nomes em minuscula: scripts, mapas e sprites que so' existem em pacote aparecem
com o nome em minuscula (o `require` de um modulo so' de pacote tambem acha
pelo nome em minuscula).

## Montagem

Os pacotes sao montados em `Project::open`, antes de qualquer leitura (jsons de
dados, fonte, scripts, mapa). `Project::close` desmonta.

## Ordem de leitura

1. Arquivo solto no projeto (e o pacote de mapa `.emap`, como sempre).
2. Pacotes `.ecp`, por cima uns dos outros (o ultimo vence).

Um arquivo solto ao lado do projeto vence o pacote sem reempacotar. Sem
pacotes declarados nada muda: a leitura e' a de sempre.

## Por dentro

`vfs::mountPacks` (`src/utils/AssetVfs.hpp`) monta os pacotes uma vez;
`vfs::read/readText/exists/stat` caem nele quando o disco nao tem o arquivo, e
`vfs::listPacked` lista uma "pasta" de pacote (mapas, pares de texturas PBR,
cenas, scripts, texturas por nome de material). Quem le' conteudo do jogo passa
por `vfs` (ou por `ImageUtils::loadRawVfs` para imagens, `std::istringstream`
sobre `vfs::readText` onde havia `ifstream`); fontes entram por
`AddFontFromMemoryTTF`. As fontes de `resources.ini` terminadas em `.ecp`
continuam entrando no `PackManager` (texturas de modelo antigas).

## O que NAO le' de pacote

- Caches que o motor ESCREVE continuam no disco gravavel, ao lado do `.glb`
  (`.etexpack`, `.probes`, cache de relevo/`.relevo`, `.cache/eruption/...`):
  num projeto so' de pacotes a pasta do `.glb` nasce no disco so' com eles. Se
  o pacote traz uma copia (`.etexpack`, `.probes`), ela e' lida e validada como
  sempre (impressao digital do `.glb` lido do pacote); sem copia, o motor gera
  e grava como sempre.
  Excecao: a grade de sondas (`.probes`) o PC grava por padrao no cache do
  usuario (`$XDG_CACHE_HOME/eruption/probes`, ou `ERUPTION_PROBE_CACHE_DIR`), nao
  dentro dos assets do projeto; `"probe_cache": "beside"` no `project.eruption`
  volta a gravar ao lado do `.glb`. A leitura olha o `.glb.probes` ao lado (e o
  do pacote) primeiro e o cache depois; `--bake-map` e `build_apk.sh` levam a
  grade para o pacote de onde quer que ela esteja.
- Saves, `preset_override.txt`, `editor_layout.ini`, telemetria e saidas de teste.
- `project.eruption` (o manifesto) e o proprio `packs/`: ficam soltos.
- Audio (`audio.*`, modulo opcional que toca por caminho de arquivo) e o icone
  da janela (`export.icon`).
- Editor (importadores, `--import-sprite`, criador de assets): trabalham em disco.
- Hot-reload por data de arquivo (scripts, CSS) nao se aplica a quem so' existe
  em pacote.
- O web usa o proprio carregador de pacotes EPAK1; o `.ecp` nao vale la'.

## Teste

`tests/test_ecp_project.sh` roda um projeto inteiro (mapa com modelos
texturizados, cena com prefabs, script com `require`, sprite com normal, fonte,
LUT, jsons de dados) so' do pacote e compara a captura com o mesmo projeto em
pastas soltas (PSNR infinito), com pacote cru e com pacote zstd do `ecp.py`.

`tests/test_ecp_map.sh` gera um mini pacote (`tests/make_test_ecp.py`: um
mapa de um triangulo com textura externa) e abre o motor so' de dentro dele,
com `packs/` varrido e com `packs` no manifesto. Com argumentos
`<mapa> <pacote.ecp>...` abre tambem um mapa de pacotes reais.
