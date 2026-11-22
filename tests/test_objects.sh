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

# Caracteres para o teste "texto_ceu": 'a' (97) e e-agudo (233) em quadros
# vizinhos, de 10 em 10 quadros (so' contam com text_begin ligado).
TEXTO=""
for f in $(seq 30 10 390); do TEXTO="${TEXTO}${f},char,97;$((f+1)),char,233;"; done

# Folha v3 com uma animacao extra "golpe" (6 quadros de 40 ms em cada uma
# das 8 direcoes) para o teste "duracao" (characters.play com seconds).
python3 - "$PROJ/assets/sprites/golpe.spr" <<'PY'
import struct, sys
img = bytes([255, 255, 255, 255]) * (8 * 16)
sprite = struct.pack("<Iiiff4fI", 0, 0, 0, 1.0, 1.0, 1, 1, 1, 1, 0)
acoes = [(150, 1)] * 16 + [(40, 6)] * 8
out = b"ERUPTSPR" + struct.pack("<II", 3, 1) + struct.pack("<II", 8, 16) + img + struct.pack("<I", len(acoes))
for delay, quadros in acoes:
    out += struct.pack("<II", delay, quadros) + (struct.pack("<I", 1) + sprite) * quadros
out += struct.pack("<I", 1) + struct.pack("<H", 5) + b"golpe" + struct.pack("<II", 16, 0)
open(sys.argv[1], "wb").write(out)
PY

# Folha v2 com andar de 4 quadros de 100 ms nas 8 direcoes, para o teste
# "passada" (characters.set_stride).
python3 - "$PROJ/assets/sprites/andador.spr" <<'PY'
import struct, sys
img = bytes([255, 255, 255, 255]) * (8 * 16)
sprite = struct.pack("<Iiiff4fI", 0, 0, 0, 1.0, 1.0, 1, 1, 1, 1, 0)
acoes = [(150, 1)] * 8 + [(100, 4)] * 8
out = b"ERUPTSPR" + struct.pack("<II", 3, 1) + struct.pack("<II", 8, 16) + img + struct.pack("<I", len(acoes))
for delay, quadros in acoes:
    out += struct.pack("<II", delay, quadros) + (struct.pack("<I", 1) + sprite) * quadros
out += struct.pack("<I", 0)
open(sys.argv[1], "wb").write(out)
PY

TESTS="api cena comportamento salvar prefab luz personagem robusto faixa terreno_vivo andar_script visao cabeca anel retrato telemetria duracao distancia banda pick texto_ceu cursor desenho passada ui_estado"

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
ERUPTION_TEST_CURSOR_COUNT=1 ERUPTION_TEST_INPUT="$TEXTO" ERUPTION_PROJECT="$PROJ" ERUPTION_SAVE_DIR="$TMP/saves" ERUPTION_TEST_SCREENSHOT_FRAME=240 \
    timeout 300 xvfb-run -a ./eruption-engine --map parana_field_lite --screenshot "$TMP/jogo.png" > "$TMP/jogo.log" 2>&1
rc=$?; [ $rc -eq 0 ] || { echo "  FALHOU: o motor saiu com $rc"; FAIL=1; }
check_log "$TMP/jogo.log" 1
# O teste "cursor" chama set_cursor 2000 vezes: so' 3 GLFWcursor sao criados (1 do PNG + 2 quadros da folha).
n=$(grep -ao 'Cursor: criados=[0-9]*' "$TMP/jogo.log" | tail -1 | cut -d= -f2)
if [ "${n:-0}" -eq 3 ]; then echo "  ok   cursor (3 criacoes em 2000 chamadas)"; else echo "  FALHOU cursor ($n criacoes, esperado 3)"; FAIL=1; fi
# O arquivo do teste "telemetria": o resumo le' sem erro e acha os 50 eventos.
TEL="$TMP/telemetry/teste_objetos.jsonl"
if python3 tools/telemetry_summary.py "$TEL" --json > "$TMP/tel.json" 2>&1 &&
   python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); c=d["channels"]; assert c["teste"]["count"]==50 and c["vazio"]["count"]==1 and c["teste"]["fields"]["valor"]["sum"]==1275 and d["bad_lines"]==0' "$TMP/tel.json"; then
    echo "  ok   telemetria (arquivo)"
else
    echo "  FALHOU telemetria (arquivo)"; head -5 "$TEL" "$TMP/tel.json"; FAIL=1
fi

