#!/bin/bash
# O data/graphics.json do PROJETO tem de valer tambem para as chaves que o motor
# decide antes de applyGraphicsPreset (lean_ops aqui): um jogo consegue desligar o
# que o motor liga por padrao. Roda o motor headless duas vezes (sem e com o
# override) e confere a linha "[FLUXO] spec 52: lean_ops=" do log.
# Precisa de ./eruption-engine e xvfb-run; sem GPU libre nao ha o que testar.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"; cd "$ROOT"
R=$(mktemp -d "${TMPDIR:-/tmp}/proj_gfx.XXXXXX"); FAIL=0
cp -al projects/parana_field/assets projects/parana_field/project.eruption "$R/" 2>/dev/null || cp -a projects/parana_field/assets projects/parana_field/project.eruption "$R/"
mkdir -p "$R/data"
roda() { # <projeto> -> imprime o valor de lean_ops
    env ERUPTION_AUDIO=null ERUPTION_NO_VALIDATION=1 ERUPTION_TEST_GRAPHICS_PRESET=mobile_medium ERUPTION_TEST_EXIT_FRAME=30 \
        timeout 300 xvfb-run -a ./eruption-engine --project "$1" --game --map parana_field_lite 2>&1 \
        | grep -ao "lean_ops=[01]" | head -1
}
echo '{}' > "$R/data/graphics.json"
sem=$(roda "$R")
echo '{ "every_preset": { "lean_ops": false } }' > "$R/data/graphics.json"
com=$(roda "$R")
echo "  projeto sem override: $sem | com every_preset lean_ops=false: $com"
[ "$sem" = "lean_ops=1" ] && [ "$com" = "lean_ops=0" ] || FAIL=1
mkdir -p ~/DESCARTE_shaders; mv "$R" ~/DESCARTE_shaders/proj_gfx_$$
[ $FAIL -eq 0 ] && echo "GRAPHICS DO PROJETO: OK" || echo "GRAPHICS DO PROJETO: FALHOU"
exit $FAIL
