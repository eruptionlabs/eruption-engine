#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Auto-detect a working C++ compiler
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

echo "Using C++ compiler: $CXX"

if [ ! -d "build" ]; then
    mkdir build
fi

cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER="$CXX" "$@"
make -j$(nproc)

echo ""
echo "========================================"
echo "Build complete!"
echo "Run: ./eruption-engine"
echo "========================================"

# Este e' um build Debug, entao a engine PEDE as camadas de validacao do
# Vulkan. Se elas nao estiverem instaladas ela roda mesmo assim, sem checagem
# NENHUMA de barreira/layout de imagem, e sem reclamar: a mensagem
# "Validation layers requested but not available" sai em nivel INFO, que esta'
# filtrado da saida. O resultado e' que quem mexe em barreiras trabalha as
# cegas e so' descobre o erro quando corrompe imagem em outra GPU.
if ! (vulkaninfo --summary 2>/dev/null | grep -q VK_LAYER_KHRONOS_validation) \
   && ! ls /usr/share/vulkan/explicit_layer.d/ 2>/dev/null | grep -qi validation; then
    echo ""
    echo "  AVISO: camadas de validacao do Vulkan NAO encontradas."
    echo "  Este e' um build Debug e vai rodar SEM checagem de barreira/layout,"
    echo "  silenciosamente. Instale com:"
    echo "      sudo apt install vulkan-validationlayers    # Debian/Ubuntu"
    echo "      sudo dnf install vulkan-validation-layers   # Fedora"
    echo ""
fi
