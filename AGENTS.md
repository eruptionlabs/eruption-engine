# Working on Eruption Engine

This file is for anyone changing the engine, people and coding agents alike.
It covers how the code is organized, the rules every change follows, and how
to prove a change is correct before opening a pull request. What is missing
and worth building is listed in [ROADMAP.md](ROADMAP.md); the legal side of
contributing (CLA, licensing, what we do not accept) is in
[CONTRIBUTING.md](CONTRIBUTING.md).

## The short version

1. The engine is generic. Game content and game rules live in a game project
   (`projects/<name>` or a folder outside the repo), never in `src/`.
2. No third-party intellectual property anywhere: no assets, maps, textures,
   names or references to existing games in code, comments, tests or tools.
3. Zero compiler warnings, on GCC and on the Android NDK's Clang.
4. Every rendering or performance change is measured before and after: frame
   time from telemetry, image from PSNR. A change that does not measurably
   help is reverted.
5. Quality scales through preset parameters. Shadows are never switched off
   by a preset; a cheaper path keeps them.
6. Every Vulkan change passes the validation layers with no errors.

## Layout

| Path | What lives there |
|---|---|
| `src/core/` | `Engine` (init, frame loop, presets), map loading, input, characters, walk grid, pathfinding |
| `src/renderer/` | Vulkan context, G-buffer, deferred lighting, shadows, post-processing, water, weather, clouds, sky, VFX |
| `src/formats/` | glTF, terrain, navigation grid and map bundle parsers |
| `src/script/` | Luau host and the script API (`world`, `camera`, `models`, `objects`, ...), save games |
| `src/editor/` | The editor (built with `ERUPTION_EDITOR=ON`), game export |
| `src/audio/`, `src/ml/` | Optional modules, each with a `*Null.cpp` fallback chosen by `cmake/modules.json` |
| `src/platform/android/` | Android entry point and the small GLFW/ImGui shim the engine needs there |
| `shaders/` | GLSL, compiled to SPIR-V by the build |
| `data/` | Engine defaults: graphics presets, weather, water, shadows, editor UI languages |
| `projects/parana_field/` | The demo game project |
| `tools/` | Build scripts, Android packaging, PBR and normal-map tools, test drivers |
| `tests/` | Unit and regression tests |
| `docs/aulas/` | Step-by-step tutorials (Portuguese) |

## Code style

- C++17, 4-space indentation, `#pragma once`, everything in `namespace eruption`
  (file-local helpers in an anonymous namespace or `static`).
- Types and files in `PascalCase`, functions in `camelCase`, members `m_name`,
  statics `s_name`, constants `kName`.
- Logging through `ERUPTION_LOG_INFO/WARN/ERROR/DEBUG`, never `printf`.
- Prefer named functions over long lambdas. Short local predicates are fine.
- Object-oriented design in reusable code; data-oriented layouts (SoA, flat
  arrays, no per-item virtual calls) on hot paths such as culling and drawing.
- Comments explain why, with the measurement that justified the choice when
  there was one. Most existing comments are in Portuguese; write in the
  language of the file you are editing.
- Do not grow the engine with features no game asked for. Optional subsystems
  go behind a module with a null fallback.

## Rules for the renderer

- Push constants stay at or under 128 bytes. Larger per-draw blocks go through
  `PushRing` (a dynamic-offset uniform buffer).
- Every descriptor a pipeline statically uses must be written before the draw,
  even when the feature is off: bind a valid fallback image (see the bloom
  and cloud bindings in `PostProcessor::render`).
- Fallback order for any computed resource: GPU, then CPU, then an analytic
  approximation.
- New quality knobs belong in `data/graphics.json` presets, read in
  `Engine::applyGraphicsPreset`. Preset keys for render effects are applied by
  `Engine::applyPresetEffects`, after the effects exist.
- Weak mobile GPUs (the `mobile_*` presets, `shader_lite`) have hard limits
  that hang the GPU until reboot when crossed:
  - specialization constants only in fragment shaders, never in vertex shaders;
  - no `vkCmdDispatchBase` with a non-zero base;
  - at most 16 vertex attributes, and 16-bit formats in pairs (`R16G16`), never
    a lone `R16` at an odd offset;
  - no linear filtering on `D32`/`R32F` (use `VulkanContext::filterFor`);
  - no frame longer than about one second of GPU time.
