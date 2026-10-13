#include "unity/CSharpHost.hpp"

#include "host/Host.hpp"

#include <cstdlib>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
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
    int (*definirPai)(uint32_t, uint32_t) = nullptr; // opcional (UnityEngine.dll antigo nao tem)
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
    if (!obter(carregar, s, "DefinirPai", g_ponte.definirPai)) g_ponte.definirPai = nullptr;
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

bool definirPai(uint32_t objeto, uint32_t pai) {
    return g_ativo && g_ponte.definirPai && g_ponte.definirPai(objeto, pai) == 1;
}

namespace {

// "{x: 1, y: 2, z: 3}" -> {"x": "1", ...}; "{fileID: 7}" -> {"fileID": "7"}.
std::map<std::string, std::string> mapaYaml(const std::string& v) {
    std::map<std::string, std::string> m;
    const size_t a = v.find('{'), b = v.rfind('}');
    if (a == std::string::npos || b == std::string::npos || b <= a) return m;
    std::string corpo = v.substr(a + 1, b - a - 1);
    size_t i = 0;
    while (i < corpo.size()) {
        size_t virg = corpo.find(',', i);
        if (virg == std::string::npos) virg = corpo.size();
        const std::string item = corpo.substr(i, virg - i);
        const size_t dp = item.find(':');
        if (dp != std::string::npos) {
            auto apara = [](std::string t) {
                const size_t x = t.find_first_not_of(' '), y = t.find_last_not_of(' ');
                return x == std::string::npos ? std::string() : t.substr(x, y - x + 1);
            };
            m[apara(item.substr(0, dp))] = apara(item.substr(dp + 1));
        }
        i = virg + 1;
    }
    return m;
}

std::string vetorTexto(const std::map<std::string, std::string>& m, bool comW) {
    auto g = [&](const char* k) { auto it = m.find(k); return it == m.end() ? std::string("0") : it->second; };
    std::string t = g("x") + "," + g("y") + "," + g("z");
    if (comW) t += "," + g("w");
    return t;
}

} // namespace

