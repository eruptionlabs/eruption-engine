#!/bin/bash
# Monta o APK do jogo para Android (NativeActivity, sem Java nem Gradle).
#   tools/android/build_apk.sh <mapa>[,mapa2,...] [saida.apk]
#   (varios mapas: o primeiro abre; o painel de toque lista todos)
#   ERUPTION_APK_ENV=extra.txt tools/android/build_apk.sh <mapa>   (variaveis do motor no APK)
#   ERUPTION_APK_JPEG=0   mantem as texturas do mapa como vieram (padrao: opacas em JPEG)
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
B=$ROOT/build-android; [ "$ABI" != "arm64-v8a" ] && B=$ROOT/build-android-$ABI
STAGE=$B/apk
OUT=${OUT:-$B/eruption-$MAP.apk}

if [ ! -f "$B/CMakeCache.txt" ]; then
    cmake -S "$ROOT" -B "$B" -G "Unix Makefiles" -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI=$ABI -DANDROID_PLATFORM=android-28 -DCMAKE_BUILD_TYPE=Release \
        -DERUPTION_EDITOR=OFF -DERUPTION_MODULE_NEURAL_PBR=OFF -DERUPTION_ONNX=OFF -DERUPTION_DEBUG=OFF
fi
cmake --build "$B" --target eruption-engine -j"$(nproc)"

rm -rf "$STAGE"; mkdir -p "$STAGE/assets/android" "$STAGE/lib/$ABI"
"$NDK"/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip -o "$STAGE/lib/$ABI/liberuption.so" "$B/liberuption.so"

# Arquivos do jogo: dados, shaders compilados e o mapa pedido. Texturas
# pre-cozidas em BC (.etex/.etexpack) ficam de fora: GPU de celular nao le BC.
cd "$ROOT"
{
    find data -type f
    find shaders -type f -name '*.spv'
    find assets/data assets/scripts assets/sprites assets/icon -type f 2>/dev/null
    for m in ${MAPS//,/ }; do
        find -L "assets/external/$m" -maxdepth 1 -type f \( -name '*.glb' -o -name '*.ter' -o -name '*.nav' -o -name '*.env' \)
    done
} | grep -v '/\.' | sort > "$STAGE/assets/android/manifest.txt"
while read -r f; do mkdir -p "$STAGE/assets/$(dirname "$f")"; cp -L "$f" "$STAGE/assets/$f"; done < "$STAGE/assets/android/manifest.txt"
# Texturas opacas do mapa em JPEG (ERUPTION_APK_JPEG=0 desliga): o .glb de
# cidade de 68 MB encolhe o bastante para o APK caber em envio de 30 MB.
if [ "${ERUPTION_APK_JPEG:-1}" != "0" ]; then
    for g in "$STAGE"/assets/assets/external/*/*.glb; do
        python3 "$ROOT/tools/android/shrink_glb.py" "$g" "$g.tmp"
        mv "$g.tmp" "$g"
    done
fi
date +%s > "$STAGE/assets/android/stamp.txt"
printf -- '--game\n--map\n%s\n' "$MAP" > "$STAGE/assets/android/args.txt"
cat > "$STAGE/assets/android/env.txt" <<ENV
ERUPTION_TEST_GRAPHICS_PRESET=low
ERUPTION_TOUCH_MAPS=$MAPS
ENV
# ERUPTION_APK_ENV=<arquivo>: variaveis extras (VAR=valor por linha) gravadas no APK.
if [ -n "${ERUPTION_APK_ENV:-}" ]; then cat "$ERUPTION_APK_ENV" >> "$STAGE/assets/android/env.txt"; fi

cat > "$STAGE/AndroidManifest.xml" <<XML
<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android"
    package="com.eruptionlabs.engine" android:versionCode="1" android:versionName="0.1">
    <uses-sdk android:minSdkVersion="28" android:targetSdkVersion="34"/>
    <uses-feature android:name="android.hardware.vulkan.version" android:version="0x401000" android:required="true"/>
    <application android:label="Eruption Engine" android:icon="@mipmap/ic_launcher" android:hasCode="false" android:extractNativeLibs="true"
        android:appCategory="game" android:isGame="true">
        <activity android:name="android.app.NativeActivity" android:exported="true"
            android:screenOrientation="sensorLandscape" android:configChanges="orientation|screenSize|smallestScreenSize|screenLayout|keyboard|keyboardHidden|navigation|uiMode|density|fontScale|layoutDirection|colorMode|touchscreen"
            android:theme="@android:style/Theme.NoTitleBar.Fullscreen">
            <meta-data android:name="android.app.lib_name" android:value="eruption"/>
            <intent-filter>
                <action android:name="android.intent.action.MAIN"/>
                <category android:name="android.intent.category.LAUNCHER"/>
            </intent-filter>
        </activity>
    </application>
</manifest>
XML

# Icone do lancador: o vulcao do motor, como recurso compilado.
mkdir -p "$STAGE/res/mipmap-xxxhdpi"
cp "$ROOT/assets/icon/eruption_v3_256.png" "$STAGE/res/mipmap-xxxhdpi/ic_launcher.png"
"$BT/aapt2" compile -o "$STAGE/res.zip" --dir "$STAGE/res"

cd "$STAGE"
"$BT/aapt2" link -o base.apk --manifest AndroidManifest.xml -I "$PLAT/android.jar" -A assets -R res.zip --auto-add-overlay
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
