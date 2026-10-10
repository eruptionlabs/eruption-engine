#!/usr/bin/env bash
# Recompila os plugins Unreal de TESTE (baixados por baixar_testes.sh unreal)
# contra os cabecalhos de compat, em compat/build/unreal-tests/.
set -euo pipefail

RAIZ="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
DADOS="$RAIZ/compat/testdata/unreal"
SAIDA="$RAIZ/compat/build/unreal-tests"
FERRAMENTA="$RAIZ/compat/unreal/ferramentas/compilar_plugin.py"

python3 -I "$FERRAMENTA" "$DADOS/AdvancedEasesPlugin/AdvancedEases/AdvancedEases.uplugin" "$SAIDA"
# SerialCOM e' so' Win64 (porta serial via Win32): compila apenas o modulo.
python3 -I "$FERRAMENTA" "$DADOS/SerialCOM/SerialCOM/SERIALCOM.uplugin" "$SAIDA" \
    --forcar-plataforma --so-fontes SERIALCOMModule.cpp
