#!/usr/bin/env bash
# Baixa o SDK do .NET (MIT, Microsoft) em compat/testdata/dotnet/ (fora do
# git) para compilar e hospedar os scripts C# de teste da camada Unity.
# Usa o instalador oficial (dotnet-install.sh, MIT); nada vai para o sistema.
#
#   bash compat/scripts/baixar_dotnet.sh [canal]     # canal padrao: 10.0 (LTS)
set -euo pipefail

RAIZ="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DESTINO="$RAIZ/testdata/dotnet"
CANAL="${1:-10.0}"

if [ -x "$DESTINO/dotnet" ]; then
    echo "ja' existe: $DESTINO ($("$DESTINO/dotnet" --version))"
    exit 0
fi
mkdir -p "$DESTINO"
curl -sSfL -o "$DESTINO/dotnet-install.sh" https://dot.net/v1/dotnet-install.sh
bash "$DESTINO/dotnet-install.sh" --channel "$CANAL" --install-dir "$DESTINO" --no-path
echo "SDK instalado: $("$DESTINO/dotnet" --version) em $DESTINO"
