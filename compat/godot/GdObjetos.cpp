// Objetos do host GDExtension: ClassDB (classes do motor e das extensoes),
// instancias, ligacoes de instancia, metodos nativos (method binds) e a
// arvore de nos com _ready/_process.

#include "godot/GdNucleo.hpp"
#include "host/Host.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <cmath>
#include <memory>
#include <mutex>

namespace eruption::compat::gd {

namespace {

struct GdClasseApi {
    const char* nome;
    const char* pai;
    bool refCounted;
};
struct GdVirtualApi {
    const char* classe;
    const char* metodo;
    uint32_t hash;
};

#include "godot/GdApiGerado.inc"

std::recursive_mutex g_mtx;
std::unordered_map<const NomeInterno*, std::unique_ptr<Classe>>& classes() {
    static std::unordered_map<const NomeInterno*, std::unique_ptr<Classe>> c;
    return c;
}

std::unordered_map<uint64_t, Objeto*>& objetos() {
    static std::unordered_map<uint64_t, Objeto*> o;
    return o;
}
uint64_t g_proximoId = 1;

const GdClasseApi* apiDe(const std::string& nome) {
    for (const auto& c : kClassesApi)
        if (nome == c.nome) return &c;
    return nullptr;
}

// Classe nativa sob demanda, a partir da tabela gerada da API.
Classe* classeNativa(const NomeInterno* nome) {
    auto& cs = classes();
    auto it = cs.find(nome);
    if (it != cs.end()) return it->second.get();
    const GdClasseApi* api = apiDe(nome->utf8);
    if (!api) return nullptr;
    Classe* pai = nullptr;
    if (api->pai[0]) pai = classeNativa(internarUtf8(api->pai));
    auto c = std::make_unique<Classe>();
    c->nome = nome;
    c->pai = pai;
    c->refCounted = api->refCounted;
    Classe* p = c.get();
    cs.emplace(nome, std::move(c));
    return p;
}

} // namespace

void versaoApi(uint32_t& major, uint32_t& minor, uint32_t& patch) {
    major = ERUPTION_GD_API_MAJOR;
    minor = ERUPTION_GD_API_MINOR;
    patch = ERUPTION_GD_API_PATCH;
}

bool Classe::herdaDe(const Classe* outra) const {
    for (const Classe* c = this; c; c = c->pai)
        if (c == outra) return true;
    return false;
}

const Classe* Classe::nativa() const {
    const Classe* c = this;
    while (c && c->extensao) c = c->pai;
    return c;
}

Classe* classePorNome(const NomeInterno* nome) {
    std::lock_guard<std::recursive_mutex> lk(g_mtx);
    return classeNativa(nome);
}

Classe* classePorNome(const std::string& nome) {
    return classePorNome(internarUtf8(nome));
}

Classe* registrarClasseExtensao(Biblioteca* lib, const NomeInterno* nome, const NomeInterno* pai,
                                const GDExtensionClassCreationInfo4& info) {
    Classe* classePai = classePorNome(pai);
    if (!classePai) {
        logf(Nivel::Erro, "godot", "classe %s herda de %s, desconhecida", nome->utf8.c_str(), pai->utf8.c_str());
        return nullptr;
    }
    std::lock_guard<std::recursive_mutex> lk(g_mtx);
    auto c = std::make_unique<Classe>();
    c->nome = nome;
    c->pai = classePai;
    c->refCounted = classePai->refCounted;
    c->extensao = true;
    c->biblioteca = lib;
    c->info = info;
    Classe* p = c.get();
    classes()[nome] = std::move(c);
    return p;
}

void removerClasseExtensao(const NomeInterno* nome) {
    std::lock_guard<std::recursive_mutex> lk(g_mtx);
    auto it = classes().find(nome);
    if (it != classes().end() && it->second->extensao) classes().erase(it);
}

std::vector<Classe*> classesExtensao() {
    std::lock_guard<std::recursive_mutex> lk(g_mtx);
    std::vector<Classe*> r;
    for (auto& kv : classes())
        if (kv.second->extensao) r.push_back(kv.second.get());
    std::sort(r.begin(), r.end(), [](const Classe* a, const Classe* b) { return a->nome->utf8 < b->nome->utf8; });
    return r;
}

// ---- Objetos ------------------------------------------------------------------------

Objeto* criarObjetoNativo(Classe* classe) {
    auto* o = new Objeto();
    o->classe = classe;
    std::lock_guard<std::recursive_mutex> lk(g_mtx);
    o->id = g_proximoId++;
    objetos()[o->id] = o;
    if (ehNo(o)) o->nome = classe->nome->utf8;
    return o;
}

Objeto* instanciar(Classe* classe) {
    if (!classe) return nullptr;
    if (classe->extensao) {
        if (classe->info.is_abstract || classe->info.is_virtual || !classe->info.create_instance_func) {
            logf(Nivel::Erro, "godot", "classe %s nao e' instanciavel", classe->nome->utf8.c_str());
            return nullptr;
        }
        // A extensao cria o objeto nativo (classdb_construct_object2) e se
        // pendura nele (object_set_instance); devolve o objeto nativo.
        Objeto* o = static_cast<Objeto*>(classe->info.create_instance_func(classe->info.class_userdata, 1));
        if (o && ehNo(o)) o->nome = classe->nome->utf8;
        return o;
    }
    Objeto* o = criarObjetoNativo(classe);
    notificar(o, 0); // NOTIFICATION_POSTINITIALIZE
    return o;
}

bool objetoVivo(const Objeto* o) {
    if (!o) return false;
    std::lock_guard<std::recursive_mutex> lk(g_mtx);
    auto it = objetos().find(o->id);
    return it != objetos().end() && it->second == o;
}

Objeto* objetoPorId(uint64_t id) {
    std::lock_guard<std::recursive_mutex> lk(g_mtx);
    auto it = objetos().find(id);
    return it == objetos().end() ? nullptr : it->second;
}

void destruirObjeto(Objeto* o) {
    if (!objetoVivo(o)) return;
    notificar(o, 1, true); // NOTIFICATION_PREDELETE
    if (ehNo(o)) {
        if (o->pai) removerFilho(o->pai, o);
        while (!o->filhos.empty()) destruirObjeto(o->filhos.back());
        if (o->noHost) host().destruirNo(o->noHost);
    }
    {
        std::lock_guard<std::recursive_mutex> lk(g_mtx);
        objetos().erase(o->id);
    }
    // Instancia da extensao primeiro, depois as ligacoes (ordem do motor).
    if (o->instancia) {
        for (Classe* c = o->classe; c; c = c->pai) {
            if (c->extensao && c->info.free_instance_func) {
                c->info.free_instance_func(c->info.class_userdata, o->instancia);
                break;
            }
        }
        o->instancia = nullptr;
    }
    for (auto& l : o->ligacoes)
        if (l.callbacks && l.callbacks->free_callback) l.callbacks->free_callback(l.token, o, l.binding);
    o->ligacoes.clear();
    delete o;
}

bool refIniciar(Objeto* o) {
    if (o->refInicial) {
        o->refInicial = false;
        return true;
    }
    return refIncrementar(o);
}

bool refIncrementar(Objeto* o) {
    o->ref.fetch_add(1);
    for (auto& l : o->ligacoes)
        if (l.callbacks && l.callbacks->reference_callback) l.callbacks->reference_callback(l.token, l.binding, 1);
    return true;
}

bool refDecrementar(Objeto* o) {
    const bool zerou = o->ref.fetch_sub(1) == 1;
    for (auto& l : o->ligacoes)
        if (l.callbacks && l.callbacks->reference_callback) l.callbacks->reference_callback(l.token, l.binding, 0);
    return zerou;
}

void notificar(Objeto* o, int32_t que, bool reverso) {
    if (!o || !o->instancia) return;
    // A extensao propaga para as proprias classes-base (notification_bind).
    for (Classe* c = o->classe; c; c = c->pai) {
        if (c->extensao && c->info.notification_func) {
            c->info.notification_func(o->instancia, que, reverso ? 1 : 0);
            return;
        }
    }
}

static uint32_t hashVirtual(const Classe* c, const char* nome) {
    for (const Classe* k = c; k; k = k->pai) {
        for (const auto& v : kVirtuaisApi)
            if (k->nome->utf8 == v.classe && std::strcmp(nome, v.metodo) == 0) return v.hash;
    }
    return 0;
}

GDExtensionClassCallVirtual virtualDe(Objeto* o, const char* nome) {
    if (!o || !o->instancia) return nullptr;
    const NomeInterno* n = internarUtf8(nome);
    auto it = o->virtuais.find(n);
    if (it != o->virtuais.end()) return it->second;
    GDExtensionClassCallVirtual fn = nullptr;
    const uint32_t hash = hashVirtual(o->classe, nome);
    GStringName sn{n};
    for (Classe* c = o->classe; c && !fn; c = c->pai) {
        if (c->extensao && c->info.get_virtual_func) {
            fn = c->info.get_virtual_func(c->info.class_userdata, &sn, hash);
            break; // o godot-cpp ja' sobe a hierarquia das classes dele
        }
    }
    o->virtuais[n] = fn;
    return fn;
}

const MetodoExtensao* metodoExtensao(const Classe* c, const NomeInterno* nome) {
    for (; c; c = c->pai) {
        auto it = c->metodos.find(nome);
        if (it != c->metodos.end()) return &it->second;
    }
    return nullptr;
}

bool chamarMetodo(Objeto* o, const NomeInterno* nome, const GVariant* const* args, int argc, GVariant* ret,
                  GDExtensionCallError* erro) {
    erro->error = GDEXTENSION_CALL_OK;
    if (const MetodoExtensao* m = metodoExtensao(o->classe, nome)) {
        if (!m->chamar) {
            erro->error = GDEXTENSION_CALL_ERROR_INVALID_METHOD;
            return false;
        }
        m->chamar(m->userdata, o->instancia, reinterpret_cast<const GDExtensionConstVariantPtr*>(args), argc, ret, erro);
        return erro->error == GDEXTENSION_CALL_OK;
    }
    if (MetodoMotor* mm = metodoMotorPorCadeia(o->classe, nome->utf8)) {
        chamarMotorVariant(mm, o, args, argc, ret, erro);
        return erro->error == GDEXTENSION_CALL_OK;
    }
    erro->error = GDEXTENSION_CALL_ERROR_INVALID_METHOD;
    return false;
}

// ---- Arvore de nos ------------------------------------------------------------------

bool ehNo(const Objeto* o) {
    static Classe* no = classePorNome("Node");
    return o && o->classe && no && o->classe->herdaDe(no);
}

static bool ehDe(const Objeto* o, const char* classe) {
    Classe* c = classePorNome(classe);
    return o && o->classe && c && o->classe->herdaDe(c);
}

Objeto* raiz() {
    static Objeto* r = [] {
        Objeto* o = criarObjetoNativo(classePorNome("Window"));
        o->nome = "root";
        o->naArvore = true;
        o->noHost = host().criarNo("root", kSemNo);
        return o;
    }();
    return r;
}

void sincronizarComHost(Objeto* o) {
    if (!o->noHost) return;
    Transformacao t;
    if (ehDe(o, "Node2D")) {
        t.posicao = {o->pos[0], o->pos[1], 0.0f};
        const float a = o->rot[2] * 0.5f;
        t.rotacao = {0.0f, 0.0f, std::sin(a), std::cos(a)};
        t.escala = {o->escala[0], o->escala[1], 1.0f};
    } else if (ehDe(o, "Node3D")) {
        t.posicao = {o->pos[0], o->pos[1], o->pos[2]};
        // Euler YXZ (convencao do Node3D) -> quaternion.
        const float cx = std::cos(o->rot[0] * 0.5f), sx = std::sin(o->rot[0] * 0.5f);
        const float cy = std::cos(o->rot[1] * 0.5f), sy = std::sin(o->rot[1] * 0.5f);
        const float cz = std::cos(o->rot[2] * 0.5f), sz = std::sin(o->rot[2] * 0.5f);
        t.rotacao = {sx * cy * cz + cx * sy * sz, cx * sy * cz - sx * cy * sz, cx * cy * sz - sx * sy * cz,
                     cx * cy * cz + sx * sy * sz};
        t.escala = {o->escala[0], o->escala[1], o->escala[2]};
    } else {
        return;
    }
    host().definirTransformacao(o->noHost, t);
}

void entrarArvore(Objeto* o) {
    o->naArvore = true;
    if (!o->noHost) o->noHost = host().criarNo(o->nome, o->pai ? o->pai->noHost : kSemNo);
    sincronizarComHost(o);
    notificar(o, 10); // NOTIFICATION_ENTER_TREE
    if (auto fn = virtualDe(o, "_enter_tree")) fn(o->instancia, nullptr, nullptr);
    // Como no motor: quem sobrescreve _process/_physics_process ja' entra processando.
    if (virtualDe(o, "_process")) o->processar = true;
    if (virtualDe(o, "_physics_process")) o->processarFisica = true;
    for (Objeto* f : std::vector<Objeto*>(o->filhos)) entrarArvore(f);
    if (!o->prontoAvisado) {
        o->prontoAvisado = true;
        notificar(o, 13); // NOTIFICATION_READY
        if (auto fn = virtualDe(o, "_ready")) fn(o->instancia, nullptr, nullptr);
    }
}

void sairArvore(Objeto* o) {
    for (Objeto* f : std::vector<Objeto*>(o->filhos)) sairArvore(f);
    if (auto fn = virtualDe(o, "_exit_tree")) fn(o->instancia, nullptr, nullptr);
    notificar(o, 11); // NOTIFICATION_EXIT_TREE
    o->naArvore = false;
}

void adicionarFilho(Objeto* pai, Objeto* filho) {
    if (!pai || !filho || filho->pai) return;
    filho->pai = pai;
    pai->filhos.push_back(filho);
    if (filho->noHost) host().definirPai(filho->noHost, pai->noHost);
    if (pai->naArvore) entrarArvore(filho);
}

void removerFilho(Objeto* pai, Objeto* filho) {
    auto it = std::find(pai->filhos.begin(), pai->filhos.end(), filho);
    if (it == pai->filhos.end()) return;
    if (filho->naArvore) sairArvore(filho);
    pai->filhos.erase(it);
    filho->pai = nullptr;
}

static void coletarAtivos(Objeto* o, std::vector<Objeto*>& lista) {
    lista.push_back(o);
    for (Objeto* f : o->filhos) coletarAtivos(f, lista);
}

void processarArvore(double dt) {
    std::vector<Objeto*> nos;
    coletarAtivos(raiz(), nos);
    for (Objeto* o : nos) {
        if (!objetoVivo(o) || !o->naArvore) continue;
        if (o->processarFisica) {
            notificar(o, 16); // NOTIFICATION_PHYSICS_PROCESS
            if (auto fn = virtualDe(o, "_physics_process")) {
                const void* args[1] = {&dt};
                fn(o->instancia, args, nullptr);
            }
        }
        if (o->processar) {
            notificar(o, 17); // NOTIFICATION_PROCESS
            if (auto fn = virtualDe(o, "_process")) {
                const void* args[1] = {&dt};
                fn(o->instancia, args, nullptr);
            }
        }
        sincronizarComHost(o);
    }
    // queue_free(): libera no fim do quadro, como o motor.
    for (Objeto* o : nos)
        if (objetoVivo(o) && o->liberarNoFim) destruirObjeto(o);
}

// ---- Metodos nativos ---------------------------------------------------------------------

namespace {

constexpr int T_NIL = GDEXTENSION_VARIANT_TYPE_NIL;
constexpr int T_BOOL = GDEXTENSION_VARIANT_TYPE_BOOL;
constexpr int T_INT = GDEXTENSION_VARIANT_TYPE_INT;
constexpr int T_FLOAT = GDEXTENSION_VARIANT_TYPE_FLOAT;
constexpr int T_STRING = GDEXTENSION_VARIANT_TYPE_STRING;
constexpr int T_V2 = GDEXTENSION_VARIANT_TYPE_VECTOR2;
constexpr int T_V3 = GDEXTENSION_VARIANT_TYPE_VECTOR3;
constexpr int T_SN = GDEXTENSION_VARIANT_TYPE_STRING_NAME;
constexpr int T_OBJ = GDEXTENSION_VARIANT_TYPE_OBJECT;
constexpr int T_T3D = GDEXTENSION_VARIANT_TYPE_TRANSFORM3D;

// Transform3D do motor: Basis em 3 linhas (Vector3 cada) e a origem.
struct Transf3D {
    float b[3][3];
    float o[3];
};
static_assert(sizeof(Transf3D) == 48, "Transform3D tem 48 bytes na ABI");

// Transformacao LOCAL de um Node3D como o motor monta: rotacao de Euler na
// ordem YXZ (Ry * Rx * Rz) e a escala aplicada antes (colunas escaladas).
Transf3D transfLocal(const Objeto* o) {
    const float cx = std::cos(o->rot[0]), sx = std::sin(o->rot[0]);
    const float cy = std::cos(o->rot[1]), sy = std::sin(o->rot[1]);
    const float cz = std::cos(o->rot[2]), sz = std::sin(o->rot[2]);
    const float rx[3][3] = {{1, 0, 0}, {0, cx, -sx}, {0, sx, cx}};
    const float ry[3][3] = {{cy, 0, sy}, {0, 1, 0}, {-sy, 0, cy}};
    const float rz[3][3] = {{cz, -sz, 0}, {sz, cz, 0}, {0, 0, 1}};
    float yx[3][3] = {}, r[3][3] = {};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k) yx[i][j] += ry[i][k] * rx[k][j];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k) r[i][j] += yx[i][k] * rz[k][j];
    Transf3D t;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) t.b[i][j] = r[i][j] * o->escala[j];
        t.o[i] = o->pos[i];
    }
    return t;
}

