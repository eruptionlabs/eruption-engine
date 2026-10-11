#!/bin/bash
# Assa no PC o pacote de texturas de um mapa para GPU de celular (so' BC1/BC3,
# normal cru) com o PBR da rede neural, e grava ao lado do .glb como
# <mapa>.glb.1024s.mobile.etexpack. build_apk.sh (com ERUPTION_APK_JPEG=0) leva.
#   tools/android/bake_mobile_pack.sh <mapa> [mapa2 ...]
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
for MAP in "$@"; do
    D="assets/external/$MAP"
    PC="$D/$MAP.glb.1024s.etexpack"
    [ -f "$PC" ] && mv "$PC" "$PC.pc"
    timeout 900 xvfb-run -a env ERUPTION_AUDIO=null ERUPTION_TEXTURE_BC=bc1,bc3 ERUPTION_TEST_GRAPHICS_PRESET=high \
        ERUPTION_TEST_EXIT_FRAME=3 ./eruption-engine --game --map "$MAP" > /dev/null 2>&1 || true
    if [ -f "$PC" ]; then mv "$PC" "$D/$MAP.glb.1024s.mobile.etexpack"; fi
    [ -f "$PC.pc" ] && mv "$PC.pc" "$PC"
    ls -la "$D/$MAP.glb.1024s.mobile.etexpack"
done
