#pragma once

// Nucleo do host GDExtension: representacao propria dos tipos que a ABI C
// do Godot trata como opacos (String, StringName, Variant, Array, objetos).
//
// A ABI so' fixa o TAMANHO de cada tipo (builtin_class_sizes do
// extension_api.json, configuracao float_64: real_t float, ponteiro de 64
// bits). O conteudo e' nosso: a extensao nunca le os bytes, so' chama as
// funcoes da tabela de interface. Por isso:
//   String / NodePath  (8)  -> ponteiro para std::u32string (nulo = vazia)
//   StringName         (8)  -> ponteiro para entrada internada (nunca liberada)
//   Array / Dictionary (8)  -> ponteiro para dado com contagem de referencia
//   Packed*Array       (16) -> ponteiro para vetor de bytes + 8 bytes livres
//   Variant            (24) -> tipo (4) + folga (4) + 16 bytes de dado;
//                              tipos maiores que 16 bytes vao para o heap.

#include "third_party/gdextension_interface.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace eruption::compat::gd {

constexpr int kTiposVariant = GDEXTENSION_VARIANT_TYPE_VARIANT_MAX;

// ---- StringName -------------------------------------------------------------

struct NomeInterno {
    std::u32string texto;
    std::string utf8;
    uint32_t hash = 0;
};

// Interna (thread-safe) e devolve a entrada unica do texto.
const NomeInterno* internar(const std::u32string& texto);
const NomeInterno* internarUtf8(const std::string& utf8);

// O godot-cpp declara esses tipos como uint8_t[N] (alinhamento 1): podem
// cair em qualquer deslocamento dentro das classes da extensao. "packed"
// faz o compilador acessar o ponteiro interno sem supor alinhamento.
struct __attribute__((packed)) GStringName {
    const NomeInterno* n;
};
static_assert(sizeof(GStringName) == 8, "StringName tem 8 bytes na ABI");

// ---- String -----------------------------------------------------------------

struct __attribute__((packed)) GString {
    std::u32string* s;
};
static_assert(sizeof(GString) == 8, "String tem 8 bytes na ABI");

std::u32string utf8ParaU32(const char* s, size_t n);
std::string u32ParaUtf8(const std::u32string& s);

// ---- Variant ----------------------------------------------------------------

struct GVariant {
    uint32_t tipo;
    uint32_t folga;
    alignas(8) uint8_t dado[16];
};
static_assert(sizeof(GVariant) == 24, "Variant tem 24 bytes na ABI (float_64)");

// Tamanho do tipo em ponteiro cru (ptrcall), segundo a ABI float_64.
size_t tamanhoTipo(int tipo);
const char* nomeTipo(int tipo);

// Ponteiro para o valor dentro do Variant (no proprio dado ou no heap).
void* valorDe(GVariant* v);
const void* valorDe(const GVariant* v);

// Operacoes basicas por tipo, sobre ponteiros crus.
void construirPadrao(int tipo, void* destino);
void copiarValor(int tipo, void* destino, const void* origem); // destino nao inicializado
void destruirValor(int tipo, void* valor);

void variantNil(GVariant* v);
void variantDe(GVariant* v, int tipo, const void* valor); // v nao inicializado
void variantCopia(GVariant* destino, const GVariant* origem);
void variantDestruir(GVariant* v);
bool variantBool(const GVariant* v);
std::u32string variantTexto(const GVariant* v);
bool variantIgual(const GVariant* a, const GVariant* b);

// Conveniencias para o host montar argumentos.
void variantInt(GVariant* v, int64_t i);
void variantFloat(GVariant* v, double f);
void variantStr(GVariant* v, const std::string& utf8);

// ---- Array / Dictionary / Packed --------------------------------------------

struct DadoArray {
    std::atomic<int> ref{1};
    std::vector<GVariant> itens;
    ~DadoArray();
};
struct __attribute__((packed)) GArray {
    DadoArray* d;
};
static_assert(sizeof(GArray) == 8, "Array tem 8 bytes na ABI");

struct DadoDicionario {
    std::atomic<int> ref{1};
    std::vector<GVariant> chaves;
    std::vector<GVariant> valores;
    ~DadoDicionario();
};
struct __attribute__((packed)) GDictionary {
    DadoDicionario* d;
};
static_assert(sizeof(GDictionary) == 8, "Dictionary tem 8 bytes na ABI");

