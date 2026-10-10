#!/usr/bin/env bash
# Sobe o motor sem tela (xvfb), com plugins GDExtension de teste carregados
# pela variavel ERUPTION_COMPAT_PLUGINS, e confere no log que carregaram e
# rodaram. Precisa do motor compilado com -DERUPTION_MODULE_COMPAT=ON e das
# extensoes de compat/tests/godot.
#
#   bash compat/scripts/testar_motor.sh [executavel] [pasta_dos_.so]
set -uo pipefail

RAIZ="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
EXE="${1:-$RAIZ/eruption-engine}"
BIN="${2:-$RAIZ/compat/build/godot-tests/bin}"
LOG="$RAIZ/compat/build/motor-run.log"
mkdir -p "$(dirname "$LOG")"

export ERUPTION_AUDIO=null
export ERUPTION_TEST_NO_DOF=1
export ERUPTION_TEST_NO_CA=1
export ERUPTION_TEST_SCREENSHOT_FRAME=60
export ERUPTION_COMPAT_PLUGINS="godot:$BIN/libgd_gdexample.so:example_library_init;godot:$BIN/libgd_summator.so:summator_library_init"
export ERUPTION_COMPAT_GODOT_CENA="GDExample"

cd "$RAIZ"
timeout 120 xvfb-run -a "$EXE" --game --screenshot "$RAIZ/compat/build/motor-compat" --auto-exit >"$LOG" 2>&1
codigo=$?
echo "motor saiu com codigo $codigo (log: $LOG)"
grep "\[compat:" "$LOG" | head -20

falhou=0
grep -q "extensao carregada: libgd_gdexample.so" "$LOG" || { echo "FALHA: GDExample nao carregou"; falhou=1; }
grep -q "extensao carregada: libgd_summator.so" "$LOG" || { echo "FALHA: Summator nao carregou"; falhou=1; }
grep -q "na cena: GDExample" "$LOG" || { echo "FALHA: GDExample nao entrou na cena"; falhou=1; }
grep -q "quadro 10 " "$LOG" || { echo "FALHA: menos de 10 quadros processados"; falhou=1; }
[ $falhou = 0 ] && echo "OK: plugins carregados e processados dentro do motor"
exit $falhou
