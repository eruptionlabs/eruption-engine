#include "unity/UnityHost.hpp"

#include "host/Host.hpp"
#include "unity/UnityAbi.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <filesystem>
#include <map>
#include <mutex>
#include <sys/uio.h>
#include <unistd.h>

namespace eruption::compat::unity {

namespace {

struct Plugin {
    std::string caminho;
    void* handle = nullptr;
    void (*descarregar)() = nullptr;
    EventoRender eventoRender = nullptr;
};

std::vector<Plugin>& plugins() {
    static std::vector<Plugin> p;
    return p;
}

std::mutex g_mtx;
std::vector<CallbackDispositivo> g_callbacks;
std::map<std::pair<uint64_t, uint64_t>, void*> g_registradas; // interfaces registradas por plugins
int32_t g_proximoEvento = 1000;
std::vector<int> g_eventosPorQuadro;

// ---- IUnityGraphics -------------------------------------------------------------

// Sem ponte de dispositivo com o motor ainda: por padrao o plugin ve um
// dispositivo "nulo" e desliga o proprio caminho de render (o resto funciona).
int32_t g_renderizador = static_cast<int32_t>(Renderizador::Nulo);

int32_t gr_renderizador() {
    return g_renderizador;
}

void gr_registrarCallback(CallbackDispositivo cb) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (cb && std::find(g_callbacks.begin(), g_callbacks.end(), cb) == g_callbacks.end()) g_callbacks.push_back(cb);
    logf(Nivel::Info, "unity", "callback de dispositivo registrado");
}

void gr_removerCallback(CallbackDispositivo cb) {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_callbacks.erase(std::remove(g_callbacks.begin(), g_callbacks.end(), cb), g_callbacks.end());
}

int32_t gr_reservarEventos(int32_t n) {
    std::lock_guard<std::mutex> lk(g_mtx);
    const int32_t ini = g_proximoEvento;
    g_proximoEvento += std::max(0, n);
    return ini;
}

const TabelaGraficos kGraficos{&gr_renderizador, &gr_registrarCallback, &gr_removerCallback, &gr_reservarEventos};

// ---- IUnityLog ------------------------------------------------------------------

void lg_log(int32_t tipo, const char* msg, const char* arquivo, int32_t linha) {
    const Nivel n = (tipo == static_cast<int32_t>(TipoLog::Erro) || tipo == static_cast<int32_t>(TipoLog::Excecao)) ? Nivel::Erro
                    : tipo == static_cast<int32_t>(TipoLog::Aviso)                                                   ? Nivel::Aviso
                                                                                                                   : Nivel::Info;
    if (arquivo && *arquivo) logf(n, "unity", "%s (%s:%d)", msg ? msg : "", arquivo, linha);
    else logf(n, "unity", "%s", msg ? msg : "");
}

const TabelaLog kLog{&lg_log};

// ---- IUnityInterfaces -------------------------------------------------------------

void* obterSeparado(uint64_t alto, uint64_t baixo) {
    const Guid g{alto, baixo};
    if (g == kGuidGraficos) return const_cast<TabelaGraficos*>(&kGraficos);
    if (g == kGuidLog) return const_cast<TabelaLog*>(&kLog);
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        auto it = g_registradas.find({alto, baixo});
        if (it != g_registradas.end()) return it->second;
    }
    if (g == kGuidGraficosVulkan) {
        logf(Nivel::Aviso, "unity", "IUnityGraphicsVulkan pedido: ainda nao ha' ponte Vulkan, devolvendo nulo");
        return nullptr;
    }
    // Modo sonda: o GUID pedido vai para o log.
    logf(Nivel::Aviso, "unity", "interface desconhecida pedida: GUID 0x%016llX 0x%016llX (devolvendo nulo)",
         static_cast<unsigned long long>(alto), static_cast<unsigned long long>(baixo));
    return nullptr;
}

bool guidConhecido(uint64_t alto, uint64_t baixo) {
    const Guid g{alto, baixo};
    if (g == kGuidGraficos || g == kGuidLog || g == kGuidGraficosVulkan) return true;
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_registradas.count({alto, baixo}) != 0;
}