Transf3D compor(const Transf3D& pai, const Transf3D& f) {
    Transf3D t;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j)
            t.b[i][j] = pai.b[i][0] * f.b[0][j] + pai.b[i][1] * f.b[1][j] + pai.b[i][2] * f.b[2][j];
        t.o[i] = pai.b[i][0] * f.o[0] + pai.b[i][1] * f.o[1] + pai.b[i][2] * f.o[2] + pai.o[i];
    }
    return t;
}

// Global: a local composta com a dos pais Node3D. Como no motor, a cadeia
// para no primeiro pai que nao e' Node3D (abaixo dele o no e' raiz).
Transf3D transfGlobal(const Objeto* o) {
    Transf3D t = transfLocal(o);
    static Classe* node3d = classePorNome("Node3D");
    for (const Objeto* p = o->pai; p && node3d && p->classe && p->classe->herdaDe(node3d); p = p->pai)
        t = compor(transfLocal(p), t);
    return t;
}

// RandomNumberGenerator: estado PCG32 por objeto (o motor usa PCG32 tambem;
// a sequencia nao precisa bater com a dele, so' ser uniforme).
struct EstadoRng {
    uint64_t estado = 0x853c49e6748fea9bull, inc = 0xda3e39cb94b95bdbull, semente = 0;
};
std::unordered_map<uint64_t, EstadoRng>& estadosRng() {
    static std::unordered_map<uint64_t, EstadoRng> m;
    return m;
}
EstadoRng& rngDe(const Objeto* o) {
    static std::mutex mtx;
    std::lock_guard<std::mutex> lk(mtx);
    return estadosRng()[o->id];
}
uint32_t rngProximo(EstadoRng& g) {
    const uint64_t velho = g.estado;
    g.estado = velho * 6364136223846793005ull + (g.inc | 1u);
    const uint32_t xs = static_cast<uint32_t>(((velho >> 18u) ^ velho) >> 27u);
    const uint32_t rot = static_cast<uint32_t>(velho >> 59u);
    return (xs >> rot) | (xs << ((32u - rot) & 31u));
}
void rngSemear(EstadoRng& g, uint64_t s) {
    g.semente = s;
    g.estado = 0;
    rngProximo(g);
    g.estado += s;
    rngProximo(g);
}
double rngF(EstadoRng& g) {
    return static_cast<double>(rngProximo(g)) / 4294967296.0;   // [0, 1)
}

