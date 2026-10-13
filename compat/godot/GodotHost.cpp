#include "godot/GodotHost.hpp"

#include "godot/GdNucleo.hpp"
#include "host/Host.hpp"

#include <cstdlib>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <cmath>
#include <map>
#include <memory>
#include <regex>
#include <sstream>

namespace eruption::compat::godot {

using namespace gd;
namespace fs = std::filesystem;

Valor Valor::deTexto(const std::string& t) {
    Valor v;
    if (t == "true" || t == "false") {
        v.tipo = Tipo::Bool;
        v.b = t == "true";
        return v;
    }
    char* fim = nullptr;
    const long long i = std::strtoll(t.c_str(), &fim, 10);
    if (!t.empty() && fim && *fim == '\0') {
        v.tipo = Tipo::Int;
        v.i = i;
        return v;
    }
    const double f = std::strtod(t.c_str(), &fim);
    if (!t.empty() && fim && *fim == '\0') {
        v.tipo = Tipo::Float;
        v.f = f;
        return v;
    }
    v.tipo = Tipo::Texto;
    v.s = t;
    return v;
}

namespace {

std::vector<std::unique_ptr<Biblioteca>>& bibliotecas() {
    static std::vector<std::unique_ptr<Biblioteca>> b;
    return b;
}

GDExtensionInterfaceFunctionPtr getProcAddress(const char* nome) {
    return obterFuncao(nome);
}

std::string semAspas(std::string s) {
    const auto ini = s.find_first_not_of(" \t\"");
    const auto fim = s.find_last_not_of(" \t\"\r");
    return ini == std::string::npos ? std::string() : s.substr(ini, fim - ini + 1);
}

// res:// e' a pasta do project.godot mais proximo acima do .gdextension.
fs::path raizProjeto(const fs::path& arquivo) {
    for (fs::path p = arquivo.parent_path(); !p.empty(); p = p.parent_path()) {
        if (fs::exists(p / "project.godot")) return p;
        if (p == p.root_path()) break;
    }
    return arquivo.parent_path();
}

// Le entry_symbol e a biblioteca linux x86_64 de um .gdextension.
bool lerGdextension(const fs::path& arquivo, std::string& so, std::string& simbolo) {
    std::ifstream in(arquivo);
    if (!in) return false;
    std::string linha, secao, melhor;
    int pontosMelhor = -1;
    while (std::getline(in, linha)) {
        const auto c = linha.find(';');
        if (c != std::string::npos) linha = linha.substr(0, c);
        linha = semAspas(linha);
        if (linha.empty()) continue;
        if (linha.front() == '[') {
            secao = linha;
            continue;
        }
        const auto eq = linha.find('=');
        if (eq == std::string::npos) continue;
        const std::string chave = semAspas(linha.substr(0, eq));
        const std::string valor = semAspas(linha.substr(eq + 1));
        if (secao == "[configuration]" && chave == "entry_symbol") simbolo = valor;
        if (secao == "[libraries]" && chave.rfind("linux", 0) == 0 &&
            (chave.find("x86_64") != std::string::npos || chave.find("arm") == std::string::npos)) {
            // Prefere debug e precisao simples (o host fala a ABI float_64).
            int pontos = 0;
            if (chave.find("x86_64") != std::string::npos) pontos += 4;
            if (chave.find("double") == std::string::npos) pontos += 2;
            if (chave.find("debug") != std::string::npos) pontos += 1;
            if (pontos > pontosMelhor) {
                pontosMelhor = pontos;
                melhor = valor;
            }
        }
    }
    if (melhor.empty() || simbolo.empty()) return false;
    fs::path caminho;
    if (melhor.rfind("res://", 0) == 0) caminho = raizProjeto(arquivo) / melhor.substr(6);
    else caminho = arquivo.parent_path() / melhor;
    so = caminho.lexically_normal().string();
    return true;
}

void inicializarNiveis(Biblioteca* b) {
    // core, servers, scene (o nivel editor fica de fora: aqui nao ha' editor Godot).
    for (int nivel = GDEXTENSION_INITIALIZATION_CORE; nivel <= GDEXTENSION_INITIALIZATION_SCENE; ++nivel) {
        if (b->init.initialize) b->init.initialize(b->init.userdata, static_cast<GDExtensionInitializationLevel>(nivel));
        b->nivelAtual = nivel;
    }
    if (b->laco.startup_func) b->laco.startup_func();
}

Objeto* porId(uint64_t id) {
    return id ? objetoPorId(id) : nullptr;
}

// Objetos criados por instanciar(): o host segura uma referencia.
std::vector<uint64_t>& segurados() {
    static std::vector<uint64_t> v;
    return v;
}

} // namespace

bool carregarExtensao(const std::string& entrada) {
    std::string so = entrada, simbolo;
    const fs::path p(entrada);
    if (p.extension() == ".gdextension") {
        if (!lerGdextension(p, so, simbolo)) {
            logf(Nivel::Erro, "godot", "%s: sem entry_symbol ou biblioteca linux", entrada.c_str());
            return false;
        }
    } else {
        const auto dp = entrada.rfind(':');
        if (dp == std::string::npos || dp + 1 >= entrada.size()) {
            logf(Nivel::Erro, "godot", "%s: use um .gdextension ou biblioteca.so:simbolo_de_entrada", entrada.c_str());
            return false;
        }
        so = entrada.substr(0, dp);
        simbolo = entrada.substr(dp + 1);
    }
    auto b = std::make_unique<Biblioteca>();
    b->caminho = fs::absolute(so).string();
    b->simbolo = simbolo;
    b->handle = dlopen(b->caminho.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!b->handle) {
        logf(Nivel::Erro, "godot", "dlopen falhou: %s", dlerror());
        return false;
    }
    auto entrada_fn = reinterpret_cast<GDExtensionInitializationFunction>(dlsym(b->handle, simbolo.c_str()));
    if (!entrada_fn) {
        logf(Nivel::Erro, "godot", "%s: simbolo %s nao encontrado", b->caminho.c_str(), simbolo.c_str());
        dlclose(b->handle);
        return false;
    }
    if (!entrada_fn(&getProcAddress, b.get(), &b->init)) {
        logf(Nivel::Erro, "godot", "%s: a extensao recusou a inicializacao", b->caminho.c_str());
        dlclose(b->handle);
        return false;
    }
    logf(Nivel::Info, "godot", "extensao carregada: %s (%s)", fs::path(b->caminho).filename().string().c_str(), simbolo.c_str());
    Biblioteca* ptr = b.get();
    bibliotecas().push_back(std::move(b));
    inicializarNiveis(ptr);
    return true;
}

std::vector<std::string> classesRegistradas() {
    std::vector<std::string> r;
    for (Classe* c : classesExtensao()) r.push_back(c->nome->utf8 + " : " + c->pai->nome->utf8);
    return r;
}

uint64_t instanciar(const std::string& nome) {
    Classe* c = classePorNome(nome);
    if (!c) {
        logf(Nivel::Erro, "godot", "classe desconhecida: %s", nome.c_str());
        return 0;
    }
    Objeto* o = instanciar(c);
    if (!o) return 0;
    if (c->refCounted) refIniciar(o); // a referencia inicial fica com o host
    segurados().push_back(o->id);
    return o->id;
}

bool adicionarNaCena(uint64_t id, uint64_t paiId) {
    Objeto* o = porId(id);
    Objeto* pai = paiId ? porId(paiId) : raiz();
    if (!o || !pai || !ehNo(o) || !ehNo(pai)) {
        logf(Nivel::Erro, "godot", "adicionarNaCena: objeto %llu nao e' um no'", static_cast<unsigned long long>(id));
        return false;
    }
    adicionarFilho(pai, o);
    return true;
}

namespace {

// Numeros de um construtor do .tscn: "Vector3(1, 2, 3)" -> {1, 2, 3}.
std::vector<double> numerosDe(const std::string& v) {
    std::vector<double> out;
    const size_t a = v.find('('), b = v.rfind(')');
    if (a == std::string::npos || b == std::string::npos || b <= a) return out;
    std::stringstream ss(v.substr(a + 1, b - a - 1));
    std::string item;
    while (std::getline(ss, item, ',')) out.push_back(std::atof(item.c_str()));
    return out;
}

// Transform3D(x.x, x.y, x.z, y.x, ..., origem) do .tscn: os nove primeiros
// numeros sao as COLUNAS da base. Vira posicao, escala (comprimento das
// colunas) e Euler YXZ (a mesma conta de Basis::get_euler do motor).
void aplicarTransform(Objeto* o, const std::vector<double>& n) {
    if (n.size() < 12) return;
    double c[3][3];   // c[coluna][linha]
    for (int col = 0; col < 3; ++col)
        for (int lin = 0; lin < 3; ++lin) c[col][lin] = n[size_t(col * 3 + lin)];
    for (int col = 0; col < 3; ++col) {
        const double len = std::sqrt(c[col][0] * c[col][0] + c[col][1] * c[col][1] + c[col][2] * c[col][2]);
        o->escala[col] = static_cast<float>(len);
        if (len > 1e-12)
            for (int lin = 0; lin < 3; ++lin) c[col][lin] /= len;
    }
    auto m = [&](int lin, int col) { return c[col][lin]; };
    const double m12 = m(1, 2);
    if (m12 < 1.0 - 1e-6 && m12 > -(1.0 - 1e-6)) {
        o->rot[0] = static_cast<float>(std::asin(-m12));
        o->rot[1] = static_cast<float>(std::atan2(m(0, 2), m(2, 2)));
        o->rot[2] = static_cast<float>(std::atan2(m(1, 0), m(1, 1)));
    } else {
        o->rot[0] = static_cast<float>(m12 < 0.0 ? M_PI * 0.5 : -M_PI * 0.5);
        o->rot[1] = static_cast<float>(m12 < 0.0 ? std::atan2(m(0, 1), m(0, 0)) : -std::atan2(m(0, 1), m(0, 0)));
        o->rot[2] = 0.0f;
    }
    for (int i = 0; i < 3; ++i) o->pos[i] = static_cast<float>(n[size_t(9 + i)]);
}

} // namespace

uint64_t carregarCena(const std::string& caminho) {
    std::ifstream f(caminho);
    if (!f) {
        logf(Nivel::Erro, "godot", "cena nao encontrada: %s", caminho.c_str());
        return 0;
    }
    struct NoTscn {
        std::map<std::string, std::string> attr;
        std::vector<std::pair<std::string, std::string>> props;
    };
    std::vector<NoTscn> nos;
    const std::regex reAttr(R"re((\w+)=("([^"]*)"|[^ \]]+))re");
    std::string linha;
    bool emNo = false;
    while (std::getline(f, linha)) {
        if (linha.empty()) continue;
        if (linha[0] == '[') {
            emNo = linha.rfind("[node ", 0) == 0;
            if (emNo) {
                NoTscn n;
                for (std::sregex_iterator it(linha.begin(), linha.end(), reAttr), fim; it != fim; ++it)
                    n.attr[(*it)[1]] = (*it)[3].matched ? (*it)[3].str() : (*it)[2].str();
                nos.push_back(std::move(n));
            }
            continue;
        }
        const size_t eq = linha.find(" = ");
        if (!emNo || nos.empty() || eq == std::string::npos) continue;
        const std::string chave = linha.substr(0, eq);
        std::string valor = linha.substr(eq + 3);
        // Valor em varias linhas (array, dicionario): junta ate' fechar.
        int abertos = 0;
        auto conta = [&abertos](const std::string& t) {
            for (char ch : t) abertos += (ch == '[' || ch == '{' || ch == '(') ? 1 : (ch == ']' || ch == '}' || ch == ')') ? -1 : 0;
        };
        conta(valor);
        while (abertos > 0 && std::getline(f, linha)) {
            valor += "\n" + linha;
            conta(linha);
        }
        nos.back().props.emplace_back(chave, valor);
    }

    // Caminho do no' relativo a' raiz da cena ("." = a raiz) -> id.
    std::map<std::string, uint64_t> porCaminho;
    uint64_t raizCena = 0;
    for (const NoTscn& n : nos) {
        const auto nome = n.attr.find("name"), tipo = n.attr.find("type"), pai = n.attr.find("parent");
        if (nome == n.attr.end()) continue;
        if (tipo == n.attr.end()) {
            logf(Nivel::Aviso, "godot", "%s: no' '%s' sem tipo (cena instanciada?) ficou de fora", caminho.c_str(), nome->second.c_str());
            continue;
        }
        const uint64_t id = instanciar(tipo->second);
        Objeto* o = porId(id);
        if (!o) continue;
        o->nome = nome->second;
        for (const auto& [chave, valor] : n.props) {
            if (chave == "position" || chave == "rotation" || chave == "scale") {
                const std::vector<double> v = numerosDe(valor);
                float* dst = chave == "position" ? o->pos : chave == "rotation" ? o->rot : o->escala;
                if (v.size() == 2 && chave == "rotation") dst[2] = static_cast<float>(v[0]);
                for (size_t i = 0; i < v.size() && i < 3; ++i)
                    if (!(v.size() == 2 && chave == "rotation")) dst[i] = static_cast<float>(v[i]);
            } else if (chave == "transform") {
                aplicarTransform(o, numerosDe(valor));
            } else if (chave == "script") {
                avisarUmaVez("tscn-script", "%s: script GDScript nao roda na camada; o no' fica sem ele", caminho.c_str());
            } else {
                // Propriedade registrada pela extensao: valor simples pelo setter.
                const PropriedadeExtensao* prop = nullptr;
                for (const Classe* c = o->classe; c && !prop; c = c->pai)
                    for (const auto& pe : c->propriedades)
                        if (pe.nome == chave) { prop = &pe; break; }
                const Valor v = Valor::deTexto(valor.size() >= 2 && valor.front() == '"' ? valor.substr(1, valor.size() - 2) : valor);
                if (prop && prop->setter && valor.find('(') == std::string::npos) chamar(id, prop->setter->utf8, {v}, nullptr);
                else avisarUmaVez("tscn-prop:" + chave, "%s: propriedade %s nao aplicada", caminho.c_str(), chave.c_str());
            }
        }
        if (pai == n.attr.end()) {
            if (raizCena) {
                logf(Nivel::Erro, "godot", "%s: mais de uma raiz", caminho.c_str());
                continue;
            }
            raizCena = id;
            porCaminho["."] = id;
            continue;
        }
        const auto it = porCaminho.find(pai->second);
        if (it == porCaminho.end()) {
            logf(Nivel::Erro, "godot", "%s: pai '%s' de '%s' nao existe", caminho.c_str(), pai->second.c_str(), nome->second.c_str());
            continue;
        }
        adicionarFilho(porId(it->second), o);
        porCaminho[pai->second == "." ? nome->second : pai->second + "/" + nome->second] = id;
    }
    if (!raizCena) {
        logf(Nivel::Erro, "godot", "%s: nenhum no' raiz", caminho.c_str());
        return 0;
    }
    // A raiz entra na arvore por ultimo: os filhos ja' estao pendurados, e o
    // _ready sai de baixo para cima, como no motor.
    return adicionarNaCena(raizCena) ? raizCena : 0;
}

bool chamar(uint64_t id, const std::string& metodo, const std::vector<Valor>& valores, std::string* resultado) {
    Objeto* o = porId(id);
    if (!o) return false;
    std::vector<GVariant> args(valores.size());
    std::vector<const GVariant*> ptrs;
    for (size_t i = 0; i < valores.size(); ++i) {
        const Valor& v = valores[i];
        switch (v.tipo) {
        case Valor::Tipo::Nulo: variantNil(&args[i]); break;
        case Valor::Tipo::Bool: {
            const uint8_t b = v.b ? 1 : 0;
            variantDe(&args[i], GDEXTENSION_VARIANT_TYPE_BOOL, &b);
            break;
        }
        case Valor::Tipo::Int: variantInt(&args[i], v.i); break;
        case Valor::Tipo::Float: variantFloat(&args[i], v.f); break;
        case Valor::Tipo::Texto: variantStr(&args[i], v.s); break;
        }
        ptrs.push_back(&args[i]);
    }
    GVariant ret;
    variantNil(&ret);
    GDExtensionCallError erro{};
    const bool ok = chamarMetodo(o, internarUtf8(metodo), ptrs.data(), static_cast<int>(ptrs.size()), &ret, &erro);
    if (!ok) {
        logf(Nivel::Erro, "godot", "chamada %s falhou (erro %d)", metodo.c_str(), static_cast<int>(erro.error));
    } else if (resultado) {
        *resultado = u32ParaUtf8(variantTexto(&ret));
    }
    variantDestruir(&ret);
    for (auto& a : args) variantDestruir(&a);
    return ok;
}

void liberar(uint64_t id) {
    Objeto* o = porId(id);
    if (!o) return;
    auto& s = segurados();
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == id) {
            s.erase(s.begin() + static_cast<long>(i));
            break;
        }
    }
    if (o->classe->refCounted) {
        if (refDecrementar(o)) destruirObjeto(o);
    } else {
        destruirObjeto(o);
    }
}

