// Valores do host GDExtension: StringName internado, String, Variant,
// Array, Dictionary, Packed*Array e as funcoes da interface que mexem neles.

#include "godot/GdNucleo.hpp"
#include "host/Host.hpp"

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <unordered_set>

namespace eruption::compat::gd {

// ---- Avisos -------------------------------------------------------------------

void avisarUmaVez(const std::string& chave, const char* fmt, ...) {
    static std::mutex mtx;
    static std::unordered_set<std::string> vistos;
    {
        std::lock_guard<std::mutex> lk(mtx);
        if (!vistos.insert(chave).second) return;
    }
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    host().log(Nivel::Aviso, "godot", buf);
}

// ---- Texto ----------------------------------------------------------------------

std::u32string utf8ParaU32(const char* s, size_t n) {
    std::u32string out;
    out.reserve(n);
    size_t i = 0;
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        char32_t cp;
        int extra;
        if (c < 0x80) {
            cp = c;
            extra = 0;
        } else if ((c >> 5) == 0x6) {
            cp = c & 0x1F;
            extra = 1;
        } else if ((c >> 4) == 0xE) {
            cp = c & 0x0F;
            extra = 2;
        } else if ((c >> 3) == 0x1E) {
            cp = c & 0x07;
            extra = 3;
        } else {
            out.push_back(0xFFFD);
            ++i;
            continue;
        }
        if (extra > 0 && i + static_cast<size_t>(extra) >= n) {
            out.push_back(0xFFFD);
            break;
        }
        for (int k = 1; k <= extra; ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        out.push_back(cp);
        i += static_cast<size_t>(extra) + 1;
    }
    return out;
}

std::string u32ParaUtf8(const std::u32string& s) {
    std::string out;
    out.reserve(s.size());
    for (char32_t c : s) {
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
        } else if (c < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (c >> 6)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else if (c < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (c >> 12)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (c >> 18)));
            out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
    }
    return out;
}

static uint32_t hashTexto(const std::u32string& s) {
    uint32_t h = 5381; // djb2, o mesmo espirito do hash de String do Godot
    for (char32_t c : s) h = ((h << 5) + h) + static_cast<uint32_t>(c);
    return h;
}

const NomeInterno* internar(const std::u32string& texto) {
    static std::mutex mtx;
    static std::unordered_map<std::u32string, std::unique_ptr<NomeInterno>> tabela;
    std::lock_guard<std::mutex> lk(mtx);
    auto it = tabela.find(texto);
    if (it != tabela.end()) return it->second.get();
    auto n = std::make_unique<NomeInterno>();
    n->texto = texto;
    n->utf8 = u32ParaUtf8(texto);
    n->hash = hashTexto(texto);
    const NomeInterno* p = n.get();
    tabela.emplace(texto, std::move(n));
    return p;
}

const NomeInterno* internarUtf8(const std::string& utf8) {
    return internar(utf8ParaU32(utf8.data(), utf8.size()));
}

// ---- Tipos ----------------------------------------------------------------------

size_t tamanhoTipo(int tipo) {
    static const size_t tam[kTiposVariant] = {
        0,  1,  8,  8,  8,  8,  8,  16, 16, 12, // NIL..VECTOR3
        12, 24, 16, 16, 16, 16, 24, 36, 48, 64, // VECTOR3I..PROJECTION
        16, 8,  8,  8,  8,  16, 16, 8,  8,  16, // COLOR..PACKED_BYTE_ARRAY
        16, 16, 16, 16, 16, 16, 16, 16, 16,     // PACKED_INT32..PACKED_VECTOR4
    };
    return (tipo >= 0 && tipo < kTiposVariant) ? tam[tipo] : 0;
}

const char* nomeTipo(int tipo) {
    static const char* const nomes[kTiposVariant] = {
        "Nil",       "bool",       "int",        "float",      "String",     "Vector2",
        "Vector2i",  "Rect2",      "Rect2i",     "Vector3",    "Vector3i",   "Transform2D",
        "Vector4",   "Vector4i",   "Plane",      "Quaternion", "AABB",       "Basis",
        "Transform3D", "Projection", "Color",    "StringName", "NodePath",   "RID",
        "Object",    "Callable",   "Signal",     "Dictionary", "Array",      "PackedByteArray",
        "PackedInt32Array", "PackedInt64Array", "PackedFloat32Array", "PackedFloat64Array",
        "PackedStringArray", "PackedVector2Array", "PackedVector3Array", "PackedColorArray",
        "PackedVector4Array",
    };
    return (tipo >= 0 && tipo < kTiposVariant) ? nomes[tipo] : "?";
}

bool ehPacked(int tipo) {
    return tipo >= GDEXTENSION_VARIANT_TYPE_PACKED_BYTE_ARRAY && tipo <= GDEXTENSION_VARIANT_TYPE_PACKED_VECTOR4_ARRAY;
}

static void elementoPacked(int tipo, int& tipoElem, size_t& tam) {
    switch (tipo) {
    case GDEXTENSION_VARIANT_TYPE_PACKED_BYTE_ARRAY: tipoElem = GDEXTENSION_VARIANT_TYPE_INT; tam = 1; return;
    case GDEXTENSION_VARIANT_TYPE_PACKED_INT32_ARRAY: tipoElem = GDEXTENSION_VARIANT_TYPE_INT; tam = 4; return;
    case GDEXTENSION_VARIANT_TYPE_PACKED_INT64_ARRAY: tipoElem = GDEXTENSION_VARIANT_TYPE_INT; tam = 8; return;
    case GDEXTENSION_VARIANT_TYPE_PACKED_FLOAT32_ARRAY: tipoElem = GDEXTENSION_VARIANT_TYPE_FLOAT; tam = 4; return;
    case GDEXTENSION_VARIANT_TYPE_PACKED_FLOAT64_ARRAY: tipoElem = GDEXTENSION_VARIANT_TYPE_FLOAT; tam = 8; return;
    case GDEXTENSION_VARIANT_TYPE_PACKED_STRING_ARRAY: tipoElem = GDEXTENSION_VARIANT_TYPE_STRING; tam = sizeof(GString); return;
    case GDEXTENSION_VARIANT_TYPE_PACKED_VECTOR2_ARRAY: tipoElem = GDEXTENSION_VARIANT_TYPE_VECTOR2; tam = 8; return;
    case GDEXTENSION_VARIANT_TYPE_PACKED_VECTOR3_ARRAY: tipoElem = GDEXTENSION_VARIANT_TYPE_VECTOR3; tam = 12; return;
    case GDEXTENSION_VARIANT_TYPE_PACKED_COLOR_ARRAY: tipoElem = GDEXTENSION_VARIANT_TYPE_COLOR; tam = 16; return;
    case GDEXTENSION_VARIANT_TYPE_PACKED_VECTOR4_ARRAY: tipoElem = GDEXTENSION_VARIANT_TYPE_VECTOR4; tam = 16; return;
    default: tipoElem = 0; tam = 0; return;
    }
}

DadoPacked::~DadoPacked() {
    if (tipoElemento == GDEXTENSION_VARIANT_TYPE_STRING) {
        for (size_t i = 0; i < tamanho(); ++i) destruirValor(tipoElemento, bytes.data() + i * tamElemento);
    }
}

DadoPacked* garantirPacked(int tipo, GPacked* p) {
    if (!p->d) {
        p->d = new DadoPacked();
        elementoPacked(tipo, p->d->tipoElemento, p->d->tamElemento);
    }
    return p->d;
}

void redimensionarPacked(DadoPacked* d, size_t n) {
    const size_t antigo = d->tamanho();
    if (d->tipoElemento == GDEXTENSION_VARIANT_TYPE_STRING) {
        for (size_t i = n; i < antigo; ++i) destruirValor(d->tipoElemento, d->bytes.data() + i * d->tamElemento);
    }
    d->bytes.resize(n * d->tamElemento, 0); // String nula = vazia, entao zero ja' e' valido
}

DadoArray::~DadoArray() {
    for (auto& v : itens) variantDestruir(&v);
}

DadoDicionario::~DadoDicionario() {
    for (auto& v : chaves) variantDestruir(&v);
    for (auto& v : valores) variantDestruir(&v);
}

void construirPadrao(int tipo, void* destino) {
    std::memset(destino, 0, tamanhoTipo(tipo));
    switch (tipo) {
    case GDEXTENSION_VARIANT_TYPE_STRING_NAME:
        static_cast<GStringName*>(destino)->n = internar(U"");
        break;
    case GDEXTENSION_VARIANT_TYPE_ARRAY:
        static_cast<GArray*>(destino)->d = new DadoArray();
        break;
    case GDEXTENSION_VARIANT_TYPE_DICTIONARY:
        static_cast<GDictionary*>(destino)->d = new DadoDicionario();
        break;
    case GDEXTENSION_VARIANT_TYPE_BASIS: {
        float* f = static_cast<float*>(destino);
        f[0] = f[4] = f[8] = 1.0f;
        break;
    }
    case GDEXTENSION_VARIANT_TYPE_TRANSFORM3D: {
        float* f = static_cast<float*>(destino);
        f[0] = f[4] = f[8] = 1.0f;
        break;
    }
    case GDEXTENSION_VARIANT_TYPE_TRANSFORM2D: {
        float* f = static_cast<float*>(destino);
        f[0] = f[3] = 1.0f;
        break;
    }
    case GDEXTENSION_VARIANT_TYPE_PROJECTION: {
        float* f = static_cast<float*>(destino);
        f[0] = f[5] = f[10] = f[15] = 1.0f;
        break;
    }
    case GDEXTENSION_VARIANT_TYPE_QUATERNION:
        static_cast<float*>(destino)[3] = 1.0f;
        break;
    case GDEXTENSION_VARIANT_TYPE_COLOR:
        static_cast<float*>(destino)[3] = 1.0f;
        break;
    default:
        break;
    }
}

