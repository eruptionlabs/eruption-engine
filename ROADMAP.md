# Roadmap

What the engine still needs, grouped by area. Each item says where the work
starts and how to tell it is done. Pick one, open an issue saying you are on
it, and read [AGENTS.md](AGENTS.md) for the rules and the verification steps
before writing code.

Status marks: **open** (nobody on it), **started** (partial code exists),
**design** (needs a decision from the maintainers first; open an issue before
writing code).

## Platforms and delivery

| Item | Status | Where to start | Done when |
|---|---|---|---|
| Continuous integration: build on Linux and run `tools/run_tests.sh` and `ctest` on every pull request | open | `.github/workflows/` (today only the CLA check) | A pull request with a warning or a failing test shows a red check |
| Windows build | open | `CMakeLists.txt` (MSVC flags exist), `src/core/Platform.hpp`; POSIX-only spots: editor remote server, AI client, export scripts | The demo project opens and plays on Windows with no warnings |
| macOS build (Vulkan through a portability layer) | design | `VulkanContext` instance and device creation | The demo project opens on macOS |
| Export from the editor to Android and Windows (today the editor exports Linux only) | open | `src/editor/GameExport.cpp`, `tools/android/build_apk.sh` | **File > Export** produces an installable package for each target |
| Native C++ game modules on Android | open | `include/eruption/native_module.h`, `docs/aulas/10-codigo-em-cpp.md` | A project with a native module runs on a phone |

## Rendering

| Item | Status | Where to start | Done when |
|---|---|---|---|
| Volumetric sky backend (falls back to the procedural one today) | open | `src/renderer/skymap/SkySystem.cpp` | Selectable backend, measured cost per preset |
| Cloud shadows for the layered clouds | open | `src/core/Render.cpp` (cloud shadow map), `CloudLayerRenderer` | Cloud layers darken the ground like the field clouds do |
| Depth-aware composite of cloud layers | started | `CloudLayerRenderer.cpp` (placeholder composite pipeline) | Clouds sort correctly against tall geometry |
| Look settings still marked not implemented: color grading, distant-layer treatment, distance fade | open | `src/renderer/LookConfig.hpp` | Each setting changes the image and has a preset value |
| Toksvig mip mode for normal maps | open | `src/renderer/MipMode.hpp` | Less specular shimmer at distance, measured by PSNR between frames |
| Parallax layers | started | `src/renderer/ParallaxSystem.cpp` (placeholder draw) | Background layers draw and scroll with the camera |
| Runtime shader compilation, for hot reload while editing | open | `src/renderer/ShaderCompiler.cpp` | Saving a `.frag` updates the running editor |

## Mobile

| Item | Status | Where to start | Done when |
|---|---|---|---|
| More devices in the `auto_presets` table | open | `data/graphics.json` | Each tier verified at 30 FPS on at least two phones; send the device, GPU name and measured FPS with the change |
| Remember the calibrated scene scale per device, so later launches skip the warm-up | open | `Engine::settleRenderScale` (`src/core/Render.cpp`) | Second launch starts at the scale the first one settled on |
| Change the graphics preset without restarting (today the **Visual** button applies on the next launch) | open | `Engine::toggleBeautyPreset`, pipeline creation for `shader_lite` | Switching between `mobile_high` and `mobile_beauty` takes effect in place, with no validation errors |
| Thermal awareness: lower the scale when the phone throttles | design | `settleRenderScale`, Android thermal API | Long sessions stay at the target instead of dropping 20% |

## Engine systems

| Item | Status | Where to start | Done when |
|---|---|---|---|
| Instant map change: preload the next map in the background | started | `src/core/MapLoad.cpp`, `Loading.cpp`, the background neighbour loading in the continuous world | `world.preload_map(name)` then `world.warp_map(name)` swaps with no frozen frame |
| Physics: rigid bodies and character collision beyond the walk grid | design | `src/core/WalkGrid.cpp`, `Areas.cpp` | Objects fall, stack and push the player; deterministic with a fixed step |
| Networking for multiplayer games | design | nothing exists yet | Two clients see each other move on the same map |
| Localization for games (today only the editor UI is translated) | open | `data/lang/`, script API (`ui`) | A project ships its strings per language and the player picks one |

## Tests and tooling

| Item | Status | Where to start | Done when |
|---|---|---|---|
| Register every test in `ctest` | open | `CMakeLists.txt` (unused `TEST_SOURCES` glob, removed GPU test names that no longer exist), `tests/test_sprite_order.cpp` | `ctest` runs every file in `tests/` |
| GPU regression tests (render a pose, compare by PSNR) | open | the pose capture variables in [AGENTS.md](AGENTS.md) | A test fails when a change alters a frame it should not |
| Code formatting config | open | repository root | A `.clang-format` that matches the existing style and does not reflow the codebase |