void retTexto(void* r, const std::string& utf8) {
    GString* s = static_cast<GString*>(r);
    delete s->s;
    s->s = utf8.empty() ? nullptr : new std::u32string(utf8ParaU32(utf8.data(), utf8.size()));
}
bool argB(const void* const* a, int i) {
    return *static_cast<const uint8_t*>(a[i]) != 0;
}
int64_t argI(const void* const* a, int i) {
    return *static_cast<const int64_t*>(a[i]);
}
double argD(const void* const* a, int i) {
    return *static_cast<const double*>(a[i]);
}
Objeto* argO(const void* const* a, int i) {
    return *static_cast<Objeto* const*>(a[i]);
}
void rB(void* r, bool v) {
    if (r) *static_cast<uint8_t*>(r) = v ? 1 : 0;
}
void rI(void* r, int64_t v) {
    if (r) *static_cast<int64_t*>(r) = v;
}
void rD(void* r, double v) {
    if (r) *static_cast<double*>(r) = v;
}
void rO(void* r, Objeto* o) {
    if (r) *static_cast<Objeto**>(r) = o;
}

MetodoMotor mm(const char* classe, const char* nome, int ret, std::vector<int> args, PtrcallMotor fn) {
    MetodoMotor m;
    m.classe = classe;
    m.nome = nome;
    m.fn = fn;
    m.tipoRetorno = ret;
    m.tiposArgs = std::move(args);
    return m;
}

