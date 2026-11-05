#!/bin/bash
# Rampa de tempo de quadro (fps baixo): o borrao de movimento nao pode estourar com dt grande.
# Anda o heroi 150 quadros com ERUPTION_TEST_FIXED_DT = 1/60, 0,05 e 0,1 e mede, nos quadros em
# movimento, a fracao da tela que o borrao altera (>8 niveis, contra a mesma cena SEM borrao).
# Falha se, em qualquer dt, a fracao passar de 2x a de 60 fps (sem a escala por dt chegava a 3x),
# e se a 60 fps a imagem mudar em relacao ao comportamento antigo (ERUPTION_TEST_MB_DT_REF=0).
# Precisa de ./eruption-engine, xvfb-run, python com numpy+Pillow ($PY, padrao ~/miniconda3/bin/python).
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"; cd "$ROOT"
PY=${PY:-$HOME/miniconda3/bin/python}; OUT=$(mktemp -d "${TMPDIR:-/tmp}/dtramp.XXXXXX"); FAIL=0
run() { # <nome> <dt> [VAR=val...]
    local n=$1 dt=$2; shift 2; mkdir -p "$OUT/$n"
    env ERUPTION_AUDIO=null ERUPTION_NO_VALIDATION=1 ERUPTION_TEST_GRAPHICS_PRESET=high ERUPTION_TEST_FIXED_DT=$dt \
        ERUPTION_TEST_TIME=0.40 ERUPTION_TEST_NO_WIND=1 ERUPTION_TEST_NO_CLOUD_SHADOW=1 ERUPTION_TEST_WEATHER_SWITCH=1,clear \
        ERUPTION_TEST_NO_DOF=1 ERUPTION_TEST_NO_CA=1 ERUPTION_TEST_BENCH_NO_HUD=1 ERUPTION_RENDER_SCALE=0.75 \
        ERUPTION_WINDOW_SIZE=1280x720 ERUPTION_TEST_PLAYER_MOVE=1,0 ERUPTION_TEST_PLAYER_MOVE_FRAMES=150 \
        ERUPTION_TEST_RECORD="$OUT/$n",100,20,1 ERUPTION_TEST_EXIT_FRAME=125 "$@" \
        timeout 600 xvfb-run -a -s "-screen 0 1280x720x24" ./eruption-engine --project projects/parana_field --game \
        --map parana_field_lite --pos 2000,122,2000 --cam-yaw -90 --cam-pitch 55 --cam-dist 400 > "$OUT/$n.log" 2>&1
}
for dt in 0.0166667 0.05 0.1; do
    run mb_$dt $dt
    run off_$dt $dt ERUPTION_TEST_NO_MOTION_BLUR=1
done
run old_0.0166667 0.0166667 ERUPTION_TEST_MB_DT_REF=0
"$PY" -I - "$OUT" <<'PY' || FAIL=1
import sys, numpy as np
from PIL import Image
O = sys.argv[1]
def ld(n, k): return np.asarray(Image.open(f"{O}/{n}/f{k:05d}.png").convert("RGB")).astype(int)
def frac(a, b):
    return float(np.median([(np.abs(ld(a, k) - ld(b, k)).max(2) > 8).mean() for k in range(20)]))
base = frac("mb_0.0166667", "off_0.0166667")
ok = True
print(f"  fracao da tela alterada pelo borrao a 60 fps: {100*base:.1f}%")
for dt in ("0.05", "0.1"):
    f = frac(f"mb_{dt}", f"off_{dt}")
    good = f <= 2.0 * base
    ok &= good
    print(f"  dt {dt}: {100*f:.1f}% (limite {200*base:.1f}%) {'ok' if good else 'FANTASMA'}")
# a 60 fps a imagem nao muda contra o comportamento antigo
d = max(float((np.abs(ld("mb_0.0166667", k) - ld("old_0.0166667", k)).max(2) > 0).mean()) for k in range(20))
print(f"  60 fps novo x antigo: pixels diferentes {100*d:.4f}%")
ok &= d == 0.0
sys.exit(0 if ok else 1)
PY
mkdir -p ~/DESCARTE_shaders; mv "$OUT" ~/DESCARTE_shaders/dtramp_$$
[ $FAIL -eq 0 ] && echo "RAMPA DE DT: OK" || echo "RAMPA DE DT: FALHOU"
exit $FAIL
