#!/bin/bash
# Teste da camada de objetos (src/scene/SceneObjects, API objects do Luau) com
# o motor de verdade, sem janela (xvfb-run). Duas partes:
#   1. jogo: os scripts de tests/objects conferem a API e cada um escreve
#      "OBJ-OK <nome>" ou "OBJ-FALHOU <nome>: ...";
#   2. editor: Play, Stop e Ctrl+S - o arquivo de cena tem que sair igual
#      (o que o jogo mudou nos objetos volta no Stop e não vai para a cena).
# Precisa de ./eruption-engine compilado e do mapa de demonstração
# (tools/fetch_demo_map.sh). Uns 2 minutos.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"; cd "$ROOT"
MAP="projects/parana_field/assets/external/parana_field_lite/parana_field_lite.glb"
[ -x ./eruption-engine ] || { echo "ERRO: compile o motor primeiro."; exit 1; }
[ -f "$MAP" ] || { echo "ERRO: falta o mapa de demonstração (tools/fetch_demo_map.sh)."; exit 1; }

TMP=$(mktemp -d); PROJ="$TMP/proj"
# O motor regrava as nuvens ao sair: a bancada não pode deixar o arquivo mexido.
CLOUDS="data/cloud_layer_config.json"
HAD_CLOUDS=0; [ -f "$CLOUDS" ] && { cp "$CLOUDS" "$TMP/clouds.json"; HAD_CLOUDS=1; }
restore() {
    if [ $HAD_CLOUDS -eq 1 ]; then cp "$TMP/clouds.json" "$CLOUDS"; else rm -f "$CLOUDS"; fi
    rm -rf "$TMP"
}
trap restore EXIT

cp -r tests/objects "$PROJ"
ln -s "$ROOT/projects/parana_field/assets/external" "$PROJ/assets/external"
mkdir -p "$PROJ/assets/sprites"
cp projects/parana_field/assets/sprites/default.png projects/parana_field/assets/sprites/default.spr "$PROJ/assets/sprites/"
# Jogo salvo torto de propósito (tipos errados): o teste "robusto" o carrega.
mkdir -p "$TMP/saves"
cat > "$TMP/saves/quebrado.json" <<'JSON'
{"version": 1, "map": "parana_field_lite", "vars": {"x": 1},
 "maps": {"parana_field_lite": {"models": [{"i": "x", "visible": 1}], "characters": [7],
  "objects": {"made": [5, {"name": 3, "local": "x"}], "marked": [{"path": 1}]}}}}
JSON
FAIL=0

TESTS="api cena comportamento salvar prefab luz personagem robusto faixa"

# Confere um log: cada teste com OBJ-OK pelo menos $2 vezes, nenhum OBJ-FALHOU,
# nenhum script parado por erro.
check_log() {
    local log="$1" want="$2" t n
    if grep -aq 'OBJ-FALHOU' "$log"; then
        grep -a 'OBJ-FALHOU' "$log" | sed 's/\x1b\[[0-9;]*m//g'
        FAIL=1
    fi
    for t in $TESTS; do
        n=$(grep -ac "OBJ-OK $t\b" "$log")
        if [ "$n" -ge "$want" ]; then echo "  ok   $t"; else echo "  FALHOU $t ($n de $want)"; FAIL=1; fi
    done
    if grep -aq 'script paused' "$log"; then
        echo "  FALHOU: script parou com erro"; grep -a -B3 'script paused' "$log" | head -8; FAIL=1
    fi
}

printf '\n=== jogo ===\n'
ERUPTION_PROJECT="$PROJ" ERUPTION_SAVE_DIR="$TMP/saves" ERUPTION_TEST_SCREENSHOT_FRAME=240 \
    timeout 300 xvfb-run -a ./eruption-engine --map parana_field_lite --screenshot "$TMP/jogo.png" > "$TMP/jogo.log" 2>&1
rc=$?; [ $rc -eq 0 ] || { echo "  FALHOU: o motor saiu com $rc"; FAIL=1; }
check_log "$TMP/jogo.log" 1

printf '\n=== editor: Play, Stop, Play de novo, Stop, Ctrl+S ===\n'
SCENE="$PROJ/assets/scenes/parana_field_lite.scene.luau"
grep -v '^--' "$SCENE" > "$TMP/cena_antes.luau"
ERUPTION_PROJECT="$PROJ" ERUPTION_SAVE_DIR="$TMP/saves" ERUPTION_TEST_EDITOR_PLAY="150,240;260,350" \
    ERUPTION_TEST_EDITOR_SAVE=380 ERUPTION_TEST_SCREENSHOT_FRAME=400 timeout 300 xvfb-run -a -s '-screen 0 1600x900x24' \
    ./eruption-engine --editor --map parana_field_lite --screenshot "$TMP/editor.png" > "$TMP/editor.log" 2>&1
rc=$?; [ $rc -eq 0 ] || { echo "  FALHOU: o editor saiu com $rc"; FAIL=1; }
# Os dois Play rodam todos os testes: o segundo só passa se o Stop devolveu tudo.
check_log "$TMP/editor.log" 2
# O Ctrl+S escreve o cabeçalho do editor no arquivo (o original não tem).
grep -q '^-- Edits of the map' "$SCENE" || { echo "  FALHOU: o Ctrl+S não aconteceu"; FAIL=1; }
if diff <(grep -v '^--' "$SCENE") "$TMP/cena_antes.luau" > "$TMP/cena.diff"; then
    echo "  ok   cena igual depois do Stop"
else
    echo "  FALHOU: o Stop deixou a cena mexida:"; cat "$TMP/cena.diff"; FAIL=1
fi

printf '\n'
[ $FAIL -eq 0 ] && echo "OBJETOS: TUDO CERTO" || echo "OBJETOS: HOUVE FALHA"
exit $FAIL
