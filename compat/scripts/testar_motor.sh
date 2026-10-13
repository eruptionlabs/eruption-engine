#!/usr/bin/env bash
# Sobe o motor sem tela (xvfb) com plugins de teste das tres origens
# carregados por ERUPTION_COMPAT_PLUGINS e confere no log que carregaram e
# rodaram quadros. Precisa do motor com -DERUPTION_MODULE_COMPAT=ON e dos
# builds de compat/tests/* (ver compat/TESTED_PLUGINS.md).
#
#   bash compat/scripts/testar_motor.sh [executavel]
set -uo pipefail

RAIZ="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
EXE="${1:-$RAIZ/eruption-engine}"
B="$RAIZ/compat/build"
LOG="$B/motor-run.log"
mkdir -p "$B"

export ERUPTION_AUDIO=null
export ERUPTION_TEST_NO_DOF=1
export ERUPTION_TEST_NO_CA=1
export ERUPTION_TEST_SCREENSHOT_FRAME=60
export ERUPTION_DOTNET_ROOT="${ERUPTION_DOTNET_ROOT:-$RAIZ/compat/testdata/dotnet}"
P="godot:$B/godot-tests/bin/libgd_gdexample.so:example_library_init"
P="$P;godot:$B/godot-tests/bin/libgd_summator.so:summator_library_init"
YP="$B/godot-tests/bin/libgd_yparticles3d.so"
[ -f "$YP" ] && P="$P;godot:$YP:yparticles3d_init"
[ -f "$B/unity-tests/bin/libRenderingPlugin.so" ] && P="$P;unity:$B/unity-tests/bin/libRenderingPlugin.so"
[ -f "$B/unity-cs/Klak.dll" ] && P="$P;unity-cs:$B/unity-cs/Klak.dll"
[ -f "$B/unreal-tests/AdvancedEases/AdvancedEases.uplugin" ] && P="$P;$B/unreal-tests/AdvancedEases/AdvancedEases.uplugin"
export ERUPTION_COMPAT_PLUGINS="$P"
CENA_GD="$RAIZ/compat/testdata/godot/YParticles3D/test_project/main.tscn"
CENA_UN="$RAIZ/compat/testdata/unity/Klak/Assets/Samples/ConstantMotion.unity"
export ERUPTION_COMPAT_GODOT_CENA="GDExample"
[ -f "$YP" ] && [ -f "$CENA_GD" ] && ERUPTION_COMPAT_GODOT_CENA="$ERUPTION_COMPAT_GODOT_CENA;$CENA_GD"
export ERUPTION_COMPAT_UNITY_CENA="Girador:ConstantMotion:_rotationMode=YAxis:_rotationSpeed=90"
[ -f "$CENA_UN" ] && ERUPTION_COMPAT_UNITY_CENA="$ERUPTION_COMPAT_UNITY_CENA;$CENA_UN"

cd "$RAIZ"
timeout 120 xvfb-run -a "$EXE" --game --screenshot "$B/motor-compat" --auto-exit >"$LOG" 2>&1
codigo=$?
echo "motor saiu com codigo $codigo (log: $LOG)"
grep "\[compat:" "$LOG" | head -30

falhou=0
confere() { grep -q "$1" "$LOG" || { echo "FALHA: $2"; falhou=1; }; }
confere "extensao carregada: libgd_gdexample.so" "GDExample nao carregou"
confere "extensao carregada: libgd_summator.so" "Summator nao carregou"
confere "na cena: GDExample" "GDExample nao entrou na cena"
[ -f "$B/unity-tests/bin/libRenderingPlugin.so" ] && confere "plugin carregado: libRenderingPlugin.so" "plugin nativo Unity nao carregou"
[ -f "$B/unity-cs/Klak.dll" ] && confere "na cena (C#): Girador" "script C# nao entrou na cena"
[ -f "$B/unreal-tests/AdvancedEases/AdvancedEases.uplugin" ] && confere "modulo AdvancedEases: StartupModule" "modulo Unreal nao subiu"
confere "quadro 10 " "menos de 10 quadros processados"
[ -f "$YP" ] && [ -f "$CENA_GD" ] && confere "desenho: multimesh .* de 400 instancias" "particulas do YParticles3D (cena .tscn) nao sairam"
[ -f "$B/unity-cs/Klak.dll" ] && [ -f "$CENA_UN" ] && confere "cena ConstantMotion.unity: 10 GameObject" "cena .unity nao montou"
[ $falhou = 0 ] && echo "OK: plugins carregados e processados dentro do motor"
exit $falhou
