#!/bin/bash
# Ordem do envio de texturas sem descriptor indexing (ERUPTION_NO_BINDLESS=1): nenhuma origem pode ser copiada
# para o array de camadas nem liberada antes de ter sido enviada. Forca o envio adiado do vizinho do mundo
# (ERUPTION_TEST_UPLOAD_DELAY=3 quadros) com a validacao Vulkan ligada e confere: 0 "Validation Error", sem
# queda, e (REQUIRE_GUARD=1) que a guarda [UPLOAD-GUARD] tenha agido. Prova de que o teste enxerga o defeito:
# NOGUARD=1 liga ERUPTION_TEST_NO_UPLOAD_GUARD e o resultado esperado e' FALHA (erros de validacao / SIGSEGV).
# Variaveis: PROJECT (padrao projects/parana_field), MAP (padrao parana_field_lite), PRESET (mobile_medium),
# EXIT_FRAME (500). Mundo com vizinhos: aponte PROJECT e MAP para um projeto que os tenha.
# Precisa de ./eruption-engine, xvfb-run e a camada VK_LAYER_KHRONOS_validation.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"; cd "$ROOT"
PROJECT=${PROJECT:-projects/parana_field}; MAP=${MAP:-parana_field_lite}; PRESET=${PRESET:-mobile_medium}
LOG=$(mktemp "${TMPDIR:-/tmp}/upload_order.XXXXXX.log")
EXTRA=(); [ "${NOGUARD:-0}" = 1 ] && EXTRA=(ERUPTION_TEST_NO_UPLOAD_GUARD=1)
env ERUPTION_AUDIO=null ERUPTION_NO_BINDLESS=1 ERUPTION_TEST_UPLOAD_DELAY=3 ERUPTION_VK_VALIDATION=1 \
    VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation ERUPTION_TEST_GRAPHICS_PRESET="$PRESET" ERUPTION_TEST_FIXED_DT=0.0333333 \
    ERUPTION_WINDOW_SIZE=1280x720 ERUPTION_TEST_EXIT_FRAME=${EXIT_FRAME:-500} "${EXTRA[@]}" \
    timeout 900 xvfb-run -a -s "-screen 0 1280x720x24" ./eruption-engine --project "$PROJECT" --game --map "$MAP" > "$LOG" 2>&1
RC=$?
CLEAN=$(sed 's/\x1b\[[0-9;]*m//g' "$LOG")
ERR=$(grep -ac 'Validation Error' <<<"$CLEAN"); GUARD=$(grep -ac '\[UPLOAD-GUARD\] .* subidas antes da copia' <<<"$CLEAN")
NOB=$(grep -ac 'forced by ERUPTION_NO_BINDLESS' <<<"$CLEAN")
echo "  rc=$RC  erros de validacao=$ERR  guarda agiu=$GUARD vez(es)  modo sem bindless=$NOB"
FAIL=0
[ "$NOB" -ge 1 ] || { echo "  o modo sem bindless nao entrou"; FAIL=1; }
[ "$RC" -eq 0 ] || FAIL=1
[ "$ERR" -eq 0 ] || FAIL=1
[ "${REQUIRE_GUARD:-0}" != 1 ] || [ "$GUARD" -ge 1 ] || { echo "  a guarda nunca agiu (cena sem vizinho?)"; FAIL=1; }
mkdir -p ~/DESCARTE_shaders; mv "$LOG" ~/DESCARTE_shaders/ 2>/dev/null
[ $FAIL -eq 0 ] && echo "ORDEM DO ENVIO: OK" || echo "ORDEM DO ENVIO: FALHOU"
exit $FAIL
