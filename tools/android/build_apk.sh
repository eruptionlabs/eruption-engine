#!/bin/bash
# Monta o APK do jogo para Android (NativeActivity, sem Gradle; o unico Java e' a
# activity de tela cheia em src/platform/android/java).
#   tools/android/build_apk.sh <mapa>[,mapa2,...] [saida.apk]
#   (varios mapas: o primeiro abre; o painel de toque lista todos)
#   ERUPTION_APK_ENV=extra.txt tools/android/build_apk.sh <mapa>   (variaveis do motor no APK)
#   ERUPTION_APK_JPEG=0   mantem as texturas do mapa como vieram (padrao: opacas em JPEG)
#   ERUPTION_APK_LAYERS=<pasta>  leva as camadas Vulkan da pasta (libVkLayer_*.so do
#     ABI, ex.: binarios Android do Vulkan-ValidationLayers); ligar com
#     ERUPTION_VK_VALIDATION=1 no env do APK
# JOGO EXPORTADO (File > Export Game ou --export-game <pasta>):
#   ERUPTION_APK_GAME=<pasta> tools/android/build_apk.sh <mapa_inicial> [saida.apk]
#   leva a pasta inteira (dados, scripts compilados, sons, mapas) no lugar dos
#   dados do motor; o programa de PC e as texturas BC ficam de fora.
#   ERUPTION_APK_PACKAGE=<id>  ERUPTION_APK_LABEL=<nome>  ERUPTION_APK_ICON=<png>
#   ERUPTION_APK_ICON_FG=<png> ERUPTION_APK_ICON_BG=<png>  icone adaptativo (Android 8+):
#     frente e fundo de 432x432, o rosto no circulo central de 288; o launcher
#     recorta o formato e o icone ocupa a area toda, sem moldura branca
#     (padrao: o app do motor, com.eruptionlabs.engine; outro id instala ao lado)
#   ERUPTION_APK_TOUCH_PAD="Z,X:Voltar,escape:Menu"  controle de toque do jogo
#     (ERUPTION_TOUCH_PAD; vazio = so' o analogico; sem ele, o painel de demonstracao)
#   ERUPTION_APK_TEX_MAX=<px>  reduz as imagens de assets/data/texture maiores que isso
#   ERUPTION_APK_CMAKE_ARGS="-DA=x -DB=y"  flags a mais no configure do build-android
#     (separadas por espaco; com ela o configure roda tambem sobre um cache existente)
# Precisa do SDK/NDK do Android (ANDROID_HOME, padrao ~/Android/Sdk) e de um
# JDK 17 (JAVA_HOME, padrao ~/Android/jdk17). Compila build-android/ se faltar.
set -euo pipefail
MAPS=${1:?uso: build_apk.sh <mapa>[,mapa2,...] [saida.apk]}
MAP=${MAPS%%,*}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${2:-}
SDK=${ANDROID_HOME:-$HOME/Android/Sdk}
JAVA_HOME=${JAVA_HOME:-$HOME/Android/jdk17}
export PATH=$JAVA_HOME/bin:$PATH
NDK=$(ls -d "$SDK"/ndk/* | sort -V | tail -1)
BT=$(ls -d "$SDK"/build-tools/* | sort -V | tail -1)
PLAT=$(ls -d "$SDK"/platforms/android-* | sort -V | tail -1)
# ERUPTION_APK_ABI=x86_64 gera o APK do emulador (padrao: arm64-v8a, celular).
ABI=${ERUPTION_APK_ABI:-arm64-v8a}
# ERUPTION_PYTHON=<python>: interpretador com Pillow para encolher as texturas
# (use o de uma venv; padrao: python3 do PATH).
PY=${ERUPTION_PYTHON:-python3}
B=$ROOT/build-android; [ "$ABI" != "arm64-v8a" ] && B=$ROOT/build-android-$ABI
STAGE=$B/apk
OUT=${OUT:-$B/eruption-$MAP.apk}

read -ra CMAKE_EXTRA <<< "${ERUPTION_APK_CMAKE_ARGS:-}"
if [ ! -f "$B/CMakeCache.txt" ] || [ ${#CMAKE_EXTRA[@]} -gt 0 ]; then
    cmake -S "$ROOT" -B "$B" -G "Unix Makefiles" -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI=$ABI -DANDROID_PLATFORM=android-28 -DCMAKE_BUILD_TYPE=Release \
        -DERUPTION_EDITOR=OFF -DERUPTION_MODULE_NEURAL_PBR=OFF -DERUPTION_ONNX=OFF -DERUPTION_DEBUG=OFF \
        ${CMAKE_EXTRA[@]+"${CMAKE_EXTRA[@]}"}
fi
cmake --build "$B" --target eruption-engine -j"$(nproc)"

rm -rf "$STAGE"; mkdir -p "$STAGE/assets/android" "$STAGE/lib/$ABI"
"$NDK"/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip -o "$STAGE/lib/$ABI/liberuption.so" "$B/liberuption.so"
if [ -n "${ERUPTION_APK_LAYERS:-}" ]; then cp "$ERUPTION_APK_LAYERS"/libVkLayer_*.so "$STAGE/lib/$ABI/"; fi

# Pasta de um mapa: assets/external/<mapa>, ou a de um projeto de jogo
# (projects/*/assets/external/<mapa>). No APK ele fica sempre em
# assets/external/<mapa>, onde o motor procura sem --project.
map_dir() {
    if [ -d "assets/external/$1" ]; then echo "assets/external/$1"; return; fi
    for d in projects/*/assets/external/"$1"; do [ -d "$d" ] && { echo "$d"; return; }; done
}

# Arquivos do jogo: dados, shaders compilados e o mapa pedido, como
# "destino<TAB>origem". Texturas pre-cozidas em BC (.etex/.etexpack) ficam de
# fora: GPU de celular nao le BC.
GAME=${ERUPTION_APK_GAME:-}
cd "$ROOT"
if [ -n "$GAME" ]; then
    # Jogo exportado: tudo da pasta, menos o programa de PC e as texturas BC.
    (cd "$GAME" && find . -type f ! -name eruption-engine ! -name 'run.sh' ! -name '*.so' ! -name '*.so.*' \
        ! -name '*.etex' \( ! -name '*.etexpack' -o -name '*.mobile.etexpack' \) | sed 's#^\./##' |
        awk -v g="$GAME" '{print $0 "\t" g "/" $0}') | grep -v '/\.' | sort > "$STAGE/files.tsv"
else
{
    {
        find data -type f
        find shaders -type f -name '*.spv'
        find assets/data assets/scripts assets/sprites assets/icon -type f 2>/dev/null
        # LOD de celular montado no PC (perfil mobile do sistema de LOD): o motor
        # le' desta pasta se ela existir, sem montar nada no aparelho.
        find assets/lodcache/mobile -type f 2>/dev/null || true
    } | awk '{print $0 "\t" $0}'
    for m in ${MAPS//,/ }; do
        src=$(map_dir "$m")
        [ -n "$src" ] || { echo "mapa nao encontrado: $m" >&2; exit 1; }
        # .glb.probes: grade de visibilidade do ceu cozida no PC (o celular so' le').
        find -L "$src" -maxdepth 1 -type f \( -name '*.glb' -o -name '*.ter' -o -name '*.nav' -o -name '*.env' -o -name '*.glb.probes' -o -name '*.relevo' \) |
            while read -r f; do printf 'assets/external/%s/%s\t%s\n' "$m" "$(basename "$f")" "$f"; done
        # Pacote de texturas assado para o celular (tools/android/bake_mobile_pack.sh):
        # PBR da rede neural, que o celular nao roda. So' vale com o .glb original.
        if [ "${ERUPTION_APK_JPEG:-1}" = "0" ] && [ -f "$src/$m.glb.1024s.mobile.etexpack" ]; then
            printf 'assets/external/%s/%s.glb.1024s.mobile.etexpack\t%s\n' "$m" "$m" "$src/$m.glb.1024s.mobile.etexpack"
        fi
    done
} | grep -v '/\.' | sort > "$STAGE/files.tsv"
fi
cut -f1 "$STAGE/files.tsv" > "$STAGE/assets/android/manifest.txt"
while IFS=$'\t' read -r dest src; do
    mkdir -p "$STAGE/assets/$(dirname "$dest")"
    cp -L "$src" "$STAGE/assets/$dest"
done < "$STAGE/files.tsv"
rm -f "$STAGE/files.tsv"
# O pacote movel vai com o nome que o motor procura no celular.
for f in "$STAGE"/assets/assets/external/*/*.mobile.etexpack; do
    [ -f "$f" ] && mv "$f" "${f%.mobile.etexpack}.etexpack"
done
sed -i 's/\.mobile\.etexpack$/.etexpack/' "$STAGE/assets/android/manifest.txt"
# Texturas opacas do mapa em JPEG (ERUPTION_APK_JPEG=0 desliga): o .glb de
# cidade de 68 MB encolhe o bastante para o APK caber em envio de 30 MB.
if [ "${ERUPTION_APK_JPEG:-1}" != "0" ]; then
    for g in "$STAGE"/assets/assets/external/*/*.glb; do
        [ -f "$g" ] || continue   # mapa so' com .relevo: nenhum .glb no glob
        "$PY" "$ROOT/tools/android/shrink_glb.py" "$g" "$g.tmp"
        mv "$g.tmp" "$g"
    done
fi
# ERUPTION_APK_TEX_MAX: imagem de textura maior que isso encolhe (o lado maior).
if [ -n "${ERUPTION_APK_TEX_MAX:-}" ] && [ -d "$STAGE/assets/assets/data/texture" ]; then
    "$PY" - "$STAGE/assets/assets/data/texture" "$ERUPTION_APK_TEX_MAX" <<'PYTEX'
import os, sys
from PIL import Image
pasta, teto = sys.argv[1], int(sys.argv[2])
for raiz, _, nomes in os.walk(pasta):
    for n in nomes:
        if not n.lower().endswith((".png", ".jpg", ".jpeg")):
            continue
        p = os.path.join(raiz, n)
        im = Image.open(p)
        if max(im.size) <= teto:
            continue
        k = teto / max(im.size)
        im.resize((max(1, round(im.size[0] * k)), max(1, round(im.size[1] * k))), Image.LANCZOS).save(p)
PYTEX
fi
date +%s > "$STAGE/assets/android/stamp.txt"
printf -- '--game\n--map\n%s\n' "$MAP" > "$STAGE/assets/android/args.txt"
# Preset pela GPU (tabela auto_presets do data/graphics.json: mobile_low,
# mobile_medium ou mobile_high); o preset tambem decide a fracao da tela do
# swapchain (display_scale). Painel de toque (analogico, hora, clima,
# telemetria e troca de mapa) com a fonte em 4,5% da altura da tela, e o
# contador de FPS grande no canto (6,5% da altura): e' a build de demonstracao.
# A compressao BC fica ligada: o pacote de PBR assado (.mobile.etexpack) e' BC1/BC3
# e o motor o recusa num conjunto de formatos diferente (ai' a rede roda no
# aparelho, 48 s por abertura no Xclipse 940).
if [ -n "$GAME" ]; then
    # Jogo: sem contador de FPS nem troca de mapa; o controle de toque e' o do jogo.
    printf 'ERUPTION_TEST_GRAPHICS_PRESET=auto\nERUPTION_TOUCH_HUD=0.045\n' > "$STAGE/assets/android/env.txt"
    # Definida, mesmo vazia: controle de toque do jogo (vazia = so' o analogico;
    # os botoes ficam com a interface do proprio jogo, por ui.clicked).
    if [ -n "${ERUPTION_APK_TOUCH_PAD+x}" ]; then echo "ERUPTION_TOUCH_PAD=$ERUPTION_APK_TOUCH_PAD" >> "$STAGE/assets/android/env.txt"; fi
else
cat > "$STAGE/assets/android/env.txt" <<ENV
ERUPTION_TEST_GRAPHICS_PRESET=auto
ERUPTION_TOUCH_HUD=0.045
ERUPTION_FPS_OVERLAY=0.065
ERUPTION_TOUCH_MAPS=$MAPS
ENV
fi
# ERUPTION_APK_ENV=<arquivo>: variaveis extras (VAR=valor por linha) gravadas no APK.
if [ -n "${ERUPTION_APK_ENV:-}" ]; then cat "$ERUPTION_APK_ENV" >> "$STAGE/assets/android/env.txt"; fi

cat > "$STAGE/AndroidManifest.xml" <<XML
<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android"
    package="${ERUPTION_APK_PACKAGE:-com.eruptionlabs.engine}" android:versionCode="1" android:versionName="0.1">
    <uses-sdk android:minSdkVersion="28" android:targetSdkVersion="34"/>
    <uses-feature android:name="android.hardware.vulkan.version" android:version="0x401000" android:required="true"/>
    <application android:label="${ERUPTION_APK_LABEL:-Eruption Engine}" android:icon="@mipmap/ic_launcher" android:hasCode="true" android:extractNativeLibs="true"
        android:appCategory="game" android:isGame="true">
        <!-- Permite medir a CPU pelo adb (simpleperf) sem build de depuracao. -->
        <profileable android:shell="true"/>
        <activity android:name="com.eruptionlabs.engine.EruptionActivity" android:exported="true"
            android:screenOrientation="sensorLandscape" android:configChanges="orientation|screenSize|smallestScreenSize|screenLayout|keyboard|keyboardHidden|navigation|uiMode|density|fontScale|layoutDirection|colorMode|touchscreen"
            android:theme="@android:style/Theme.NoTitleBar.Fullscreen">
            <meta-data android:name="android.app.lib_name" android:value="eruption"/>
            <intent-filter>
                <action android:name="android.intent.action.MAIN"/>
                <category android:name="android.intent.category.LAUNCHER"/>
            </intent-filter>
        </activity>
        <!-- Share sheet (telemetry.export): o unico jeito de outro app ler um
             arquivo do nosso espaco privado e' por content://; a propria
             activity chama EruptionActivity.shareFile, que monta o Intent. -->
        <provider android:name="com.eruptionlabs.engine.EruptionFileProvider"
            android:authorities="${ERUPTION_APK_PACKAGE:-com.eruptionlabs.engine}.fileprovider"
            android:exported="false" android:grantUriPermissions="true"/>
    </application>
</manifest>
XML

# Icone do lancador: o vulcao do motor (ou ERUPTION_APK_ICON), como recurso
# compilado, em todas as densidades: so' o xxxhdpi fazia o launcher encolher
# a imagem grande na hora, borrada em tela de densidade baixa. Icone legado
# de 48 dp; adaptativo de 108 dp (frente e fundo).
"$PY" - "$STAGE/res" "${ERUPTION_APK_ICON:-$ROOT/assets/icon/eruption_v3_256.png}" \
    "${ERUPTION_APK_ICON_FG:-}" "${ERUPTION_APK_ICON_BG:-}" <<'PYICON'
import os, sys
from PIL import Image
res, legado, frente, fundo = sys.argv[1:5]
dens = {"mdpi": 1.0, "hdpi": 1.5, "xhdpi": 2.0, "xxhdpi": 3.0, "xxxhdpi": 4.0}
def grava(origem, dp, nome):
    im = Image.open(origem).convert("RGBA")
    for d, k in dens.items():
        os.makedirs(os.path.join(res, "mipmap-" + d), exist_ok=True)
        lado = round(dp * k)
        im.resize((lado, lado), Image.LANCZOS).save(os.path.join(res, "mipmap-" + d, nome + ".png"))
grava(legado, 48, "ic_launcher")
if frente and fundo:
    grava(frente, 108, "ic_launcher_fg")
    grava(fundo, 108, "ic_launcher_bg")
PYICON
if [ -n "${ERUPTION_APK_ICON_FG:-}" ] && [ -n "${ERUPTION_APK_ICON_BG:-}" ]; then
    mkdir -p "$STAGE/res/mipmap-anydpi-v26"
    cat > "$STAGE/res/mipmap-anydpi-v26/ic_launcher.xml" <<XML
<?xml version="1.0" encoding="utf-8"?>
<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">
    <background android:drawable="@mipmap/ic_launcher_bg"/>
    <foreground android:drawable="@mipmap/ic_launcher_fg"/>
</adaptive-icon>
XML
fi
"$BT/aapt2" compile -o "$STAGE/res.zip" --dir "$STAGE/res"

cd "$STAGE"
"$BT/aapt2" link -o base.apk --manifest AndroidManifest.xml -I "$PLAT/android.jar" -A assets -R res.zip --auto-add-overlay
# Activity de tela cheia (Java minimo): javac + d8 -> classes.dex.
mkdir -p "$STAGE/javac"
javac -source 8 -target 8 -nowarn -classpath "$PLAT/android.jar" -d "$STAGE/javac" \
    "$ROOT"/src/platform/android/java/com/eruptionlabs/engine/*.java 2>&1 | grep -v "warning: \[options\]" || true
"$BT/d8" --min-api 28 --lib "$PLAT/android.jar" --output "$STAGE" $(find "$STAGE/javac" -name '*.class')
zip -qj base.apk "$STAGE/classes.dex"
zip -qr base.apk lib
"$BT/zipalign" -f -p 4 base.apk aligned.apk
KS=$HOME/.android/eruption-debug.keystore
if [ ! -f "$KS" ]; then
    mkdir -p "$(dirname "$KS")"
    keytool -genkeypair -keystore "$KS" -storepass android -keypass android -alias debug \
        -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=Eruption Debug" >/dev/null
fi
"$BT/apksigner" sign --ks "$KS" --ks-pass pass:android --key-pass pass:android --out "$OUT" aligned.apk
rm -f base.apk aligned.apk res.zip
ls -la "$OUT"