void quadro(double dt) {
    processarArvore(dt);
    for (auto& b : bibliotecas())
        if (b->laco.frame_func) b->laco.frame_func();
}

std::string descrever(uint64_t id) {
    Objeto* o = porId(id);
    if (!o) return "<liberado>";
    std::ostringstream s;
    s << o->classe->nome->utf8 << "#" << o->id;
    if (ehNo(o)) {
        s << " '" << o->nome << "'";
        s << " pos=(" << o->pos[0] << ", " << o->pos[1] << ", " << o->pos[2] << ")";
        // Rotacao (graus) e escala so' quando fogem do padrao.
        if (o->rot[0] != 0.0f || o->rot[1] != 0.0f || o->rot[2] != 0.0f)
            s << " rot=(" << std::round(o->rot[0] * 57.29578f * 100.0f) / 100.0f + 0.0f << ", "
              << std::round(o->rot[1] * 57.29578f * 100.0f) / 100.0f + 0.0f << ", "
              << std::round(o->rot[2] * 57.29578f * 100.0f) / 100.0f + 0.0f << ")";
        if (o->escala[0] != 1.0f || o->escala[1] != 1.0f || o->escala[2] != 1.0f)
            s << " escala=(" << o->escala[0] << ", " << o->escala[1] << ", " << o->escala[2] << ")";
    }
    return s.str();
}