// Corpo de metodo nativo: self = objeto, a = argumentos crus, r = retorno.
#define L(...) [](Objeto* self, const void* const* a, void* r) { (void)self; (void)a; (void)r; __VA_ARGS__; }

std::vector<MetodoMotor>& metodosMotor() {
    static std::vector<MetodoMotor> m = {
        // Object
        mm("Object", "get_class", T_STRING, {}, L(retTexto(r, self->classe->nome->utf8))),
        mm("Object", "is_class", T_BOOL, {T_STRING}, L(const GString* s = static_cast<const GString*>(a[0]);
            Classe* c = classePorNome(s->s ? u32ParaUtf8(*s->s) : std::string());
            rB(r, c && self->classe->herdaDe(c));)),
        mm("Object", "get_instance_id", T_INT, {}, L(rI(r, static_cast<int64_t>(self->id)))),
        mm("Object", "notification", T_NIL, {T_INT, T_BOOL}, L(notificar(self, static_cast<int32_t>(argI(a, 0)), argB(a, 1)))),
        mm("Object", "is_queued_for_deletion", T_BOOL, {}, L(rB(r, self->liberarNoFim))),
        // RefCounted
        mm("RefCounted", "init_ref", T_BOOL, {}, L(rB(r, refIniciar(self)))),
        mm("RefCounted", "reference", T_BOOL, {}, L(rB(r, refIncrementar(self)))),
        mm("RefCounted", "unreference", T_BOOL, {}, L(rB(r, refDecrementar(self)))),
        mm("RefCounted", "get_reference_count", T_INT, {}, L(rI(r, self->ref.load()))),
        // Node
        mm("Node", "get_name", T_SN, {}, L(GStringName* s = static_cast<GStringName*>(r);
            s->n = internarUtf8(self->nome);)),
        mm("Node", "set_name", T_NIL, {T_STRING}, L(const GString* s = static_cast<const GString*>(a[0]);
            self->nome = s->s ? u32ParaUtf8(*s->s) : std::string();
            if (self->noHost) host().definirNome(self->noHost, self->nome);)),
        mm("Node", "add_child", T_NIL, {T_OBJ, T_BOOL, T_INT}, L(adicionarFilho(self, argO(a, 0)))),
        mm("Node", "remove_child", T_NIL, {T_OBJ}, L(removerFilho(self, argO(a, 0)))),
        mm("Node", "get_child_count", T_INT, {T_BOOL}, L(rI(r, static_cast<int64_t>(self->filhos.size())))),
        mm("Node", "get_child", T_OBJ, {T_INT, T_BOOL}, L(int64_t i = argI(a, 0);
            if (i < 0) i += static_cast<int64_t>(self->filhos.size());
            rO(r, (i >= 0 && static_cast<size_t>(i) < self->filhos.size()) ? self->filhos[static_cast<size_t>(i)] : nullptr);)),
        mm("Node", "get_parent", T_OBJ, {}, L(rO(r, self->pai))),
        mm("Node", "is_inside_tree", T_BOOL, {}, L(rB(r, self->naArvore))),
        mm("Node", "is_node_ready", T_BOOL, {}, L(rB(r, self->prontoAvisado))),
        mm("Node", "set_process", T_NIL, {T_BOOL}, L(self->processar = argB(a, 0))),
        mm("Node", "is_processing", T_BOOL, {}, L(rB(r, self->processar))),
        mm("Node", "set_physics_process", T_NIL, {T_BOOL}, L(self->processarFisica = argB(a, 0))),
        mm("Node", "is_physics_processing", T_BOOL, {}, L(rB(r, self->processarFisica))),
        mm("Node", "get_process_delta_time", T_FLOAT, {}, L(rD(r, host().dt()))),
        mm("Node", "get_physics_process_delta_time", T_FLOAT, {}, L(rD(r, host().dt()))),
        mm("Node", "queue_free", T_NIL, {}, L(self->liberarNoFim = true)),
        // Node2D
        mm("Node2D", "set_position", T_NIL, {T_V2}, L(std::memcpy(self->pos, a[0], 8))),
        mm("Node2D", "get_position", T_V2, {}, L(std::memcpy(r, self->pos, 8))),
        mm("Node2D", "set_rotation", T_NIL, {T_FLOAT}, L(self->rot[2] = static_cast<float>(argD(a, 0)))),
        mm("Node2D", "get_rotation", T_FLOAT, {}, L(rD(r, self->rot[2]))),
        mm("Node2D", "set_scale", T_NIL, {T_V2}, L(std::memcpy(self->escala, a[0], 8))),
        mm("Node2D", "get_scale", T_V2, {}, L(std::memcpy(r, self->escala, 8))),
        mm("Node2D", "rotate", T_NIL, {T_FLOAT}, L(self->rot[2] += static_cast<float>(argD(a, 0)))),
        mm("Node2D", "translate", T_NIL, {T_V2}, L(const float* v = static_cast<const float*>(a[0]);
            self->pos[0] += v[0];
            self->pos[1] += v[1];)),
        // Node3D
        mm("Node3D", "set_position", T_NIL, {T_V3}, L(std::memcpy(self->pos, a[0], 12))),
        mm("Node3D", "get_position", T_V3, {}, L(std::memcpy(r, self->pos, 12))),
        mm("Node3D", "set_rotation", T_NIL, {T_V3}, L(std::memcpy(self->rot, a[0], 12))),
        mm("Node3D", "get_rotation", T_V3, {}, L(std::memcpy(r, self->rot, 12))),
        mm("Node3D", "set_scale", T_NIL, {T_V3}, L(std::memcpy(self->escala, a[0], 12))),
        mm("Node3D", "get_scale", T_V3, {}, L(std::memcpy(r, self->escala, 12))),
        mm("Node3D", "rotate_x", T_NIL, {T_FLOAT}, L(self->rot[0] += static_cast<float>(argD(a, 0)))),
        mm("Node3D", "rotate_y", T_NIL, {T_FLOAT}, L(self->rot[1] += static_cast<float>(argD(a, 0)))),
        mm("Node3D", "rotate_z", T_NIL, {T_FLOAT}, L(self->rot[2] += static_cast<float>(argD(a, 0)))),
        mm("Node3D", "translate", T_NIL, {T_V3}, L(const float* v = static_cast<const float*>(a[0]);
            for (int i = 0; i < 3; ++i) self->pos[i] += v[i];)),
        // Aviso de transformacao (NOTIFICATION_TRANSFORM_CHANGED): a camada
        // nao entrega essa notificacao, entao ligar/desligar nao muda nada.
        mm("Node3D", "set_notify_transform", T_NIL, {T_BOOL}, L((void)0)),
        mm("Node3D", "is_transform_notification_enabled", T_BOOL, {}, L(rB(r, false))),
        mm("Node3D", "get_transform", T_T3D, {}, L(const Transf3D t = transfLocal(self); std::memcpy(r, &t, sizeof(t)))),
        mm("Node3D", "get_global_transform", T_T3D, {}, L(const Transf3D t = transfGlobal(self); std::memcpy(r, &t, sizeof(t)))),
        mm("Node3D", "get_global_position", T_V3, {}, L(const Transf3D t = transfGlobal(self); std::memcpy(r, t.o, 12))),
        // RandomNumberGenerator
        mm("RandomNumberGenerator", "randf", T_FLOAT, {}, L(rD(r, rngF(rngDe(self))))),
        mm("RandomNumberGenerator", "randf_range", T_FLOAT, {T_FLOAT, T_FLOAT}, L(const double lo = argD(a, 0), hi = argD(a, 1);
            rD(r, lo + (hi - lo) * rngF(rngDe(self))))),
        mm("RandomNumberGenerator", "randi", T_INT, {}, L(rI(r, static_cast<int64_t>(rngProximo(rngDe(self)))))),
        mm("RandomNumberGenerator", "randi_range", T_INT, {T_INT, T_INT}, L(int64_t lo = argI(a, 0), hi = argI(a, 1);
            if (lo > hi) std::swap(lo, hi);
            const uint64_t span = static_cast<uint64_t>(hi - lo) + 1u;
            rI(r, lo + static_cast<int64_t>(rngProximo(rngDe(self)) % span)))),
        mm("RandomNumberGenerator", "randomize", T_NIL, {}, L(rngSemear(rngDe(self), static_cast<uint64_t>(host().tempo() * 1e9) ^ self->id))),
        mm("RandomNumberGenerator", "set_seed", T_NIL, {T_INT}, L(rngSemear(rngDe(self), static_cast<uint64_t>(argI(a, 0))))),
        mm("RandomNumberGenerator", "get_seed", T_INT, {}, L(rI(r, static_cast<int64_t>(rngDe(self).semente)))),
        // Engine (singleton)
        mm("Engine", "is_editor_hint", T_BOOL, {}, L(rB(r, false))),
        mm("Engine", "get_frames_drawn", T_INT, {}, L(rI(r, static_cast<int64_t>(host().quadro())))),
        mm("Engine", "get_process_frames", T_INT, {}, L(rI(r, static_cast<int64_t>(host().quadro())))),
        mm("Engine", "get_physics_frames", T_INT, {}, L(rI(r, static_cast<int64_t>(host().quadro())))),
        mm("Engine", "get_frames_per_second", T_FLOAT, {}, L(rD(r, host().dt() > 0 ? 1.0 / host().dt() : 0.0))),
        // OS (singleton)
        mm("OS", "get_name", T_STRING, {}, L(retTexto(r, "Linux"))),
        mm("OS", "is_debug_build", T_BOOL, {}, L(rB(r, true))),
        mm("OS", "get_ticks_msec", T_INT, {}, L(rI(r, static_cast<int64_t>(host().tempo() * 1000.0)))),
        mm("OS", "get_ticks_usec", T_INT, {}, L(rI(r, static_cast<int64_t>(host().tempo() * 1e6)))),
        // Time (singleton)
        mm("Time", "get_ticks_msec", T_INT, {}, L(rI(r, static_cast<int64_t>(host().tempo() * 1000.0)))),
        mm("Time", "get_ticks_usec", T_INT, {}, L(rI(r, static_cast<int64_t>(host().tempo() * 1e6)))),
    };
    return m;
}

