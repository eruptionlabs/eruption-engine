// Tipos builtin do host GDExtension: construtores por indice, operadores,
// metodos builtin por nome e funcoes utilitarias (print, sin, randf...).
//
// Convencao de ponteiro cru (ptrcall) da ABI: int vai como int64_t, float
// como double, bool como uint8_t; vetores e cores em float (real_t); String,
// Array etc. no layout opaco do GdNucleo.hpp. Retornos apontam para um valor
// JA construido (atribuicao), construtores para memoria crua.

#include "godot/GdNucleo.hpp"
#include "host/Host.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <random>
#include <utility>

namespace eruption::compat::gd {

// ---- Trampolins -----------------------------------------------------------------

namespace {

constexpr size_t kSlots = 4096;
Slot g_slots[kSlots];
size_t g_slotsUsados = 0;
std::mutex g_slotsMtx;
std::unordered_map<std::string, Generica> g_porChave;

template <size_t N>
uintptr_t pulo(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d) {
    const Slot& s = g_slots[N];
    return s.fn ? s.fn(s, a, b, c, d) : 0;
}

template <size_t... Ns>
constexpr std::array<Generica, sizeof...(Ns)> fazerPulos(std::index_sequence<Ns...>) {
    return {{&pulo<Ns>...}};
}

const std::array<Generica, kSlots>& pulos() {
    static const std::array<Generica, kSlots> t = fazerPulos(std::make_index_sequence<kSlots>{});
    return t;
}

uintptr_t executarStub(const Slot& s, uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
    avisarUmaVez(s.msg, "NAO IMPLEMENTADO: %s", s.msg.c_str());
    return 0;
}

} // namespace

Generica trampolim(const std::string& chave, const Slot& s) {
    std::lock_guard<std::mutex> lk(g_slotsMtx);
    auto it = g_porChave.find(chave);
    if (it != g_porChave.end()) return it->second;
    if (g_slotsUsados >= kSlots) {
        host().log(Nivel::Erro, "godot", "trampolins esgotados");
        return nullptr;
    }
    const size_t i = g_slotsUsados++;
    g_slots[i] = s;
    Generica g = pulos()[i];
    g_porChave.emplace(chave, g);
    return g;
}

Generica stubFaltante(const std::string& chave, const std::string& msg) {
    Slot s;
    s.fn = &executarStub;
    s.msg = msg;
    return trampolim("stub:" + chave, s);
}

namespace {

// ---- Auxiliares de valor ------------------------------------------------------------

constexpr int NIL = GDEXTENSION_VARIANT_TYPE_NIL;
constexpr int BOOL = GDEXTENSION_VARIANT_TYPE_BOOL;
constexpr int INT = GDEXTENSION_VARIANT_TYPE_INT;
constexpr int FLOAT = GDEXTENSION_VARIANT_TYPE_FLOAT;
constexpr int STRING = GDEXTENSION_VARIANT_TYPE_STRING;
constexpr int VECTOR2 = GDEXTENSION_VARIANT_TYPE_VECTOR2;
constexpr int VECTOR2I = GDEXTENSION_VARIANT_TYPE_VECTOR2I;
constexpr int RECT2 = GDEXTENSION_VARIANT_TYPE_RECT2;
constexpr int RECT2I = GDEXTENSION_VARIANT_TYPE_RECT2I;
constexpr int VECTOR3 = GDEXTENSION_VARIANT_TYPE_VECTOR3;
constexpr int VECTOR3I = GDEXTENSION_VARIANT_TYPE_VECTOR3I;
constexpr int TRANSFORM2D = GDEXTENSION_VARIANT_TYPE_TRANSFORM2D;
constexpr int VECTOR4 = GDEXTENSION_VARIANT_TYPE_VECTOR4;
constexpr int VECTOR4I = GDEXTENSION_VARIANT_TYPE_VECTOR4I;
constexpr int PLANE = GDEXTENSION_VARIANT_TYPE_PLANE;
constexpr int QUATERNION = GDEXTENSION_VARIANT_TYPE_QUATERNION;
constexpr int AABB = GDEXTENSION_VARIANT_TYPE_AABB;
constexpr int BASIS = GDEXTENSION_VARIANT_TYPE_BASIS;
constexpr int TRANSFORM3D = GDEXTENSION_VARIANT_TYPE_TRANSFORM3D;
constexpr int PROJECTION = GDEXTENSION_VARIANT_TYPE_PROJECTION;
constexpr int COLOR = GDEXTENSION_VARIANT_TYPE_COLOR;
constexpr int STRING_NAME = GDEXTENSION_VARIANT_TYPE_STRING_NAME;
constexpr int NODE_PATH = GDEXTENSION_VARIANT_TYPE_NODE_PATH;
constexpr int OBJECT = GDEXTENSION_VARIANT_TYPE_OBJECT;
constexpr int CALLABLE = GDEXTENSION_VARIANT_TYPE_CALLABLE;
constexpr int SIGNAL = GDEXTENSION_VARIANT_TYPE_SIGNAL;
constexpr int DICTIONARY = GDEXTENSION_VARIANT_TYPE_DICTIONARY;
constexpr int ARRAY = GDEXTENSION_VARIANT_TYPE_ARRAY;
constexpr int PACKED_STRING = GDEXTENSION_VARIANT_TYPE_PACKED_STRING_ARRAY;

const std::u32string& txt(const void* p) {
    static const std::u32string vazio;
    const GString* s = static_cast<const GString*>(p);
    return s->s ? *s->s : vazio;
}

// Atribui um texto a uma String ja' construida.
void atribuirTexto(void* r, std::u32string t) {
    GString* s = static_cast<GString*>(r);
    delete s->s;
    s->s = t.empty() ? nullptr : new std::u32string(std::move(t));
}

void atribuirVariant(void* r, const GVariant* v) {
    GVariant* d = static_cast<GVariant*>(r);
    variantDestruir(d);
    variantCopia(d, v);
}

std::u32string ascii(const std::string& s) {
    std::u32string r;
    for (char c : s) r.push_back(static_cast<unsigned char>(c));
    return r;
}

double comoDouble(const GVariant* v) {
    const void* p = valorDe(v);
    if (v->tipo == INT) return static_cast<double>(*static_cast<const int64_t*>(p));
    if (v->tipo == FLOAT) return *static_cast<const double*>(p);
    if (v->tipo == BOOL) return *static_cast<const uint8_t*>(p) ? 1.0 : 0.0;
    return 0.0;
}

// Componentes numericos de um tipo de valor: quantidade e se sao inteiros.
bool componentes(int tipo, int& n, char& tipoComp) {
    switch (tipo) {
    case BOOL: n = 1; tipoComp = 'b'; return true;
    case INT: n = 1; tipoComp = 'l'; return true;
    case FLOAT: n = 1; tipoComp = 'd'; return true;
    case VECTOR2: n = 2; tipoComp = 'f'; return true;
    case VECTOR2I: n = 2; tipoComp = 'i'; return true;
    case RECT2: n = 4; tipoComp = 'f'; return true;
    case RECT2I: n = 4; tipoComp = 'i'; return true;
    case VECTOR3: n = 3; tipoComp = 'f'; return true;
    case VECTOR3I: n = 3; tipoComp = 'i'; return true;
    case TRANSFORM2D: n = 6; tipoComp = 'f'; return true;
    case VECTOR4: n = 4; tipoComp = 'f'; return true;
    case VECTOR4I: n = 4; tipoComp = 'i'; return true;
    case PLANE: n = 4; tipoComp = 'f'; return true;
    case QUATERNION: n = 4; tipoComp = 'f'; return true;
    case AABB: n = 6; tipoComp = 'f'; return true;
    case BASIS: n = 9; tipoComp = 'f'; return true;
    case TRANSFORM3D: n = 12; tipoComp = 'f'; return true;
    case PROJECTION: n = 16; tipoComp = 'f'; return true;
    case COLOR: n = 4; tipoComp = 'f'; return true;
    default: return false;
    }
}

double lerComp(const void* p, char k, int i) {
    switch (k) {
    case 'b': return static_cast<const uint8_t*>(p)[i] ? 1.0 : 0.0;
    case 'l': return static_cast<double>(static_cast<const int64_t*>(p)[i]);
    case 'd': return static_cast<const double*>(p)[i];
    case 'i': return static_cast<const int32_t*>(p)[i];
    default: return static_cast<const float*>(p)[i];
    }
}

void escreverComp(void* p, char k, int i, double v) {
    switch (k) {
    case 'b': static_cast<uint8_t*>(p)[i] = v != 0.0; break;
    case 'l': static_cast<int64_t*>(p)[i] = static_cast<int64_t>(v); break;
    case 'd': static_cast<double*>(p)[i] = v; break;
    case 'i': static_cast<int32_t*>(p)[i] = static_cast<int32_t>(v); break;
    default: static_cast<float*>(p)[i] = static_cast<float>(v); break;
    }
}

// ---- Construtores -------------------------------------------------------------------

// Assinaturas dos construtores por tipo e indice (fatos da API publica,
// conferidos com o extension_api.json 4.5).
const char* const* assinaturas(int tipo) {
    static const char* const nil[] = {"", "Variant", nullptr};
    static const char* const b[] = {"", "bool", "int", "float", nullptr};
    static const char* const i[] = {"", "int", "float", "bool", "String", nullptr};
    static const char* const f[] = {"", "float", "int", "bool", "String", nullptr};
    static const char* const s[] = {"", "String", "StringName", "NodePath", nullptr};
    static const char* const v2[] = {"", "Vector2", "Vector2i", "float,float", nullptr};
    static const char* const v2i[] = {"", "Vector2i", "Vector2", "int,int", nullptr};
    static const char* const r2[] = {"", "Rect2", "Rect2i", "Vector2,Vector2", "float,float,float,float", nullptr};
    static const char* const r2i[] = {"", "Rect2i", "Rect2", "Vector2i,Vector2i", "int,int,int,int", nullptr};
    static const char* const v3[] = {"", "Vector3", "Vector3i", "float,float,float", nullptr};
    static const char* const v3i[] = {"", "Vector3i", "Vector3", "int,int,int", nullptr};
    static const char* const t2[] = {"", "Transform2D", "float,Vector2", "float,Vector2,float,Vector2", "Vector2,Vector2,Vector2", nullptr};
    static const char* const v4[] = {"", "Vector4", "Vector4i", "float,float,float,float", nullptr};
    static const char* const v4i[] = {"", "Vector4i", "Vector4", "int,int,int,int", nullptr};
    static const char* const pl[] = {"", "Plane", "Vector3", "Vector3,float", "Vector3,Vector3", "Vector3,Vector3,Vector3", "float,float,float,float", nullptr};
    static const char* const q[] = {"", "Quaternion", "Basis", "Vector3,float", "Vector3,Vector3", "float,float,float,float", nullptr};
    static const char* const ab[] = {"", "AABB", "Vector3,Vector3", nullptr};
    static const char* const ba[] = {"", "Basis", "Quaternion", "Vector3,float", "Vector3,Vector3,Vector3", nullptr};
    static const char* const t3[] = {"", "Transform3D", "Basis,Vector3", "Vector3,Vector3,Vector3,Vector3", "Projection", nullptr};
    static const char* const pr[] = {"", "Projection", "Transform3D", "Vector4,Vector4,Vector4,Vector4", nullptr};
    static const char* const co[] = {"", "Color", "Color,float", "float,float,float", "float,float,float,float", "String", "String,float", nullptr};
    static const char* const sn[] = {"", "StringName", "String", nullptr};
    static const char* const np[] = {"", "NodePath", "String", nullptr};
    static const char* const rid[] = {"", "RID", nullptr};
    static const char* const ca[] = {"", "Callable", "Object,StringName", nullptr};
    static const char* const si[] = {"", "Signal", "Object,StringName", nullptr};
    static const char* const di[] = {"", "Dictionary", "Dictionary,int,StringName,Variant,int,StringName,Variant", nullptr};
    static const char* const ar[] = {"", "Array", "Array,int,StringName,Variant", "PackedByteArray", "PackedInt32Array",
                                     "PackedInt64Array", "PackedFloat32Array", "PackedFloat64Array", "PackedStringArray",
                                     "PackedVector2Array", "PackedVector3Array", "PackedColorArray", "PackedVector4Array", nullptr};
    static const char* const pk[] = {"", "<mesmo>", "Array", nullptr};
    switch (tipo) {
    case NIL: return nil;
    case BOOL: return b;
    case INT: return i;
    case FLOAT: return f;
    case STRING: return s;
    case VECTOR2: return v2;
    case VECTOR2I: return v2i;
    case RECT2: return r2;
    case RECT2I: return r2i;
    case VECTOR3: return v3;
    case VECTOR3I: return v3i;
    case TRANSFORM2D: return t2;
    case VECTOR4: return v4;
    case VECTOR4I: return v4i;
    case PLANE: return pl;
    case QUATERNION: return q;
    case AABB: return ab;
    case BASIS: return ba;
    case TRANSFORM3D: return t3;
    case PROJECTION: return pr;
    case COLOR: return co;
    case STRING_NAME: return sn;
    case NODE_PATH: return np;
    case GDEXTENSION_VARIANT_TYPE_RID: return rid;
    case CALLABLE: return ca;
    case SIGNAL: return si;
    case DICTIONARY: return di;
    case ARRAY: return ar;
    default: return ehPacked(tipo) ? pk : nullptr;
    }
}

int tipoPorNome(const std::string& n) {
    for (int t = 0; t < kTiposVariant; ++t)
        if (n == nomeTipo(t)) return t;
    return NIL; // "Variant"
}

std::vector<int> argsConstrutor(int tipo, int indice) {
    std::vector<int> r;
    const char* const* a = assinaturas(tipo);
    if (!a) return r;
    for (int k = 0; k < indice; ++k)
        if (!a[k]) return {-1};
    if (!a[indice]) return {-1};
    const std::string s = a[indice];
    if (s == "<mesmo>") return {tipo};
    size_t ini = 0;
    while (ini < s.size()) {
        size_t fim = s.find(',', ini);
        if (fim == std::string::npos) fim = s.size();
        r.push_back(tipoPorNome(s.substr(ini, fim - ini)));
        ini = fim + 1;
    }
    return r;
}

void quatParaBase(const float* q, float* m) {
    const float d = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
    const float s = d > 0.0f ? 2.0f / d : 0.0f;
    const float xs = q[0] * s, ys = q[1] * s, zs = q[2] * s;
    const float wx = q[3] * xs, wy = q[3] * ys, wz = q[3] * zs;
    const float xx = q[0] * xs, xy = q[0] * ys, xz = q[0] * zs;
    const float yy = q[1] * ys, yz = q[1] * zs, zz = q[2] * zs;
    const float r[9] = {1.0f - (yy + zz), xy - wz, xz + wy, xy + wz, 1.0f - (xx + zz), yz - wx, xz - wy, yz + wx, 1.0f - (xx + yy)};
    std::memcpy(m, r, sizeof(r));
}

void baseParaQuat(const float* m, float* q) {
    const float tr = m[0] + m[4] + m[8];
    if (tr > 0.0f) {
        const float s = std::sqrt(tr + 1.0f) * 2.0f;
        q[3] = 0.25f * s;
        q[0] = (m[7] - m[5]) / s;
        q[1] = (m[2] - m[6]) / s;
        q[2] = (m[3] - m[1]) / s;
    } else if (m[0] > m[4] && m[0] > m[8]) {
        const float s = std::sqrt(1.0f + m[0] - m[4] - m[8]) * 2.0f;
        q[3] = (m[7] - m[5]) / s;
        q[0] = 0.25f * s;
        q[1] = (m[1] + m[3]) / s;
        q[2] = (m[2] + m[6]) / s;
    } else if (m[4] > m[8]) {
        const float s = std::sqrt(1.0f + m[4] - m[0] - m[8]) * 2.0f;
        q[3] = (m[2] - m[6]) / s;
        q[0] = (m[1] + m[3]) / s;
        q[1] = 0.25f * s;
        q[2] = (m[5] + m[7]) / s;
    } else {
        const float s = std::sqrt(1.0f + m[8] - m[0] - m[4]) * 2.0f;
        q[3] = (m[3] - m[1]) / s;
        q[0] = (m[2] + m[6]) / s;
        q[1] = (m[5] + m[7]) / s;
        q[2] = 0.25f * s;
    }
}

void eixoAnguloParaQuat(const float* eixo, double ang, float* q) {
    const float s = static_cast<float>(std::sin(ang * 0.5));
    q[0] = eixo[0] * s;
    q[1] = eixo[1] * s;
    q[2] = eixo[2] * s;
    q[3] = static_cast<float>(std::cos(ang * 0.5));
}

// Copia um Array de Variants para um Packed (convertendo cada elemento).
void arrayParaPacked(int tipoPacked, const GArray* a, GPacked* p) {
    p->d = nullptr;
    p->livre = nullptr;
    DadoPacked* d = garantirPacked(tipoPacked, p);
    const size_t n = a->d ? a->d->itens.size() : 0;
    redimensionarPacked(d, n);
    for (size_t i = 0; i < n; ++i) {
        const GVariant* v = &a->d->itens[i];
        uint8_t* dst = d->bytes.data() + i * d->tamElemento;
        if (d->tipoElemento == STRING) {
            atribuirTexto(dst, variantTexto(v));
        } else if (d->tipoElemento == INT || d->tipoElemento == FLOAT) {
            const double x = comoDouble(v);
            if (tipoPacked == GDEXTENSION_VARIANT_TYPE_PACKED_BYTE_ARRAY) *dst = static_cast<uint8_t>(x);
            else if (tipoPacked == GDEXTENSION_VARIANT_TYPE_PACKED_INT32_ARRAY) *reinterpret_cast<int32_t*>(dst) = static_cast<int32_t>(x);
            else if (tipoPacked == GDEXTENSION_VARIANT_TYPE_PACKED_INT64_ARRAY) *reinterpret_cast<int64_t*>(dst) = static_cast<int64_t>(x);
            else if (tipoPacked == GDEXTENSION_VARIANT_TYPE_PACKED_FLOAT32_ARRAY) *reinterpret_cast<float*>(dst) = static_cast<float>(x);
            else *reinterpret_cast<double*>(dst) = x;
        } else if (static_cast<int>(v->tipo) == d->tipoElemento) {
            std::memcpy(dst, valorDe(v), d->tamElemento);
        }
    }
}

// Elemento i de um Packed como Variant.
void elementoComoVariant(int tipoPacked, const DadoPacked* d, size_t i, GVariant* out) {
    const uint8_t* src = d->bytes.data() + i * d->tamElemento;
    switch (tipoPacked) {
    case GDEXTENSION_VARIANT_TYPE_PACKED_BYTE_ARRAY: variantInt(out, *src); return;
    case GDEXTENSION_VARIANT_TYPE_PACKED_INT32_ARRAY: variantInt(out, *reinterpret_cast<const int32_t*>(src)); return;
    case GDEXTENSION_VARIANT_TYPE_PACKED_INT64_ARRAY: variantInt(out, *reinterpret_cast<const int64_t*>(src)); return;
    case GDEXTENSION_VARIANT_TYPE_PACKED_FLOAT32_ARRAY: variantFloat(out, *reinterpret_cast<const float*>(src)); return;
    case GDEXTENSION_VARIANT_TYPE_PACKED_FLOAT64_ARRAY: variantFloat(out, *reinterpret_cast<const double*>(src)); return;
    default: variantDe(out, d->tipoElemento, src); return;
    }
}

void packedParaArray(int tipoPacked, const GPacked* p, GArray* a) {
    a->d = new DadoArray();
    const size_t n = p->d ? p->d->tamanho() : 0;
    a->d->itens.resize(n);
    for (size_t i = 0; i < n; ++i) elementoComoVariant(tipoPacked, p->d, i, &a->d->itens[i]);
}

struct ParObjNome {
    Objeto* obj;
    const NomeInterno* nome;
};
static_assert(sizeof(ParObjNome) == 16, "Callable/Signal tem 16 bytes na ABI");

bool construir(int T, int I, void* base, const void* const* args) {
    const std::vector<int> sig = argsConstrutor(T, I);
    if (!sig.empty() && sig[0] == -1) return false;
    if (sig.empty()) {
        construirPadrao(T, base);
        return true;
    }
    if (sig.size() == 1 && sig[0] == T) {
        copiarValor(T, base, args[0]);
        return true;
    }
    if (T == STRING || T == NODE_PATH) {
        GString* s = static_cast<GString*>(base);
        s->s = nullptr;
        if (sig[0] == STRING_NAME) atribuirTexto(s, static_cast<const GStringName*>(args[0])->n->texto);
        else atribuirTexto(s, txt(args[0]));
        return true;
    }
    if (T == STRING_NAME) {
        static_cast<GStringName*>(base)->n = internar(txt(args[0]));
        return true;
    }
    if ((T == INT || T == FLOAT || T == BOOL) && sig[0] == STRING) {
        const std::string u = u32ParaUtf8(txt(args[0]));
        if (T == INT) *static_cast<int64_t*>(base) = std::strtoll(u.c_str(), nullptr, 10);
        else *static_cast<double*>(base) = std::strtod(u.c_str(), nullptr);
        return true;
    }
    if (T == ARRAY) {
        if (sig[0] == ARRAY) {
            copiarValor(ARRAY, base, args[0]);
            return true;
        }
        packedParaArray(sig[0], static_cast<const GPacked*>(args[0]), static_cast<GArray*>(base));
        return true;
    }
    if (ehPacked(T) && sig[0] == ARRAY) {
        arrayParaPacked(T, static_cast<const GArray*>(args[0]), static_cast<GPacked*>(base));
        return true;
    }
    if (T == DICTIONARY) {
        copiarValor(DICTIONARY, base, args[0]);
        return true;
    }
    if (T == CALLABLE || T == SIGNAL) {
        ParObjNome* p = static_cast<ParObjNome*>(base);
        p->obj = *static_cast<Objeto* const*>(args[0]);
        p->nome = static_cast<const GStringName*>(args[1])->n;
        return true;
    }
    float* f = static_cast<float*>(base);
    auto fl = [&](int k) { return static_cast<const float*>(args[k]); };
    auto dbl = [&](int k) { return *static_cast<const double*>(args[k]); };
    if (T == COLOR && I == 2) {
        std::memcpy(f, fl(0), 16);
        f[3] = static_cast<float>(dbl(1));
        return true;
    }
    if (T == COLOR && I == 3) {
        f[0] = static_cast<float>(dbl(0));
        f[1] = static_cast<float>(dbl(1));
        f[2] = static_cast<float>(dbl(2));
        f[3] = 1.0f;
        return true;
    }
    if (T == BASIS && I == 4) { // colunas -> linhas
        for (int c = 0; c < 3; ++c)
            for (int r = 0; r < 3; ++r) f[r * 3 + c] = fl(c)[r];
        return true;
    }
    if (T == TRANSFORM3D && I == 3) {
        for (int c = 0; c < 3; ++c)
            for (int r = 0; r < 3; ++r) f[r * 3 + c] = fl(c)[r];
        std::memcpy(f + 9, fl(3), 12);
        return true;
    }
    if (T == BASIS && I == 2) {
        quatParaBase(fl(0), f);
        return true;
    }
    if (T == BASIS && I == 3) {
        float q[4];
        eixoAnguloParaQuat(fl(0), dbl(1), q);
        quatParaBase(q, f);
        return true;
    }
    if (T == QUATERNION && I == 2) {
        baseParaQuat(fl(0), f);
        return true;
    }
    if (T == QUATERNION && I == 3) {
        eixoAnguloParaQuat(fl(0), dbl(1), f);
        return true;
    }
    if (T == TRANSFORM2D && I == 2) {
        const double r = dbl(0);
        f[0] = static_cast<float>(std::cos(r));
        f[1] = static_cast<float>(std::sin(r));
        f[2] = -f[1];
        f[3] = f[0];
        std::memcpy(f + 4, fl(1), 8);
        return true;
    }
    if (T == PLANE && I == 2) {
        std::memcpy(f, fl(0), 12);
        f[3] = 0.0f;
        return true;
    }
    if (T == PLANE && I == 4) {
        std::memcpy(f, fl(0), 12);
        f[3] = f[0] * fl(1)[0] + f[1] * fl(1)[1] + f[2] * fl(1)[2];
        return true;
    }
    // Concatenacao de componentes com conversao (Vector2(x, y), Rect2(pos, size),
    // Vector2(Vector2i), AABB(pos, size), Transform3D(Basis, origem)...).
    int nDest;
    char kDest;
    if (!componentes(T, nDest, kDest)) return false;
    std::vector<double> vals;
    for (size_t k = 0; k < sig.size(); ++k) {
        int n;
        char kk;
        if (!componentes(sig[k], n, kk)) return false;
        for (int c = 0; c < n; ++c) vals.push_back(lerComp(args[k], kk, c));
    }
    if (static_cast<int>(vals.size()) != nDest) return false;
    for (int c = 0; c < nDest; ++c) escreverComp(base, kDest, c, vals[static_cast<size_t>(c)]);
    return true;
}

uintptr_t executarConstrutor(const Slot& s, uintptr_t base, uintptr_t args, uintptr_t, uintptr_t) {
    void* b = reinterpret_cast<void*>(base);
    const void* const* a = reinterpret_cast<const void* const*>(args);
    if (!construir(s.a, s.b, b, a)) {
        avisarUmaVez(s.msg, "NAO IMPLEMENTADO: %s", s.msg.c_str());
        construirPadrao(s.a, b);
    }
    return 0;
}

// ---- Operadores ------------------------------------------------------------------------

std::u32string formatar(const std::u32string& fmt, const GVariant* const* vals, size_t n, bool& ok);

bool texto(int t) {
    return t == STRING || t == STRING_NAME || t == NODE_PATH;
}
bool numero(int t) {
    return t == INT || t == FLOAT || t == BOOL;
}

// Le um operando cru como Variant temporario (o tipo NIL significa "Variant").
void comoVariant(int tipo, const void* p, GVariant* out) {
    if (tipo == NIL) variantCopia(out, static_cast<const GVariant*>(p));
    else variantDe(out, tipo, p);
}

bool avaliar(int op, const GVariant* a, const GVariant* b, int tipoRet, void* r) {
    const int ta = static_cast<int>(a->tipo), tb = static_cast<int>(b->tipo);
    auto retBool = [&](bool v) {
        *static_cast<uint8_t*>(r) = v ? 1 : 0;
        return true;
    };
    switch (op) {
    case GDEXTENSION_VARIANT_OP_EQUAL: return retBool(variantIgual(a, b));
    case GDEXTENSION_VARIANT_OP_NOT_EQUAL: return retBool(!variantIgual(a, b));
    case GDEXTENSION_VARIANT_OP_NOT: return retBool(!variantBool(a));
    case GDEXTENSION_VARIANT_OP_AND: return retBool(variantBool(a) && variantBool(b));
    case GDEXTENSION_VARIANT_OP_OR: return retBool(variantBool(a) || variantBool(b));
    case GDEXTENSION_VARIANT_OP_XOR: return retBool(variantBool(a) != variantBool(b));
    default: break;
    }
    if (op >= GDEXTENSION_VARIANT_OP_LESS && op <= GDEXTENSION_VARIANT_OP_GREATER_EQUAL) {
        int cmp;
        if (numero(ta) && numero(tb)) {
            const double x = comoDouble(a), y = comoDouble(b);
            cmp = x < y ? -1 : (x > y ? 1 : 0);
        } else if (texto(ta) && texto(tb)) {
            const std::u32string x = variantTexto(a), y = variantTexto(b);
            cmp = x < y ? -1 : (x > y ? 1 : 0);
        } else {
            return false;
        }
        switch (op) {
        case GDEXTENSION_VARIANT_OP_LESS: return retBool(cmp < 0);
        case GDEXTENSION_VARIANT_OP_LESS_EQUAL: return retBool(cmp <= 0);
        case GDEXTENSION_VARIANT_OP_GREATER: return retBool(cmp > 0);
        default: return retBool(cmp >= 0);
        }
    }
    if (op == GDEXTENSION_VARIANT_OP_IN) {
        if (tb == ARRAY) {
            const DadoArray* d = static_cast<const GArray*>(valorDe(b))->d;
            for (size_t i = 0; d && i < d->itens.size(); ++i)
                if (variantIgual(a, &d->itens[i])) return retBool(true);
            return retBool(false);
        }
        if (texto(ta) && texto(tb)) return retBool(variantTexto(b).find(variantTexto(a)) != std::u32string::npos);
        return false;
    }
    // Texto: concatenacao e formatacao ("%d" % [x]).
    if (texto(ta) && op == GDEXTENSION_VARIANT_OP_ADD && texto(tb)) {
        atribuirTexto(r, variantTexto(a) + variantTexto(b));
        return true;
    }
    if (texto(ta) && op == GDEXTENSION_VARIANT_OP_MODULE) {
        bool ok = true;
        std::u32string s;
        if (tb == ARRAY) {
            const DadoArray* d = static_cast<const GArray*>(valorDe(b))->d;
            std::vector<const GVariant*> vs;
            for (size_t i = 0; d && i < d->itens.size(); ++i) vs.push_back(&d->itens[i]);
            s = formatar(variantTexto(a), vs.data(), vs.size(), ok);
        } else {
            const GVariant* um = b;
            s = formatar(variantTexto(a), &um, 1, ok);
        }
        atribuirTexto(r, s);
        return true;
    }
    // Numeros.
    if (numero(ta) && (numero(tb) || op == GDEXTENSION_VARIANT_OP_NEGATE || op == GDEXTENSION_VARIANT_OP_POSITIVE)) {
        const bool inteiro = tipoRet == INT;
        if (inteiro) {
            const int64_t x = static_cast<int64_t>(comoDouble(a)), y = static_cast<int64_t>(comoDouble(b));
            int64_t v;
            switch (op) {
            case GDEXTENSION_VARIANT_OP_ADD: v = x + y; break;
            case GDEXTENSION_VARIANT_OP_SUBTRACT: v = x - y; break;
            case GDEXTENSION_VARIANT_OP_MULTIPLY: v = x * y; break;
            case GDEXTENSION_VARIANT_OP_DIVIDE: v = y ? x / y : 0; break;
            case GDEXTENSION_VARIANT_OP_MODULE: v = y ? x % y : 0; break;
            case GDEXTENSION_VARIANT_OP_NEGATE: v = -x; break;
            case GDEXTENSION_VARIANT_OP_POSITIVE: v = x; break;
            case GDEXTENSION_VARIANT_OP_SHIFT_LEFT: v = x << y; break;
            case GDEXTENSION_VARIANT_OP_SHIFT_RIGHT: v = x >> y; break;
            case GDEXTENSION_VARIANT_OP_BIT_AND: v = x & y; break;
            case GDEXTENSION_VARIANT_OP_BIT_OR: v = x | y; break;
            case GDEXTENSION_VARIANT_OP_BIT_XOR: v = x ^ y; break;
            case GDEXTENSION_VARIANT_OP_BIT_NEGATE: v = ~x; break;
            case GDEXTENSION_VARIANT_OP_POWER: v = static_cast<int64_t>(std::pow(static_cast<double>(x), static_cast<double>(y))); break;
            default: return false;
            }
            *static_cast<int64_t*>(r) = v;
            return true;
        }
        const double x = comoDouble(a), y = comoDouble(b);
        double v;
        switch (op) {
        case GDEXTENSION_VARIANT_OP_ADD: v = x + y; break;
        case GDEXTENSION_VARIANT_OP_SUBTRACT: v = x - y; break;
        case GDEXTENSION_VARIANT_OP_MULTIPLY: v = x * y; break;
        case GDEXTENSION_VARIANT_OP_DIVIDE: v = x / y; break;
        case GDEXTENSION_VARIANT_OP_NEGATE: v = -x; break;
        case GDEXTENSION_VARIANT_OP_POSITIVE: v = x; break;
        case GDEXTENSION_VARIANT_OP_POWER: v = std::pow(x, y); break;
        default: return false;
        }
        *static_cast<double*>(r) = v;
        return true;
    }
    // Vetores (componente a componente; escalar a direita para * e /).
    int na, nb;
    char ka, kb;
    if (componentes(ta, na, ka) && na > 1 && ta != BASIS && ta != TRANSFORM2D && ta != TRANSFORM3D &&
        ta != PROJECTION && ta != QUATERNION) {
        const bool escalar = numero(tb);
        if (!escalar && !(componentes(tb, nb, kb) && nb == na) && op != GDEXTENSION_VARIANT_OP_NEGATE &&
            op != GDEXTENSION_VARIANT_OP_POSITIVE)
            return false;
        for (int c = 0; c < na; ++c) {
            const double x = lerComp(valorDe(a), ka, c);
            const double y = escalar ? comoDouble(b) : (tb == ta ? lerComp(valorDe(b), ka, c) : 0.0);
            double v;
            switch (op) {
            case GDEXTENSION_VARIANT_OP_ADD: v = x + y; break;
            case GDEXTENSION_VARIANT_OP_SUBTRACT: v = x - y; break;
            case GDEXTENSION_VARIANT_OP_MULTIPLY: v = x * y; break;
            case GDEXTENSION_VARIANT_OP_DIVIDE: v = ka == 'i' ? (y != 0.0 ? std::trunc(x / y) : 0.0) : x / y; break;
            case GDEXTENSION_VARIANT_OP_NEGATE: v = -x; break;
            case GDEXTENSION_VARIANT_OP_POSITIVE: v = x; break;
            default: return false;
            }
            escreverComp(r, ka, c, v);
        }
        return true;
    }
    return false;
}

int tipoResultado(int op, int ta, int tb) {
    if (op <= GDEXTENSION_VARIANT_OP_GREATER_EQUAL || op >= GDEXTENSION_VARIANT_OP_AND) return BOOL;
    if (texto(ta) && (op == GDEXTENSION_VARIANT_OP_ADD || op == GDEXTENSION_VARIANT_OP_MODULE)) return STRING;
    if (numero(ta) && numero(tb)) return (ta == FLOAT || tb == FLOAT) ? FLOAT : INT;
    if (numero(ta) && (op == GDEXTENSION_VARIANT_OP_NEGATE || op == GDEXTENSION_VARIANT_OP_POSITIVE))
        return ta == FLOAT ? FLOAT : INT;
    return ta;
}

uintptr_t executarOperador(const Slot& s, uintptr_t esq, uintptr_t dir, uintptr_t ret, uintptr_t) {
    GVariant a, b;
    comoVariant(s.b, reinterpret_cast<const void*>(esq), &a);
    if (dir) comoVariant(s.c, reinterpret_cast<const void*>(dir), &b);
    else variantNil(&b);
    void* r = reinterpret_cast<void*>(ret);
    const int tipoRet = tipoResultado(s.a, static_cast<int>(a.tipo), static_cast<int>(b.tipo));
    bool ok;
    if (tipoRet == STRING) {
        // O destino ja' e' uma String construida: avaliar() atribui direto.
        ok = avaliar(s.a, &a, &b, tipoRet, r);
    } else {
        ok = avaliar(s.a, &a, &b, tipoRet, r);
    }
    if (!ok) avisarUmaVez(s.msg, "NAO IMPLEMENTADO: %s", s.msg.c_str());
    variantDestruir(&a);
    variantDestruir(&b);
    return 0;
}

const char* nomeOperador(int op) {
    static const char* const n[] = {"==", "!=", "<", "<=", ">", ">=", "+", "-", "*", "/", "neg", "pos", "%", "**",
                                    "<<", ">>", "&", "|", "^", "~", "and", "or", "xor", "not", "in"};
    return (op >= 0 && op < GDEXTENSION_VARIANT_OP_MAX) ? n[op] : "?";
}

// ---- Formatacao "%" (subconjunto do motor: %s %d %i %f %x %X %c %% e largura/precisao) ----

std::u32string formatar(const std::u32string& fmt, const GVariant* const* vals, size_t n, bool& ok) {
    std::u32string out;
    size_t vi = 0;
    for (size_t i = 0; i < fmt.size(); ++i) {
        if (fmt[i] != U'%') {
            out.push_back(fmt[i]);
            continue;
        }
        ++i;
        if (i >= fmt.size()) break;
        if (fmt[i] == U'%') {
            out.push_back(U'%');
            continue;
        }
        std::string spec = "%";
        while (i < fmt.size() && (fmt[i] == U'-' || fmt[i] == U'+' || fmt[i] == U'0' || fmt[i] == U' ' ||
                                  fmt[i] == U'.' || (fmt[i] >= U'1' && fmt[i] <= U'9') || fmt[i] == U'0'))
            spec.push_back(static_cast<char>(fmt[i++]));
        if (i >= fmt.size()) break;
        const char32_t conv = fmt[i];
        if (vi >= n) {
            ok = false;
            break;
        }
        const GVariant* v = vals[vi++];
        char buf[128];
        switch (conv) {
        case U'd':
        case U'i':
            spec += "lld";
            std::snprintf(buf, sizeof(buf), spec.c_str(), static_cast<long long>(comoDouble(v)));
            out += ascii(buf);
            break;
        case U'x':
        case U'X':
            spec += conv == U'x' ? "llx" : "llX";
            std::snprintf(buf, sizeof(buf), spec.c_str(), static_cast<long long>(comoDouble(v)));
            out += ascii(buf);
            break;
        case U'f':
            spec += "f";
            std::snprintf(buf, sizeof(buf), spec.c_str(), comoDouble(v));
            out += ascii(buf);
            break;
        case U'c':
            out.push_back(static_cast<char32_t>(comoDouble(v)));
            break;
        default:
            out += variantTexto(v);
            break;
        }
    }
    return out;
}

// ---- Metodos builtin -----------------------------------------------------------------------

using FnBuiltin = void (*)(void* base, const void* const* args, void* ret, int argc);

int64_t argInt(const void* const* a, int i) {
    return *static_cast<const int64_t*>(a[i]);
}
double argF(const void* const* a, int i) {
    return *static_cast<const double*>(a[i]);
}
void retInt(void* r, int64_t v) {
    *static_cast<int64_t*>(r) = v;
}
void retBool(void* r, bool v) {
    *static_cast<uint8_t*>(r) = v ? 1 : 0;
}
void retF(void* r, double v) {
    *static_cast<double*>(r) = v;
}

std::u32string minusc(std::u32string s) {
    for (auto& c : s)
        if (c >= U'A' && c <= U'Z') c = c - U'A' + U'a';
    return s;
}
std::u32string maiusc(std::u32string s) {
    for (auto& c : s)
        if (c >= U'a' && c <= U'z') c = c - U'a' + U'A';
    return s;
}

uint32_t hashU32(const std::u32string& s) {
    uint32_t h = 5381;
    for (char32_t c : s) h = ((h << 5) + h) + static_cast<uint32_t>(c);
    return h;
}

DadoArray* arr(void* base) {
    GArray* a = static_cast<GArray*>(base);
    if (!a->d) a->d = new DadoArray();
    return a->d;
}

const std::unordered_map<std::string, FnBuiltin>& tabelaBuiltins() {
    static const std::unordered_map<std::string, FnBuiltin> t = {
        // String
        {"String.length", [](void* b, const void* const*, void* r, int) { retInt(r, static_cast<int64_t>(txt(b).size())); }},
        {"String.is_empty", [](void* b, const void* const*, void* r, int) { retBool(r, txt(b).empty()); }},
        {"String.hash", [](void* b, const void* const*, void* r, int) { retInt(r, hashU32(txt(b))); }},
        {"String.to_upper", [](void* b, const void* const*, void* r, int) { atribuirTexto(r, maiusc(txt(b))); }},
        {"String.to_lower", [](void* b, const void* const*, void* r, int) { atribuirTexto(r, minusc(txt(b))); }},
        {"String.begins_with", [](void* b, const void* const* a, void* r, int) { retBool(r, txt(b).rfind(txt(a[0]), 0) == 0); }},
        {"String.ends_with", [](void* b, const void* const* a, void* r, int) {
             const auto &s = txt(b), &x = txt(a[0]);
             retBool(r, s.size() >= x.size() && s.compare(s.size() - x.size(), x.size(), x) == 0);
         }},
        {"String.contains", [](void* b, const void* const* a, void* r, int) { retBool(r, txt(b).find(txt(a[0])) != std::u32string::npos); }},
        {"String.find", [](void* b, const void* const* a, void* r, int) {
             const size_t p = txt(b).find(txt(a[0]), static_cast<size_t>(std::max<int64_t>(0, argInt(a, 1))));
             retInt(r, p == std::u32string::npos ? -1 : static_cast<int64_t>(p));
         }},
        {"String.substr", [](void* b, const void* const* a, void* r, int) {
             const auto& s = txt(b);
             const size_t ini = static_cast<size_t>(std::max<int64_t>(0, argInt(a, 0)));
             const int64_t len = argInt(a, 1);
             atribuirTexto(r, ini >= s.size() ? std::u32string() : s.substr(ini, len < 0 ? std::u32string::npos : static_cast<size_t>(len)));
         }},
        {"String.left", [](void* b, const void* const* a, void* r, int) {
             const auto& s = txt(b);
             const int64_t n = argInt(a, 0);
             const size_t k = n < 0 ? (s.size() > static_cast<size_t>(-n) ? s.size() - static_cast<size_t>(-n) : 0) : std::min(s.size(), static_cast<size_t>(n));
             atribuirTexto(r, s.substr(0, k));
         }},
        {"String.right", [](void* b, const void* const* a, void* r, int) {
             const auto& s = txt(b);
             const int64_t n = argInt(a, 0);
             const size_t k = n < 0 ? (s.size() > static_cast<size_t>(-n) ? s.size() - static_cast<size_t>(-n) : 0) : std::min(s.size(), static_cast<size_t>(n));
             atribuirTexto(r, s.substr(s.size() - k));
         }},
        {"String.to_int", [](void* b, const void* const*, void* r, int) { retInt(r, std::strtoll(u32ParaUtf8(txt(b)).c_str(), nullptr, 10)); }},
        {"String.to_float", [](void* b, const void* const*, void* r, int) { retF(r, std::strtod(u32ParaUtf8(txt(b)).c_str(), nullptr)); }},
        {"String.replace", [](void* b, const void* const* a, void* r, int) {
             std::u32string s = txt(b);
             const auto &de = txt(a[0]), &para = txt(a[1]);
             if (!de.empty()) {
                 for (size_t p = s.find(de); p != std::u32string::npos; p = s.find(de, p + para.size())) s.replace(p, de.size(), para);
             }
             atribuirTexto(r, s);
         }},
        {"String.strip_edges", [](void* b, const void* const* a, void* r, int) {
             std::u32string s = txt(b);
             const bool esq = *static_cast<const uint8_t*>(a[0]), dir = *static_cast<const uint8_t*>(a[1]);
             auto branco = [](char32_t c) { return c <= U' '; };
             if (dir)
                 while (!s.empty() && branco(s.back())) s.pop_back();
             if (esq) {
                 size_t k = 0;
                 while (k < s.size() && branco(s[k])) ++k;
                 s.erase(0, k);
             }
             atribuirTexto(r, s);
         }},
        {"String.split", [](void* b, const void* const* a, void* r, int) {
             const auto &s = txt(b), &sep = txt(a[0]);
             const bool vazios = *static_cast<const uint8_t*>(a[1]);
             GPacked* p = static_cast<GPacked*>(r);
             DadoPacked* d = garantirPacked(PACKED_STRING, p);
             redimensionarPacked(d, 0);
             auto add = [&](const std::u32string& x) {
                 if (x.empty() && !vazios) return;
                 redimensionarPacked(d, d->tamanho() + 1);
                 atribuirTexto(d->bytes.data() + (d->tamanho() - 1) * d->tamElemento, x);
             };
             if (sep.empty()) {
                 add(s);
                 return;
             }
             size_t ini = 0;
             for (size_t pos = s.find(sep); pos != std::u32string::npos; pos = s.find(sep, ini)) {
                 add(s.substr(ini, pos - ini));
                 ini = pos + sep.size();
             }
             add(s.substr(ini));
         }},
        {"String.format", [](void* b, const void* const* a, void* r, int) {
             // "{0} e {1}" com Array, ou "{nome}" com Dictionary.
             std::u32string s = txt(b);
             const GVariant* v = static_cast<const GVariant*>(a[0]);
             auto troca = [&](const std::u32string& chave, const GVariant* val) {
                 const std::u32string alvo = U"{" + chave + U"}";
                 const std::u32string novo = variantTexto(val);
                 for (size_t p = s.find(alvo); p != std::u32string::npos; p = s.find(alvo, p + novo.size())) s.replace(p, alvo.size(), novo);
             };
             if (v->tipo == ARRAY) {
                 const DadoArray* d = static_cast<const GArray*>(valorDe(v))->d;
                 for (size_t i = 0; d && i < d->itens.size(); ++i) troca(ascii(std::to_string(i)), &d->itens[i]);
             } else if (v->tipo == DICTIONARY) {
                 const DadoDicionario* d = static_cast<const GDictionary*>(valorDe(v))->d;
                 for (size_t i = 0; d && i < d->chaves.size(); ++i) troca(variantTexto(&d->chaves[i]), &d->valores[i]);
             }
             atribuirTexto(r, s);
         }},
        // StringName
        {"StringName.hash", [](void* b, const void* const*, void* r, int) { retInt(r, static_cast<const GStringName*>(b)->n->hash); }},
        {"StringName.length", [](void* b, const void* const*, void* r, int) { retInt(r, static_cast<int64_t>(static_cast<const GStringName*>(b)->n->texto.size())); }},
        {"StringName.is_empty", [](void* b, const void* const*, void* r, int) { retBool(r, static_cast<const GStringName*>(b)->n->texto.empty()); }},
        {"NodePath.is_empty", [](void* b, const void* const*, void* r, int) { retBool(r, txt(b).empty()); }},
        // Array
        {"Array.size", [](void* b, const void* const*, void* r, int) { retInt(r, static_cast<int64_t>(arr(b)->itens.size())); }},
        {"Array.is_empty", [](void* b, const void* const*, void* r, int) { retBool(r, arr(b)->itens.empty()); }},
        {"Array.clear", [](void* b, const void* const*, void*, int) {
             for (auto& v : arr(b)->itens) variantDestruir(&v);
             arr(b)->itens.clear();
         }},
        {"Array.hash", [](void* b, const void* const*, void* r, int) {
             GVariant v;
             variantDe(&v, ARRAY, b);
             retInt(r, hashU32(variantTexto(&v)));
             variantDestruir(&v);
         }},
        {"Array.get", [](void* b, const void* const* a, void* r, int) {
             auto& it = arr(b)->itens;
             const int64_t i = argInt(a, 0);
             if (i >= 0 && static_cast<size_t>(i) < it.size()) atribuirVariant(r, &it[static_cast<size_t>(i)]);
         }},
        {"Array.set", [](void* b, const void* const* a, void*, int) {
             auto& it = arr(b)->itens;
             const int64_t i = argInt(a, 0);
             if (i >= 0 && static_cast<size_t>(i) < it.size()) atribuirVariant(&it[static_cast<size_t>(i)], static_cast<const GVariant*>(a[1]));
         }},
        {"Array.push_back", [](void* b, const void* const* a, void*, int) {
             arr(b)->itens.emplace_back();
             variantCopia(&arr(b)->itens.back(), static_cast<const GVariant*>(a[0]));
         }},
        {"Array.append", [](void* b, const void* const* a, void*, int) {
             arr(b)->itens.emplace_back();
             variantCopia(&arr(b)->itens.back(), static_cast<const GVariant*>(a[0]));
         }},
        {"Array.push_front", [](void* b, const void* const* a, void*, int) {
             GVariant v;
             variantCopia(&v, static_cast<const GVariant*>(a[0]));
             arr(b)->itens.insert(arr(b)->itens.begin(), v);
         }},
        {"Array.append_array", [](void* b, const void* const* a, void*, int) {
             const DadoArray* o = static_cast<const GArray*>(a[0])->d;
             const size_t n = o ? o->itens.size() : 0;
             for (size_t i = 0; i < n; ++i) {
                 arr(b)->itens.emplace_back();
                 variantCopia(&arr(b)->itens.back(), &o->itens[i]);
             }
         }},
        {"Array.resize", [](void* b, const void* const* a, void* r, int) {
             auto& it = arr(b)->itens;
             const size_t n = static_cast<size_t>(std::max<int64_t>(0, argInt(a, 0)));
             for (size_t i = n; i < it.size(); ++i) variantDestruir(&it[i]);
             const size_t antigo = it.size();
             it.resize(n);
             for (size_t i = antigo; i < n; ++i) variantNil(&it[i]);
             retInt(r, 0);
         }},
        {"Array.insert", [](void* b, const void* const* a, void* r, int) {
             auto& it = arr(b)->itens;
             const int64_t i = argInt(a, 0);
             if (i < 0 || static_cast<size_t>(i) > it.size()) {
                 retInt(r, 1);
                 return;
             }
             GVariant v;
             variantCopia(&v, static_cast<const GVariant*>(a[1]));
             it.insert(it.begin() + i, v);
             retInt(r, 0);
         }},
        {"Array.remove_at", [](void* b, const void* const* a, void*, int) {
             auto& it = arr(b)->itens;
             const int64_t i = argInt(a, 0);
             if (i >= 0 && static_cast<size_t>(i) < it.size()) {
                 variantDestruir(&it[static_cast<size_t>(i)]);
                 it.erase(it.begin() + i);
             }
         }},
        {"Array.erase", [](void* b, const void* const* a, void*, int) {
             auto& it = arr(b)->itens;
             for (size_t i = 0; i < it.size(); ++i) {
                 if (variantIgual(&it[i], static_cast<const GVariant*>(a[0]))) {
                     variantDestruir(&it[i]);
                     it.erase(it.begin() + static_cast<long>(i));
                     return;
                 }
             }
         }},
        {"Array.front", [](void* b, const void* const*, void* r, int) {
             if (!arr(b)->itens.empty()) atribuirVariant(r, &arr(b)->itens.front());
         }},
        {"Array.back", [](void* b, const void* const*, void* r, int) {
             if (!arr(b)->itens.empty()) atribuirVariant(r, &arr(b)->itens.back());
         }},
        {"Array.pop_back", [](void* b, const void* const*, void* r, int) {
             auto& it = arr(b)->itens;
             if (it.empty()) return;
             atribuirVariant(r, &it.back());
             variantDestruir(&it.back());
             it.pop_back();
         }},
        {"Array.has", [](void* b, const void* const* a, void* r, int) {
             bool achou = false;
             for (const auto& v : arr(b)->itens) achou = achou || variantIgual(&v, static_cast<const GVariant*>(a[0]));
             retBool(r, achou);
         }},
        {"Array.find", [](void* b, const void* const* a, void* r, int) {
             const auto& it = arr(b)->itens;
             for (size_t i = static_cast<size_t>(std::max<int64_t>(0, argInt(a, 1))); i < it.size(); ++i) {
                 if (variantIgual(&it[i], static_cast<const GVariant*>(a[0]))) {
                     retInt(r, static_cast<int64_t>(i));
                     return;
                 }
             }
             retInt(r, -1);
         }},
        {"Array.duplicate", [](void* b, const void* const*, void* r, int) {
             GArray novo{new DadoArray()};
             for (const auto& v : arr(b)->itens) {
                 novo.d->itens.emplace_back();
                 variantCopia(&novo.d->itens.back(), &v);
             }
             destruirValor(ARRAY, r);
             *static_cast<GArray*>(r) = novo;
         }},
        // Dictionary
        {"Dictionary.size", [](void* b, const void* const*, void* r, int) {
             const DadoDicionario* d = static_cast<GDictionary*>(b)->d;
             retInt(r, d ? static_cast<int64_t>(d->chaves.size()) : 0);
         }},
        {"Dictionary.is_empty", [](void* b, const void* const*, void* r, int) {
             const DadoDicionario* d = static_cast<GDictionary*>(b)->d;
             retBool(r, !d || d->chaves.empty());
         }},
        {"Dictionary.has", [](void* b, const void* const* a, void* r, int) {
             const DadoDicionario* d = static_cast<GDictionary*>(b)->d;
             bool achou = false;
             for (size_t i = 0; d && i < d->chaves.size(); ++i) achou = achou || variantIgual(&d->chaves[i], static_cast<const GVariant*>(a[0]));
             retBool(r, achou);
         }},
        {"Dictionary.get", [](void* b, const void* const* a, void* r, int) {
             const DadoDicionario* d = static_cast<GDictionary*>(b)->d;
             for (size_t i = 0; d && i < d->chaves.size(); ++i) {
                 if (variantIgual(&d->chaves[i], static_cast<const GVariant*>(a[0]))) {
                     atribuirVariant(r, &d->valores[i]);
                     return;
                 }
             }
             atribuirVariant(r, static_cast<const GVariant*>(a[1]));
         }},
        {"Dictionary.keys", [](void* b, const void* const*, void* r, int) {
             const DadoDicionario* d = static_cast<GDictionary*>(b)->d;
             GArray novo{new DadoArray()};
             for (size_t i = 0; d && i < d->chaves.size(); ++i) {
                 novo.d->itens.emplace_back();
                 variantCopia(&novo.d->itens.back(), &d->chaves[i]);
             }
             destruirValor(ARRAY, r);
             *static_cast<GArray*>(r) = novo;
         }},
        {"Dictionary.values", [](void* b, const void* const*, void* r, int) {
             const DadoDicionario* d = static_cast<GDictionary*>(b)->d;
             GArray novo{new DadoArray()};
             for (size_t i = 0; d && i < d->valores.size(); ++i) {
                 novo.d->itens.emplace_back();
                 variantCopia(&novo.d->itens.back(), &d->valores[i]);
             }
             destruirValor(ARRAY, r);
             *static_cast<GArray*>(r) = novo;
         }},
        {"Dictionary.clear", [](void* b, const void* const*, void*, int) {
             DadoDicionario* d = static_cast<GDictionary*>(b)->d;
             if (!d) return;
             for (auto& v : d->chaves) variantDestruir(&v);
             for (auto& v : d->valores) variantDestruir(&v);
             d->chaves.clear();
             d->valores.clear();
         }},
        {"Dictionary.erase", [](void* b, const void* const* a, void* r, int) {
             DadoDicionario* d = static_cast<GDictionary*>(b)->d;
             for (size_t i = 0; d && i < d->chaves.size(); ++i) {
                 if (variantIgual(&d->chaves[i], static_cast<const GVariant*>(a[0]))) {
                     variantDestruir(&d->chaves[i]);
                     variantDestruir(&d->valores[i]);
                     d->chaves.erase(d->chaves.begin() + static_cast<long>(i));
                     d->valores.erase(d->valores.begin() + static_cast<long>(i));
                     retBool(r, true);
                     return;
                 }
             }
             retBool(r, false);
         }},
        // Vetores
        {"Vector2.length", [](void* b, const void* const*, void* r, int) {
             const float* v = static_cast<const float*>(b);
             retF(r, std::sqrt(v[0] * v[0] + v[1] * v[1]));
         }},
        {"Vector2.length_squared", [](void* b, const void* const*, void* r, int) {
             const float* v = static_cast<const float*>(b);
             retF(r, v[0] * v[0] + v[1] * v[1]);
         }},
        {"Vector2.normalized", [](void* b, const void* const*, void* r, int) {
             const float* v = static_cast<const float*>(b);
             const float l = std::sqrt(v[0] * v[0] + v[1] * v[1]);
             float* o = static_cast<float*>(r);
             o[0] = l > 0 ? v[0] / l : 0;
             o[1] = l > 0 ? v[1] / l : 0;
         }},
        {"Vector2.dot", [](void* b, const void* const* a, void* r, int) {
             const float *v = static_cast<const float*>(b), *w = static_cast<const float*>(a[0]);
             retF(r, v[0] * w[0] + v[1] * w[1]);
         }},
        {"Vector2.distance_to", [](void* b, const void* const* a, void* r, int) {
             const float *v = static_cast<const float*>(b), *w = static_cast<const float*>(a[0]);
             retF(r, std::hypot(v[0] - w[0], v[1] - w[1]));
         }},
        {"Vector3.length", [](void* b, const void* const*, void* r, int) {
             const float* v = static_cast<const float*>(b);
             retF(r, std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]));
         }},
        {"Vector3.length_squared", [](void* b, const void* const*, void* r, int) {
             const float* v = static_cast<const float*>(b);
             retF(r, v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
         }},
        {"Vector3.normalized", [](void* b, const void* const*, void* r, int) {
             const float* v = static_cast<const float*>(b);
             const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
             float* o = static_cast<float*>(r);
             for (int i = 0; i < 3; ++i) o[i] = l > 0 ? v[i] / l : 0;
         }},
        {"Vector3.dot", [](void* b, const void* const* a, void* r, int) {
             const float *v = static_cast<const float*>(b), *w = static_cast<const float*>(a[0]);
             retF(r, v[0] * w[0] + v[1] * w[1] + v[2] * w[2]);
         }},
        {"Vector3.cross", [](void* b, const void* const* a, void* r, int) {
             const float *v = static_cast<const float*>(b), *w = static_cast<const float*>(a[0]);
             float* o = static_cast<float*>(r);
             const float c[3] = {v[1] * w[2] - v[2] * w[1], v[2] * w[0] - v[0] * w[2], v[0] * w[1] - v[1] * w[0]};
             std::memcpy(o, c, sizeof(c));
         }},
        {"Vector3.distance_to", [](void* b, const void* const* a, void* r, int) {
             const float *v = static_cast<const float*>(b), *w = static_cast<const float*>(a[0]);
             const float d[3] = {v[0] - w[0], v[1] - w[1], v[2] - w[2]};
             retF(r, std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]));
         }},
    };
    return t;
}

