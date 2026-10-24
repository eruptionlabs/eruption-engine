#!/bin/bash
# Roda todos os testes que linkam contra os objetos ja' compilados da engine.
# Sao rapidos (segundos) porque nao sobem Vulkan: exercitam a logica pura.
# Exige um build antes (./build.sh).
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"; cd "$ROOT"
OBJ="build/CMakeFiles/eruption-engine.dir/src"
[ -f "$OBJ/renderer/WeatherSystem.cpp.o" ] || { echo "ERRO: rode ./build.sh primeiro."; exit 1; }

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
FAIL=0

run() {
    local name="$1"; shift
    printf '\n=== %s ===\n' "$name"
    if ! g++ -std=c++17 -DGLM_FORCE_DEPTH_ZERO_TO_ONE -I src -I include \
        -I build/_deps/glm-src -I build/_deps/nlohmann_json-src/single_include \
        "tests/$name.cpp" \
        "$OBJ/renderer/WeatherSystem.cpp.o" "$OBJ/renderer/WindField.cpp.o" \
        "$OBJ/renderer/ClimateField.cpp.o" "$OBJ/renderer/ClimateFuzzy.cpp.o" \
        "$OBJ/core/Logger.cpp.o" -o "$TMP/$name" 2>&1 | head -20; then
        echo "  ERRO DE COMPILACAO"; FAIL=1; return
    fi
    "$TMP/$name" || FAIL=1
}

# Teste so' de geometria: compila o fonte junto (sem os objetos da engine).
run_src() {
    local name="$1"; shift
    printf '\n=== %s ===\n' "$name"
    if ! g++ -std=c++17 -DGLM_FORCE_DEPTH_ZERO_TO_ONE -DGLM_FORCE_LEFT_HANDED -I src -I build/_deps/glm-src \
        "tests/$name.cpp" "$@" -o "$TMP/$name" 2>&1 | head -20; then
        echo "  ERRO DE COMPILACAO"; FAIL=1; return
    fi
    "$TMP/$name" || FAIL=1
}

run test_surface_wetness
run test_climate_fuzzy
run test_weather_overrides
run_src test_shadow_receivers src/math/Frustum.cpp
run_src test_mesh_clusters src/utils/MeshClusters.cpp
run_src test_object_world src/scene/ObjectWorld.cpp
run_src test_css_classes src/utils/CSSParser.cpp
run_src test_ground_pack src/formats/GroundPack.cpp -I build/_deps/nlohmann_json-src/include
run_src test_ribbon src/formats/Ribbon.cpp

printf '\n========================================\n'
[ $FAIL -eq 0 ] && echo "TODOS OS TESTES PASSARAM" || echo "HOUVE FALHA"
printf '========================================\n'
exit $FAIL
