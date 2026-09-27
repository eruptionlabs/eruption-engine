#!/usr/bin/env bash
# Downloads the parana_field demo map (.glb) from the GitHub release and
# extracts it into assets/external/parana_field/. The CMake configure does the
# same thing automatically when the file is missing; this is the manual path.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
URL="${ERUPTION_DEMO_MAP_URL:-https://github.com/eruptionlabs/eruption-engine/releases/download/demo-map-v1/parana_field.zip}"
SHA256="40396b6ac5f3d97fb6ad636d8d6d6284a7f8c750f5a54c74ddd9695533a214a3"
GLB="$ROOT/assets/external/parana_field/parana_field.glb"

if [ -f "$GLB" ] && [ "${1:-}" != "--force" ]; then
    echo "Demo map already present: $GLB (use --force to download again)"
    exit 0
fi

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
echo "Downloading $URL"
curl -fL --progress-bar -o "$TMP/parana_field.zip" "$URL"
echo "$SHA256  $TMP/parana_field.zip" | sha256sum -c --quiet
python3 -c "import zipfile,sys; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])" "$TMP/parana_field.zip" "$ROOT"
echo "Demo map installed: $GLB"
