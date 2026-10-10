#!/usr/bin/env bash
# Bateria da camada de compatibilidade sem o motor: roda cada plugin de
# teste pelo eruption-compat-run e confere a saida esperada.
# Pre-requisitos: baixar_testes.sh e os builds de compat/tests/* e de compat/.
#
#   bash compat/scripts/testar_compat.sh [pasta_do_build_compat]
set -uo pipefail

RAIZ="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="${1:-$RAIZ/compat/build/sozinho}"
RUN="$BUILD/eruption-compat-run"
GD="$RAIZ/compat/build/godot-tests/bin"
UN="$RAIZ/compat/build/unity-tests/bin"
UE="$RAIZ/compat/build/unreal-tests"
ok=0
falhas=0

confere() { # nome, saida, texto esperado
    if grep -qF -- "$3" <<<"$2"; then
        echo "  ok    $1"
        ok=$((ok + 1))
    else
        echo "  FALHA $1 (esperado: $3)"
        falhas=$((falhas + 1))
    fi
}

echo "Godot (GDExtension)"
if [ -f "$GD/libgd_summator.so" ]; then
    s=$(timeout 60 "$RUN" --godot "$GD/libgd_summator.so:summator_library_init" --novo Summator \
        --chamar add 10 --chamar add 20 --chamar get_total --chamar reset --chamar add --chamar get_total 2>&1)
    confere "Summator add/get_total" "$s" "get_total() -> 30"
    confere "Summator argumento padrao" "$s" "get_total() -> 1"
    s=$(timeout 60 "$RUN" --godot "$GD/libgd_template.so:example_library_init" --novo ExampleClass \
        --chamar print_type 42 --chamar print_type ola 2>&1)
    confere "ExampleClass vformat (int)" "$s" "Type: 2"
    confere "ExampleClass vformat (String)" "$s" "Type: 4"
    s=$(timeout 60 "$RUN" --godot "$GD/libgd_gdexample.so:example_library_init" --novo GDExample --cena \
        --quadros 60 --descrever 2>&1)
    confere "GDExample _process/set_position" "$s" "pos=(19.093, 10.7074, 0)"
    s=$(timeout 60 "$RUN" --godot "$GD/libgd_yparticles3d.so:yparticles3d_init" --novo YParticles3D --cena \
        --quadros 30 2>&1)
    confere "YParticles3D carrega e roda" "$s" "falhas: 0"
else
    echo "  (extensoes Godot nao compiladas)"
fi

echo "Unity (plugin nativo)"
if [ -f "$UN/libRenderingPlugin.so" ]; then
    s=$(timeout 60 "$RUN" --unity "$UN/libRenderingPlugin.so" --unity-chamar SetTimeFromUnity "v(f)" 1.5 \
        --unity-evento 1 --quadros 3 2>&1)
    confere "UnityPluginLoad + IUnityGraphics" "$s" "callback de dispositivo registrado"
    confere "GetRenderEventFunc" "$s" "evento de render 1 entregue a 1 plugin(s)"
    confere "UnityPluginUnload" "$s" "1 plugin(s) descarregado(s)"
    if [ -x "$BUILD/eruption-compat-unity-gl" ]; then
        s=$(timeout 60 "$BUILD/eruption-compat-unity-gl" "$UN/libRenderingPlugin.so" 2>&1)
        confere "render OpenGL do plugin (EGL)" "$s" "OK: o plugin desenhou"
    fi
else
    echo "  (plugin Unity nao compilado)"
fi

echo "Unreal (modulos)"
if [ -d "$UE" ]; then
    for up in "$UE"/*/*.uplugin; do
        [ -f "$up" ] || continue
        nome=$(basename "$up" .uplugin)
        s=$(timeout 60 "$RUN" --unreal "$up" 2>&1)
        confere "$nome StartupModule" "$s" "StartupModule"
    done
    if [ -f "$UE/AdvancedEases/AdvancedEases.uplugin" ]; then
        s=$(timeout 60 "$RUN" --unreal "$UE/AdvancedEases/AdvancedEases.uplugin" \
            --unreal-chamar UAdvancedEasesBPLibrary.AdvancedEase_Float 7 0.5 0 10 2>&1)
        confere "AdvancedEase_Float(OutBounce, 0.5, 0, 10)" "$s" "-> 7.65625"
    fi
else
    echo "  (plugins Unreal nao compilados)"
fi

echo "resultado: $ok ok, $falhas falha(s)"
[ "$falhas" = 0 ]