struct DadoPacked {
    int tipoElemento; // tipo Variant do elemento (STRING para PackedStringArray)
    size_t tamElemento;
    std::vector<uint8_t> bytes;
    size_t tamanho() const { return tamElemento ? bytes.size() / tamElemento : 0; }
    ~DadoPacked();
};
struct __attribute__((packed)) GPacked {
    DadoPacked* d;
    void* livre;
};
static_assert(sizeof(GPacked) == 16, "Packed*Array tem 16 bytes na ABI");

bool ehPacked(int tipo);
DadoPacked* garantirPacked(int tipo, GPacked* p);
void redimensionarPacked(DadoPacked* d, size_t n);

// ---- Objetos ----------------------------------------------------------------

struct Biblioteca;
struct Classe;

struct MetodoExtensao {
    const NomeInterno* nome = nullptr;
    void* userdata = nullptr;
    GDExtensionClassMethodCall chamar = nullptr;
    GDExtensionClassMethodPtrCall ptrcall = nullptr;
    uint32_t flags = 0;
    bool temRetorno = false;
    int tipoRetorno = 0;
    std::vector<int> tiposArgs;
    std::vector<std::string> nomesArgs;
};

struct PropriedadeExtensao {
    std::string nome;
    int tipo = 0;
    const NomeInterno* setter = nullptr;
    const NomeInterno* getter = nullptr;
};

struct Classe {
    const NomeInterno* nome = nullptr;
    Classe* pai = nullptr;
    bool refCounted = false;
    bool extensao = false;
    Biblioteca* biblioteca = nullptr;
    GDExtensionClassCreationInfo4 info{};
    std::unordered_map<const NomeInterno*, MetodoExtensao> metodos;
    std::vector<PropriedadeExtensao> propriedades;
    std::vector<std::string> sinais;
    std::vector<std::pair<std::string, int64_t>> constantes;

    bool herdaDe(const Classe* outra) const;
    // Primeira classe nativa (do "motor") da cadeia.
    const Classe* nativa() const;
};

// Classe pelo nome (motor ou extensao); nulo se desconhecida.
Classe* classePorNome(const NomeInterno* nome);
Classe* classePorNome(const std::string& nome);
Classe* registrarClasseExtensao(Biblioteca* lib, const NomeInterno* nome, const NomeInterno* pai,
                                const GDExtensionClassCreationInfo4& info);
void removerClasseExtensao(const NomeInterno* nome);
std::vector<Classe*> classesExtensao();

struct LigacaoInstancia {
    void* token;
    void* binding;
    const GDExtensionInstanceBindingCallbacks* callbacks;
};

struct Objeto {
    uint64_t id = 0;
    Classe* classe = nullptr;          // classe efetiva (pode ser de extensao)
    void* instancia = nullptr;         // GDExtensionClassInstancePtr
    std::vector<LigacaoInstancia> ligacoes;

    // RefCounted
    std::atomic<int> ref{1};
    bool refInicial = true;

    // Node
    std::string nome;
    Objeto* pai = nullptr;
    std::vector<Objeto*> filhos;
    bool naArvore = false;
    bool processar = false;
    bool processarFisica = false;
    bool prontoAvisado = false;
    bool liberarNoFim = false;
    uint32_t noHost = 0; // NoId no host de compat

    // Node2D / Node3D (transformacao local)
    float pos[3] = {0, 0, 0};
    float rot[3] = {0, 0, 0}; // Node2D usa rot[2] (radianos)
    float escala[3] = {1, 1, 1};

    // Cache dos virtuais ja' consultados (nulo = nao sobrescrito).
    std::unordered_map<const NomeInterno*, GDExtensionClassCallVirtual> virtuais;
};

Objeto* criarObjetoNativo(Classe* classe);
// Instancia uma classe (de extensao ou nativa) como o motor faria.
Objeto* instanciar(Classe* classe);
void destruirObjeto(Objeto* o);
Objeto* objetoPorId(uint64_t id);
bool objetoVivo(const Objeto* o);

// RefCounted: semantica do motor (init_ref/reference/unreference).
bool refIniciar(Objeto* o);
bool refIncrementar(Objeto* o);
bool refDecrementar(Objeto* o); // true quando chegou a zero

void notificar(Objeto* o, int32_t que, bool reverso = false);
GDExtensionClassCallVirtual virtualDe(Objeto* o, const char* nome);

