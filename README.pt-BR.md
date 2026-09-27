<p align="center">
  <img src="assets/icon/eruption_v3_256.png" width="160" alt="Eruption Engine">
</p>

<h1 align="center">Eruption Engine</h1>

<p align="center">
  <a href="README.md">English</a> · <b>Português</b> · <a href="README.ja.md">日本語</a>
</p>

<p align="center">
  Motor de renderização para mundos em diorama, em C++17 e Vulkan 1.3.
</p>

<p align="center">
  <a href="LICENSE"><img src="https://img.shields.io/badge/licen%C3%A7a-Apache%202.0-blue.svg" alt="Apache 2.0"></a>
  <img src="https://img.shields.io/badge/C%2B%2B-17-informational.svg" alt="C++17">
  <img src="https://img.shields.io/badge/Vulkan-1.3-red.svg" alt="Vulkan 1.3">
</p>

## Sobre

A Eruption Engine renderiza cenas 3D com estética de diorama: câmera inclinada, profundidade de campo em miniatura, sprites 2D convivendo com geometria 3D e iluminação física. É escrita em C++17 puro sobre Vulkan 1.3, sem camadas intermediárias, e foi pensada para rodar bem tanto em uma GPU integrada antiga quanto em placas atuais, degradando qualidade por parâmetro em vez de desligar recursos.

## Recursos

- **Backend Vulkan 1.3** com `dynamic_rendering`, `synchronization2` e texturas *bindless* via descriptor indexing.
- **Deferred shading** com G-buffer de cinco attachments e centenas de luzes dinâmicas.
- **Iluminação física**: Cook-Torrance com compensação de energia, IBL, probes de irradiância e síntese de mapas PBR a partir de albedo em tempo de carga.
- **Sombras**: cascaded shadow maps para a luz direcional, cubemaps para luzes pontuais, LOD de sombra independente do LOD de malha.
- **Clima e atmosfera**: mais de 40 tipos de clima, chuva e neve com oclusão por geometria, superfícies molhadas, névoa, ciclo dia/noite, nuvens volumétricas.
- **Água**: ondas de Gerstner, reflexão e refração.
- **Pós-processamento**: tilt-shift com círculo de confusão físico, bloom, god rays, tone mapping AgX/ACES, FXAA com upscale.
- **Terreno e modelos**: malhas de terreno por célula com blend de materiais, modelos glTF com LOD de malha e de folhagem, instanciamento.
- **Sprites 2D** com normal mapping gerado em runtime, sombras planares e integração no pipeline deferred.
- **HUD dirigida por CSS**: layout dos widgets em `data/hud/default.css`, com recarga a quente ao salvar e editor visual (Caldera).
- **Ferramentas**: editor visual de HUD (Caldera), cooker de PBR, geração de normal maps.

## Compilando

Requisitos:

- Compilador C++17 (GCC 13+, Clang 16+ ou MSVC 2022+)
- CMake 3.28+
- Vulkan SDK 1.3+
- GLFW 3.3+, zlib, iconv
- Para builds Debug: camadas de validação do Vulkan (`vulkan-validationlayers`)

As demais dependências (GLM, VMA, shaderc, Dear ImGui, nlohmann/json, tinygltf, Draco, bc7enc) são baixadas pelo CMake.

```bash
git clone --recurse-submodules https://github.com/eruptionlabs/eruption-engine.git
cd eruption-engine
./build_and_run.sh          # build otimizado e execução
./build.sh                  # build Debug com validação do Vulkan
```

Para rodar um mapa específico:

```bash
./launch.sh --map parana_field
```

O mapa de demonstração `parana_field` usa apenas texturas CC0. Seu arquivo `.glb` passa do limite do GitHub, então ele é distribuído como asset de release: a primeira configuração do CMake baixa o arquivo sozinha (cerca de 170 MB) e confere o checksum. Para baixar na mão, rode `tools/fetch_demo_map.sh`; para pular, passe `-DERUPTION_FETCH_DEMO_MAP=OFF`. No primeiro carregamento a engine comprime as texturas embutidas e guarda o resultado em cache ao lado do mapa.

## Estrutura

| Pasta | Conteúdo |
|---|---|
| `src/` | Núcleo da engine: renderer, formatos, clima, câmera e HUD |
| `shaders/` | GLSL compilado para SPIR-V no build |
| `assets/` | Ícones, sprite de exemplo e mapa de demonstração |
| `data/` | Configurações de gráficos, clima, água, sombras |
| `tools/` | Cooker de PBR, normalgen e scripts usados pelo build |
| `tools/caldera/` | Caldera, editor visual da HUD (submódulo) |
| `tests/` | Testes de unidade e de regressão |

## Repositórios relacionados

- [caldera](https://github.com/eruptionlabs/caldera): editor visual da HUD.

## Contribuindo

Leia o [CONTRIBUTING.md](CONTRIBUTING.md) antes de abrir um pull request. Contribuições de código entram sob a Apache License 2.0. Não aceitamos assets, modelos, texturas ou mapas com direitos de terceiros.

## Licença

O código-fonte é distribuído sob a [Apache License 2.0](LICENSE). Bibliotecas de terceiros e suas licenças estão listadas em [NOTICE](NOTICE). Os assets em `assets/` seguem suas próprias licenças e não fazem parte da licença do código.

"Eruption Engine" e o logotipo são marcas da Gdg Soluções Digitais LTDA. A licença do código não concede direito de uso da marca.
