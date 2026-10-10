#!/usr/bin/env bash
# Compila o UnityEngine PROPRIO da camada compat e os scripts C# de TESTE
# (Klak, MIT, baixado por baixar_testes.sh unity) contra ele.
# Precisa do SDK do .NET: bash compat/scripts/baixar_dotnet.sh
#
# Saida: compat/build/unity-cs/{UnityEngine.dll, UnityEngine.runtimeconfig.json, Klak.dll}
set -euo pipefail

RAIZ="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
DOTNET="${DOTNET:-$RAIZ/compat/testdata/dotnet/dotnet}"
SAIDA="$RAIZ/compat/build/unity-cs"
KLAK="$RAIZ/compat/testdata/unity/Klak/Assets/Klak"

export DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1 DOTNET_CLI_HOME="$RAIZ/compat/build/dotnet-home"
mkdir -p "$SAIDA"

"$DOTNET" build "$RAIZ/compat/unity/csharp/UnityEngine/UnityEngine.csproj" -c Release -o "$SAIDA" --nologo -v q

if [ -d "$KLAK" ]; then
    PROJ="$SAIDA/klak"
    mkdir -p "$PROJ"
    # Projeto gerado: so' as pastas Runtime de Motion, Math e Extension (vetores).
    cat >"$PROJ/Klak.csproj" <<EOF
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <TargetFramework>net10.0</TargetFramework>
    <AssemblyName>Klak</AssemblyName>
    <EnableDefaultCompileItems>false</EnableDefaultCompileItems>
    <Nullable>disable</Nullable>
    <ImplicitUsings>disable</ImplicitUsings>
    <NoWarn>CS0414;CS0649;CS0169</NoWarn>
  </PropertyGroup>
  <ItemGroup>
    <Compile Include="$KLAK/Motion/Runtime/*.cs" />
    <Compile Include="$KLAK/Math/Runtime/*.cs" />
    <Compile Include="$KLAK/Extension/Runtime/VectorMathExtension.cs" />
    <Reference Include="UnityEngine"><HintPath>$SAIDA/UnityEngine.dll</HintPath><Private>false</Private></Reference>
  </ItemGroup>
</Project>
EOF
    "$DOTNET" build "$PROJ/Klak.csproj" -c Release -o "$SAIDA" --nologo -v q
fi
ls -1 "$SAIDA"/*.dll