// Metodos comuns a todos os Packed*Array (o tipo vem no Slot).
void escreverElemento(int tipoPacked, DadoPacked* d, size_t i, const void* v) {
    uint8_t* dst = d->bytes.data() + i * d->tamElemento;
    switch (tipoPacked) {
    case GDEXTENSION_VARIANT_TYPE_PACKED_BYTE_ARRAY: *dst = static_cast<uint8_t>(*static_cast<const int64_t*>(v)); return;
    case GDEXTENSION_VARIANT_TYPE_PACKED_INT32_ARRAY: *reinterpret_cast<int32_t*>(dst) = static_cast<int32_t>(*static_cast<const int64_t*>(v)); return;
    case GDEXTENSION_VARIANT_TYPE_PACKED_FLOAT32_ARRAY: *reinterpret_cast<float*>(dst) = static_cast<float>(*static_cast<const double*>(v)); return;
    case PACKED_STRING: atribuirTexto(dst, txt(v)); return;
    default: std::memcpy(dst, v, d->tamElemento); return;
    }
}

void lerElemento(int tipoPacked, const DadoPacked* d, size_t i, void* r) {
    const uint8_t* src = d->bytes.data() + i * d->tamElemento;
    switch (tipoPacked) {
    case GDEXTENSION_VARIANT_TYPE_PACKED_BYTE_ARRAY: retInt(r, *src); return;
    case GDEXTENSION_VARIANT_TYPE_PACKED_INT32_ARRAY: retInt(r, *reinterpret_cast<const int32_t*>(src)); return;
    case GDEXTENSION_VARIANT_TYPE_PACKED_FLOAT32_ARRAY: retF(r, *reinterpret_cast<const float*>(src)); return;
    case PACKED_STRING: atribuirTexto(r, txt(src)); return;
    default: std::memcpy(r, src, d->tamElemento); return;
    }
}

