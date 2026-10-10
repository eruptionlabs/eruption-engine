#pragma once

// Registro de modulos (camada compat). IMPLEMENT_MODULE exporta as funcoes
// C que o host procura com dlsym:
//   EruptionUE_DefinirHost   recebe a tabela do host (log)
//   InitializeModule         cria a instancia do modulo
//   EruptionUE_LiberarModulo destroi a instancia (no alocador do proprio .so)
//   EruptionUE_Funcoes       lista as UFUNCTION estaticas refletidas

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"

class FModuleManager {
public:
    static FModuleManager& Get() {
        static FModuleManager m;
        return m;
    }
    // Modulos de outras bibliotecas nao sao visiveis daqui: so' o proprio.
    IModuleInterface* GetModule(const FName&) { return nullptr; }
    bool IsModuleLoaded(const FName&) const { return false; }
    IModuleInterface* LoadModule(const FName&) { return nullptr; }
    template <typename T>
    static T* GetModulePtr(const FName&) { return nullptr; }
};

#define IMPLEMENT_MODULE(ClasseModulo, NomeModulo)                                                                     \
    extern "C" DLLEXPORT void EruptionUE_DefinirHost(const FEruptionUEHost* Host) { ::EruptionUE::HostAtual() = Host; } \
    extern "C" DLLEXPORT IModuleInterface* InitializeModule() { return new ClasseModulo(); }                          \
    extern "C" DLLEXPORT void EruptionUE_LiberarModulo(IModuleInterface* Modulo) { delete Modulo; }                  \
    extern "C" DLLEXPORT const char* EruptionUE_NomeModulo() { return #NomeModulo; }                                  \
    extern "C" DLLEXPORT const FEruptionUEFuncao* EruptionUE_Funcoes(int* Quantas) {                                  \
        *Quantas = static_cast<int>(::EruptionUE::Funcoes().size());                                                  \
        return ::EruptionUE::Funcoes().data();                                                                         \
    }

#define IMPLEMENT_GAME_MODULE(ClasseModulo, NomeModulo) IMPLEMENT_MODULE(ClasseModulo, NomeModulo)
#define IMPLEMENT_PRIMARY_GAME_MODULE(ClasseModulo, NomeModulo, NomeJogo) IMPLEMENT_MODULE(ClasseModulo, NomeModulo)
