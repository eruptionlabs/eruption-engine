#include "unity/CSharpHost.hpp"

#include "host/Host.hpp"

#include <cstdlib>
#include <dlfcn.h>
#include <filesystem>
#include <vector>

namespace eruption::compat::unity::cs {

namespace fs = std::filesystem;

namespace {

// ---- API de hospedagem do .NET (hostfxr) ----------------------------------------
// Declaracoes proprias segundo a documentacao publica "Write a custom .NET
// host" (Microsoft Learn). No Linux, char_t e' char.

struct ParametrosHostfxr {
    size_t tamanho;
    const char* caminhoHost;
    const char* raizDotnet;
};

using FnInicializar = int (*)(const char* runtimeConfig, const ParametrosHostfxr* parametros, void** contexto);
using FnObterDelegate = int (*)(void* contexto, int tipo, void** delegate);
using FnFechar = int (*)(void* contexto);
using FnCarregarEObter = int (*)(const char* assembly, const char* tipo, const char* metodo, const char* tipoDelegate,
                                 void* reservado, void** funcao);

constexpr int kDelegateCarregarAssemblyEObterPonteiro = 5; // hdt_load_assembly_and_get_function_pointer
const char* const kMetodoUnmanagedCallersOnly = reinterpret_cast<const char*>(static_cast<intptr_t>(-1));

// ---- Tabela que a ponte C# usa para falar com o host -----------------------------

struct TabelaHost {
    void (*log)(int nivel, const char* utf8);
    uint32_t (*criarNo)(const char* nome, uint32_t pai);
    void (*definirTransformacao)(uint32_t no, const float* v);
    void (*destruirNo)(uint32_t no);
};

void cbLog(int nivel, const char* s) {
    host().log(nivel == 0 ? Nivel::Info : nivel == 1 ? Nivel::Aviso : Nivel::Erro, "unity-cs", s ? s : "");
}
uint32_t cbCriarNo(const char* nome, uint32_t pai) {
    return host().criarNo(nome ? nome : "", pai);
}
void cbDefinirTransformacao(uint32_t no, const float* v) {
    Transformacao t;
    t.posicao = {v[0], v[1], v[2]};
    t.rotacao = {v[3], v[4], v[5], v[6]};
    t.escala = {v[7], v[8], v[9]};
    host().definirTransformacao(no, t);
}
void cbDestruirNo(uint32_t no) {
    host().destruirNo(no);
}

TabelaHost g_tabela{&cbLog, &cbCriarNo, &cbDefinirTransformacao, &cbDestruirNo};

struct Ponte {
    int (*iniciar)(TabelaHost*) = nullptr;
    int (*carregarAssembly)(const char*) = nullptr;
    uint32_t (*criarObjeto)(const char*) = nullptr;
    int (*adicionarComponente)(uint32_t, const char*) = nullptr;
    int (*definirCampo)(uint32_t, const char*, const char*, const char*) = nullptr;
    void (*quadro)(float) = nullptr;
    int (*descrever)(uint32_t, char*, int) = nullptr;
    void (*encerrar)() = nullptr;
};

Ponte g_ponte;
bool g_ativo = false;

std::string raizDotnet() {
    for (const char* var : {"ERUPTION_DOTNET_ROOT", "DOTNET_ROOT"})
        if (const char* v = std::getenv(var); v && *v && fs::exists(fs::path(v) / "host" / "fxr")) return fs::absolute(v).string();
    std::vector<fs::path> candidatos = {"/usr/share/dotnet", "/usr/lib/dotnet", "/usr/local/share/dotnet"};
    if (const char* h = std::getenv("HOME")) candidatos.push_back(fs::path(h) / ".dotnet");
    for (const fs::path& c : candidatos)
        if (fs::exists(c / "host" / "fxr")) return c.string();
    return {};
}

// Maior versao em host/fxr/ (comparando os numeros, nao o texto).
fs::path caminhoHostfxr(const std::string& raiz) {
    fs::path melhor;
    std::vector<int> melhorV;
    for (const auto& e : fs::directory_iterator(fs::path(raiz) / "host" / "fxr")) {
        std::vector<int> v;
        std::string nome = e.path().filename().string(), parte;
        for (char c : nome + ".") {
            if (c == '.' || c == '-') {
                v.push_back(std::atoi(parte.c_str()));
                parte.clear();
            } else {
                parte.push_back(c);
            }
        }
        if (fs::exists(e.path() / "libhostfxr.so") && v > melhorV) {
            melhorV = v;
            melhor = e.path() / "libhostfxr.so";
        }
    }
    return melhor;
}

template <typename F>
bool obter(FnCarregarEObter carregar, const std::string& dll, const char* metodo, F& destino) {
    void* fn = nullptr;
    const int rc = carregar(dll.c_str(), "Eruption.Compat.Ponte, UnityEngine", metodo, kMetodoUnmanagedCallersOnly, nullptr, &fn);
    if (rc != 0 || !fn) {
        logf(Nivel::Erro, "unity-cs", "ponte: metodo %s indisponivel (0x%x)", metodo, static_cast<unsigned>(rc));
        return false;
    }
    destino = reinterpret_cast<F>(fn);
    return true;
}

bool subirRuntime(const fs::path& pasta) {
    const std::string raiz = raizDotnet();
    if (raiz.empty()) {
        logf(Nivel::Erro, "unity-cs", "runtime .NET nao encontrado (defina ERUPTION_DOTNET_ROOT ou DOTNET_ROOT)");
        return false;
    }
    const fs::path fxr = caminhoHostfxr(raiz);
    void* h = fxr.empty() ? nullptr : dlopen(fxr.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        logf(Nivel::Erro, "unity-cs", "hostfxr nao carregou em %s", raiz.c_str());
        return false;
    }
    auto inicializar = reinterpret_cast<FnInicializar>(dlsym(h, "hostfxr_initialize_for_runtime_config"));
    auto obterDelegate = reinterpret_cast<FnObterDelegate>(dlsym(h, "hostfxr_get_runtime_delegate"));
    auto fechar = reinterpret_cast<FnFechar>(dlsym(h, "hostfxr_close"));
    if (!inicializar || !obterDelegate || !fechar) return false;

    const fs::path dll = pasta / "UnityEngine.dll";
    const fs::path config = pasta / "UnityEngine.runtimeconfig.json";
    ParametrosHostfxr p{sizeof(ParametrosHostfxr), nullptr, raiz.c_str()};
    void* ctx = nullptr;
    const int rc = inicializar(config.c_str(), &p, &ctx);
    if (rc < 0 || !ctx) {
        logf(Nivel::Erro, "unity-cs", "hostfxr_initialize_for_runtime_config falhou (0x%x) com %s", static_cast<unsigned>(rc),
             config.c_str());
        if (ctx) fechar(ctx);
        return false;
    }
    void* d = nullptr;
    const int rd = obterDelegate(ctx, kDelegateCarregarAssemblyEObterPonteiro, &d);
    fechar(ctx);
    if (rd != 0 || !d) return false;
    auto carregar = reinterpret_cast<FnCarregarEObter>(d);
    const std::string s = dll.string();
    bool ok = obter(carregar, s, "Iniciar", g_ponte.iniciar) && obter(carregar, s, "CarregarAssembly", g_ponte.carregarAssembly) &&
              obter(carregar, s, "CriarObjeto", g_ponte.criarObjeto) &&
              obter(carregar, s, "AdicionarComponente", g_ponte.adicionarComponente) &&
              obter(carregar, s, "DefinirCampo", g_ponte.definirCampo) && obter(carregar, s, "Quadro", g_ponte.quadro) &&
              obter(carregar, s, "Descrever", g_ponte.descrever) && obter(carregar, s, "Encerrar", g_ponte.encerrar);
    if (!ok) return false;
    g_ponte.iniciar(&g_tabela);
    logf(Nivel::Info, "unity-cs", "runtime .NET ativo (%s), UnityEngine da camada compat carregado", fxr.parent_path().filename().c_str());
    return true;
}

} // namespace

bool carregarAssembly(const std::string& caminhoDll) {
    const fs::path dll = fs::absolute(caminhoDll);
    if (!g_ativo) {
        if (!subirRuntime(dll.parent_path())) return false;
        g_ativo = true;
    }
    return g_ponte.carregarAssembly(dll.c_str()) >= 0;
}

uint32_t criarObjeto(const std::string& nome) {
    return g_ativo ? g_ponte.criarObjeto(nome.c_str()) : 0;
}

bool adicionarComponente(uint32_t objeto, const std::string& tipo) {
    return g_ativo && g_ponte.adicionarComponente(objeto, tipo.c_str()) == 1;
}

bool definirCampo(uint32_t objeto, const std::string& tipo, const std::string& campo, const std::string& valor) {
    return g_ativo && g_ponte.definirCampo(objeto, tipo.c_str(), campo.c_str(), valor.c_str()) == 1;
}

void quadro(double dt) {
    if (g_ativo) g_ponte.quadro(static_cast<float>(dt));
}

std::string descrever(uint32_t objeto) {
    if (!g_ativo) return {};
    char buf[512];
    g_ponte.descrever(objeto, buf, sizeof(buf));
    return buf;
}

void encerrar() {
    // O runtime .NET nao descarrega dentro do processo; so' destroi os objetos.
    if (g_ativo) g_ponte.encerrar();
}

bool ativo() {
    return g_ativo;
}

} // namespace eruption::compat::unity::cs