uintptr_t executarPacked(const Slot& s, uintptr_t base, uintptr_t args, uintptr_t ret, uintptr_t) {
    const int tipo = s.a;
    DadoPacked* d = garantirPacked(tipo, reinterpret_cast<GPacked*>(base));
    const void* const* a = reinterpret_cast<const void* const*>(args);
    void* r = reinterpret_cast<void*>(ret);
    const std::string& m = s.msg;
    if (m == "size") retInt(r, static_cast<int64_t>(d->tamanho()));
    else if (m == "is_empty") retBool(r, d->tamanho() == 0);
    else if (m == "clear") redimensionarPacked(d, 0);
    else if (m == "resize") {
        redimensionarPacked(d, static_cast<size_t>(std::max<int64_t>(0, argInt(a, 0))));
        retInt(r, 0);
    } else if (m == "push_back" || m == "append") {
        redimensionarPacked(d, d->tamanho() + 1);
        escreverElemento(tipo, d, d->tamanho() - 1, a[0]);
        if (r) retBool(r, false);
    } else if (m == "set") {
        const int64_t i = argInt(a, 0);
        if (i >= 0 && static_cast<size_t>(i) < d->tamanho()) escreverElemento(tipo, d, static_cast<size_t>(i), a[1]);
    } else if (m == "get") {
        const int64_t i = argInt(a, 0);
        if (i >= 0 && static_cast<size_t>(i) < d->tamanho()) lerElemento(tipo, d, static_cast<size_t>(i), r);
    } else if (m == "fill") {
        for (size_t i = 0; i < d->tamanho(); ++i) escreverElemento(tipo, d, i, a[0]);
    }
    return 0;
}