std::vector<uint32_t> carregarCena(const std::string& caminho) {
    std::vector<uint32_t> criados;
    if (!g_ativo) {
        logf(Nivel::Erro, "unity-cs", "cena %s: carregue um assembly de scripts antes", caminho.c_str());
        return criados;
    }
    std::ifstream f(caminho);
    if (!f) {
        logf(Nivel::Erro, "unity-cs", "cena nao encontrada: %s", caminho.c_str());
        return criados;
    }
    struct Doc {
        int classe = 0;
        std::string id, tipo;
        std::vector<std::pair<std::string, std::string>> campos; // so' o primeiro nivel
        std::string campo(const std::string& k) const {
            for (const auto& [a, b] : campos) if (a == k) return b;
            return {};
        }
    };
    std::vector<Doc> docs;
    std::string linha;
    while (std::getline(f, linha)) {
        if (linha.rfind("--- !u!", 0) == 0) {
            Doc d;
            d.classe = std::atoi(linha.c_str() + 7);
            const size_t amp = linha.find('&');
            if (amp != std::string::npos) d.id = linha.substr(amp + 1, linha.find(' ', amp) - amp - 1);
            docs.push_back(std::move(d));
        } else if (!docs.empty() && !linha.empty() && linha[0] != ' ' && linha.back() == ':') {
            docs.back().tipo = linha.substr(0, linha.size() - 1);
        } else if (!docs.empty() && linha.size() > 2 && linha[0] == ' ' && linha[1] == ' ' && linha[2] != ' ' && linha[2] != '-') {
            const size_t dp = linha.find(':');
            if (dp == std::string::npos) continue;
            const std::string valor = dp + 1 < linha.size() ? linha.substr(dp + 2 < linha.size() ? dp + 2 : dp + 1) : std::string();
            docs.back().campos.emplace_back(linha.substr(2, dp - 2), valor);
        }
    }

    // guid do script -> nome da classe: os .cs.meta da pasta Assets da cena.
    std::map<std::string, std::string> classePorGuid;
    fs::path assets = fs::absolute(caminho).parent_path();
    while (!assets.empty() && assets.filename() != "Assets" && assets != assets.parent_path()) assets = assets.parent_path();
    std::error_code ec;
    if (assets.filename() == "Assets") {
        for (auto it = fs::recursive_directory_iterator(assets, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            const std::string nome = it->path().filename().string();
            if (nome.size() <= 8 || nome.compare(nome.size() - 8, 8, ".cs.meta") != 0) continue;
            std::ifstream m(it->path());
            std::string l;
            while (std::getline(m, l))
                if (l.rfind("guid: ", 0) == 0) { classePorGuid[l.substr(6)] = nome.substr(0, nome.size() - 8); break; }
        }
    }

    // GameObjects (classe 1), na ordem do arquivo.
    std::map<std::string, uint32_t> objetoPorId;
    for (const Doc& d : docs) {
        if (d.classe != 1) continue;
        const uint32_t o = criarObjeto(d.campo("m_Name"));
        if (!o) continue;
        objetoPorId[d.id] = o;
        criados.push_back(o);
    }
    // Transform (classe 4): pai primeiro (mantem o local), depois os valores locais.
    std::map<std::string, std::string> objetoDoTransform;
    for (const Doc& d : docs)
        if (d.classe == 4) objetoDoTransform[d.id] = mapaYaml(d.campo("m_GameObject"))["fileID"];
    bool avisouPai = false;
    for (const Doc& d : docs) {
        if (d.classe != 4) continue;
        auto go = objetoPorId.find(objetoDoTransform[d.id]);
        if (go == objetoPorId.end()) continue;
        const std::string pai = mapaYaml(d.campo("m_Father"))["fileID"];
        if (!pai.empty() && pai != "0") {
            auto paiGo = objetoPorId.find(objetoDoTransform[pai]);
            if (paiGo != objetoPorId.end() && !definirPai(go->second, paiGo->second) && !avisouPai) {
                logf(Nivel::Aviso, "unity-cs", "UnityEngine.dll sem DefinirPai: hierarquia da cena ignorada");
                avisouPai = true;
            }
        }
        definirCampo(go->second, "Transform", "localPosition", vetorTexto(mapaYaml(d.campo("m_LocalPosition")), false));
        definirCampo(go->second, "Transform", "localRotation", vetorTexto(mapaYaml(d.campo("m_LocalRotation")), true));
        definirCampo(go->second, "Transform", "localScale", vetorTexto(mapaYaml(d.campo("m_LocalScale")), false));
    }
    // MonoBehaviour (classe 114) cujo script esta' num assembly carregado.
    std::set<std::string> avisados;
    for (const Doc& d : docs) {
        if (d.classe != 114) continue;
        auto go = objetoPorId.find(mapaYaml(d.campo("m_GameObject"))["fileID"]);
        if (go == objetoPorId.end()) continue;
        const std::string guid = mapaYaml(d.campo("m_Script"))["guid"];
        auto cls = classePorGuid.find(guid);
        if (cls == classePorGuid.end()) {
            if (avisados.insert(guid).second)
                logf(Nivel::Aviso, "unity-cs", "%s: script de guid %s fora da pasta Assets; componente ignorado", caminho.c_str(), guid.c_str());
            continue;
        }
        if (!adicionarComponente(go->second, cls->second)) continue;
        for (const auto& [chave, valor] : d.campos) {
            if (chave.rfind("m_", 0) == 0 || valor.empty() || valor[0] == '[') continue;
            std::string texto = valor;
            if (valor[0] == '{') {
                const auto m = mapaYaml(valor);
                if (m.count("fileID")) continue;   // referencia a outro objeto: ainda nao
                texto = vetorTexto(m, m.count("w") > 0);
            }
            definirCampo(go->second, cls->second, chave, texto);
        }
    }
    logf(Nivel::Info, "unity-cs", "cena %s: %zu GameObject(s)", fs::path(caminho).filename().c_str(), criados.size());
    return criados;
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