// Metodo de extensao pelo nome, subindo na hierarquia.
const MetodoExtensao* metodoExtensao(const Classe* c, const NomeInterno* nome);
// Chamada por nome com Variants (metodo de extensao ou do motor).
bool chamarMetodo(Objeto* o, const NomeInterno* nome, const GVariant* const* args, int argc, GVariant* ret,
                  GDExtensionCallError* erro);

// ---- Arvore de nos ----------------------------------------------------------

bool ehNo(const Objeto* o);
void adicionarFilho(Objeto* pai, Objeto* filho);
void removerFilho(Objeto* pai, Objeto* filho);
void entrarArvore(Objeto* o);
void sairArvore(Objeto* o);
// Raiz da arvore de cena do host GDExtension (um Node "root").
Objeto* raiz();
void processarArvore(double dt);
void sincronizarComHost(Objeto* o);

// ---- Metodos do motor (classdb_get_method_bind) -----------------------------

using PtrcallMotor = void (*)(Objeto* self, const void* const* args, void* ret);
struct MetodoMotor {
    std::string classe;
    std::string nome;
    PtrcallMotor fn = nullptr;     // nulo = conhecido mas nao implementado
    int tipoRetorno = 0;           // NIL = sem retorno
    std::vector<int> tiposArgs;
    bool avisado = false;
};
MetodoMotor* metodoMotor(const std::string& classe, const std::string& nome);
MetodoMotor* metodoMotorPorCadeia(const Classe* c, const std::string& nome);
MetodoMotor* metodoMotorParaBind(const std::string& classe, const std::string& nome);
void chamarMotorVariant(MetodoMotor* m, Objeto* o, const GVariant* const* args, int argc, GVariant* ret,
                        GDExtensionCallError* erro);

// Versao da API gerada (anunciada em get_godot_version2).
void versaoApi(uint32_t& major, uint32_t& minor, uint32_t& patch);

// Singletons (Engine, OS...): objeto nativo criado sob demanda.
Objeto* singleton(const NomeInterno* nome);

// ---- Tabela de interface ----------------------------------------------------

GDExtensionInterfaceFunctionPtr obterFuncao(const char* nome);
void registrarFuncoesVariant(std::unordered_map<std::string, GDExtensionInterfaceFunctionPtr>& t);
void registrarFuncoesObjeto(std::unordered_map<std::string, GDExtensionInterfaceFunctionPtr>& t);

// Construtores, operadores, metodos e utilitarios por nome (Variant).
GDExtensionPtrConstructor construtorPtr(int tipo, int indice);
GDExtensionPtrOperatorEvaluator operadorPtr(int op, int tipoA, int tipoB);
GDExtensionPtrBuiltInMethod metodoBuiltin(int tipo, const NomeInterno* nome);
GDExtensionPtrUtilityFunction funcaoUtilitaria(const NomeInterno* nome);

// Biblioteca carregada (uma por .so).
struct Biblioteca {
    std::string caminho;
    std::string simbolo;
    void* handle = nullptr;
    GDExtensionInitialization init{};
    int nivelAtual = -1;
    GDExtensionMainLoopCallbacks laco{};
};

// ---- Trampolins ---------------------------------------------------------------
// A ABI devolve ponteiros de funcao C sem contexto (construtor N do tipo T,
// operador X entre A e B...). Cada trampolim e' uma funcao distinta que le
// o proprio Slot; assim uma implementacao generica atende muitas entradas,
// e o que falta vira um stub que diz NOME do que faltou quando chamado.
struct Slot;
using FnSlot = uintptr_t (*)(const Slot& s, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d);
struct Slot {
    FnSlot fn = nullptr;
    int a = 0, b = 0, c = 0;
    const void* dado = nullptr;
    std::string msg;
};
using Generica = uintptr_t (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);
// Mesmo `chave` devolve sempre o mesmo trampolim.
Generica trampolim(const std::string& chave, const Slot& s);
// Stub que avisa (uma vez) "msg" e devolve zero.
Generica stubFaltante(const std::string& chave, const std::string& msg);

// Passa por void(*)(), o tipo "coringa" de ponteiro de funcao.
template <typename F>
F comoFn(Generica g) {
    return reinterpret_cast<F>(reinterpret_cast<void (*)()>(g));
}

// Aviso unico por chave (para nao inundar o log com o mesmo faltante).
void avisarUmaVez(const std::string& chave, const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

} // namespace eruption::compat::gd
