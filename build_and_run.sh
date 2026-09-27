#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# MODO. Por padrao compila OTIMIZADO e com o Vulkan Debug DESLIGADO - e' o script
# que o autor usa para JOGAR, e o Vulkan Debug custa caro: medido em
# parana_field/rainy, mesmo binario, so' ligando/desligando o Vulkan Debug, o FPS vai
# de 17,3 para 38,1 e o "render (record)" de 55,5 para 24,0 ms. A camada e' um
# depurador que se instala entre a engine e o driver e cobra em cada chamada
# gravada; nao e' custo do jogo.
#   ./build_and_run.sh            -> RelWithDebInfo, Vulkan Debug OFF (jogar/medir)
#   ./build_and_run.sh --debug    -> Debug -O0 com Vulkan Debug ON (cacar bug de Vulkan)
# Diretorios SEPARADOS de proposito: o build.sh usa "build" com Debug, e se os
# dois compartilhassem o diretorio o CMake ficaria reconfigurando de um tipo
# para o outro a cada troca, forcando recompilacao total toda vez.
MODO_DEBUG=0
if [ "${1:-}" = "--debug" ]; then MODO_DEBUG=1; shift; fi

if [ "$MODO_DEBUG" = "1" ]; then
    BUILD_DIR="build"
    BUILD_TYPE="Debug"
else
    BUILD_DIR="build-rel"
    BUILD_TYPE="RelWithDebInfo"
fi
SHADERS_DIR="shaders"

# Auto-detect a working C++ compiler (can be overridden via CXX/CC env vars)
if [ -z "$CXX" ]; then
    for candidate in g++ g++-13 g++-12 g++-11 clang++; do
        if command -v "$candidate" &> /dev/null; then
            CXX="$candidate"
            break
        fi
    done
fi

if [ -z "$CXX" ]; then
    echo "ERROR: No C++ compiler found. Please install g++ or clang++."
    exit 1
fi

if [ -z "$CC" ]; then
    # Pick a matching C compiler for the chosen C++ compiler when possible.
    case "$CXX" in
        clang++) CC="clang" ;;
        g++|g++-*) CC="${CXX/g++/gcc}" ;;
        *) CC="gcc" ;;
    esac
fi

echo "Using C++ compiler: $CXX"
echo "Using C compiler:   $CC"

echo "Ensuring build directory exists..."
mkdir -p "$BUILD_DIR"

# Only remove engine artifacts and shaders to force recompilation
# without nuking the _deps folder (where git clones live)
echo "Cleaning engine artifacts and SPIR-V shaders..."
rm -f "eruption-engine"
rm -f "$BUILD_DIR/eruption-engine"
find "$SHADERS_DIR" -name "*.spv" -delete
find "$BUILD_DIR/shaders" -name "*.spv" -delete 2>/dev/null || true

cd "$BUILD_DIR"

echo "Configuring CMake (FetchContent will skip clones if already present)..."
cmake .. -DCMAKE_BUILD_TYPE="$BUILD_TYPE" -DCMAKE_CXX_COMPILER="$CXX" -DCMAKE_C_COMPILER="$CC"

echo "Building project..."
make -j$(nproc)

cd ..

echo "Running engine..."
if [ "$MODO_DEBUG" = "1" ]; then
    echo "  modo DEBUG: -O0 + Vulkan Debug ON. Lento de proposito;"
    echo "  os numeros do F3 NAO representam o jogo (o painel avisa em laranja)."
else
    echo "  modo otimizado ($BUILD_TYPE), Vulkan Debug OFF."
fi
./eruption-engine "$@"
