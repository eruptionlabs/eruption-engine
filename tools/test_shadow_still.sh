#!/bin/bash
# Sombra parada com a camera quase parada: o jogador anda 150 quadros e para, a
# camera desacelera pelo suavizador real, e conta quantos pixels mudam (>8 niveis)
# entre quadros consecutivos com e SEM sombra; o excesso que a sombra causa tem de
# ficar <= 0,2% da tela (medido: <= 0,16% com camera_near_scale; ~3,5% sem ele,
# por causa do passo de ~0,1 u do depth buffer com near 0,1 e far 50000).
# Precisa de ./eruption-engine, do mapa de demonstracao, xvfb-run e python com numpy e Pillow.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"; cd "$ROOT"
PY="${PYTHON:-$HOME/miniconda3/bin/python}"
LIMITE="${LIMITE:-0.2}"
PRESETS="${PRESETS:-high medium low mobile_high mobile_medium mobile_low}"
TMP=$(mktemp -d); FAIL=0
roda() { # preset pasta [VAR=val ...]
    local p=$1 d=$2; shift 2
    env ERUPTION_AUDIO=null ERUPTION_NO_VALIDATION=1 ERUPTION_TEST_GRAPHICS_PRESET=$p ERUPTION_TEST_FIXED_DT=0.0166667 \
        ERUPTION_TEST_TIME=0.40 ERUPTION_TEST_NO_WIND=1 ERUPTION_TEST_NO_CLOUD_SHADOW=1 ERUPTION_TEST_WEATHER_SWITCH=1,clear \
        ERUPTION_TEST_NO_MOTION_BLUR=1 ERUPTION_TEST_NO_DOF=1 ERUPTION_TEST_NO_CA=1 ERUPTION_TEST_NO_AA=1 ERUPTION_TEST_NO_BLOOM=1 \
        ERUPTION_TEST_BENCH_NO_HUD=1 ERUPTION_WINDOW_SIZE=1280x720 ERUPTION_TEST_SHADOW_ANCHOR_LOG=1 ERUPTION_TEST_PLAYER_MOVE=1,0 \
        ERUPTION_TEST_PLAYER_MOVE_FRAMES=150 ERUPTION_TEST_RECORD="$d,170,70,1" ERUPTION_TEST_EXIT_FRAME=245 "$@" \
        timeout 600 xvfb-run -a -s "-screen 0 1280x720x24" ./eruption-engine --project projects/parana_field --game \
        --map parana_field_lite --pos 2000,122,2000 --cam-yaw -90 --cam-pitch 55 --cam-dist 400 > "$d.log" 2>&1
}
for p in $PRESETS; do
    roda "$p" "$TMP/$p"
    roda "$p" "$TMP/${p}_ns" ERUPTION_TEST_NO_SHADOW=1
    "$PY" -I - "$TMP/$p" "$TMP/${p}_ns" "$p" "$LIMITE" <<'PY' || FAIL=1
import sys, re, numpy as np
from PIL import Image
d, dns, p, lim = sys.argv[1], sys.argv[2], sys.argv[3], float(sys.argv[4])
cam = []
for l in open(d + ".log"):
    m = re.search(r'cascades cam=(\S+),(\S+),(\S+)', re.sub(r'\x1b\[[0-9;]*m', '', l))
    if m: cam.append(tuple(map(float, m.groups())))
def serie(dir_):
    prev = None; out = []
    for k in range(70):
        a = np.asarray(Image.open(f"{dir_}/f{k:05d}.png").convert("RGB")).astype(int)
        if prev is not None and len(cam) > 170 + k:
            st = float(np.linalg.norm(np.subtract(cam[170 + k], cam[170 + k - 1])))
            if 1e-4 < st <= 0.1: out.append(int((np.abs(a - prev).max(2) > 8).sum()))
        prev = a
    return out
s, n = serie(d), serie(dns)
if not s or not n: print(f"  {p}: sem quadros com camera quase parada"); sys.exit(1)
exc = (np.median(s) - np.median(n)) / (720 * 1280) * 100
ok = exc <= lim
print(f"  {p:14s} com sombra {int(np.median(s)):6d} sem {int(np.median(n)):6d} excesso {exc:.3f}% da tela (limite {lim}%) {'ok' if ok else 'FALHOU'}")
sys.exit(0 if ok else 1)
PY
done
mkdir -p ~/DESCARTE_shaders; mv "$TMP" ~/DESCARTE_shaders/shadow_still_$$
[ $FAIL -eq 0 ] && echo "SOMBRA PARADA: OK" || echo "SOMBRA PARADA: FALHOU"
exit $FAIL