void copiarValor(int tipo, void* destino, const void* origem) {
    switch (tipo) {
    case GDEXTENSION_VARIANT_TYPE_STRING:
    case GDEXTENSION_VARIANT_TYPE_NODE_PATH: {
        const GString* o = static_cast<const GString*>(origem);
        static_cast<GString*>(destino)->s = (o->s && !o->s->empty()) ? new std::u32string(*o->s) : nullptr;
        return;
    }
    case GDEXTENSION_VARIANT_TYPE_ARRAY: {
        DadoArray* d = static_cast<const GArray*>(origem)->d;
        if (d) d->ref.fetch_add(1);
        static_cast<GArray*>(destino)->d = d;
        return;
    }
    case GDEXTENSION_VARIANT_TYPE_DICTIONARY: {
        DadoDicionario* d = static_cast<const GDictionary*>(origem)->d;
        if (d) d->ref.fetch_add(1);
        static_cast<GDictionary*>(destino)->d = d;
        return;
    }
    case GDEXTENSION_VARIANT_TYPE_OBJECT: {
        Objeto* o = *static_cast<Objeto* const*>(origem);
        *static_cast<Objeto**>(destino) = o;
        return;
    }
    default:
        break;
    }
    if (ehPacked(tipo)) {
        const GPacked* o = static_cast<const GPacked*>(origem);
        GPacked* d = static_cast<GPacked*>(destino);
        d->d = nullptr;
        d->livre = nullptr;
        if (o->d) {
            DadoPacked* n = garantirPacked(tipo, d);
            if (n->tipoElemento == GDEXTENSION_VARIANT_TYPE_STRING) {
                redimensionarPacked(n, o->d->tamanho());
                for (size_t i = 0; i < o->d->tamanho(); ++i)
                    copiarValor(GDEXTENSION_VARIANT_TYPE_STRING, n->bytes.data() + i * n->tamElemento,
                                o->d->bytes.data() + i * n->tamElemento);
            } else {
                n->bytes = o->d->bytes;
            }
        }
        return;
    }
    std::memcpy(destino, origem, tamanhoTipo(tipo));
}

void destruirValor(int tipo, void* valor) {
    switch (tipo) {
    case GDEXTENSION_VARIANT_TYPE_STRING:
    case GDEXTENSION_VARIANT_TYPE_NODE_PATH: {
        GString* s = static_cast<GString*>(valor);
        delete s->s;
        s->s = nullptr;
        return;
    }
    case GDEXTENSION_VARIANT_TYPE_ARRAY: {
        GArray* a = static_cast<GArray*>(valor);
        if (a->d && a->d->ref.fetch_sub(1) == 1) delete a->d;
        a->d = nullptr;
        return;
    }
    case GDEXTENSION_VARIANT_TYPE_DICTIONARY: {
        GDictionary* a = static_cast<GDictionary*>(valor);
        if (a->d && a->d->ref.fetch_sub(1) == 1) delete a->d;
        a->d = nullptr;
        return;
    }
    default:
        break;
    }
    if (ehPacked(tipo)) {
        GPacked* p = static_cast<GPacked*>(valor);
        delete p->d;
        p->d = nullptr;
    }
}

// ---- Variant --------------------------------------------------------------------

static bool noHeap(int tipo) {
    return tamanhoTipo(tipo) > 16;
}

void* valorDe(GVariant* v) {
    if (noHeap(static_cast<int>(v->tipo))) {
        void* p;
        std::memcpy(&p, v->dado, sizeof(p));
        return p;
    }
    return v->dado;
}

const void* valorDe(const GVariant* v) {
    return valorDe(const_cast<GVariant*>(v));
}

void variantNil(GVariant* v) {
    std::memset(v, 0, sizeof(GVariant));
}

void variantDe(GVariant* v, int tipo, const void* valor) {
    variantNil(v);
    v->tipo = static_cast<uint32_t>(tipo);
    if (tipo == GDEXTENSION_VARIANT_TYPE_NIL) return;
    void* destino = v->dado;
    if (noHeap(tipo)) {
        void* p = std::malloc(tamanhoTipo(tipo));
        std::memcpy(v->dado, &p, sizeof(p));
        destino = p;
    }
    copiarValor(tipo, destino, valor);
    // Variant com RefCounted segura uma referencia, como no motor.
    if (tipo == GDEXTENSION_VARIANT_TYPE_OBJECT) {
        Objeto* o = *static_cast<Objeto* const*>(valor);
        if (o && o->classe && o->classe->refCounted) refIncrementar(o);
    }
}

void variantCopia(GVariant* destino, const GVariant* origem) {
    variantDe(destino, static_cast<int>(origem->tipo), valorDe(origem));
}

void variantDestruir(GVariant* v) {
    const int tipo = static_cast<int>(v->tipo);
    if (tipo == GDEXTENSION_VARIANT_TYPE_OBJECT) {
        Objeto* o = *static_cast<Objeto**>(valorDe(v));
        if (o && objetoVivo(o) && o->classe && o->classe->refCounted && refDecrementar(o)) destruirObjeto(o);
    } else if (tipo != GDEXTENSION_VARIANT_TYPE_NIL) {
        destruirValor(tipo, valorDe(v));
    }
    if (noHeap(tipo)) std::free(valorDe(v));
    variantNil(v);
}

void variantInt(GVariant* v, int64_t i) {
    variantDe(v, GDEXTENSION_VARIANT_TYPE_INT, &i);
}

void variantFloat(GVariant* v, double f) {
    variantDe(v, GDEXTENSION_VARIANT_TYPE_FLOAT, &f);
}

void variantStr(GVariant* v, const std::string& utf8) {
    GString s{utf8.empty() ? nullptr : new std::u32string(utf8ParaU32(utf8.data(), utf8.size()))};
    variantDe(v, GDEXTENSION_VARIANT_TYPE_STRING, &s);
    delete s.s;
}

bool variantBool(const GVariant* v) {
    const void* p = valorDe(v);
    switch (v->tipo) {
    case GDEXTENSION_VARIANT_TYPE_NIL: return false;
    case GDEXTENSION_VARIANT_TYPE_BOOL: return *static_cast<const uint8_t*>(p) != 0;
    case GDEXTENSION_VARIANT_TYPE_INT: return *static_cast<const int64_t*>(p) != 0;
    case GDEXTENSION_VARIANT_TYPE_FLOAT: return *static_cast<const double*>(p) != 0.0;
    case GDEXTENSION_VARIANT_TYPE_STRING:
    case GDEXTENSION_VARIANT_TYPE_NODE_PATH: {
        const GString* s = static_cast<const GString*>(p);
        return s->s && !s->s->empty();
    }
    case GDEXTENSION_VARIANT_TYPE_STRING_NAME: return !static_cast<const GStringName*>(p)->n->texto.empty();
    case GDEXTENSION_VARIANT_TYPE_OBJECT: return *static_cast<Objeto* const*>(p) != nullptr;
    case GDEXTENSION_VARIANT_TYPE_ARRAY: {
        const DadoArray* d = static_cast<const GArray*>(p)->d;
        return d && !d->itens.empty();
    }
    case GDEXTENSION_VARIANT_TYPE_DICTIONARY: {
        const DadoDicionario* d = static_cast<const GDictionary*>(p)->d;
        return d && !d->chaves.empty();
    }
    default: {
        // Tipos de valor: verdadeiro se algum byte for diferente de zero.
        const uint8_t* b = static_cast<const uint8_t*>(p);
        for (size_t i = 0; i < tamanhoTipo(static_cast<int>(v->tipo)); ++i)
            if (b[i]) return true;
        return false;
    }
    }
}

static std::u32string ascii(const char* s) {
    std::u32string r;
    while (*s) r.push_back(static_cast<char32_t>(static_cast<unsigned char>(*s++)));
    return r;
}

// Numero real como o motor imprime: "2.0", "1.5", "0.333333333333333".
static std::u32string textoReal(double d) {
    if (std::isnan(d)) return U"nan";
    if (std::isinf(d)) return d > 0 ? U"inf" : U"-inf";
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.14g", d);
    std::string s = buf;
    if (s.find_first_of(".e") == std::string::npos) s += ".0";
    return ascii(s.c_str());
}

static std::u32string textoFloats(const float* f, int n) {
    std::u32string r = U"(";
    for (int i = 0; i < n; ++i) {
        if (i) r += U", ";
        r += textoReal(f[i]);
    }
    return r + U")";
}

static std::u32string textoInts(const int32_t* v, int n) {
    std::u32string r = U"(";
    for (int i = 0; i < n; ++i) {
        if (i) r += U", ";
        r += ascii(std::to_string(v[i]).c_str());
    }
    return r + U")";
}

