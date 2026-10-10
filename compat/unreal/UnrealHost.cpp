#include "unreal/UnrealHost.hpp"

#include "host/Host.hpp"
#include "unreal/include/EruptionUE/Host.h"
#include "unreal/include/Modules/ModuleInterface.h"

#include <algorithm>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace eruption::compat::unreal {

namespace fs = std::filesystem;

namespace {

struct Modulo {
    std::string nome;
    std::string fase;
    void* handle = nullptr;
    IModuleInterface* instancia = nullptr;
    void (*liberar)(IModuleInterface*) = nullptr;
};

std::vector<Modulo>& modulos() {
    static std::vector<Modulo> m;
    return m;
}

struct FuncaoCarregada {
    std::string nome;
    std::string assinatura;
    FEruptionUEThunk thunk;
};

std::vector<FuncaoCarregada>& registradas() {
    static std::vector<FuncaoCarregada> f;
    return f;
}

void hostLog(const char* categoria, int verbosidade, const char* msg) {
    // 1 Fatal, 2 Error, 3 Warning, 4 Display, 5 Log...
    const Nivel n = verbosidade <= 2 ? Nivel::Erro : verbosidade == 3 ? Nivel::Aviso : Nivel::Info;
    logf(n, "unreal", "%s: %s", categoria, msg);
}

const FEruptionUEHost kHost{&hostLog};

// Valor de texto de "chave": "valor" dentro de um objeto JSON simples.
std::string campo(const std::string& obj, const std::string& chave) {
    const std::string alvo = "\"" + chave + "\"";
    size_t p = obj.find(alvo);
    if (p == std::string::npos) return {};
    p = obj.find(':', p + alvo.size());
    if (p == std::string::npos) return {};
    p = obj.find('"', p);
    if (p == std::string::npos) return {};
    const size_t f = obj.find('"', p + 1);
    return f == std::string::npos ? std::string() : obj.substr(p + 1, f - p - 1);
}

// Objetos { ... } do array "Modules" do .uplugin (descritor JSON).
std::vector<std::string> objetosModulos(const std::string& json) {
    std::vector<std::string> r;
    size_t p = json.find("\"Modules\"");
    if (p == std::string::npos) return r;
    p = json.find('[', p);
    if (p == std::string::npos) return r;
    int nivel = 0;
    size_t ini = 0;
    for (size_t i = p + 1; i < json.size(); ++i) {
        const char c = json[i];
        if (c == '{') {
            if (nivel++ == 0) ini = i;
        } else if (c == '}') {
            if (--nivel == 0) r.push_back(json.substr(ini, i - ini + 1));
        } else if (c == ']' && nivel == 0) {
            break;
        }
    }
    return r;
}

int ordemFase(const std::string& fase) {
    static const char* const fases[] = {"EarliestPossible", "PostConfigInit", "PostSplashScreen", "PreEarlyLoadingScreen",
                                        "PreLoadingScreen", "PreDefault",     "Default",          "PostDefault",
                                        "PostEngineInit"};
    if (fase.empty()) return 6; // Default
    for (int i = 0; i < 9; ++i)
        if (fase == fases[i]) return i;
    return 100; // "None": carregado so' sob demanda
}

} // namespace

bool carregarPlugin(const std::string& uplugin) {
    std::ifstream in(uplugin);
    if (!in) {
        logf(Nivel::Erro, "unreal", "nao abriu %s", uplugin.c_str());
        return false;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    const fs::path pasta = fs::absolute(uplugin).parent_path();
    std::vector<Modulo> lista;
    for (const std::string& obj : objetosModulos(ss.str())) {
        Modulo m;
        m.nome = campo(obj, "Name");
        m.fase = campo(obj, "LoadingPhase");
        if (!m.nome.empty()) lista.push_back(m);
    }
    std::stable_sort(lista.begin(), lista.end(), [](const Modulo& a, const Modulo& b) { return ordemFase(a.fase) < ordemFase(b.fase); });
    int carregados = 0;
    for (Modulo& m : lista) {
        if (ordemFase(m.fase) >= 100) {
            logf(Nivel::Info, "unreal", "modulo %s: LoadingPhase None, fica para carga manual", m.nome.c_str());
            continue;
        }
        const fs::path so = pasta / "Binaries" / "EruptionCompat" / ("lib" + m.nome + ".so");
        m.handle = dlopen(so.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!m.handle) {
            logf(Nivel::Erro, "unreal", "modulo %s: %s", m.nome.c_str(), dlerror());
            continue;
        }
        auto definirHost = reinterpret_cast<void (*)(const FEruptionUEHost*)>(dlsym(m.handle, "EruptionUE_DefinirHost"));
        auto inicializar = reinterpret_cast<IModuleInterface* (*)()>(dlsym(m.handle, "InitializeModule"));
        m.liberar = reinterpret_cast<void (*)(IModuleInterface*)>(dlsym(m.handle, "EruptionUE_LiberarModulo"));
        auto listar = reinterpret_cast<const FEruptionUEFuncao* (*)(int*)>(dlsym(m.handle, "EruptionUE_Funcoes"));
        if (!definirHost || !inicializar) {
            logf(Nivel::Erro, "unreal", "modulo %s: sem IMPLEMENT_MODULE (compilado fora da ferramenta de compat?)", m.nome.c_str());
            continue;
        }
        definirHost(&kHost);
        m.instancia = inicializar();
        if (!m.instancia) continue;
        m.instancia->StartupModule();
        logf(Nivel::Info, "unreal", "modulo %s: StartupModule (fase %s)", m.nome.c_str(), m.fase.empty() ? "Default" : m.fase.c_str());
        if (listar) {
            int n = 0;
            const FEruptionUEFuncao* f = listar(&n);
            for (int i = 0; i < n; ++i) {
                registradas().push_back({f[i].Nome, f[i].Assinatura, f[i].Thunk});
                logf(Nivel::Info, "unreal", "  funcao refletida: %s %s", f[i].Nome, f[i].Assinatura);
            }
        }
        modulos().push_back(m);
        ++carregados;
    }
    return carregados > 0;
}

std::vector<std::string> funcoes() {
    std::vector<std::string> r;
    for (const auto& f : registradas()) r.push_back(f.nome + " " + f.assinatura);
    return r;
}

bool chamarFuncao(const std::string& nome, const std::vector<std::string>& args, std::string* resultado) {
    for (const auto& f : registradas()) {
        if (f.nome != nome) continue;
        std::vector<const char*> ptrs;
        for (const auto& a : args) ptrs.push_back(a.c_str());
        char saida[512] = {};
        if (!f.thunk(ptrs.data(), static_cast<int>(ptrs.size()), saida, sizeof(saida))) {
            logf(Nivel::Erro, "unreal", "%s: argumentos invalidos para %s", nome.c_str(), f.assinatura.c_str());
            return false;
        }
        if (resultado) *resultado = saida;
        return true;
    }
    logf(Nivel::Erro, "unreal", "funcao refletida nao encontrada: %s", nome.c_str());
    return false;
}

void encerrar() {
    for (auto it = modulos().rbegin(); it != modulos().rend(); ++it) {
        if (!it->instancia) continue;
        it->instancia->ShutdownModule();
        logf(Nivel::Info, "unreal", "modulo %s: ShutdownModule", it->nome.c_str());
        if (it->liberar) it->liberar(it->instancia);
        it->instancia = nullptr;
    }
    modulos().clear();
    registradas().clear();
}

bool algumCarregado() {
    return !modulos().empty();
}

} // namespace eruption::compat::unreal
