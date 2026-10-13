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
    confere "YParticles3D emite (MultiMesh no RenderingServer)" "$s" "de 400 instancias, malha de 4 vertices"
    s=$(timeout 60 "$RUN" --godot "$GD/libgd_yparticles3d.so:yparticles3d_init" \
        --tscn "$RAIZ/compat/testdata/godot/YParticles3D/test_project/main.tscn" --quadros 60 2>&1)
    confere "cena .tscn do YParticles3D" "$s" "de 400 instancias"
    s=$(timeout 60 "$RUN" --godot "$GD/libgd_summator.so:summator_library_init" \
        --tscn "$RAIZ/compat/tests/godot/cenas/hierarquia.tscn" --quadros 2 2>&1)
    confere "cena .tscn: Transform3D" "$s" "'Filho' pos=(1, 2, 3) rot=(0, 90, 0) escala=(2, 2, 2)"
    confere "cena .tscn: pai por caminho" "$s" "    Node3D#3 'Neto' pos=(0, 0, 5)"
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

echo "Unity (scripts C#)"
CS="$RAIZ/compat/build/unity-cs"
if [ -f "$CS/Klak.dll" ]; then
    export ERUPTION_DOTNET_ROOT="${ERUPTION_DOTNET_ROOT:-$RAIZ/compat/testdata/dotnet}"
    s=$(timeout 60 "$RUN" --unity-cs "$CS/Klak.dll" \
        --unity-cs-objeto Girador --unity-cs-componente ConstantMotion _rotationMode=YAxis _rotationSpeed=90 \
            _translationMode=XAxis _translationSpeed=2 \
        --unity-cs-objeto Errante --unity-cs-componente BrownianMotion \
        --quadros 60 --unity-cs-objeto Vazio --unity-cs-descrever 2>&1)
    confere "runtime .NET + UnityEngine da compat" "$s" "runtime .NET ativo"
    confere "assembly de scripts (3 MonoBehaviour)" "$s" "assembly carregado: Klak (3 MonoBehaviour)"
    s=$(timeout 60 "$RUN" --unity-cs "$CS/Klak.dll" \
        --unity-cs-objeto Girador --unity-cs-componente ConstantMotion _rotationMode=YAxis _rotationSpeed=90 \
            _translationMode=XAxis _translationSpeed=2 --quadros 60 --unity-cs-descrever 2>&1)
    confere "ConstantMotion Update/Time.deltaTime (1 s)" "$s" "pos=(2.00, 0.00, 0.00) rot=(0.00, 90.00, 0.00)"
    s=$(timeout 60 "$RUN" --unity-cs "$CS/Klak.dll" --unity-cs-objeto Errante \
        --unity-cs-componente BrownianMotion --quadros 30 --unity-cs-descrever 2>&1)
    confere "BrownianMotion Start/Update (ruido)" "$s" "gameobject: Errante pos="
else
    echo "  (scripts C# nao compilados: compat/tests/unity/compilar_csharp.sh)"
fi

echo "Unreal (modulos)"
if [ -d "$UE" ]; then
    for up in "$UE"/*/*.uplugin; do
        [ -f "$up" ] || continue
        nome=$(basename "$up" .uplugin)
        s=$(timeout 60 "$RUN" --unreal "$up" 2>&1)
        confere "$nome StartupModule" "$s" "StartupModule"
    done
    if [ -f "$UE/SERIALCOM/SERIALCOM.uplugin" ]; then
        s=$(timeout 60 "$RUN" --unreal "$UE/SERIALCOM/SERIALCOM.uplugin" 2>&1)
        confere "SERIALCOM UE_LOG no StartupModule" "$s" "SERIALCOMLog: SERIALCOM: Log Started"
        confere "SERIALCOM UE_LOG no ShutdownModule" "$s" "SERIALCOMLog: SERIALCOM: Log Ended"
    fi
    if [ -f "$UE/AdvancedEases/AdvancedEases.uplugin" ]; then
        s=$(timeout 60 "$RUN" --unreal "$UE/AdvancedEases/AdvancedEases.uplugin" \
            --unreal-chamar UAdvancedEasesBPLibrary.AdvancedEase_Float 7 0.5 0 10 2>&1)
        confere "AdvancedEase_Float(OutBounce, 0.5, 0, 10)" "$s" "-> 7.65625"
        s=$(timeout 60 "$RUN" --unreal "$UE/AdvancedEases/AdvancedEases.uplugin" \
            --unreal-chamar UAdvancedEasesBPLibrary.AdvancedEase_Vector AE_LERP 0,0,0 10,20,30 0.5 2>&1)
        confere "AdvancedEase_Vector (FVector, enum por nome)" "$s" "-> (5, 10, 15)"
    fi
else
    echo "  (plugins Unreal nao compilados)"
fi

echo "resultado: $ok ok, $falhas falha(s)"
[ "$falhas" = 0 ]