std::u32string variantTexto(const GVariant* v) {
    const void* p = valorDe(v);
    const float* f = static_cast<const float*>(p);
    switch (v->tipo) {
    case GDEXTENSION_VARIANT_TYPE_NIL: return U"<null>";
    case GDEXTENSION_VARIANT_TYPE_BOOL: return *static_cast<const uint8_t*>(p) ? U"true" : U"false";
    case GDEXTENSION_VARIANT_TYPE_INT: return ascii(std::to_string(*static_cast<const int64_t*>(p)).c_str());
    case GDEXTENSION_VARIANT_TYPE_FLOAT: return textoReal(*static_cast<const double*>(p));
    case GDEXTENSION_VARIANT_TYPE_STRING:
    case GDEXTENSION_VARIANT_TYPE_NODE_PATH: {
        const GString* s = static_cast<const GString*>(p);
        return s->s ? *s->s : std::u32string();
    }
    case GDEXTENSION_VARIANT_TYPE_STRING_NAME: return static_cast<const GStringName*>(p)->n->texto;
    case GDEXTENSION_VARIANT_TYPE_VECTOR2: return textoFloats(f, 2);
    case GDEXTENSION_VARIANT_TYPE_VECTOR3: return textoFloats(f, 3);
    case GDEXTENSION_VARIANT_TYPE_VECTOR4:
    case GDEXTENSION_VARIANT_TYPE_QUATERNION:
    case GDEXTENSION_VARIANT_TYPE_COLOR:
    case GDEXTENSION_VARIANT_TYPE_PLANE: return textoFloats(f, 4);
    case GDEXTENSION_VARIANT_TYPE_RECT2:
        return U"[P: " + textoFloats(f, 2) + U", S: " + textoFloats(f + 2, 2) + U"]";
    case GDEXTENSION_VARIANT_TYPE_VECTOR2I: return textoInts(static_cast<const int32_t*>(p), 2);
    case GDEXTENSION_VARIANT_TYPE_VECTOR3I: return textoInts(static_cast<const int32_t*>(p), 3);
    case GDEXTENSION_VARIANT_TYPE_VECTOR4I:
    case GDEXTENSION_VARIANT_TYPE_RECT2I: return textoInts(static_cast<const int32_t*>(p), 4);
    case GDEXTENSION_VARIANT_TYPE_OBJECT: {
        Objeto* o = *static_cast<Objeto* const*>(p);
        if (!o) return U"<Object#null>";
        return U"<" + (o->classe ? o->classe->nome->texto : std::u32string(U"Object")) + U"#" +
               ascii(std::to_string(o->id).c_str()) + U">";
    }
    case GDEXTENSION_VARIANT_TYPE_ARRAY: {
        const DadoArray* d = static_cast<const GArray*>(p)->d;
        std::u32string r = U"[";
        if (d) {
            for (size_t i = 0; i < d->itens.size(); ++i) {
                if (i) r += U", ";
                r += variantTexto(&d->itens[i]);
            }
        }
        return r + U"]";
    }
    case GDEXTENSION_VARIANT_TYPE_DICTIONARY: {
        const DadoDicionario* d = static_cast<const GDictionary*>(p)->d;
        std::u32string r = U"{ ";
        if (d) {
            for (size_t i = 0; i < d->chaves.size(); ++i) {
                if (i) r += U", ";
                r += variantTexto(&d->chaves[i]) + U": " + variantTexto(&d->valores[i]);
            }
        }
        return r + U" }";
    }
    default: {
        return U"<" + ascii(nomeTipo(static_cast<int>(v->tipo))) + U">";
    }
    }
}

static bool ehNumero(int t) {
    return t == GDEXTENSION_VARIANT_TYPE_INT || t == GDEXTENSION_VARIANT_TYPE_FLOAT;
}

static double comoDouble(const GVariant* v) {
    if (v->tipo == GDEXTENSION_VARIANT_TYPE_INT) return static_cast<double>(*static_cast<const int64_t*>(valorDe(v)));
    if (v->tipo == GDEXTENSION_VARIANT_TYPE_FLOAT) return *static_cast<const double*>(valorDe(v));
    if (v->tipo == GDEXTENSION_VARIANT_TYPE_BOOL) return *static_cast<const uint8_t*>(valorDe(v)) ? 1.0 : 0.0;
    return 0.0;
}

static bool ehTexto(int t) {
    return t == GDEXTENSION_VARIANT_TYPE_STRING || t == GDEXTENSION_VARIANT_TYPE_STRING_NAME;
}

bool variantIgual(const GVariant* a, const GVariant* b) {
    const int ta = static_cast<int>(a->tipo), tb = static_cast<int>(b->tipo);
    if (ehNumero(ta) && ehNumero(tb)) return comoDouble(a) == comoDouble(b);
    if (ehTexto(ta) && ehTexto(tb)) return variantTexto(a) == variantTexto(b);
    if (ta != tb) return false;
    const void* pa = valorDe(a);
    const void* pb = valorDe(b);
    switch (ta) {
    case GDEXTENSION_VARIANT_TYPE_NIL: return true;
    case GDEXTENSION_VARIANT_TYPE_NODE_PATH: return variantTexto(a) == variantTexto(b);
    case GDEXTENSION_VARIANT_TYPE_ARRAY: {
        const DadoArray* da = static_cast<const GArray*>(pa)->d;
        const DadoArray* db = static_cast<const GArray*>(pb)->d;
        if (da == db) return true;
        if (!da || !db || da->itens.size() != db->itens.size()) return false;
        for (size_t i = 0; i < da->itens.size(); ++i)
            if (!variantIgual(&da->itens[i], &db->itens[i])) return false;
        return true;
    }
    case GDEXTENSION_VARIANT_TYPE_DICTIONARY:
        return static_cast<const GDictionary*>(pa)->d == static_cast<const GDictionary*>(pb)->d;
    default:
        break;
    }
    if (ehPacked(ta)) {
        const DadoPacked* da = static_cast<const GPacked*>(pa)->d;
        const DadoPacked* db = static_cast<const GPacked*>(pb)->d;
        const size_t na = da ? da->tamanho() : 0, nb = db ? db->tamanho() : 0;
        if (na != nb) return false;
        if (na == 0) return true;
        if (da->tipoElemento == GDEXTENSION_VARIANT_TYPE_STRING) {
            for (size_t i = 0; i < na; ++i) {
                const GString* sa = reinterpret_cast<const GString*>(da->bytes.data() + i * da->tamElemento);
                const GString* sb = reinterpret_cast<const GString*>(db->bytes.data() + i * db->tamElemento);
                if ((sa->s ? *sa->s : U"") != (sb->s ? *sb->s : U"")) return false;
            }
            return true;
        }
        return da->bytes == db->bytes;
    }
    return std::memcmp(pa, pb, tamanhoTipo(ta)) == 0;
}

// ---- Funcoes da interface: Variant ------------------------------------------------

