#!/bin/bash
# Sombra ancorada ao mundo no motor de verdade (sem GPU de verdade: xvfb).
# Anda a camera em passos sub-texel girando um pouco e confere, por preset, que
# as matrizes que a luz usa mantem os pontos fixos do mundo na mesma fracao de
# texel (ERUPTION_TEST_SHADOW_ANCHOR_LOG, lido em DeferredLighting). Precisa de
# ./eruption-engine compilado e do mapa de demonstracao. ~30 s por preset.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"; cd "$ROOT"
BIN="${ERUPTION_BIN:-./eruption-engine}"
PY="${PYTHON:-python3}"
TMP=$(mktemp -d)
cat > "$TMP/path.txt" <<'P'
0 2806.83 0 2739.04 -90 55 1000
5 2856.83 0 2739.04 -86 55 1000
10 2906.83 0 2739.04 -82 55 1000
11 2906.83 0 2739.04 -82 55 1000
P
FAIL=0
for pr in "high 3072" "medium 1024" "low 1024" "mobile_high 3072" "mobile_medium 1024" "mobile_low 1024"; do
    set -- $pr
    echo "== $1"
    env ERUPTION_AUDIO=null ERUPTION_NO_VALIDATION=1 ERUPTION_TEST_GRAPHICS_PRESET=$1 ERUPTION_TEST_FIXED_DT=0.0333333 \
        ERUPTION_TEST_TIME=0.12 ERUPTION_TEST_NO_WIND=1 ERUPTION_TEST_WEATHER_SWITCH=1,clear ERUPTION_WINDOW_SIZE=1280x720 \
        ERUPTION_TEST_SHADOW_ANCHOR_LOG=1 ERUPTION_TEST_CAM_PATH="$TMP/path.txt" ERUPTION_TEST_EXIT_FRAME=330 \
        timeout 900 xvfb-run -a -s "-screen 0 1280x720x24" "$BIN" --project projects/parana_field --game \
        --map parana_field_lite > "$TMP/$1.log" 2>&1
    "$PY" tools/shadow_anchor_check.py "$TMP/$1.log" "$2" || FAIL=1
done
mkdir -p ~/DESCARTE_shaders; mv "$TMP" ~/DESCARTE_shaders/shadow_anchor_$$
[ $FAIL -eq 0 ] && echo "SOMBRA ANCORADA: OK" || echo "SOMBRA ANCORADA: FALHOU"
exit $FAIL
