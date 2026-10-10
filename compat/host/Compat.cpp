#include "host/Compat.hpp"

#include "godot/GodotHost.hpp"
#include "host/Host.hpp"
#include "unity/UnityHost.hpp"

#include <cstdlib>
#include <dlfcn.h>
#include <filesystem>

namespace eruption::compat {

namespace {

bool g_godot = false;

std::vector<std::string> dividir(const std::string& s, char sep) {
    std::vector<std::string> r;
    size_t ini = 0;
    while (ini <= s.size()) {
        size_t fim = s.find(sep, ini);
        if (fim == std::string::npos) fim = s.size();
        if (fim > ini) r.push_back(s.substr(ini, fim - ini));
        ini = fim + 1;
    }
    return r;
}

bool comecaCom(const std::string& s, const char* p) {
    return s.rfind(p, 0) == 0;
}

// Descobre a origem de uma .so pelos simbolos que ela exporta.
std::string detectar(const std::string& so) {
    void* h = dlopen(so.c_str(), RTLD_LAZY | RTLD_LOCAL);
    if (!h) return {};
    std::string tipo;
    if (dlsym(h, "UnityPluginLoad") || dlsym(h, "GetRenderEventFunc")) tipo = "unity";
    dlclose(h);
    return tipo;
}

} // namespace

bool carregarPlugin(const std::string& esp) {
    bool ok = false;
    const std::filesystem::path caminho(esp);
    if (comecaCom(esp, "godot:")) {
        ok = godot::carregarExtensao(esp.substr(6));
        g_godot = g_godot || ok;
    } else if (caminho.extension() == ".gdextension") {
        ok = godot::carregarExtensao(esp);
        g_godot = g_godot || ok;
    } else if (comecaCom(esp, "unity:")) {
        ok = unity::carregarPlugin(esp.substr(6));
    } else if (caminho.extension() == ".so" && detectar(esp) == "unity") {
        ok = unity::carregarPlugin(esp);
    } else {
        logf(Nivel::Erro, "compat", "plugin nao reconhecido: %s (use godot:, unity: ou um .gdextension)", esp.c_str());
    }
    return ok;
}

int carregarDaVariavel(const char* variavel) {
    const char* v = std::getenv(variavel);
    if (!v || !*v) return 0;
    int n = 0;
    for (const std::string& item : dividir(v, ';')) n += carregarPlugin(item) ? 1 : 0;
    logf(Nivel::Info, "compat", "%d plugin(s) carregado(s) de %s", n, variavel);
    // Eventos de render dos plugins Unity a emitir por quadro (GL.IssuePluginEvent).
    if (const char* ev = std::getenv("ERUPTION_COMPAT_UNITY_EVENTOS")) {
        std::vector<int> ids;
        for (const std::string& s : dividir(ev, ';')) ids.push_back(std::atoi(s.c_str()));
        unity::definirEventosPorQuadro(ids);
    }
    return n;
}

int instanciarDaVariavel(const char* variavel) {
    const char* v = std::getenv(variavel);
    if (!v || !*v || !g_godot) return 0;
    int n = 0;
    for (const std::string& classe : dividir(v, ';')) {
        const uint64_t id = godot::instanciar(classe);
        if (id && godot::adicionarNaCena(id)) {
            logf(Nivel::Info, "compat", "na cena: %s", godot::descrever(id).c_str());
            ++n;
        }
    }
    return n;
}

void quadro(double dt) {
    if (auto* h = dynamic_cast<HostPadrao*>(&host())) h->avancar(dt);
    if (g_godot) godot::quadro(dt);
    if (unity::algumCarregado()) unity::quadro(dt);
}

void encerrar() {
    if (g_godot) {
        logf(Nivel::Info, "compat", "quadros GDExtension: %llu (t=%.2fs)", static_cast<unsigned long long>(host().quadro()),
             host().tempo());
        for (const std::string& linha : godot::descreverCena()) logf(Nivel::Info, "compat", "cena: %s", linha.c_str());
        godot::encerrar();
    }
    g_godot = false;
    unity::encerrar();
}

bool algumCarregado() {
    return g_godot || unity::algumCarregado();
}

} // namespace eruption::compat