namespace {

GVariant* V(GDExtensionVariantPtr p) {
    return static_cast<GVariant*>(p);
}
const GVariant* CV(GDExtensionConstVariantPtr p) {
    return static_cast<const GVariant*>(p);
}

void fn_variant_new_copy(GDExtensionUninitializedVariantPtr dst, GDExtensionConstVariantPtr src) {
    variantCopia(static_cast<GVariant*>(dst), CV(src));
}

void fn_variant_new_nil(GDExtensionUninitializedVariantPtr dst) {
    variantNil(static_cast<GVariant*>(dst));
}

void fn_variant_destroy(GDExtensionVariantPtr self) {
    variantDestruir(V(self));
}

GDExtensionVariantType fn_variant_get_type(GDExtensionConstVariantPtr self) {
    return static_cast<GDExtensionVariantType>(CV(self)->tipo);
}

GDExtensionBool fn_variant_booleanize(GDExtensionConstVariantPtr self) {
    return variantBool(CV(self)) ? 1 : 0;
}

void fn_variant_stringify(GDExtensionConstVariantPtr self, GDExtensionStringPtr r_ret) {
    GString* s = static_cast<GString*>(r_ret);
    delete s->s;
    const std::u32string t = variantTexto(CV(self));
    s->s = t.empty() ? nullptr : new std::u32string(t);
}

GDExtensionInt fn_variant_hash(GDExtensionConstVariantPtr self) {
    const GVariant* v = CV(self);
    if (ehTexto(static_cast<int>(v->tipo))) return hashTexto(variantTexto(v));
    if (v->tipo == GDEXTENSION_VARIANT_TYPE_OBJECT) {
        return static_cast<GDExtensionInt>(reinterpret_cast<uintptr_t>(*static_cast<Objeto* const*>(valorDe(v))));
    }
    return hashTexto(variantTexto(v));
}

GDExtensionInt fn_variant_recursive_hash(GDExtensionConstVariantPtr self, GDExtensionInt) {
    return fn_variant_hash(self);
}

GDExtensionBool fn_variant_hash_compare(GDExtensionConstVariantPtr a, GDExtensionConstVariantPtr b) {
    return variantIgual(CV(a), CV(b)) ? 1 : 0;
}

void fn_variant_duplicate(GDExtensionConstVariantPtr self, GDExtensionVariantPtr r_ret, GDExtensionBool) {
    variantDestruir(V(r_ret));
    variantCopia(V(r_ret), CV(self));
}

GDExtensionObjectPtr objetoDoVariant(const GVariant* v) {
    if (v->tipo != GDEXTENSION_VARIANT_TYPE_OBJECT) return nullptr;
    return *static_cast<Objeto* const*>(valorDe(v));
}

GDExtensionInt fn_variant_get_object_instance_id(GDExtensionConstVariantPtr self) {
    Objeto* o = static_cast<Objeto*>(objetoDoVariant(CV(self)));
    return o ? static_cast<GDExtensionInt>(o->id) : 0;
}

void fn_variant_get_type_name(GDExtensionVariantType tipo, GDExtensionUninitializedStringPtr r_name) {
    const std::u32string t = ascii(nomeTipo(tipo));
    static_cast<GString*>(r_name)->s = new std::u32string(t);
}

GDExtensionBool fn_variant_can_convert(GDExtensionVariantType de, GDExtensionVariantType para) {
    // Qualquer tipo vira Variant (NIL como destino); nulo serve de objeto.
    if (de == para || para == GDEXTENSION_VARIANT_TYPE_NIL) return 1;
    if (de == GDEXTENSION_VARIANT_TYPE_NIL && para == GDEXTENSION_VARIANT_TYPE_OBJECT) return 1;
    if (ehNumero(de) && ehNumero(para)) return 1;
    if (ehTexto(de) && ehTexto(para)) return 1;
    if (para == GDEXTENSION_VARIANT_TYPE_BOOL && (ehNumero(de) || de == GDEXTENSION_VARIANT_TYPE_NIL)) return 1;
    if (para == GDEXTENSION_VARIANT_TYPE_STRING) return 1;
    return 0;
}

GDExtensionBool fn_variant_can_convert_strict(GDExtensionVariantType de, GDExtensionVariantType para) {
    // Qualquer tipo vira Variant (NIL como destino); nulo serve de objeto.
    if (de == para || para == GDEXTENSION_VARIANT_TYPE_NIL) return 1;
    if (de == GDEXTENSION_VARIANT_TYPE_NIL && para == GDEXTENSION_VARIANT_TYPE_OBJECT) return 1;
    if (ehNumero(de) && ehNumero(para)) return 1;
    if (ehTexto(de) && ehTexto(para)) return 1;
    return 0;
}

// Chamada de metodo sobre um Variant (objeto, ou metodo builtin por nome).
void fn_variant_call(GDExtensionVariantPtr self, GDExtensionConstStringNamePtr metodo, const GDExtensionConstVariantPtr* args,
                     GDExtensionInt argc, GDExtensionUninitializedVariantPtr r_ret, GDExtensionCallError* r_erro) {
    GVariant* ret = static_cast<GVariant*>(r_ret);
    variantNil(ret);
    r_erro->error = GDEXTENSION_CALL_OK;
    const NomeInterno* nome = static_cast<const GStringName*>(metodo)->n;
    GVariant* v = V(self);
    if (v->tipo == GDEXTENSION_VARIANT_TYPE_OBJECT) {
        Objeto* o = *static_cast<Objeto**>(valorDe(v));
        if (!o) {
            r_erro->error = GDEXTENSION_CALL_ERROR_INSTANCE_IS_NULL;
            return;
        }
        chamarMetodo(o, nome, reinterpret_cast<const GVariant* const*>(args), static_cast<int>(argc), ret, r_erro);
        return;
    }
    avisarUmaVez("variant_call:" + nome->utf8, "variant_call em %s.%s ainda nao suportado",
                 nomeTipo(static_cast<int>(v->tipo)), nome->utf8.c_str());
    r_erro->error = GDEXTENSION_CALL_ERROR_INVALID_METHOD;
}

void fn_variant_evaluate(GDExtensionVariantOperator op, GDExtensionConstVariantPtr a, GDExtensionConstVariantPtr b,
                         GDExtensionUninitializedVariantPtr r_ret, GDExtensionBool* r_valido) {
    GVariant* ret = static_cast<GVariant*>(r_ret);
    variantNil(ret);
    const GVariant* va = CV(a);
    const GVariant* vb = CV(b);
    GDExtensionPtrOperatorEvaluator f = operadorPtr(op, static_cast<int>(va->tipo), static_cast<int>(vb->tipo));
    if (!f) {
        *r_valido = 0;
        return;
    }
    // Tipo do resultado: comparacoes e logicos dao bool; o resto segue o operando.
    int tipoRet;
    if (op <= GDEXTENSION_VARIANT_OP_GREATER_EQUAL || op >= GDEXTENSION_VARIANT_OP_AND)
        tipoRet = GDEXTENSION_VARIANT_TYPE_BOOL;
    else if (ehNumero(static_cast<int>(va->tipo)) && ehNumero(static_cast<int>(vb->tipo)) &&
             (va->tipo == GDEXTENSION_VARIANT_TYPE_FLOAT || vb->tipo == GDEXTENSION_VARIANT_TYPE_FLOAT))
        tipoRet = GDEXTENSION_VARIANT_TYPE_FLOAT;
    else if (ehTexto(static_cast<int>(va->tipo)))
        tipoRet = GDEXTENSION_VARIANT_TYPE_STRING;
    else
        tipoRet = static_cast<int>(va->tipo);
    alignas(16) uint8_t tmp[64];
    construirPadrao(tipoRet, tmp);
    f(valorDe(va), valorDe(vb), tmp);
    variantDe(ret, tipoRet, tmp);
    destruirValor(tipoRet, tmp);
    *r_valido = 1;
}

void fn_variant_construct(GDExtensionVariantType tipo, GDExtensionUninitializedVariantPtr r_base,
                          const GDExtensionConstVariantPtr* args, int32_t argc, GDExtensionCallError* r_erro) {
    GVariant* v = static_cast<GVariant*>(r_base);
    r_erro->error = GDEXTENSION_CALL_OK;
    alignas(16) uint8_t tmp[64];
    if (argc == 0) {
        construirPadrao(tipo, tmp);
    } else if (argc == 1 && CV(args[0])->tipo == static_cast<uint32_t>(tipo)) {
        copiarValor(tipo, tmp, valorDe(CV(args[0])));
    } else {
        avisarUmaVez(std::string("variant_construct:") + nomeTipo(tipo), "variant_construct de %s com %d argumentos nao suportado",
                     nomeTipo(tipo), argc);
        variantNil(v);
        r_erro->error = GDEXTENSION_CALL_ERROR_INVALID_METHOD;
        return;
    }
    variantDe(v, tipo, tmp);
    destruirValor(tipo, tmp);
}

// Propriedades e indices de Variant: so' o essencial para Array/Dictionary.
void fn_variant_get_indexed(GDExtensionConstVariantPtr self, GDExtensionInt i, GDExtensionUninitializedVariantPtr r_ret,
                            GDExtensionBool* r_valido, GDExtensionBool* r_oob) {
    GVariant* ret = static_cast<GVariant*>(r_ret);
    variantNil(ret);
    *r_valido = 0;
    *r_oob = 0;
    const GVariant* v = CV(self);
    if (v->tipo == GDEXTENSION_VARIANT_TYPE_ARRAY) {
        DadoArray* d = static_cast<const GArray*>(valorDe(v))->d;
        if (!d || i < 0 || static_cast<size_t>(i) >= d->itens.size()) {
            *r_oob = 1;
            return;
        }
        variantCopia(ret, &d->itens[static_cast<size_t>(i)]);
        *r_valido = 1;
    }
}

void fn_variant_get_keyed(GDExtensionConstVariantPtr self, GDExtensionConstVariantPtr chave, GDExtensionUninitializedVariantPtr r_ret,
                          GDExtensionBool* r_valido) {
    GVariant* ret = static_cast<GVariant*>(r_ret);
    variantNil(ret);
    *r_valido = 0;
    const GVariant* v = CV(self);
    if (v->tipo == GDEXTENSION_VARIANT_TYPE_DICTIONARY) {
        DadoDicionario* d = static_cast<const GDictionary*>(valorDe(v))->d;
        for (size_t i = 0; d && i < d->chaves.size(); ++i) {
            if (variantIgual(&d->chaves[i], CV(chave))) {
                variantCopia(ret, &d->valores[i]);
                *r_valido = 1;
                return;
            }
        }
    }
}

void fn_variant_get(GDExtensionConstVariantPtr self, GDExtensionConstVariantPtr chave, GDExtensionUninitializedVariantPtr r_ret,
                    GDExtensionBool* r_valido) {
    const GVariant* v = CV(self);
    if (v->tipo == GDEXTENSION_VARIANT_TYPE_ARRAY && CV(chave)->tipo == GDEXTENSION_VARIANT_TYPE_INT) {
        GDExtensionBool oob = 0;
        fn_variant_get_indexed(self, *static_cast<const int64_t*>(valorDe(CV(chave))), r_ret, r_valido, &oob);
        return;
    }
    fn_variant_get_keyed(self, chave, r_ret, r_valido);
}

void fn_variant_set_keyed(GDExtensionVariantPtr self, GDExtensionConstVariantPtr chave, GDExtensionConstVariantPtr valor,
                          GDExtensionBool* r_valido) {
    *r_valido = 0;
    GVariant* v = V(self);
    if (v->tipo != GDEXTENSION_VARIANT_TYPE_DICTIONARY) return;
    DadoDicionario* d = static_cast<GDictionary*>(valorDe(v))->d;
    if (!d) return;
    for (size_t i = 0; i < d->chaves.size(); ++i) {
        if (variantIgual(&d->chaves[i], CV(chave))) {
            variantDestruir(&d->valores[i]);
            variantCopia(&d->valores[i], CV(valor));
            *r_valido = 1;
            return;
        }
    }
    d->chaves.emplace_back();
    variantCopia(&d->chaves.back(), CV(chave));
    d->valores.emplace_back();
    variantCopia(&d->valores.back(), CV(valor));
    *r_valido = 1;
}

void fn_variant_set_indexed(GDExtensionVariantPtr self, GDExtensionInt i, GDExtensionConstVariantPtr valor, GDExtensionBool* r_valido,
                            GDExtensionBool* r_oob) {
    *r_valido = 0;
    *r_oob = 0;
    GVariant* v = V(self);
    if (v->tipo != GDEXTENSION_VARIANT_TYPE_ARRAY) return;
    DadoArray* d = static_cast<GArray*>(valorDe(v))->d;
    if (!d || i < 0 || static_cast<size_t>(i) >= d->itens.size()) {
        *r_oob = 1;
        return;
    }
    variantDestruir(&d->itens[static_cast<size_t>(i)]);
    variantCopia(&d->itens[static_cast<size_t>(i)], CV(valor));
    *r_valido = 1;
}

void fn_variant_set(GDExtensionVariantPtr self, GDExtensionConstVariantPtr chave, GDExtensionConstVariantPtr valor,
                    GDExtensionBool* r_valido) {
    if (V(self)->tipo == GDEXTENSION_VARIANT_TYPE_ARRAY && CV(chave)->tipo == GDEXTENSION_VARIANT_TYPE_INT) {
        GDExtensionBool oob = 0;
        fn_variant_set_indexed(self, *static_cast<const int64_t*>(valorDe(CV(chave))), valor, r_valido, &oob);
        return;
    }
    fn_variant_set_keyed(self, chave, valor, r_valido);
}

void fn_variant_get_named(GDExtensionConstVariantPtr self, GDExtensionConstStringNamePtr nome, GDExtensionUninitializedVariantPtr r_ret,
                          GDExtensionBool* r_valido) {
    variantNil(static_cast<GVariant*>(r_ret));
    *r_valido = 0;
    avisarUmaVez("variant_get_named", "variant_get_named (%s) ainda nao suportado",
                 static_cast<const GStringName*>(nome)->n->utf8.c_str());
    (void)self;
}

void fn_variant_set_named(GDExtensionVariantPtr, GDExtensionConstStringNamePtr nome, GDExtensionConstVariantPtr, GDExtensionBool* r_valido) {
    *r_valido = 0;
    avisarUmaVez("variant_set_named", "variant_set_named (%s) ainda nao suportado",
                 static_cast<const GStringName*>(nome)->n->utf8.c_str());
}

GDExtensionBool fn_variant_has_key(GDExtensionConstVariantPtr self, GDExtensionConstVariantPtr chave, GDExtensionBool* r_valido) {
    GDExtensionBool ok = 0;
    GVariant tmp;
    fn_variant_get_keyed(self, chave, &tmp, &ok);
    variantDestruir(&tmp);
    *r_valido = CV(self)->tipo == GDEXTENSION_VARIANT_TYPE_DICTIONARY;
    return ok;
}

GDExtensionBool fn_variant_has_method(GDExtensionConstVariantPtr self, GDExtensionConstStringNamePtr metodo) {
    const GVariant* v = CV(self);
    if (v->tipo != GDEXTENSION_VARIANT_TYPE_OBJECT) return 0;
    Objeto* o = *static_cast<Objeto* const*>(valorDe(v));
    if (!o) return 0;
    const NomeInterno* n = static_cast<const GStringName*>(metodo)->n;
    return (metodoExtensao(o->classe, n) || metodoMotorPorCadeia(o->classe, n->utf8)) ? 1 : 0;
}

GDExtensionBool fn_variant_has_member(GDExtensionVariantType, GDExtensionConstStringNamePtr) {
    return 0;
}

GDExtensionBool fn_variant_iter_init(GDExtensionConstVariantPtr self, GDExtensionUninitializedVariantPtr r_iter, GDExtensionBool* r_valido) {
    GVariant* it = static_cast<GVariant*>(r_iter);
    variantInt(it, 0);
    const GVariant* v = CV(self);
    *r_valido = 1;
    if (v->tipo == GDEXTENSION_VARIANT_TYPE_ARRAY) {
        const DadoArray* d = static_cast<const GArray*>(valorDe(v))->d;
        return d && !d->itens.empty();
    }
    if (v->tipo == GDEXTENSION_VARIANT_TYPE_DICTIONARY) {
        const DadoDicionario* d = static_cast<const GDictionary*>(valorDe(v))->d;
        return d && !d->chaves.empty();
    }
    *r_valido = 0;
    return 0;
}

GDExtensionBool fn_variant_iter_next(GDExtensionConstVariantPtr self, GDExtensionVariantPtr r_iter, GDExtensionBool* r_valido) {
    GVariant* it = V(r_iter);
    int64_t* i = static_cast<int64_t*>(valorDe(it));
    ++*i;
    const GVariant* v = CV(self);
    *r_valido = 1;
    size_t n = 0;
    if (v->tipo == GDEXTENSION_VARIANT_TYPE_ARRAY) {
        const DadoArray* d = static_cast<const GArray*>(valorDe(v))->d;
        n = d ? d->itens.size() : 0;
    } else if (v->tipo == GDEXTENSION_VARIANT_TYPE_DICTIONARY) {
        const DadoDicionario* d = static_cast<const GDictionary*>(valorDe(v))->d;
        n = d ? d->chaves.size() : 0;
    }
    return static_cast<size_t>(*i) < n;
}

void fn_variant_iter_get(GDExtensionConstVariantPtr self, GDExtensionVariantPtr r_iter, GDExtensionUninitializedVariantPtr r_ret,
                         GDExtensionBool* r_valido) {
    GVariant* ret = static_cast<GVariant*>(r_ret);
    variantNil(ret);
    const size_t i = static_cast<size_t>(*static_cast<int64_t*>(valorDe(V(r_iter))));
    const GVariant* v = CV(self);
    *r_valido = 0;
    if (v->tipo == GDEXTENSION_VARIANT_TYPE_ARRAY) {
        const DadoArray* d = static_cast<const GArray*>(valorDe(v))->d;
        if (d && i < d->itens.size()) {
            variantCopia(ret, &d->itens[i]);
            *r_valido = 1;
        }
    } else if (v->tipo == GDEXTENSION_VARIANT_TYPE_DICTIONARY) {
        const DadoDicionario* d = static_cast<const GDictionary*>(valorDe(v))->d;
        if (d && i < d->chaves.size()) {
            variantCopia(ret, &d->chaves[i]);
            *r_valido = 1;
        }
    }
}

// Construtores Variant <-> tipo, um par por tipo (o tipo vem do indice).
template <int T>
void deTipo(GDExtensionUninitializedVariantPtr dst, GDExtensionTypePtr src) {
    variantDe(static_cast<GVariant*>(dst), T, src);
}

template <int T>
void paraTipo(GDExtensionUninitializedTypePtr dst, GDExtensionVariantPtr src) {
    const GVariant* v = CV(src);
    if (static_cast<int>(v->tipo) == T) {
        copiarValor(T, dst, valorDe(v));
        return;
    }
    // Conversoes implicitas que o motor aceita entre numeros e textos.
    construirPadrao(T, dst);
    if constexpr (T == GDEXTENSION_VARIANT_TYPE_INT) {
        *static_cast<int64_t*>(dst) = static_cast<int64_t>(comoDouble(v));
    } else if constexpr (T == GDEXTENSION_VARIANT_TYPE_FLOAT) {
        *static_cast<double*>(dst) = comoDouble(v);
    } else if constexpr (T == GDEXTENSION_VARIANT_TYPE_BOOL) {
        *static_cast<uint8_t*>(dst) = variantBool(v) ? 1 : 0;
    } else if constexpr (T == GDEXTENSION_VARIANT_TYPE_STRING || T == GDEXTENSION_VARIANT_TYPE_NODE_PATH) {
        const std::u32string t = variantTexto(v);
        static_cast<GString*>(dst)->s = t.empty() ? nullptr : new std::u32string(t);
    } else if constexpr (T == GDEXTENSION_VARIANT_TYPE_STRING_NAME) {
        static_cast<GStringName*>(dst)->n = internar(variantTexto(v));
    } else if constexpr (T == GDEXTENSION_VARIANT_TYPE_OBJECT) {
        *static_cast<Objeto**>(dst) = nullptr;
    }
}

template <int T>
void* internoDe(GDExtensionVariantPtr v) {
    return valorDe(V(v));
}

template <int... Is>
struct TabelasVariant {
    static constexpr GDExtensionVariantFromTypeConstructorFunc de[] = {&deTipo<Is>...};
    static constexpr GDExtensionTypeFromVariantConstructorFunc para[] = {&paraTipo<Is>...};
    static constexpr GDExtensionVariantGetInternalPtrFunc interno[] = {&internoDe<Is>...};
};

template <int... Is>
constexpr TabelasVariant<Is...> fazerTabelas(std::integer_sequence<int, Is...>) {
    return {};
}

using Tabelas = decltype(fazerTabelas(std::make_integer_sequence<int, kTiposVariant>{}));

GDExtensionVariantFromTypeConstructorFunc fn_get_variant_from_type_constructor(GDExtensionVariantType t) {
    return (t > 0 && t < kTiposVariant) ? Tabelas::de[t] : nullptr;
}

GDExtensionTypeFromVariantConstructorFunc fn_get_variant_to_type_constructor(GDExtensionVariantType t) {
    return (t > 0 && t < kTiposVariant) ? Tabelas::para[t] : nullptr;
}

GDExtensionVariantGetInternalPtrFunc fn_variant_get_ptr_internal_getter(GDExtensionVariantType t) {
    return (t > 0 && t < kTiposVariant) ? Tabelas::interno[t] : nullptr;
}

template <int T>
void destrutor(GDExtensionTypePtr p) {
    destruirValor(T, p);
}

template <int... Is>
struct TabelaDestrutores {
    static constexpr GDExtensionPtrDestructor fn[] = {&destrutor<Is>...};
};
template <int... Is>
constexpr TabelaDestrutores<Is...> fazerDestrutores(std::integer_sequence<int, Is...>) {
    return {};
}
using Destrutores = decltype(fazerDestrutores(std::make_integer_sequence<int, kTiposVariant>{}));

GDExtensionPtrDestructor fn_variant_get_ptr_destructor(GDExtensionVariantType t) {
    return (t > 0 && t < kTiposVariant) ? Destrutores::fn[t] : nullptr;
}

GDExtensionPtrConstructor fn_variant_get_ptr_constructor(GDExtensionVariantType t, int32_t indice) {
    return construtorPtr(t, indice);
}

GDExtensionPtrOperatorEvaluator fn_variant_get_ptr_operator_evaluator(GDExtensionVariantOperator op, GDExtensionVariantType a,
                                                                      GDExtensionVariantType b) {
    return operadorPtr(op, a, b);
}

GDExtensionPtrBuiltInMethod fn_variant_get_ptr_builtin_method(GDExtensionVariantType t, GDExtensionConstStringNamePtr nome,
                                                              GDExtensionInt) {
    return metodoBuiltin(t, static_cast<const GStringName*>(nome)->n);
}

GDExtensionPtrUtilityFunction fn_variant_get_ptr_utility_function(GDExtensionConstStringNamePtr nome, GDExtensionInt) {
    return funcaoUtilitaria(static_cast<const GStringName*>(nome)->n);
}

// Setters/getters de membro (Vector2.x etc.): offsets fixos de float.
template <int Indice>
void setterFloat(GDExtensionTypePtr base, GDExtensionConstTypePtr valor) {
    static_cast<float*>(base)[Indice] = static_cast<float>(*static_cast<const double*>(valor));
}
template <int Indice>
void getterFloat(GDExtensionConstTypePtr base, GDExtensionTypePtr r) {
    *static_cast<double*>(r) = static_cast<const float*>(base)[Indice];
}
template <int Indice>
void setterInt(GDExtensionTypePtr base, GDExtensionConstTypePtr valor) {
    static_cast<int32_t*>(base)[Indice] = static_cast<int32_t>(*static_cast<const int64_t*>(valor));
}
template <int Indice>
void getterInt(GDExtensionConstTypePtr base, GDExtensionTypePtr r) {
    *static_cast<int64_t*>(r) = static_cast<const int32_t*>(base)[Indice];
}

int indiceMembro(int t, const std::string& m) {
    const bool cor = t == GDEXTENSION_VARIANT_TYPE_COLOR;
    if (m == (cor ? "r" : "x")) return 0;
    if (m == (cor ? "g" : "y")) return 1;
    if (m == (cor ? "b" : "z")) return 2;
    if (m == (cor ? "a" : "w")) return 3;
    return -1;
}

bool componentesInt(int t) {
    return t == GDEXTENSION_VARIANT_TYPE_VECTOR2I || t == GDEXTENSION_VARIANT_TYPE_VECTOR3I ||
           t == GDEXTENSION_VARIANT_TYPE_VECTOR4I;
}

GDExtensionPtrSetter fn_variant_get_ptr_setter(GDExtensionVariantType t, GDExtensionConstStringNamePtr membro) {
    const std::string& m = static_cast<const GStringName*>(membro)->n->utf8;
    static const GDExtensionPtrSetter fl[] = {&setterFloat<0>, &setterFloat<1>, &setterFloat<2>, &setterFloat<3>};
    static const GDExtensionPtrSetter in[] = {&setterInt<0>, &setterInt<1>, &setterInt<2>, &setterInt<3>};
    const int i = indiceMembro(t, m);
    if (i < 0) {
        avisarUmaVez(std::string("setter:") + nomeTipo(t) + "." + m, "membro %s.%s nao suportado", nomeTipo(t), m.c_str());
        return nullptr;
    }
    return componentesInt(t) ? in[i] : fl[i];
}

GDExtensionPtrGetter fn_variant_get_ptr_getter(GDExtensionVariantType t, GDExtensionConstStringNamePtr membro) {
    const std::string& m = static_cast<const GStringName*>(membro)->n->utf8;
    static const GDExtensionPtrGetter fl[] = {&getterFloat<0>, &getterFloat<1>, &getterFloat<2>, &getterFloat<3>};
    static const GDExtensionPtrGetter in[] = {&getterInt<0>, &getterInt<1>, &getterInt<2>, &getterInt<3>};
    const int i = indiceMembro(t, m);
    if (i < 0) {
        avisarUmaVez(std::string("getter:") + nomeTipo(t) + "." + m, "membro %s.%s nao suportado", nomeTipo(t), m.c_str());
        return nullptr;
    }
    return componentesInt(t) ? in[i] : fl[i];
}

// Indexacao tipada (Array[i], PackedX[i]) via ponteiro: Variant por indice.
void indexadoArrayGet(GDExtensionConstTypePtr base, GDExtensionInt i, GDExtensionTypePtr r) {
    const DadoArray* d = static_cast<const GArray*>(base)->d;
    GVariant* ret = static_cast<GVariant*>(r);
    variantDestruir(ret);
    if (d && i >= 0 && static_cast<size_t>(i) < d->itens.size()) variantCopia(ret, &d->itens[static_cast<size_t>(i)]);
}
void indexadoArraySet(GDExtensionTypePtr base, GDExtensionInt i, GDExtensionConstTypePtr v) {
    DadoArray* d = static_cast<GArray*>(base)->d;
    if (d && i >= 0 && static_cast<size_t>(i) < d->itens.size()) {
        variantDestruir(&d->itens[static_cast<size_t>(i)]);
        variantCopia(&d->itens[static_cast<size_t>(i)], static_cast<const GVariant*>(v));
    }
}

GDExtensionPtrIndexedSetter fn_variant_get_ptr_indexed_setter(GDExtensionVariantType t) {
    if (t == GDEXTENSION_VARIANT_TYPE_ARRAY) return &indexadoArraySet;
    return nullptr;
}
GDExtensionPtrIndexedGetter fn_variant_get_ptr_indexed_getter(GDExtensionVariantType t) {
    if (t == GDEXTENSION_VARIANT_TYPE_ARRAY) return &indexadoArrayGet;
    return nullptr;
}
GDExtensionPtrKeyedSetter fn_variant_get_ptr_keyed_setter(GDExtensionVariantType) {
    return nullptr;
}
GDExtensionPtrKeyedGetter fn_variant_get_ptr_keyed_getter(GDExtensionVariantType) {
    return nullptr;
}
GDExtensionPtrKeyedChecker fn_variant_get_ptr_keyed_checker(GDExtensionVariantType) {
    return nullptr;
}

void fn_variant_get_constant_value(GDExtensionVariantType t, GDExtensionConstStringNamePtr nome, GDExtensionUninitializedVariantPtr r_ret) {
    variantNil(static_cast<GVariant*>(r_ret));
    avisarUmaVez(std::string("const:") + nomeTipo(t), "constante %s.%s nao suportada", nomeTipo(t),
                 static_cast<const GStringName*>(nome)->n->utf8.c_str());
}

void fn_variant_call_static(GDExtensionVariantType t, GDExtensionConstStringNamePtr metodo, const GDExtensionConstVariantPtr*, GDExtensionInt,
                            GDExtensionUninitializedVariantPtr r_ret, GDExtensionCallError* r_erro) {
    variantNil(static_cast<GVariant*>(r_ret));
    r_erro->error = GDEXTENSION_CALL_ERROR_INVALID_METHOD;
    avisarUmaVez(std::string("call_static:") + nomeTipo(t), "chamada estatica %s.%s nao suportada", nomeTipo(t),
                 static_cast<const GStringName*>(metodo)->n->utf8.c_str());
}

// ---- Funcoes da interface: String / StringName -------------------------------------

GString* S(GDExtensionStringPtr p) {
    return static_cast<GString*>(p);
}

void novaString(GDExtensionUninitializedStringPtr r, std::u32string&& t) {
    static_cast<GString*>(r)->s = t.empty() ? nullptr : new std::u32string(std::move(t));
}

void fn_string_new_with_latin1_chars(GDExtensionUninitializedStringPtr r, const char* c) {
    std::u32string t;
    for (const char* p = c; p && *p; ++p) t.push_back(static_cast<unsigned char>(*p));
    novaString(r, std::move(t));
}
void fn_string_new_with_utf8_chars(GDExtensionUninitializedStringPtr r, const char* c) {
    novaString(r, c ? utf8ParaU32(c, std::strlen(c)) : std::u32string());
}
void fn_string_new_with_utf16_chars(GDExtensionUninitializedStringPtr r, const char16_t* c) {
    std::u32string t;
    for (const char16_t* p = c; p && *p; ++p) t.push_back(*p);
    novaString(r, std::move(t));
}
void fn_string_new_with_utf32_chars(GDExtensionUninitializedStringPtr r, const char32_t* c) {
    novaString(r, c ? std::u32string(c) : std::u32string());
}
void fn_string_new_with_wide_chars(GDExtensionUninitializedStringPtr r, const wchar_t* c) {
    std::u32string t;
    for (const wchar_t* p = c; p && *p; ++p) t.push_back(static_cast<char32_t>(*p));
    novaString(r, std::move(t));
}
void fn_string_new_with_latin1_chars_and_len(GDExtensionUninitializedStringPtr r, const char* c, GDExtensionInt n) {
    std::u32string t;
    for (GDExtensionInt i = 0; c && i < n && c[i]; ++i) t.push_back(static_cast<unsigned char>(c[i]));
    novaString(r, std::move(t));
}
void fn_string_new_with_utf8_chars_and_len(GDExtensionUninitializedStringPtr r, const char* c, GDExtensionInt n) {
    size_t len = 0;
    while (c && static_cast<GDExtensionInt>(len) < n && c[len]) ++len;
    novaString(r, c ? utf8ParaU32(c, len) : std::u32string());
}
GDExtensionInt fn_string_new_with_utf8_chars_and_len2(GDExtensionUninitializedStringPtr r, const char* c, GDExtensionInt n) {
    fn_string_new_with_utf8_chars_and_len(r, c, n);
    return 0;
}
void fn_string_new_with_utf16_chars_and_len(GDExtensionUninitializedStringPtr r, const char16_t* c, GDExtensionInt n) {
    std::u32string t;
    for (GDExtensionInt i = 0; c && i < n && c[i]; ++i) t.push_back(c[i]);
    novaString(r, std::move(t));
}
GDExtensionInt fn_string_new_with_utf16_chars_and_len2(GDExtensionUninitializedStringPtr r, const char16_t* c, GDExtensionInt n,
                                                       GDExtensionBool) {
    fn_string_new_with_utf16_chars_and_len(r, c, n);
    return 0;
}
void fn_string_new_with_utf32_chars_and_len(GDExtensionUninitializedStringPtr r, const char32_t* c, GDExtensionInt n) {
    std::u32string t;
    for (GDExtensionInt i = 0; c && i < n && c[i]; ++i) t.push_back(c[i]);
    novaString(r, std::move(t));
}
void fn_string_new_with_wide_chars_and_len(GDExtensionUninitializedStringPtr r, const wchar_t* c, GDExtensionInt n) {
    std::u32string t;
    for (GDExtensionInt i = 0; c && i < n && c[i]; ++i) t.push_back(static_cast<char32_t>(c[i]));
    novaString(r, std::move(t));
}

const std::u32string& textoDe(GDExtensionConstStringPtr p) {
    static const std::u32string vazio;
    const GString* s = static_cast<const GString*>(p);
    return s->s ? *s->s : vazio;
}

// Copia para o buffer (sem terminador) e devolve o tamanho total.
template <typename C, typename Conv>
GDExtensionInt copiaPara(const std::u32string& t, C* buf, GDExtensionInt max, Conv conv) {
    std::basic_string<C> out = conv(t);
    if (buf) {
        const GDExtensionInt n = std::min<GDExtensionInt>(max, static_cast<GDExtensionInt>(out.size()));
        std::memcpy(buf, out.data(), static_cast<size_t>(n) * sizeof(C));
    }
    return static_cast<GDExtensionInt>(out.size());
}

GDExtensionInt fn_string_to_latin1_chars(GDExtensionConstStringPtr s, char* buf, GDExtensionInt max) {
    return copiaPara(textoDe(s), buf, max, [](const std::u32string& t) {
        std::string r;
        for (char32_t c : t) r.push_back(c < 256 ? static_cast<char>(c) : '?');
        return r;
    });
}
GDExtensionInt fn_string_to_utf8_chars(GDExtensionConstStringPtr s, char* buf, GDExtensionInt max) {
    return copiaPara(textoDe(s), buf, max, [](const std::u32string& t) { return u32ParaUtf8(t); });
}
GDExtensionInt fn_string_to_utf16_chars(GDExtensionConstStringPtr s, char16_t* buf, GDExtensionInt max) {
    return copiaPara(textoDe(s), buf, max, [](const std::u32string& t) {
        std::u16string r;
        for (char32_t c : t) {
            if (c >= 0x10000) {
                c -= 0x10000;
                r.push_back(static_cast<char16_t>(0xD800 + (c >> 10)));
                r.push_back(static_cast<char16_t>(0xDC00 + (c & 0x3FF)));
            } else {
                r.push_back(static_cast<char16_t>(c));
            }
        }
        return r;
    });
}
GDExtensionInt fn_string_to_utf32_chars(GDExtensionConstStringPtr s, char32_t* buf, GDExtensionInt max) {
    return copiaPara(textoDe(s), buf, max, [](const std::u32string& t) { return t; });
}
GDExtensionInt fn_string_to_wide_chars(GDExtensionConstStringPtr s, wchar_t* buf, GDExtensionInt max) {
    return copiaPara(textoDe(s), buf, max, [](const std::u32string& t) {
        std::wstring r;
        for (char32_t c : t) r.push_back(static_cast<wchar_t>(c));
        return r;
    });
}

char32_t* fn_string_operator_index(GDExtensionStringPtr s, GDExtensionInt i) {
    GString* g = S(s);
    if (!g->s || i < 0 || static_cast<size_t>(i) > g->s->size()) return nullptr;
    return g->s->data() + i;
}
const char32_t* fn_string_operator_index_const(GDExtensionConstStringPtr s, GDExtensionInt i) {
    const GString* g = static_cast<const GString*>(s);
    if (!g->s || i < 0 || static_cast<size_t>(i) > g->s->size()) return nullptr;
    return g->s->data() + i;
}

std::u32string& mutavel(GDExtensionStringPtr s) {
    GString* g = S(s);
    if (!g->s) g->s = new std::u32string();
    return *g->s;
}

void fn_string_operator_plus_eq_string(GDExtensionStringPtr s, GDExtensionConstStringPtr b) {
    const std::u32string copia = textoDe(b); // b pode ser o proprio s
    mutavel(s) += copia;
}
void fn_string_operator_plus_eq_char(GDExtensionStringPtr s, char32_t c) {
    mutavel(s).push_back(c);
}
void fn_string_operator_plus_eq_cstr(GDExtensionStringPtr s, const char* c) {
    for (const char* p = c; p && *p; ++p) mutavel(s).push_back(static_cast<unsigned char>(*p));
}
void fn_string_operator_plus_eq_wcstr(GDExtensionStringPtr s, const wchar_t* c) {
    for (const wchar_t* p = c; p && *p; ++p) mutavel(s).push_back(static_cast<char32_t>(*p));
}
void fn_string_operator_plus_eq_c32str(GDExtensionStringPtr s, const char32_t* c) {
    for (const char32_t* p = c; p && *p; ++p) mutavel(s).push_back(*p);
}
// O tamanho pedido inclui o terminador nulo (convencao do motor).
GDExtensionInt fn_string_resize(GDExtensionStringPtr s, GDExtensionInt tam) {
    std::u32string& t = mutavel(s);
    t.resize(tam > 0 ? static_cast<size_t>(tam - 1) : 0);
    return 0;
}

void fn_string_name_new_with_latin1_chars(GDExtensionUninitializedStringNamePtr r, const char* c, GDExtensionBool) {
    std::u32string t;
    for (const char* p = c; p && *p; ++p) t.push_back(static_cast<unsigned char>(*p));
    static_cast<GStringName*>(r)->n = internar(t);
}
void fn_string_name_new_with_utf8_chars(GDExtensionUninitializedStringNamePtr r, const char* c) {
    static_cast<GStringName*>(r)->n = internar(c ? utf8ParaU32(c, std::strlen(c)) : std::u32string());
}
void fn_string_name_new_with_utf8_chars_and_len(GDExtensionUninitializedStringNamePtr r, const char* c, GDExtensionInt n) {
    size_t len = 0;
    while (c && static_cast<GDExtensionInt>(len) < n && c[len]) ++len;
    static_cast<GStringName*>(r)->n = internar(c ? utf8ParaU32(c, len) : std::u32string());
}

// ---- Packed / Array / Dictionary por indice --------------------------------------------

template <int T>
uint8_t* indicePacked(GDExtensionTypePtr self, GDExtensionInt i) {
    DadoPacked* d = garantirPacked(T, static_cast<GPacked*>(self));
    if (i < 0 || static_cast<size_t>(i) >= d->tamanho()) return nullptr;
    return d->bytes.data() + static_cast<size_t>(i) * d->tamElemento;
}

template <int T, typename R>
R* idx(GDExtensionTypePtr self, GDExtensionInt i) {
    return reinterpret_cast<R*>(indicePacked<T>(self, i));
}
template <int T, typename R>
const R* idxc(GDExtensionConstTypePtr self, GDExtensionInt i) {
    return reinterpret_cast<const R*>(indicePacked<T>(const_cast<GDExtensionTypePtr>(self), i));
}

GDExtensionVariantPtr fn_array_operator_index(GDExtensionTypePtr self, GDExtensionInt i) {
    DadoArray* d = static_cast<GArray*>(self)->d;
    if (!d || i < 0 || static_cast<size_t>(i) >= d->itens.size()) return nullptr;
    return &d->itens[static_cast<size_t>(i)];
}
GDExtensionVariantPtr fn_array_operator_index_const(GDExtensionConstTypePtr self, GDExtensionInt i) {
    return fn_array_operator_index(const_cast<GDExtensionTypePtr>(self), i);
}
void fn_array_set_typed(GDExtensionTypePtr, GDExtensionVariantType, GDExtensionConstStringNamePtr, GDExtensionConstVariantPtr) {
    // Arrays tipados se comportam como comuns aqui (sem verificacao de tipo).
}

GDExtensionVariantPtr fn_dictionary_operator_index(GDExtensionTypePtr self, GDExtensionConstVariantPtr chave) {
    DadoDicionario* d = static_cast<GDictionary*>(self)->d;
    if (!d) return nullptr;
    for (size_t i = 0; i < d->chaves.size(); ++i)
        if (variantIgual(&d->chaves[i], CV(chave))) return &d->valores[i];
    d->chaves.emplace_back();
    variantCopia(&d->chaves.back(), CV(chave));
    d->valores.emplace_back();
    variantNil(&d->valores.back());
    return &d->valores.back();
}
GDExtensionVariantPtr fn_dictionary_operator_index_const(GDExtensionConstTypePtr self, GDExtensionConstVariantPtr chave) {
    DadoDicionario* d = static_cast<const GDictionary*>(self)->d;
    if (!d) return nullptr;
    for (size_t i = 0; i < d->chaves.size(); ++i)
        if (variantIgual(&d->chaves[i], CV(chave))) return &d->valores[i];
    return nullptr;
}
void fn_dictionary_set_typed(GDExtensionTypePtr, GDExtensionVariantType, GDExtensionConstStringNamePtr, GDExtensionConstVariantPtr,
                             GDExtensionVariantType, GDExtensionConstStringNamePtr, GDExtensionConstVariantPtr) {}

template <typename F>
GDExtensionInterfaceFunctionPtr P(F f) {
    return reinterpret_cast<GDExtensionInterfaceFunctionPtr>(f);
}

} // namespace

