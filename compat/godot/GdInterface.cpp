// Tabela de interface GDExtension (get_proc_address): objetos, ClassDB,
// memoria, mensagens e o resto que nao e' Variant.

#include "godot/GdNucleo.hpp"
#include "host/Host.hpp"

#include <cstdlib>
#include <cstring>
#include <mutex>

namespace eruption::compat::gd {

namespace {

Objeto* O(GDExtensionConstObjectPtr p) {
    return static_cast<Objeto*>(const_cast<void*>(p));
}
const NomeInterno* N(GDExtensionConstStringNamePtr p) {
    return static_cast<const GStringName*>(p)->n;
}
Biblioteca* L(GDExtensionClassLibraryPtr p) {
    return static_cast<Biblioteca*>(p);
}

// ---- Memoria e mensagens --------------------------------------------------------------

// O motor guarda um cabecalho de 16 bytes antes de cada bloco (tamanho e
// contagem de elementos). Extensoes de debug contam com ele: o godot-cpp
// escreve a contagem de memnew_arr ali em vez de reservar o proprio espaco.
constexpr size_t kCabecalho = 16;

void* fn_mem_alloc(size_t n) {
    uint8_t* p = static_cast<uint8_t*>(std::malloc(n + kCabecalho));
    if (!p) return nullptr;
    std::memset(p, 0, kCabecalho);
    *reinterpret_cast<uint64_t*>(p) = n;
    return p + kCabecalho;
}
void* fn_mem_realloc(void* p, size_t n) {
    if (!p) return fn_mem_alloc(n);
    uint8_t* base = static_cast<uint8_t*>(p) - kCabecalho;
    uint8_t* novo = static_cast<uint8_t*>(std::realloc(base, n + kCabecalho));
    if (!novo) return nullptr;
    *reinterpret_cast<uint64_t*>(novo) = n;
    return novo + kCabecalho;
}
void fn_mem_free(void* p) {
    if (p) std::free(static_cast<uint8_t*>(p) - kCabecalho);
}

void mensagem(Nivel n, const char* desc, const char* msg, const char* funcao, const char* arquivo, int32_t linha) {
    if (msg && *msg) logf(n, "godot", "%s: %s (%s em %s:%d)", desc, msg, funcao, arquivo, linha);
    else logf(n, "godot", "%s (%s em %s:%d)", desc, funcao, arquivo, linha);
}
void fn_print_error(const char* d, const char* f, const char* a, int32_t l, GDExtensionBool) {
    mensagem(Nivel::Erro, d, nullptr, f, a, l);
}
void fn_print_error_with_message(const char* d, const char* m, const char* f, const char* a, int32_t l, GDExtensionBool) {
    mensagem(Nivel::Erro, d, m, f, a, l);
}
void fn_print_warning(const char* d, const char* f, const char* a, int32_t l, GDExtensionBool) {
    mensagem(Nivel::Aviso, d, nullptr, f, a, l);
}
void fn_print_warning_with_message(const char* d, const char* m, const char* f, const char* a, int32_t l, GDExtensionBool) {
    mensagem(Nivel::Aviso, d, m, f, a, l);
}

// Versao anunciada: a da API gerada (o godot-cpp recusa motor mais velho
// que ele). "eruption-compat" no build para ninguem confundir com o Godot.
void fn_get_godot_version2(GDExtensionGodotVersion2* v) {
    versaoApi(v->major, v->minor, v->patch);
    v->hex = (v->major << 16) | (v->minor << 8) | v->patch;
    v->status = "stable";
    v->build = "eruption-compat";
    v->hash = "";
    v->timestamp = 0;
    v->string = "Eruption Engine (camada de compatibilidade GDExtension)";
}
void fn_get_godot_version(GDExtensionGodotVersion* v) {
    versaoApi(v->major, v->minor, v->patch);
    v->string = "Eruption Engine (camada de compatibilidade GDExtension)";
}

size_t fn_get_native_struct_size(GDExtensionConstStringNamePtr nome) {
    avisarUmaVez("struct:" + N(nome)->utf8, "NAO IMPLEMENTADO: tamanho da estrutura nativa %s", N(nome)->utf8.c_str());
    return 0;
}

void fn_get_library_path(GDExtensionClassLibraryPtr lib, GDExtensionUninitializedStringPtr r) {
    const std::string& c = L(lib)->caminho;
    static_cast<GString*>(r)->s = new std::u32string(utf8ParaU32(c.data(), c.size()));
}

void fn_register_main_loop_callbacks(GDExtensionClassLibraryPtr lib, const GDExtensionMainLoopCallbacks* cb) {
    L(lib)->laco = *cb;
}

// ---- Objetos ------------------------------------------------------------------------------

void fn_object_method_bind_ptrcall(GDExtensionMethodBindPtr mb, GDExtensionObjectPtr obj, const GDExtensionConstTypePtr* args,
                                   GDExtensionTypePtr ret) {
    MetodoMotor* m = static_cast<MetodoMotor*>(const_cast<void*>(mb));
    if (!m->fn) {
        avisarUmaVez("mb:" + m->classe + "." + m->nome, "NAO IMPLEMENTADO: metodo nativo %s.%s", m->classe.c_str(), m->nome.c_str());
        return;
    }
    m->fn(O(obj), args, ret);
}

void fn_object_method_bind_call(GDExtensionMethodBindPtr mb, GDExtensionObjectPtr obj, const GDExtensionConstVariantPtr* args,
                                GDExtensionInt argc, GDExtensionUninitializedVariantPtr ret, GDExtensionCallError* erro) {
    GVariant* r = static_cast<GVariant*>(ret);
    variantNil(r);
    GVariant tmp;
    variantNil(&tmp);
    chamarMotorVariant(static_cast<MetodoMotor*>(const_cast<void*>(mb)), O(obj), reinterpret_cast<const GVariant* const*>(args),
                       static_cast<int>(argc), &tmp, erro);
    *r = tmp;
}

void fn_object_destroy(GDExtensionObjectPtr obj) {
    destruirObjeto(O(obj));
}

GDExtensionObjectPtr fn_global_get_singleton(GDExtensionConstStringNamePtr nome) {
    Objeto* o = singleton(N(nome));
    if (!o) avisarUmaVez("singleton:" + N(nome)->utf8, "singleton %s indisponivel", N(nome)->utf8.c_str());
    return o;
}

void* fn_object_get_instance_binding(GDExtensionObjectPtr obj, void* token, const GDExtensionInstanceBindingCallbacks* cb) {
    Objeto* o = O(obj);
    if (!o) return nullptr;
    for (auto& l : o->ligacoes)
        if (l.token == token) return l.binding;
    if (!cb || !cb->create_callback) return nullptr;
    void* b = cb->create_callback(token, o);
    o->ligacoes.push_back({token, b, cb});
    return b;
}

void fn_object_set_instance_binding(GDExtensionObjectPtr obj, void* token, void* binding, const GDExtensionInstanceBindingCallbacks* cb) {
    Objeto* o = O(obj);
    for (auto& l : o->ligacoes) {
        if (l.token == token) {
            l.binding = binding;
            l.callbacks = cb;
            return;
        }
    }
    o->ligacoes.push_back({token, binding, cb});
}

void fn_object_free_instance_binding(GDExtensionObjectPtr obj, void* token) {
    Objeto* o = O(obj);
    for (size_t i = 0; i < o->ligacoes.size(); ++i) {
        if (o->ligacoes[i].token == token) {
            o->ligacoes.erase(o->ligacoes.begin() + static_cast<long>(i));
            return;
        }
    }
}

void fn_object_set_instance(GDExtensionObjectPtr obj, GDExtensionConstStringNamePtr classe, GDExtensionClassInstancePtr inst) {
    Objeto* o = O(obj);
    o->instancia = inst;
    if (Classe* c = classePorNome(N(classe))) o->classe = c;
}

GDExtensionBool fn_object_get_class_name(GDExtensionConstObjectPtr obj, GDExtensionClassLibraryPtr lib, GDExtensionUninitializedStringNamePtr r) {
    Objeto* o = O(obj);
    if (!o) return 0;
    // Classe de extensao so' aparece para a biblioteca que a registrou.
    const Classe* c = o->classe;
    while (c && c->extensao && c->biblioteca != L(lib)) c = c->pai;
    static_cast<GStringName*>(r)->n = c ? c->nome : internar(U"Object");
    return 1;
}

GDExtensionObjectPtr fn_object_cast_to(GDExtensionConstObjectPtr obj, void* tag) {
    Objeto* o = O(obj);
    if (!o || !tag) return nullptr;
    return o->classe->herdaDe(static_cast<Classe*>(tag)) ? o : nullptr;
}

GDExtensionObjectPtr fn_object_get_instance_from_id(GDObjectInstanceID id) {
    return objetoPorId(id);
}

GDObjectInstanceID fn_object_get_instance_id(GDExtensionConstObjectPtr obj) {
    return obj ? O(obj)->id : 0;
}

GDExtensionBool fn_object_has_script_method(GDExtensionConstObjectPtr, GDExtensionConstStringNamePtr) {
    return 0;
}

void fn_object_call_script_method(GDExtensionObjectPtr, GDExtensionConstStringNamePtr, const GDExtensionConstVariantPtr*, GDExtensionInt,
                                  GDExtensionUninitializedVariantPtr r, GDExtensionCallError* erro) {
    variantNil(static_cast<GVariant*>(r));
    erro->error = GDEXTENSION_CALL_ERROR_INVALID_METHOD;
}

GDExtensionObjectPtr fn_ref_get_object(GDExtensionConstRefPtr ref) {
    return ref ? *static_cast<Objeto* const*>(ref) : nullptr;
}

void fn_ref_set_object(GDExtensionRefPtr ref, GDExtensionObjectPtr obj) {
    Objeto** r = static_cast<Objeto**>(ref);
    Objeto* novo = O(obj);
    if (*r == novo) return;
    if (novo && novo->classe->refCounted) refIncrementar(novo);
    Objeto* antigo = *r;
    *r = novo;
    if (antigo && objetoVivo(antigo) && antigo->classe->refCounted && refDecrementar(antigo)) destruirObjeto(antigo);
}

// ---- ClassDB -----------------------------------------------------------------------------------

GDExtensionObjectPtr fn_classdb_construct_object2(GDExtensionConstStringNamePtr classe) {
    Classe* c = classePorNome(N(classe));
    if (!c) {
        logf(Nivel::Erro, "godot", "classdb_construct_object: classe %s desconhecida", N(classe)->utf8.c_str());
        return nullptr;
    }
    if (c->extensao) return instanciar(c);
    return criarObjetoNativo(c);
}

GDExtensionMethodBindPtr fn_classdb_get_method_bind(GDExtensionConstStringNamePtr classe, GDExtensionConstStringNamePtr metodo,
                                                    GDExtensionInt) {
    return metodoMotorParaBind(N(classe)->utf8, N(metodo)->utf8);
}

void* fn_classdb_get_class_tag(GDExtensionConstStringNamePtr classe) {
    return classePorNome(N(classe));
}

void fn_classdb_register_extension_class(GDExtensionClassLibraryPtr lib, GDExtensionConstStringNamePtr classe, GDExtensionConstStringNamePtr pai,
                                         const GDExtensionClassCreationInfo4* info) {
    if (registrarClasseExtensao(L(lib), N(classe), N(pai), *info))
        logf(Nivel::Info, "godot", "classe registrada: %s (herda de %s)", N(classe)->utf8.c_str(), N(pai)->utf8.c_str());
}

Classe* classeExt(GDExtensionConstStringNamePtr classe) {
    Classe* c = classePorNome(N(classe));
    return (c && c->extensao) ? c : nullptr;
}

void fn_classdb_register_extension_class_method(GDExtensionClassLibraryPtr, GDExtensionConstStringNamePtr classe,
                                                const GDExtensionClassMethodInfo* info) {
    Classe* c = classeExt(classe);
    if (!c) return;
    MetodoExtensao m;
    m.nome = N(info->name);
    m.userdata = info->method_userdata;
    m.chamar = info->call_func;
    m.ptrcall = info->ptrcall_func;
    m.flags = info->method_flags;
    m.temRetorno = info->has_return_value;
    m.tipoRetorno = (info->has_return_value && info->return_value_info) ? info->return_value_info->type : 0;
    for (uint32_t i = 0; i < info->argument_count; ++i) {
        m.tiposArgs.push_back(info->arguments_info ? info->arguments_info[i].type : 0);
        m.nomesArgs.push_back(info->arguments_info && info->arguments_info[i].name ? N(info->arguments_info[i].name)->utf8 : "");
    }
    c->metodos[m.nome] = std::move(m);
}

void fn_classdb_register_extension_class_virtual_method(GDExtensionClassLibraryPtr, GDExtensionConstStringNamePtr,
                                                        const GDExtensionClassVirtualMethodInfo*) {
    // Virtuais declarados pela extensao (para scripts sobrescreverem): sem scripts aqui.
}

void fn_classdb_register_extension_class_integer_constant(GDExtensionClassLibraryPtr, GDExtensionConstStringNamePtr classe,
                                                          GDExtensionConstStringNamePtr, GDExtensionConstStringNamePtr nome,
                                                          GDExtensionInt valor, GDExtensionBool) {
    if (Classe* c = classeExt(classe)) c->constantes.emplace_back(N(nome)->utf8, valor);
}

void fn_classdb_register_extension_class_property(GDExtensionClassLibraryPtr, GDExtensionConstStringNamePtr classe,
                                                  const GDExtensionPropertyInfo* info, GDExtensionConstStringNamePtr setter,
                                                  GDExtensionConstStringNamePtr getter) {
    Classe* c = classeExt(classe);
    if (!c) return;
    PropriedadeExtensao p;
    p.nome = N(info->name)->utf8;
    p.tipo = info->type;
    p.setter = N(setter);
    p.getter = N(getter);
    c->propriedades.push_back(p);
}

void fn_classdb_register_extension_class_property_indexed(GDExtensionClassLibraryPtr lib, GDExtensionConstStringNamePtr classe,
                                                          const GDExtensionPropertyInfo* info, GDExtensionConstStringNamePtr setter,
                                                          GDExtensionConstStringNamePtr getter, GDExtensionInt) {
    fn_classdb_register_extension_class_property(lib, classe, info, setter, getter);
}

void fn_classdb_register_extension_class_property_group(GDExtensionClassLibraryPtr, GDExtensionConstStringNamePtr, GDExtensionConstStringPtr,
                                                        GDExtensionConstStringPtr) {}

void fn_classdb_register_extension_class_property_subgroup(GDExtensionClassLibraryPtr, GDExtensionConstStringNamePtr,
                                                           GDExtensionConstStringPtr, GDExtensionConstStringPtr) {}

void fn_classdb_register_extension_class_signal(GDExtensionClassLibraryPtr, GDExtensionConstStringNamePtr classe,
                                                GDExtensionConstStringNamePtr nome, const GDExtensionPropertyInfo*, GDExtensionInt) {
    if (Classe* c = classeExt(classe)) c->sinais.push_back(N(nome)->utf8);
}

void fn_classdb_unregister_extension_class(GDExtensionClassLibraryPtr, GDExtensionConstStringNamePtr classe) {
    removerClasseExtensao(N(classe));
}

// ---- Editor, scripts, threads e afins ------------------------------------------------------------

void fn_editor_add_plugin(GDExtensionConstStringNamePtr) {}
void fn_editor_remove_plugin(GDExtensionConstStringNamePtr) {}
void fn_editor_register_get_classes_used_callback(GDExtensionClassLibraryPtr, GDExtensionEditorGetClassesUsedCallback) {}
void fn_editor_help_load_xml_from_utf8_chars(const char*) {}
void fn_editor_help_load_xml_from_utf8_chars_and_len(const char*, GDExtensionInt) {}

// Pool de threads do motor: aqui roda na hora, na thread de quem pediu.
int64_t fn_worker_thread_pool_add_native_group_task(GDExtensionObjectPtr, void (*fn)(void*, uint32_t), void* ud, int elementos, int,
                                                    GDExtensionBool, GDExtensionConstStringPtr) {
    for (int i = 0; i < elementos; ++i) fn(ud, static_cast<uint32_t>(i));
    return 0;
}
int64_t fn_worker_thread_pool_add_native_task(GDExtensionObjectPtr, void (*fn)(void*), void* ud, GDExtensionBool, GDExtensionConstStringPtr) {
    fn(ud);
    return 0;
}

template <typename F>
GDExtensionInterfaceFunctionPtr P(F f) {
    return reinterpret_cast<GDExtensionInterfaceFunctionPtr>(f);
}

std::unordered_map<std::string, GDExtensionInterfaceFunctionPtr> montarTabela() {
    std::unordered_map<std::string, GDExtensionInterfaceFunctionPtr> t;
    registrarFuncoesVariant(t);
    registrarFuncoesObjeto(t);
    return t;
}

} // namespace

void registrarFuncoesObjeto(std::unordered_map<std::string, GDExtensionInterfaceFunctionPtr>& t) {
    t["get_godot_version"] = P(&fn_get_godot_version);
    t["get_godot_version2"] = P(&fn_get_godot_version2);
    t["mem_alloc"] = P(&fn_mem_alloc);
    t["mem_realloc"] = P(&fn_mem_realloc);
    t["mem_free"] = P(&fn_mem_free);
    t["print_error"] = P(&fn_print_error);
    t["print_error_with_message"] = P(&fn_print_error_with_message);
    t["print_warning"] = P(&fn_print_warning);
    t["print_warning_with_message"] = P(&fn_print_warning_with_message);
    t["print_script_error"] = P(&fn_print_error);
    t["print_script_error_with_message"] = P(&fn_print_error_with_message);
    t["get_native_struct_size"] = P(&fn_get_native_struct_size);
    t["get_library_path"] = P(&fn_get_library_path);
    t["register_main_loop_callbacks"] = P(&fn_register_main_loop_callbacks);

    t["object_method_bind_call"] = P(&fn_object_method_bind_call);
    t["object_method_bind_ptrcall"] = P(&fn_object_method_bind_ptrcall);
    t["object_destroy"] = P(&fn_object_destroy);
    t["global_get_singleton"] = P(&fn_global_get_singleton);
    t["object_get_instance_binding"] = P(&fn_object_get_instance_binding);
    t["object_set_instance_binding"] = P(&fn_object_set_instance_binding);
    t["object_free_instance_binding"] = P(&fn_object_free_instance_binding);
    t["object_set_instance"] = P(&fn_object_set_instance);
    t["object_get_class_name"] = P(&fn_object_get_class_name);
    t["object_cast_to"] = P(&fn_object_cast_to);
    t["object_get_instance_from_id"] = P(&fn_object_get_instance_from_id);
    t["object_get_instance_id"] = P(&fn_object_get_instance_id);
    t["object_has_script_method"] = P(&fn_object_has_script_method);
    t["object_call_script_method"] = P(&fn_object_call_script_method);
    t["ref_get_object"] = P(&fn_ref_get_object);
    t["ref_set_object"] = P(&fn_ref_set_object);

    t["classdb_construct_object"] = P(&fn_classdb_construct_object2);
    t["classdb_construct_object2"] = P(&fn_classdb_construct_object2);
    t["classdb_get_method_bind"] = P(&fn_classdb_get_method_bind);
    t["classdb_get_class_tag"] = P(&fn_classdb_get_class_tag);
    // Estruturas 4 e 5 tem o mesmo layout (o 5 e' typedef do 4 no 4.5).
    t["classdb_register_extension_class4"] = P(&fn_classdb_register_extension_class);
    t["classdb_register_extension_class5"] = P(&fn_classdb_register_extension_class);
    t["classdb_register_extension_class_method"] = P(&fn_classdb_register_extension_class_method);
    t["classdb_register_extension_class_virtual_method"] = P(&fn_classdb_register_extension_class_virtual_method);
    t["classdb_register_extension_class_integer_constant"] = P(&fn_classdb_register_extension_class_integer_constant);
    t["classdb_register_extension_class_property"] = P(&fn_classdb_register_extension_class_property);
    t["classdb_register_extension_class_property_indexed"] = P(&fn_classdb_register_extension_class_property_indexed);
    t["classdb_register_extension_class_property_group"] = P(&fn_classdb_register_extension_class_property_group);
    t["classdb_register_extension_class_property_subgroup"] = P(&fn_classdb_register_extension_class_property_subgroup);
    t["classdb_register_extension_class_signal"] = P(&fn_classdb_register_extension_class_signal);
    t["classdb_unregister_extension_class"] = P(&fn_classdb_unregister_extension_class);

    t["editor_add_plugin"] = P(&fn_editor_add_plugin);
    t["editor_remove_plugin"] = P(&fn_editor_remove_plugin);
    t["editor_register_get_classes_used_callback"] = P(&fn_editor_register_get_classes_used_callback);
    t["editor_help_load_xml_from_utf8_chars"] = P(&fn_editor_help_load_xml_from_utf8_chars);
    t["editor_help_load_xml_from_utf8_chars_and_len"] = P(&fn_editor_help_load_xml_from_utf8_chars_and_len);
    t["worker_thread_pool_add_native_group_task"] = P(&fn_worker_thread_pool_add_native_group_task);
    t["worker_thread_pool_add_native_task"] = P(&fn_worker_thread_pool_add_native_task);
}

GDExtensionInterfaceFunctionPtr obterFuncao(const char* nome) {
    static const std::unordered_map<std::string, GDExtensionInterfaceFunctionPtr> tabela = montarTabela();
    auto it = tabela.find(nome);
    if (it != tabela.end()) return it->second;
    // Funcao que o host nao tem: devolve um stub que avisa o nome se for
    // chamado (o godot-cpp pede a lista inteira no inicio, use ou nao).
    return reinterpret_cast<GDExtensionInterfaceFunctionPtr>(
        stubFaltante(std::string("iface:") + nome, std::string("funcao da interface ") + nome));
}

} // namespace eruption::compat::gd