// Le 16 bytes do proprio processo sem risco de falha de segmentacao: se o
// endereco nao for legivel, process_vm_readv so' devolve erro.
bool lerGuid(uint64_t endereco, Guid& g) {
    if (endereco < 0x10000) return false;
    struct iovec local{&g, sizeof(g)};
    struct iovec remoto{reinterpret_cast<void*>(endereco), sizeof(g)};
    return process_vm_readv(getpid(), &local, 1, &remoto, 1, 0) == static_cast<ssize_t>(sizeof(g));
}

// GUID por valor (a, b) ou por ponteiro (a): decide pelo conteudo.
bool decodificarGuid(uint64_t a, uint64_t b, Guid& g, bool& porPonteiro) {
    porPonteiro = false;
    if (guidConhecido(a, b)) {
        g = {a, b};
        return true;
    }
    if (lerGuid(a, g)) {
        porPonteiro = true;
        return true;
    }
    g = {a, b};
    return true;
}

void* obter(uint64_t a, uint64_t b) {
    Guid g;
    bool porPonteiro;
    decodificarGuid(a, b, g, porPonteiro);
    return obterSeparado(g.alto, g.baixo);
}

void registrarSeparado(uint64_t alto, uint64_t baixo, void* ptr) {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_registradas[{alto, baixo}] = ptr;
}

void registrar(uint64_t a, uint64_t b, uint64_t c) {
    Guid g;
    bool porPonteiro;
    decodificarGuid(a, b, g, porPonteiro);
    registrarSeparado(g.alto, g.baixo, reinterpret_cast<void*>(porPonteiro ? b : c));
}

TabelaInterfaces kInterfaces{&obter, &registrar, &obterSeparado, &registrarSeparado};

void enviarEventoDispositivo(EventoDispositivo e) {
    std::vector<CallbackDispositivo> cbs;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        cbs = g_callbacks;
    }
    for (CallbackDispositivo cb : cbs) cb(static_cast<int32_t>(e));
}

// ---- Chamada de funcoes exportadas ---------------------------------------------------
//
// Monta a chamada pela convencao System V x86-64: inteiros e ponteiros nos
// registradores de inteiro, float/double nos de vetor, cada um na sua
// sequencia. Chamar com registradores a mais e' inofensivo (o chamado ignora).

using I = int64_t;

template <typename R, typename F>
R chamarCom(void* fn, const I* i, const F* f) {
    using Proto = R (*)(I, I, I, I, I, I, F, F, F, F, F, F, F, F);
    Proto p = reinterpret_cast<Proto>(fn);
    return p(i[0], i[1], i[2], i[3], i[4], i[5], f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]);
}

template <typename F>
bool chamarTipado(void* fn, char ret, const I* i, const F* f, std::string* resultado) {
    char buf[64];
    switch (ret) {
    case 'v': chamarCom<void, F>(fn, i, f); std::snprintf(buf, sizeof(buf), "(void)"); break;
    case 'i': std::snprintf(buf, sizeof(buf), "%d", static_cast<int32_t>(chamarCom<I, F>(fn, i, f))); break;
    case 'l': std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(chamarCom<I, F>(fn, i, f))); break;
    case 'b': std::snprintf(buf, sizeof(buf), "%s", (chamarCom<I, F>(fn, i, f) & 0xFF) ? "true" : "false"); break;
    case 'p': std::snprintf(buf, sizeof(buf), "%p", reinterpret_cast<void*>(chamarCom<I, F>(fn, i, f))); break;
    case 'f': std::snprintf(buf, sizeof(buf), "%g", static_cast<double>(chamarCom<float, F>(fn, i, f))); break;
    case 'd': std::snprintf(buf, sizeof(buf), "%g", chamarCom<double, F>(fn, i, f)); break;
    default: return false;
    }
    if (resultado) *resultado = buf;
    return true;
}

void* simbolo(const std::string& nome) {
    for (Plugin& p : plugins())
        if (void* s = dlsym(p.handle, nome.c_str())) return s;
    return nullptr;
}

} // namespace

void definirRenderizador(int tipo) {
    g_renderizador = tipo;
}

