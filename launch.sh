#!/bin/bash
# Simple launcher for Eruption Engine.
# The engine itself detects and selects the best Vulkan GPU on-the-fly.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EXE="$SCRIPT_DIR/eruption-engine"

if [ ! -x "$EXE" ]; then
    echo "ERROR: $EXE not found or not executable. Run ./build.sh first."
    exit 1
fi

exec "$EXE" "$@"