#undef L

std::vector<std::unique_ptr<MetodoMotor>>& metodosDesconhecidos() {
    static std::vector<std::unique_ptr<MetodoMotor>> v;
    return v;
}

} // namespace

MetodoMotor* metodoMotor(const std::string& classe, const std::string& nome) {
    for (auto& m : metodosMotor())
        if (m.classe == classe && m.nome == nome) return &m;
    return nullptr;
}

MetodoMotor* metodoMotorPorCadeia(const Classe* c, const std::string& nome) {
    for (; c; c = c->pai) {
        if (c->extensao) continue;
        if (MetodoMotor* m = metodoMotor(c->nome->utf8, nome)) return m;
    }
    return nullptr;
}

// Metodo nativo pedido pela extensao: o implementado, ou um registro vazio
// que avisa (uma vez, com nome) quando for chamado.
MetodoMotor* metodoMotorParaBind(const std::string& classe, const std::string& nome) {
    if (MetodoMotor* m = metodoMotor(classe, nome)) return m;
    static std::mutex mtx;
    std::lock_guard<std::mutex> lk(mtx);
    for (auto& m : metodosDesconhecidos())
        if (m->classe == classe && m->nome == nome) return m.get();
    auto m = std::make_unique<MetodoMotor>();
    m->classe = classe;
    m->nome = nome;
    metodosDesconhecidos().push_back(std::move(m));
    return metodosDesconhecidos().back().get();
}