static void descreverRec(Objeto* o, int nivel, std::vector<std::string>& out) {
    for (Objeto* f : o->filhos) {
        out.push_back(std::string(static_cast<size_t>(nivel) * 2, ' ') + descrever(f->id));
        descreverRec(f, nivel + 1, out);
    }
}

std::vector<std::string> descreverCena() {
    std::vector<std::string> out;
    descreverRec(raiz(), 0, out);
    return out;
}

std::vector<std::string> descreverDesenho() {
    return gd::descreverDesenho();
}

void encerrar() {
    for (uint64_t id : std::vector<uint64_t>(segurados())) liberar(id);
    for (auto it = bibliotecas().rbegin(); it != bibliotecas().rend(); ++it) {
        Biblioteca* b = it->get();
        if (b->laco.shutdown_func) b->laco.shutdown_func();
        for (int nivel = b->nivelAtual; nivel >= GDEXTENSION_INITIALIZATION_CORE; --nivel)
            if (b->init.deinitialize) b->init.deinitialize(b->init.userdata, static_cast<GDExtensionInitializationLevel>(nivel));
        b->nivelAtual = -1;
    }
    // As bibliotecas ficam mapeadas ate' o fim do processo: destrutores
    // estaticos do godot-cpp ainda podem rodar na saida.
}

} // namespace eruption::compat::godot