void registrarFuncoesVariant(std::unordered_map<std::string, GDExtensionInterfaceFunctionPtr>& t) {
    t["variant_new_copy"] = P(&fn_variant_new_copy);
    t["variant_new_nil"] = P(&fn_variant_new_nil);
    t["variant_destroy"] = P(&fn_variant_destroy);
    t["variant_call"] = P(&fn_variant_call);
    t["variant_call_static"] = P(&fn_variant_call_static);
    t["variant_evaluate"] = P(&fn_variant_evaluate);
    t["variant_set"] = P(&fn_variant_set);
    t["variant_set_named"] = P(&fn_variant_set_named);
    t["variant_set_keyed"] = P(&fn_variant_set_keyed);
    t["variant_set_indexed"] = P(&fn_variant_set_indexed);
    t["variant_get"] = P(&fn_variant_get);
    t["variant_get_named"] = P(&fn_variant_get_named);
    t["variant_get_keyed"] = P(&fn_variant_get_keyed);
    t["variant_get_indexed"] = P(&fn_variant_get_indexed);
    t["variant_iter_init"] = P(&fn_variant_iter_init);
    t["variant_iter_next"] = P(&fn_variant_iter_next);
    t["variant_iter_get"] = P(&fn_variant_iter_get);
    t["variant_hash"] = P(&fn_variant_hash);
    t["variant_recursive_hash"] = P(&fn_variant_recursive_hash);
    t["variant_hash_compare"] = P(&fn_variant_hash_compare);
    t["variant_booleanize"] = P(&fn_variant_booleanize);
    t["variant_duplicate"] = P(&fn_variant_duplicate);
    t["variant_stringify"] = P(&fn_variant_stringify);
    t["variant_get_type"] = P(&fn_variant_get_type);
    t["variant_has_method"] = P(&fn_variant_has_method);
    t["variant_has_member"] = P(&fn_variant_has_member);
    t["variant_has_key"] = P(&fn_variant_has_key);
    t["variant_get_object_instance_id"] = P(&fn_variant_get_object_instance_id);
    t["variant_get_type_name"] = P(&fn_variant_get_type_name);
    t["variant_can_convert"] = P(&fn_variant_can_convert);
    t["variant_can_convert_strict"] = P(&fn_variant_can_convert_strict);
    t["get_variant_from_type_constructor"] = P(&fn_get_variant_from_type_constructor);
    t["get_variant_to_type_constructor"] = P(&fn_get_variant_to_type_constructor);
    t["variant_get_ptr_internal_getter"] = P(&fn_variant_get_ptr_internal_getter);
    t["variant_get_ptr_operator_evaluator"] = P(&fn_variant_get_ptr_operator_evaluator);
    t["variant_get_ptr_builtin_method"] = P(&fn_variant_get_ptr_builtin_method);
    t["variant_get_ptr_constructor"] = P(&fn_variant_get_ptr_constructor);
    t["variant_get_ptr_destructor"] = P(&fn_variant_get_ptr_destructor);
    t["variant_construct"] = P(&fn_variant_construct);
    t["variant_get_ptr_setter"] = P(&fn_variant_get_ptr_setter);
    t["variant_get_ptr_getter"] = P(&fn_variant_get_ptr_getter);
    t["variant_get_ptr_indexed_setter"] = P(&fn_variant_get_ptr_indexed_setter);
    t["variant_get_ptr_indexed_getter"] = P(&fn_variant_get_ptr_indexed_getter);
    t["variant_get_ptr_keyed_setter"] = P(&fn_variant_get_ptr_keyed_setter);
    t["variant_get_ptr_keyed_getter"] = P(&fn_variant_get_ptr_keyed_getter);
    t["variant_get_ptr_keyed_checker"] = P(&fn_variant_get_ptr_keyed_checker);
    t["variant_get_constant_value"] = P(&fn_variant_get_constant_value);
    t["variant_get_ptr_utility_function"] = P(&fn_variant_get_ptr_utility_function);

    t["string_new_with_latin1_chars"] = P(&fn_string_new_with_latin1_chars);
    t["string_new_with_utf8_chars"] = P(&fn_string_new_with_utf8_chars);
    t["string_new_with_utf16_chars"] = P(&fn_string_new_with_utf16_chars);
    t["string_new_with_utf32_chars"] = P(&fn_string_new_with_utf32_chars);
    t["string_new_with_wide_chars"] = P(&fn_string_new_with_wide_chars);
    t["string_new_with_latin1_chars_and_len"] = P(&fn_string_new_with_latin1_chars_and_len);
    t["string_new_with_utf8_chars_and_len"] = P(&fn_string_new_with_utf8_chars_and_len);
    t["string_new_with_utf8_chars_and_len2"] = P(&fn_string_new_with_utf8_chars_and_len2);
    t["string_new_with_utf16_chars_and_len"] = P(&fn_string_new_with_utf16_chars_and_len);
    t["string_new_with_utf16_chars_and_len2"] = P(&fn_string_new_with_utf16_chars_and_len2);
    t["string_new_with_utf32_chars_and_len"] = P(&fn_string_new_with_utf32_chars_and_len);
    t["string_new_with_wide_chars_and_len"] = P(&fn_string_new_with_wide_chars_and_len);
    t["string_to_latin1_chars"] = P(&fn_string_to_latin1_chars);
    t["string_to_utf8_chars"] = P(&fn_string_to_utf8_chars);
    t["string_to_utf16_chars"] = P(&fn_string_to_utf16_chars);
    t["string_to_utf32_chars"] = P(&fn_string_to_utf32_chars);
    t["string_to_wide_chars"] = P(&fn_string_to_wide_chars);
    t["string_operator_index"] = P(&fn_string_operator_index);
    t["string_operator_index_const"] = P(&fn_string_operator_index_const);
    t["string_operator_plus_eq_string"] = P(&fn_string_operator_plus_eq_string);
    t["string_operator_plus_eq_char"] = P(&fn_string_operator_plus_eq_char);
    t["string_operator_plus_eq_cstr"] = P(&fn_string_operator_plus_eq_cstr);
    t["string_operator_plus_eq_wcstr"] = P(&fn_string_operator_plus_eq_wcstr);
    t["string_operator_plus_eq_c32str"] = P(&fn_string_operator_plus_eq_c32str);
    t["string_resize"] = P(&fn_string_resize);
    t["string_name_new_with_latin1_chars"] = P(&fn_string_name_new_with_latin1_chars);
    t["string_name_new_with_utf8_chars"] = P(&fn_string_name_new_with_utf8_chars);
    t["string_name_new_with_utf8_chars_and_len"] = P(&fn_string_name_new_with_utf8_chars_and_len);

    constexpr int B = GDEXTENSION_VARIANT_TYPE_PACKED_BYTE_ARRAY;
    t["packed_byte_array_operator_index"] = P(&idx<B, uint8_t>);
    t["packed_byte_array_operator_index_const"] = P(&idxc<B, uint8_t>);
    constexpr int I32 = GDEXTENSION_VARIANT_TYPE_PACKED_INT32_ARRAY;
    t["packed_int32_array_operator_index"] = P(&idx<I32, int32_t>);
    t["packed_int32_array_operator_index_const"] = P(&idxc<I32, int32_t>);
    constexpr int I64 = GDEXTENSION_VARIANT_TYPE_PACKED_INT64_ARRAY;
    t["packed_int64_array_operator_index"] = P(&idx<I64, int64_t>);
    t["packed_int64_array_operator_index_const"] = P(&idxc<I64, int64_t>);
    constexpr int F32 = GDEXTENSION_VARIANT_TYPE_PACKED_FLOAT32_ARRAY;
    t["packed_float32_array_operator_index"] = P(&idx<F32, float>);
    t["packed_float32_array_operator_index_const"] = P(&idxc<F32, float>);
    constexpr int F64 = GDEXTENSION_VARIANT_TYPE_PACKED_FLOAT64_ARRAY;
    t["packed_float64_array_operator_index"] = P(&idx<F64, double>);
    t["packed_float64_array_operator_index_const"] = P(&idxc<F64, double>);
    constexpr int STR = GDEXTENSION_VARIANT_TYPE_PACKED_STRING_ARRAY;
    t["packed_string_array_operator_index"] = P(&idx<STR, GString>);
    t["packed_string_array_operator_index_const"] = P(&idxc<STR, GString>);
    constexpr int V2 = GDEXTENSION_VARIANT_TYPE_PACKED_VECTOR2_ARRAY;
    t["packed_vector2_array_operator_index"] = P(&idx<V2, float>);
    t["packed_vector2_array_operator_index_const"] = P(&idxc<V2, float>);
    constexpr int V3 = GDEXTENSION_VARIANT_TYPE_PACKED_VECTOR3_ARRAY;
    t["packed_vector3_array_operator_index"] = P(&idx<V3, float>);
    t["packed_vector3_array_operator_index_const"] = P(&idxc<V3, float>);
    constexpr int C = GDEXTENSION_VARIANT_TYPE_PACKED_COLOR_ARRAY;
    t["packed_color_array_operator_index"] = P(&idx<C, float>);
    t["packed_color_array_operator_index_const"] = P(&idxc<C, float>);
    constexpr int V4 = GDEXTENSION_VARIANT_TYPE_PACKED_VECTOR4_ARRAY;
    t["packed_vector4_array_operator_index"] = P(&idx<V4, float>);
    t["packed_vector4_array_operator_index_const"] = P(&idxc<V4, float>);
    t["array_operator_index"] = P(&fn_array_operator_index);
    t["array_operator_index_const"] = P(&fn_array_operator_index_const);
    t["array_set_typed"] = P(&fn_array_set_typed);
    t["dictionary_operator_index"] = P(&fn_dictionary_operator_index);
    t["dictionary_operator_index_const"] = P(&fn_dictionary_operator_index_const);
    t["dictionary_set_typed"] = P(&fn_dictionary_set_typed);
}

} // namespace eruption::compat::gd