printf '\n=== editor: Play, Stop, Play de novo, Stop, Ctrl+S ===\n'
SCENE="$PROJ/assets/scenes/parana_field_lite.scene.luau"
grep -v '^--' "$SCENE" > "$TMP/cena_antes.luau"
ERUPTION_TEST_INPUT="$TEXTO" ERUPTION_PROJECT="$PROJ" ERUPTION_SAVE_DIR="$TMP/saves" ERUPTION_TEST_EDITOR_PLAY="150,240;260,350" \
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

printf '\n=== editor: previa dos scripts de editor (editor_scripts) ===\n'
# Projeto proprio: ../extra esta' em "editor_read_paths"; ../secret nao.
PREV="$TMP/prev"; mkdir -p "$PREV/secret"
cp -r tests/objects/editor_preview "$PREV/proj"
mv "$PREV/proj/extra" "$PREV/extra"
echo segredo > "$PREV/secret/x.txt"
ln -s "$ROOT/projects/parana_field/assets/external" "$PREV/proj/assets/external"
mkdir -p "$PREV/proj/assets/sprites" "$PREV/proj/assets/scenes"
cp projects/parana_field/assets/sprites/default.png projects/parana_field/assets/sprites/default.spr "$PREV/proj/assets/sprites/"
ERUPTION_PROJECT="$PREV/proj" ERUPTION_SAVE_DIR="$TMP/saves" ERUPTION_TEST_EDITOR_PLAY="150,240" ERUPTION_TEST_EDITOR_SAVE=300 \
    ERUPTION_TEST_SCREENSHOT_FRAME=330 timeout 300 xvfb-run -a -s '-screen 0 1600x900x24' \
    ./eruption-engine --editor --map parana_field_lite --screenshot "$TMP/previa.png" > "$TMP/previa.log" 2>&1
rc=$?; [ $rc -eq 0 ] || { echo "  FALHOU: o editor saiu com $rc"; FAIL=1; }
PL="$TMP/previa.log"
n=$(grep -ac 'PREV-MAPA parana_field_lite personagens=3 pedra=true arquivos=1' "$PL")
if [ "$n" -ge 2 ]; then echo "  ok   previa criada no editor e refeita no Stop ($n vezes)"; else echo "  FALHOU previa criada ($n de 2)"; FAIL=1; fi
# Ordem: previa, Play (jogo sem previa), Stop (previa de volta).
primeira=$(grep -an 'PREV-MAPA' "$PL" | head -1 | cut -d: -f1); jogo=$(grep -an 'PREV-JOGO personagens=0 pedra=false' "$PL" | head -1 | cut -d: -f1)
ultima=$(grep -an 'PREV-MAPA' "$PL" | tail -1 | cut -d: -f1)
if [ -n "$primeira" ] && [ -n "$jogo" ] && [ "$primeira" -lt "$jogo" ] && [ "$jogo" -lt "$ultima" ]; then
    echo "  ok   previa some no Play e volta no Stop"
else echo "  FALHOU ordem previa/Play/Stop ($primeira $jogo $ultima)"; FAIL=1; fi
grep -aq 'PREV-LIMPA' "$PL" && echo "  ok   on_preview_clear chamado" || { echo "  FALHOU on_preview_clear"; FAIL=1; }
grep -aq 'PREV-JOGO-SEM-IO' "$PL" && echo "  ok   io.read_text nao existe fora do editor" || { echo "  FALHOU io no jogo"; FAIL=1; }
n=$(grep -ac 'PREV-NEGADO' "$PL")
if [ "$n" -ge 8 ]; then echo "  ok   leitura fora da lista negada ($n negacoes)"; else echo "  FALHOU negacoes ($n de 8)"; FAIL=1; fi
if grep -aq 'PREV-FALHOU' "$PL"; then grep -a 'PREV-FALHOU' "$PL"; FAIL=1; fi
if grep -aq 'script paused' "$PL"; then echo "  FALHOU: script parou com erro"; grep -a -B3 'script paused' "$PL" | head -8; FAIL=1; fi
PSCENE="$PREV/proj/assets/scenes/parana_field_lite.scene.luau"
if [ ! -f "$PSCENE" ]; then echo "  FALHOU: o Ctrl+S nao gravou a cena"; FAIL=1
elif grep -q 'prev_' "$PSCENE"; then echo "  FALHOU: a previa entrou na cena salva:"; grep 'prev_' "$PSCENE"; FAIL=1
else echo "  ok   cena salva sem a previa"; fi

printf '\n'
[ $FAIL -eq 0 ] && echo "OBJETOS: TUDO CERTO" || echo "OBJETOS: HOUVE FALHA"
exit $FAIL