bool carregarPlugin(const std::string& caminhoSo) {
    Plugin p;
    p.caminho = std::filesystem::absolute(caminhoSo).string();
    p.handle = dlopen(p.caminho.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!p.handle) {
        logf(Nivel::Erro, "unity", "dlopen falhou: %s", dlerror());
        return false;
    }
    using FnCarregar = void (*)(TabelaInterfaces*);
    auto carregar = reinterpret_cast<FnCarregar>(dlsym(p.handle, "UnityPluginLoad"));
    p.descarregar = reinterpret_cast<void (*)()>(dlsym(p.handle, "UnityPluginUnload"));
    using FnGetEvento = EventoRender (*)();
    if (auto get = reinterpret_cast<FnGetEvento>(dlsym(p.handle, "GetRenderEventFunc"))) p.eventoRender = get();
    const std::string nome = std::filesystem::path(p.caminho).filename().string();
    if (carregar) {
        carregar(&kInterfaces);
        logf(Nivel::Info, "unity", "plugin carregado: %s (UnityPluginLoad chamado%s)", nome.c_str(),
             p.eventoRender ? ", tem GetRenderEventFunc" : "");
    } else {
        // Plugin nativo "simples": so' funcoes C chamadas por DllImport.
        logf(Nivel::Info, "unity", "biblioteca nativa carregada: %s (sem UnityPluginLoad)", nome.c_str());
    }
    plugins().push_back(p);
    return true;
}

bool chamarFuncao(const std::string& nome, const std::string& assinatura, const std::vector<std::string>& valores,
                  std::string* resultado) {
    void* fn = simbolo(nome);
    if (!fn) {
        logf(Nivel::Erro, "unity", "funcao exportada nao encontrada: %s", nome.c_str());
        return false;
    }
    // "r(a,b,...)"
    const auto ab = assinatura.find('(');
    const auto fe = assinatura.rfind(')');
    if (assinatura.empty() || ab == std::string::npos || fe == std::string::npos || ab != 1) {
        logf(Nivel::Erro, "unity", "assinatura invalida: %s", assinatura.c_str());
        return false;
    }
    const char ret = assinatura[0];
    std::vector<char> tipos;
    for (size_t k = ab + 1; k < fe; ++k)
        if (assinatura[k] != ',' && assinatura[k] != ' ') tipos.push_back(assinatura[k]);
    if (tipos.size() != valores.size()) {
        logf(Nivel::Erro, "unity", "%s: %zu argumentos para assinatura com %zu", nome.c_str(), valores.size(), tipos.size());
        return false;
    }
    I ints[6] = {};
    float fs[8] = {};
    double ds[8] = {};
    int ni = 0, nf = 0, nd = 0;
    for (size_t k = 0; k < tipos.size(); ++k) {
        const std::string& v = valores[k];
        switch (tipos[k]) {
        case 'i':
        case 'l':
        case 'b': ints[ni++] = std::strtoll(v.c_str(), nullptr, 10); break;
        case 'p': ints[ni++] = static_cast<I>(std::strtoull(v.c_str(), nullptr, 0)); break;
        case 's': ints[ni++] = reinterpret_cast<I>(v.c_str()); break;
        case 'f': fs[nf++] = std::strtof(v.c_str(), nullptr); break;
        case 'd': ds[nd++] = std::strtod(v.c_str(), nullptr); break;
        default: logf(Nivel::Erro, "unity", "tipo de argumento desconhecido: %c", tipos[k]); return false;
        }
        if (ni > 6 || nf > 8 || nd > 8) return false;
    }
    if (nf && nd) {
        logf(Nivel::Erro, "unity", "%s: misturar float e double ainda nao e' suportado", nome.c_str());
        return false;
    }
    return nd ? chamarTipado<double>(fn, ret, ints, ds, resultado) : chamarTipado<float>(fn, ret, ints, fs, resultado);
}

int emitirEventoRender(int id) {
    int n = 0;
    for (Plugin& p : plugins()) {
        if (p.eventoRender) {
            p.eventoRender(id);
            ++n;
        }
    }
    return n;
}

void definirEventosPorQuadro(const std::vector<int>& ids) {
    g_eventosPorQuadro = ids;
}

void quadro(double) {
    for (int id : g_eventosPorQuadro) emitirEventoRender(id);
}

void encerrar() {
    if (plugins().empty()) return;
    enviarEventoDispositivo(EventoDispositivo::Desligar);
    for (auto it = plugins().rbegin(); it != plugins().rend(); ++it)
        if (it->descarregar) it->descarregar();
    logf(Nivel::Info, "unity", "%zu plugin(s) descarregado(s)", plugins().size());
    plugins().clear();
}

bool algumCarregado() {
    return !plugins().empty();
}

} // namespace eruption::compat::unity