bool metodoPackedSuportado(const std::string& m) {
    return m == "size" || m == "is_empty" || m == "clear" || m == "resize" || m == "push_back" || m == "append" ||
           m == "set" || m == "get" || m == "fill";
}

// ---- Funcoes utilitarias ---------------------------------------------------------------------

using FnUtil = void (*)(void* ret, const void* const* args, int argc);

std::u32string juntar(const void* const* args, int argc, const std::u32string& sep) {
    std::u32string s;
    for (int i = 0; i < argc; ++i) {
        if (i && !sep.empty()) s += sep;
        s += variantTexto(static_cast<const GVariant*>(args[i]));
    }
    return s;
}

void imprimir(Nivel n, const std::u32string& s) {
    host().log(n, "godot", u32ParaUtf8(s).c_str());
}

std::mt19937_64& rng() {
    static std::mt19937_64 g(0x5eed);
    return g;
}

template <double (*F)(double)>
void util1(void* r, const void* const* a, int) {
    retF(r, F(argF(a, 0)));
}

double sinD(double x) { return std::sin(x); }
double cosD(double x) { return std::cos(x); }
double tanD(double x) { return std::tan(x); }
double sqrtD(double x) { return std::sqrt(x); }
double floorD(double x) { return std::floor(x); }
double ceilD(double x) { return std::ceil(x); }
double roundD(double x) { return std::round(x); }
double absD(double x) { return std::fabs(x); }
double expD(double x) { return std::exp(x); }
double logD(double x) { return std::log(x); }
double signD(double x) { return x > 0 ? 1.0 : (x < 0 ? -1.0 : 0.0); }
double degRad(double x) { return x * 3.14159265358979323846 / 180.0; }
double radDeg(double x) { return x * 180.0 / 3.14159265358979323846; }

