# Pacotes .tuf (Tufo)

Um `.tuf` guarda milhares de arquivos de um projeto num arquivo só, como uma
**pasta virtual**: o motor pede `assets/external/x/x.glb` e o pacote entrega só
essa entrada, descomprimida na hora (zstd por blob, sem bloco sólido). O formato
e a ferramenta são o projeto **Tufo** (submódulo `tools/tufo`,
<https://github.com/eruptionlabs/tufo>); a especificação binária completa está
em `tools/tufo/docs/format.md`.

## O formato em poucas linhas

- Cabeçalho em dois slots; vale o slot válido de maior geração. Um acréscimo
  grava blobs novos, depois o índice novo, e só então troca o slot inativo
  (com `fsync`): parar no meio nunca perde o estado anterior.
- Cada blob comprimido sozinho; blobs que não ganham 3% (PNG, MP3) ficam crus.
- Deduplicação por conteúdo, inclusive dos PNG embutidos num `.glb`: o arquivo
  é partido numa entrada composta e volta idêntico bit a bit.
- Hashes XXH64 por blob, por entrada e no índice; a leitura confere.
- Caminhos: `/`, minúsculas ASCII, sem `..`.

## Texturas prontas para a GPU (formato versao 2)

Um mapa `.glb` com imagens embutidas costuma ser comprimido para BC na primeira
carga (dezenas de segundos) e o resultado ficava num `<glb>.<teto><s|n>.<fmt>.etexpack`
ao lado do arquivo. Com `--gpu-ready` o pacote ja' leva isso:

```bash
eruption-engine --tuf-pack assets.tuf --base projeto --gpu-ready projeto/assets
```

- o `.glb` entra **sem as imagens** (marcador no lugar) e o pacote de texturas
  (BC, mips, PBR resolvido, os mesmos bytes do `.etexpack` do motor para o preset
  ativo) entra como entrada companheira; um `.etexpack` valido em disco e' reaproveitado;
- ao carregar, o motor le' o pacote direto do `.tuf` (`vfs`): nenhuma compressao,
  nenhum `.etexpack` lido ou gravado (`[TUFGPU] ... sem compressao` no log);
- uma companheira por combinacao (teto, sintese de PBR, opcoes) que algum preset de
  `data/graphics.json` (mais o patch do jogo, com `ERUPTION_PROJECT=<pasta>`) pode
  escolher: `1024s` (high/medium), `512n` (low), `256n` (menor preset de celular).
  Todo preset funciona com o mapa migrado; GPU sem BC (ASTC/ETC2) nao esta' coberta;
- **desfazer**: o painel Pacotes (previa = folha de contato das texturas decodificadas),
  `eruption-engine --tuf-extract` e `--tuf-cat` decodificam o BC e devolvem um `.glb`
  com PNG embutidos. E' **com perda** (as imagens saem do BC, no teto de resolucao);
  o `tufo` avulso nao tem o codec e se recusa a extrair essas entradas.
- o arquivo vira versao 2 do formato; leitores antigos (so' versao 1) nao o abrem.
  Sem entradas `--gpu-ready` o arquivo continua versao 1.

A especificacao esta' em `tools/tufo/docs/format.md`, secao 9.

## No projeto

O motor monta os `.tuf` junto dos `.ecp` (`packs/*.tuf`, ou a chave `packs` do
`project.eruption`, ver `docs/pacotes_ecp.md`) como camada abaixo dos arquivos
soltos: o arquivo solto vence o pacote.

## Linha de comando

`tufo` (alvo `tufo`, construído com o motor, instalado em `bin/` pelo
`cmake --install`) e, dentro do motor, o atalho `eruption-engine --tuf-<comando>`:

```bash
tufo pack    assets.tuf --base projeto projeto/assets
tufo add     assets.tuf --base extra extra/texturas --level 12
tufo list    assets.tuf assets/external/x
tufo extract assets.tuf /tmp/saida assets/external/x
tufo rm      assets.tuf assets/external/velho/
tufo verify  assets.tuf --threads 8
tufo stats   assets.tuf
tufo repack  assets.tuf compacto.tuf
```

`tufo --help` lista todos os comandos com exemplos.

## Painel Pacotes (.tuf) no editor

**Janela > Pacotes (.tuf)** (`src/editor/EditorTuf.cpp`, com `ERUPTION_EDITOR=ON`):

- abre um `.tuf` por caminho, ou um dos pacotes que o projeto monta (**Pacotes do projeto**);
- árvore de pastas com filtro por nome; por linha, tamanho original, guardado,
  codec e a marca de blob dividido (dedup);
- prévia: imagem (PNG/JPG/BMP/TGA), texto (json, luau, txt...) ou hexdump curto;
- **Extrair** o arquivo ou pasta selecionado, **Adicionar** arquivos ou pasta do
  disco (acréscimo seguro), **Remover**, **Verificar** (em segundo plano, com
  barra de progresso e Cancelar), **Repack** para um arquivo novo e
  **Estatísticas** (as mesmas do `tufo stats`);
- Adicionar e Remover pedem confirmação e **não** escrevem num pacote que o
  projeto em execução tem montado (o jogo mapeia o arquivo): o painel avisa e
  oferece o Repack para uma cópia, que se edita à vontade;
- **Novo pacote** cria um `.tuf` com os primeiros arquivos adicionados;
- textos pelo sistema de idiomas do editor (`data/lang/pt-BR.json`, inglês como chave).

Teste e captura: `ERUPTION_TEST_TUF_PANEL=<arquivo.tuf>[|<entrada>]` abre a
janela no frame 40 com o pacote, as pastas abertas e a entrada selecionada.
