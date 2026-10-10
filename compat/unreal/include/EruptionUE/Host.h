#pragma once

// Fronteira C entre um modulo compilado com estes cabecalhos e o host de
// compatibilidade (compat/unreal/UnrealHost.cpp). O modulo exporta, via
// IMPLEMENT_MODULE, quatro funcoes extern "C" (ver ModuleManager.h); o host
// entrega esta tabela antes de criar o modulo.

#include <cstddef>
#include <vector>

extern "C" {

struct FEruptionUEHost {
    // verbosidade: 1 Fatal, 2 Error, 3 Warning, 4 Display, 5 Log, 6 Verbose, 7 VeryVerbose
    void (*Log)(const char* Categoria, int Verbosidade, const char* MensagemUtf8);
};

// Funcao refletida (UFUNCTION estatica) gerada pela ferramenta de compat:
// recebe os argumentos como texto e escreve o retorno como texto.
typedef bool (*FEruptionUEThunk)(const char* const* Args, int NumArgs, char* Saida, int TamSaida);

struct FEruptionUEFuncao {
    const char* Nome;       // "Classe.Funcao"
    const char* Assinatura; // "float(EAdvancedEases,float,float,float)"
    FEruptionUEThunk Thunk;
};
}

namespace EruptionUE {

inline const FEruptionUEHost*& HostAtual() {
    static const FEruptionUEHost* Host = nullptr;
    return Host;
}

// Registro por biblioteca (cada .so tem o proprio, inicializado na carga).
inline std::vector<FEruptionUEFuncao>& Funcoes() {
    static std::vector<FEruptionUEFuncao> Lista;
    return Lista;
}

struct FRegistroFuncao {
    FRegistroFuncao(const char* Nome, const char* Assinatura, FEruptionUEThunk Thunk) {
        Funcoes().push_back({Nome, Assinatura, Thunk});
    }
};

} // namespace EruptionUE