// Versoes "Variant" (abs, floor, clamp, lerp...) para numeros.
void retNumeroComo(void* r, const GVariant* modelo, double v) {
    GVariant out;
    if (modelo->tipo == INT) variantInt(&out, static_cast<int64_t>(v));
    else variantFloat(&out, v);
    atribuirVariant(r, &out);
    variantDestruir(&out);
}

const GVariant* va(const void* const* a, int i) {
    return static_cast<const GVariant*>(a[i]);
}

const std::unordered_map<std::string, FnUtil>& tabelaUtil() {
    static const std::unordered_map<std::string, FnUtil> t = {
        {"print", [](void*, const void* const* a, int n) { imprimir(Nivel::Info, juntar(a, n, U"")); }},
        {"print_rich", [](void*, const void* const* a, int n) { imprimir(Nivel::Info, juntar(a, n, U"")); }},
        {"print_verbose", [](void*, const void* const* a, int n) { imprimir(Nivel::Info, juntar(a, n, U"")); }},
        {"printraw", [](void*, const void* const* a, int n) { imprimir(Nivel::Info, juntar(a, n, U"")); }},
        {"prints", [](void*, const void* const* a, int n) { imprimir(Nivel::Info, juntar(a, n, U" ")); }},
        {"printt", [](void*, const void* const* a, int n) { imprimir(Nivel::Info, juntar(a, n, U"\t")); }},
        {"printerr", [](void*, const void* const* a, int n) { imprimir(Nivel::Erro, juntar(a, n, U"")); }},
        {"push_error", [](void*, const void* const* a, int n) { imprimir(Nivel::Erro, juntar(a, n, U"")); }},
        {"push_warning", [](void*, const void* const* a, int n) { imprimir(Nivel::Aviso, juntar(a, n, U"")); }},
        {"str", [](void* r, const void* const* a, int n) { atribuirTexto(r, juntar(a, n, U"")); }},
        {"sin", &util1<sinD>},
        {"cos", &util1<cosD>},
        {"tan", &util1<tanD>},
        {"sqrt", &util1<sqrtD>},
        {"floorf", &util1<floorD>},
        {"ceilf", &util1<ceilD>},
        {"roundf", &util1<roundD>},
        {"absf", &util1<absD>},
        {"signf", &util1<signD>},
        {"exp", &util1<expD>},
        {"log", &util1<logD>},
        {"deg_to_rad", &util1<degRad>},
        {"rad_to_deg", &util1<radDeg>},
        {"atan2", [](void* r, const void* const* a, int) { retF(r, std::atan2(argF(a, 0), argF(a, 1))); }},
        {"pow", [](void* r, const void* const* a, int) { retF(r, std::pow(argF(a, 0), argF(a, 1))); }},
        {"fmod", [](void* r, const void* const* a, int) { retF(r, std::fmod(argF(a, 0), argF(a, 1))); }},
        {"lerpf", [](void* r, const void* const* a, int) { retF(r, argF(a, 0) + (argF(a, 1) - argF(a, 0)) * argF(a, 2)); }},
        {"clampf", [](void* r, const void* const* a, int) { retF(r, std::min(std::max(argF(a, 0), argF(a, 1)), argF(a, 2))); }},
        {"clampi", [](void* r, const void* const* a, int) { retInt(r, std::min(std::max(argInt(a, 0), argInt(a, 1)), argInt(a, 2))); }},
        {"minf", [](void* r, const void* const* a, int) { retF(r, std::min(argF(a, 0), argF(a, 1))); }},
        {"maxf", [](void* r, const void* const* a, int) { retF(r, std::max(argF(a, 0), argF(a, 1))); }},
        {"mini", [](void* r, const void* const* a, int) { retInt(r, std::min(argInt(a, 0), argInt(a, 1))); }},
        {"maxi", [](void* r, const void* const* a, int) { retInt(r, std::max(argInt(a, 0), argInt(a, 1))); }},
        {"absi", [](void* r, const void* const* a, int) { retInt(r, std::llabs(argInt(a, 0))); }},
        {"floori", [](void* r, const void* const* a, int) { retInt(r, static_cast<int64_t>(std::floor(argF(a, 0)))); }},
        {"ceili", [](void* r, const void* const* a, int) { retInt(r, static_cast<int64_t>(std::ceil(argF(a, 0)))); }},
        {"roundi", [](void* r, const void* const* a, int) { retInt(r, static_cast<int64_t>(std::round(argF(a, 0)))); }},
        {"randomize", [](void*, const void* const*, int) { rng().seed(std::random_device{}()); }},
        {"seed", [](void*, const void* const* a, int) { rng().seed(static_cast<uint64_t>(argInt(a, 0))); }},
        {"randi", [](void* r, const void* const*, int) { retInt(r, static_cast<int64_t>(rng()() & 0xFFFFFFFFu)); }},
        {"randf", [](void* r, const void* const*, int) { retF(r, std::uniform_real_distribution<double>(0.0, 1.0)(rng())); }},
        {"randf_range", [](void* r, const void* const* a, int) { retF(r, std::uniform_real_distribution<double>(argF(a, 0), argF(a, 1))(rng())); }},
        {"randi_range", [](void* r, const void* const* a, int) {
             const int64_t lo = std::min(argInt(a, 0), argInt(a, 1)), hi = std::max(argInt(a, 0), argInt(a, 1));
             retInt(r, std::uniform_int_distribution<int64_t>(lo, hi)(rng()));
         }},
        {"typeof", [](void* r, const void* const* a, int) { retInt(r, va(a, 0)->tipo); }},
        {"is_instance_valid", [](void* r, const void* const* a, int) {
             const GVariant* v = va(a, 0);
             retBool(r, v->tipo == OBJECT && objetoVivo(*static_cast<Objeto* const*>(valorDe(v))));
         }},
        {"abs", [](void* r, const void* const* a, int) { retNumeroComo(r, va(a, 0), std::fabs(comoDouble(va(a, 0)))); }},
        {"floor", [](void* r, const void* const* a, int) { retNumeroComo(r, va(a, 0), std::floor(comoDouble(va(a, 0)))); }},
        {"ceil", [](void* r, const void* const* a, int) { retNumeroComo(r, va(a, 0), std::ceil(comoDouble(va(a, 0)))); }},
        {"round", [](void* r, const void* const* a, int) { retNumeroComo(r, va(a, 0), std::round(comoDouble(va(a, 0)))); }},
        {"sign", [](void* r, const void* const* a, int) { retNumeroComo(r, va(a, 0), signD(comoDouble(va(a, 0)))); }},
        {"clamp", [](void* r, const void* const* a, int) {
             retNumeroComo(r, va(a, 0), std::min(std::max(comoDouble(va(a, 0)), comoDouble(va(a, 1))), comoDouble(va(a, 2))));
         }},
        {"lerp", [](void* r, const void* const* a, int) {
             const double x = comoDouble(va(a, 0)), y = comoDouble(va(a, 1));
             retNumeroComo(r, va(a, 0), x + (y - x) * comoDouble(va(a, 2)));
         }},
        {"min", [](void* r, const void* const* a, int n) {
             const GVariant* m = va(a, 0);
             for (int i = 1; i < n; ++i)
                 if (comoDouble(va(a, i)) < comoDouble(m)) m = va(a, i);
             atribuirVariant(r, m);
         }},
        {"max", [](void* r, const void* const* a, int n) {
             const GVariant* m = va(a, 0);
             for (int i = 1; i < n; ++i)
                 if (comoDouble(va(a, i)) > comoDouble(m)) m = va(a, i);
             atribuirVariant(r, m);
         }},
    };
    return t;
}

} // namespace