- Environment variables named `ERUPTION_TEST_*` are for tests and debugging.
  Document each one where it is read.

## Graphics presets

`data/graphics.json` holds the presets. `low`, `medium` and `high` are for
desktop GPUs. `mobile_low`, `mobile_medium` and `mobile_high` are for phones;
the Android package uses the preset `auto`, which picks one of them by GPU
name through the `auto_presets` table. A preset with `target_fps` also
calibrates the scene resolution once per map, between `render_scale_min` and
`render_scale_max` (`Engine::settleRenderScale`). On phones where the table
picks `mobile_high`, the touch panel offers a **Visual** button that switches
to `mobile_beauty` (the desktop look at a lower output resolution); the choice
is stored in `preset_override.txt` and applies on the next launch, because the
shader path is chosen before the pipelines exist.

## Building

```bash
./build_and_run.sh                          # optimized build (build-rel/), then run
./build.sh                                  # Debug build with Vulkan validation
cmake -S . -B build-rel -DERUPTION_BUILD_TESTS=ON && cmake --build build-rel -j
tools/android/build_apk.sh <map>[,<map>...] # Android package (needs SDK, NDK and JDK 17)
```

`build_apk.sh` finds each map in `assets/external/<map>` or in a game project
(`projects/*/assets/external/<map>`). Install packages with
`adb install --no-incremental -r`: an incremental install streams the APK from
the computer on demand, and a phone that reboots before it finishes crashes on
the first launch with `SIGBUS`, before the engine logs anything.

## Proving a change

Run what applies, in this order:

1. **Build** with no warnings on desktop. For changes to shared code or
   shaders, build for Android too (`tools/android/build_apk.sh`): the NDK's
   Clang reports warnings GCC does not.
2. **Fast tests**: `tools/run_tests.sh` (logic only, no GPU) and `ctest` in the
   build directory.
3. **Engine tests**: `tools/test_objects.sh` runs the real engine headless
   (script object API, save/load, editor play/stop round trip).
4. **Validation**: run once with the Khronos validation layer
   (`VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation`, or
   `ERUPTION_VK_VALIDATION=1` with the layers packaged on Android). The log
   must have no `Validation Error`.
5. **Image**: capture the same frame before and after and compare by PSNR.
   For a frame that repeats exactly between runs use `ERUPTION_TEST_FIXED_DT`,
   `ERUPTION_TEST_TIME` (time of day, 0..1), `ERUPTION_TEST_NO_WIND=1`,
   `ERUPTION_TEST_NO_CLOUD_SHADOW=1`, `ERUPTION_TEST_WEATHER_SWITCH=1,clear`,
   `ERUPTION_TEST_NO_MOTION_BLUR=1`, `ERUPTION_TEST_NO_DOF=1`,
   `ERUPTION_TEST_NO_CA=1`, then `ERUPTION_TEST_SCREENSHOT_FRAME=<n>` with
   `--screenshot <file>`, or `ERUPTION_TEST_POSE_LIST`/`ERUPTION_TEST_POSE_OUT`
   for several camera poses in one run. Two identical runs are pixel-equal
   under these settings. An optimization that is meant to be invisible must
   stay above 60 dB with almost no pixel differing by more than 8; take some
   captures with a low sun, where long shadows reveal culling mistakes.
6. **Performance**: `ERUPTION_TELEMETRY_OUT=<file.jsonl>` records per-frame CPU
   phases and GPU pass times. Compare averages over the same frames before and
   after. Headless runs under `xvfb-run` are fine for images and GPU pass
   times, but they inflate frames per second, so do not quote FPS from them.
   On phones, results vary by up to 30% between identical runs (heat, clocks):
   reboot before each A/B and alternate the runs. To try preset values without
   editing `data/graphics.json` (or rebuilding a phone package), set
   `ERUPTION_TEST_PRESET_SET="key=value,key=value"`; the log lists each key it
   changed.

## Commits

- One logical change per commit. Subject in the form `Subsystem: short
  description` (Portuguese, like the history), body with the measurement.
- Do not commit generated or machine-local files: build directories, caches,
  `editor_layout.ini`, map data, baked texture packs. `eruption.d.luau` is
  regenerated by the engine on every start; commit it only with the script
  API change that produced it.