void chamarMotorVariant(MetodoMotor* m, Objeto* o, const GVariant* const* args, int argc, GVariant* ret,
                        GDExtensionCallError* erro) {
    if (!m->fn) {
        avisarUmaVez("mb:" + m->classe + "." + m->nome, "NAO IMPLEMENTADO: metodo nativo %s.%s", m->classe.c_str(), m->nome.c_str());
        erro->error = GDEXTENSION_CALL_ERROR_INVALID_METHOD;
        return;
    }
    // Converte cada Variant para o tipo cru do argumento (como o motor faz).
    const size_t n = m->tiposArgs.size();
    std::vector<std::array<uint8_t, 64>> buf(n);
    std::vector<const void*> ptrs(n);
    for (size_t i = 0; i < n; ++i) {
        const int t = m->tiposArgs[i];
        if (static_cast<int>(i) < argc) {
            if (static_cast<int>(args[i]->tipo) == t) copiarValor(t, buf[i].data(), valorDe(args[i]));
            else {
                construirPadrao(t, buf[i].data());
                if (t == T_INT) *reinterpret_cast<int64_t*>(buf[i].data()) = static_cast<int64_t>(
                    args[i]->tipo == T_FLOAT ? *static_cast<const double*>(valorDe(args[i])) : 0.0);
                else if (t == T_FLOAT && args[i]->tipo == T_INT)
                    *reinterpret_cast<double*>(buf[i].data()) = static_cast<double>(*static_cast<const int64_t*>(valorDe(args[i])));
            }
        } else {
            construirPadrao(t, buf[i].data());
        }
        ptrs[i] = buf[i].data();
    }
    std::array<uint8_t, 64> r{};
    if (m->tipoRetorno != T_NIL) construirPadrao(m->tipoRetorno, r.data());
    m->fn(o, ptrs.data(), m->tipoRetorno != T_NIL ? r.data() : nullptr);
    variantNil(ret);
    if (m->tipoRetorno != T_NIL) {
        variantDe(ret, m->tipoRetorno, r.data());
        destruirValor(m->tipoRetorno, r.data());
    }
    for (size_t i = 0; i < n; ++i) destruirValor(m->tiposArgs[i], buf[i].data());
    erro->error = GDEXTENSION_CALL_OK;
}

Objeto* singleton(const NomeInterno* nome) {
    static std::mutex mtx;
    static std::unordered_map<const NomeInterno*, Objeto*> unicos;
    bool conhecido = false;
    for (const char* s : kSingletonsApi) conhecido = conhecido || nome->utf8 == s;
    if (!conhecido) return nullptr;
    std::lock_guard<std::mutex> lk(mtx);
    auto it = unicos.find(nome);
    if (it != unicos.end()) return it->second;
    Classe* c = classePorNome(nome);
    Objeto* o = c ? criarObjetoNativo(c) : nullptr;
    unicos[nome] = o;
    return o;
}

} // namespace eruption::compat::gd
