#!/usr/bin/env bash
# Baixa os plugins de TESTE da camada de compatibilidade em compat/testdata/
# (fora do git). So' projetos de codigo aberto com licenca permissiva; cada
# um esta' listado, com licenca e commit, em compat/TESTED_PLUGINS.md.
# O motor nao distribui nenhum deles.
#
#   bash compat/scripts/baixar_testes.sh            # tudo
#   bash compat/scripts/baixar_testes.sh godot      # so' um grupo (godot|unity|unreal)
set -euo pipefail

RAIZ="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DADOS="$RAIZ/testdata"
GRUPO="${1:-tudo}"

# clona <url> <destino> <commit>: copia rasa fixada no commit exato.
clona() {
    local url="$1" destino="$2" commit="$3"
    if [ -d "$destino/.git" ]; then
        echo "ja' existe: $destino"
        return
    fi
    mkdir -p "$destino"
    git -C "$destino" init -q
    git -C "$destino" remote add origin "$url"
    git -C "$destino" fetch -q --depth 1 origin "$commit"
    git -C "$destino" checkout -q FETCH_HEAD
    echo "baixado: $destino @ $commit"
}

if [ "$GRUPO" = tudo ] || [ "$GRUPO" = godot ]; then
    G="$DADOS/godot"
    clona https://github.com/godotengine/godot-cpp.git          "$G/godot-cpp"           e83fd0904c13356ed1d4c3d09f8bb9132bdc6b77
    clona https://github.com/godotengine/godot-cpp-template.git "$G/godot-cpp-template"  d36f9d03e9e02dad22829f1faf4e03a347616df5
    clona https://github.com/paddy-exe/GDExtensionSummator.git  "$G/GDExtensionSummator" 4f687536b5cab10c6d9bb8937566c39a04b1f915
    clona https://github.com/DanielSnd/YParticles3D.git         "$G/YParticles3D"        22ee8109068d93591dc01dd7eb5d44ead8d136d0
    # GDExample: o codigo vem do tutorial oficial de GDExtension em C++ da
    # documentacao (CC-BY 3.0); o texto e' baixado e so' os blocos de codigo
    # sao extraidos para arquivos.
    if [ ! -f "$G/gdexample/src/register_types.cpp" ]; then
        mkdir -p "$G/gdexample"
        DOCS_COMMIT=33e08e14c517541fac3d9069e82fb3ee2fc99a10
        curl -sfL -o "$G/gdexample/gdextension_cpp_example.rst" \
            "https://raw.githubusercontent.com/godotengine/godot-docs/$DOCS_COMMIT/tutorials/scripting/cpp/gdextension_cpp_example.rst"
        python3 -I "$RAIZ/scripts/extrai_gdexample.py" "$G/gdexample/gdextension_cpp_example.rst" "$G/gdexample/src"
    fi
fi

if [ "$GRUPO" = tudo ] || [ "$GRUPO" = unity ]; then
    U="$DADOS/unity"
    # Exemplo oficial de plugin nativo de renderizacao (MIT). Traz os proprios
    # cabecalhos da API de plugin; o host de compat NAO usa esses cabecalhos.
    clona https://github.com/Unity-Technologies/NativeRenderingPlugin.git "$U/NativeRenderingPlugin" 522254181faf188efa8b50c3e3bf6fce720b26e4
    # Scripts C# de movimento/ruido (MIT): MonoBehaviour com Transform, Time, Random, Mathf.
    clona https://github.com/keijiro/Klak.git "$U/Klak" b38ec4dbc0c614fd6fba08c44ddef80783855c02
fi

if [ "$GRUPO" = tudo ] || [ "$GRUPO" = unreal ]; then
    R="$DADOS/unreal"
    # Biblioteca de funcoes estaticas (UBlueprintFunctionLibrary) de easing (MIT).
    clona https://github.com/DKesserich/AdvancedEasesPlugin.git "$R/AdvancedEasesPlugin" 4fe1c6d00d0eb3b2b9a4dfdd65075f80d7348855
    # Porta serial (MIT); so' Win64, entao so' o arquivo do modulo e' usado no teste.
    clona https://github.com/videofeedback/Unreal_Engine_SerialCOM_Plugin.git "$R/SerialCOM" bd3b2ee44cec7016fe64975c2d03ffd7fda20a2d
fi
