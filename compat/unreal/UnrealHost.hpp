#pragma once

// Host de modulos de plugin Unreal recompilados contra os cabecalhos de
// compatibilidade (compat/unreal/include) pela ferramenta
// compat/unreal/ferramentas/compilar_plugin.py. Le o .uplugin, carrega cada
// modulo na ordem de LoadingPhase, chama StartupModule/ShutdownModule e
// expoe as UFUNCTION estaticas refletidas para chamada por nome.

#include <string>
#include <vector>

namespace eruption::compat::unreal {

// Caminho do .uplugin; as bibliotecas ficam em Binaries/EruptionCompat/.
bool carregarPlugin(const std::string& uplugin);

// "Classe.Funcao" -> assinatura, das bibliotecas carregadas.
std::vector<std::string> funcoes();

// Chama uma UFUNCTION estatica com argumentos em texto (enum aceita o nome).
bool chamarFuncao(const std::string& nome, const std::vector<std::string>& args, std::string* resultado);

void encerrar();
bool algumCarregado();

} // namespace eruption::compat::unreal
