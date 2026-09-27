<p align="center">
  <img src="assets/icon/eruption_v3_256.png" width="160" alt="Eruption Engine">
</p>

<h1 align="center">Eruption Engine</h1>

<p align="center">
  <b>English</b> · <a href="README.pt-BR.md">Português</a> · <a href="README.ja.md">日本語</a>
</p>

<p align="center">
  A rendering engine for diorama-style worlds, written in C++17 on Vulkan 1.3.
</p>

<p align="center">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-Apache%202.0-blue.svg" alt="Apache 2.0"></a>
  <img src="https://img.shields.io/badge/C%2B%2B-17-informational.svg" alt="C++17">
  <img src="https://img.shields.io/badge/Vulkan-1.3-red.svg" alt="Vulkan 1.3">
</p>

## About

Eruption Engine renders 3D scenes with a diorama look: a tilted camera, miniature depth of field, 2D sprites living alongside 3D geometry, and physically based lighting. It is written in plain C++17 directly on Vulkan 1.3, with no intermediate layers. It is designed to run well on anything from an old integrated GPU to current cards, scaling quality through parameters instead of switching features off.

## Features

- **Vulkan 1.3 backend** with `dynamic_rendering`, `synchronization2` and bindless textures through descriptor indexing.
- **Deferred shading** with a five-attachment G-buffer and hundreds of dynamic lights.
- **Physically based lighting**: energy-compensated Cook-Torrance, IBL, irradiance probes, and PBR maps synthesized from albedo at load time.
- **Shadows**: cascaded shadow maps for the sun, cubemaps for point lights, and a shadow LOD independent from the mesh LOD.
- **Weather and atmosphere**: more than 40 weather types, rain and snow occluded by geometry, wet surfaces, fog, day/night cycle, volumetric clouds.
- **Water**: Gerstner waves, reflection and refraction.
- **Post-processing**: tilt-shift with a physical circle of confusion, bloom, god rays, AgX/ACES tone mapping, FXAA with upscaling.
- **Terrain and models**: cell-based terrain meshes with material blending, glTF models with mesh and foliage LOD, instancing.
- **2D sprites** with runtime-generated normal maps, planar shadows, and full integration into the deferred pipeline.
- **CSS-driven HUD**: widget layout lives in `data/hud/default.css`, hot-reloads on save, and has a visual editor (Caldera).
- **Tools**: visual HUD editor (Caldera), PBR cooker, normal map generator.

## Building

Requirements:

- A C++17 compiler (GCC 13+, Clang 16+ or MSVC 2022+)
- CMake 3.28+
- Vulkan SDK 1.3+
- GLFW 3.3+, zlib, iconv
- For Debug builds: the Vulkan validation layers (`vulkan-validationlayers`)

All other dependencies (GLM, VMA, shaderc, Dear ImGui, nlohmann/json, tinygltf, Draco, bc7enc) are fetched by CMake.

```bash
git clone --recurse-submodules https://github.com/eruptionlabs/eruption-engine.git
cd eruption-engine
./build_and_run.sh          # optimized build, then run
./build.sh                  # Debug build with Vulkan validation
```

To run a specific map:

```bash
./launch.sh --map parana_field
```

The `parana_field` demo map uses CC0 textures only. Its `.glb` file is larger than GitHub allows, so it ships as a release asset: the first CMake configure downloads it automatically (about 170 MB) and verifies its checksum. To fetch it by hand, run `tools/fetch_demo_map.sh`; to skip it, pass `-DERUPTION_FETCH_DEMO_MAP=OFF`. On the first load the engine compresses the embedded textures and caches the result next to the map.

## Layout

| Folder | Contents |
|---|---|
| `src/` | Engine core: renderer, formats, weather, camera and HUD |
| `shaders/` | GLSL, compiled to SPIR-V during the build |
| `assets/` | Icons, sample sprite and demo map |
| `data/` | Graphics, weather, water and shadow settings |
| `tools/` | PBR cooker, normal map generator and build scripts |
| `tools/caldera/` | Caldera, the visual HUD editor (submodule) |
| `tests/` | Unit and regression tests |

## Related repositories

- [caldera](https://github.com/eruptionlabs/caldera): visual HUD editor.

## Contributing

Please read [CONTRIBUTING.md](CONTRIBUTING.md) before opening a pull request. Code contributions are accepted under the Apache License 2.0. We do not accept assets, models, textures or maps that carry third-party rights.

## License

The source code is released under the [Apache License 2.0](LICENSE). Third-party libraries and their licenses are listed in [NOTICE](NOTICE). The assets in `assets/` follow their own licenses and are not covered by the code license.

"Eruption Engine" and its logo are trademarks of Gdg Soluções Digitais LTDA. The code license does not grant any right to use the trademarks.