// ---- Pontos de entrada usados pela tabela de interface ------------------------------------

GDExtensionPtrConstructor construtorPtr(int tipo, int indice) {
    const std::string chave = std::string("ctor:") + nomeTipo(tipo) + "#" + std::to_string(indice);
    const std::vector<int> sig = argsConstrutor(tipo, indice);
    if (!sig.empty() && sig[0] == -1) return nullptr;
    Slot s;
    s.fn = &executarConstrutor;
    s.a = tipo;
    s.b = indice;
    s.msg = std::string("construtor ") + nomeTipo(tipo) + "(" + (assinaturas(tipo) ? assinaturas(tipo)[indice] : "?") + ")";
    return comoFn<GDExtensionPtrConstructor>(trampolim(chave, s));
}

GDExtensionPtrOperatorEvaluator operadorPtr(int op, int tipoA, int tipoB) {
    const std::string desc = std::string("operador ") + nomeTipo(tipoA) + " " + nomeOperador(op) + " " +
                             (tipoB == NIL ? "Variant" : nomeTipo(tipoB));
    Slot s;
    s.fn = &executarOperador;
    s.a = op;
    s.b = tipoA;
    s.c = tipoB;
    s.msg = desc;
    return comoFn<GDExtensionPtrOperatorEvaluator>(trampolim(desc, s));
}

GDExtensionPtrBuiltInMethod metodoBuiltin(int tipo, const NomeInterno* nome) {
    const std::string chave = std::string(nomeTipo(tipo)) + "." + nome->utf8;
    const auto& t = tabelaBuiltins();
    auto it = t.find(chave);
    if (it != t.end()) return it->second;
    if (ehPacked(tipo) && metodoPackedSuportado(nome->utf8)) {
        Slot s;
        s.fn = &executarPacked;
        s.a = tipo;
        s.msg = nome->utf8;
        return comoFn<GDExtensionPtrBuiltInMethod>(trampolim("packed:" + chave, s));
    }
    return comoFn<GDExtensionPtrBuiltInMethod>(stubFaltante("builtin:" + chave, "metodo builtin " + chave));
}

GDExtensionPtrUtilityFunction funcaoUtilitaria(const NomeInterno* nome) {
    const auto& t = tabelaUtil();
    auto it = t.find(nome->utf8);
    if (it != t.end()) return it->second;
    return comoFn<GDExtensionPtrUtilityFunction>(stubFaltante("util:" + nome->utf8, "funcao utilitaria " + nome->utf8));
}

} // namespace eruption::compat::gd
