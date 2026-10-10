#pragma once

// Scripts C# de Unity (MonoBehaviour) sobre o .NET hospedado por hostfxr.
//
// O runtime e' o .NET do usuario (MIT; DOTNET_ROOT ou ERUPTION_DOTNET_ROOT).
// A API UnityEngine e' a NOSSA (compat/unity/csharp/UnityEngine), compilada
// para UnityEngine.dll; os scripts do usuario sao compilados contra ela
// (compatibilidade de fonte, ver compat/tests/unity/compilar_csharp.sh).

#include <cstdint>
#include <string>

namespace eruption::compat::unity::cs {

// Carrega um assembly de scripts; o UnityEngine.dll (+ runtimeconfig) tem de
// estar na mesma pasta. Na primeira chamada sobe o runtime .NET.
bool carregarAssembly(const std::string& caminhoDll);

// GameObject novo (0 = falhou) e componentes por nome de tipo.
uint32_t criarObjeto(const std::string& nome);
bool adicionarComponente(uint32_t objeto, const std::string& tipo);
// Campo ou propriedade (inclusive [SerializeField] privado), valor em texto:
// numero, true/false, nome de enum, "x,y,z" para Vector3.
bool definirCampo(uint32_t objeto, const std::string& tipo, const std::string& campo, const std::string& valor);

void quadro(double dt);
std::string descrever(uint32_t objeto);
void encerrar();
bool ativo();

} // namespace eruption::compat::unity::cs
