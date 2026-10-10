#pragma once

// ABI de plugins nativos do Unity, em declaracoes PROPRIAS.
//
// Fonte: manual publico do Unity ("Low-level native plug-in interface" e
// "Native plug-in API for graphics and rendering") e a referencia de
// script publica (enum Rendering.GraphicsDeviceType). Nenhum cabecalho do
// Unity e' usado: o que esta' aqui sao fatos de interoperabilidade - ordem
// das funcoes em cada tabela, valores numericos e identificadores (GUID) -
// necessarios para um plugin compilado conversar com outro hospedeiro.
// Os GUIDs pedidos por um plugin real aparecem no log (modo sonda), entao
// qualquer divergencia fica visivel no primeiro teste.
//
// Convencao de chamada: C padrao da plataforma (no Linux, System V; no
// Windows o Unity usa __stdcall, irrelevante em x86-64).

#include <cstdint>

namespace eruption::compat::unity {

// Identificador de interface: 128 bits, passado por valor (dois registradores).
struct Guid {
    uint64_t alto;
    uint64_t baixo;
};

inline bool operator==(const Guid& a, const Guid& b) {
    return a.alto == b.alto && a.baixo == b.baixo;
}

// Tabela entregue a UnityPluginLoad(IUnityInterfaces*).
//
// As duas primeiras recebem o GUID "inteiro". Observado por sonda caixa-preta
// num plugin C++ real: o GUID chega por PONTEIRO (o tipo nao e' trivial em
// C++, entao vai por referencia invisivel). Num plugin em C ele chegaria por
// valor, em dois registradores. As funcoes do host aceitam os dois jeitos:
// recebem palavras cruas e decidem pelo conteudo (ver UnityHost.cpp).
struct TabelaInterfaces {
    void* (*obter)(uint64_t a, uint64_t b);
    void (*registrar)(uint64_t a, uint64_t b, uint64_t c);
    void* (*obterSeparado)(uint64_t alto, uint64_t baixo);
    void (*registrarSeparado)(uint64_t alto, uint64_t baixo, void* interfacePtr);
};

// Tipo de dispositivo grafico (mesmos valores de Rendering.GraphicsDeviceType).
enum class Renderizador : int32_t {
    Direct3D11 = 2,
    Nulo = 4,
    OpenGLES20 = 8,
    OpenGLES30 = 11,
    Metal = 16,
    OpenGLCore = 17,
    Direct3D12 = 18,
    Vulkan = 21,
};

// Eventos do dispositivo grafico entregues aos callbacks registrados.
enum class EventoDispositivo : int32_t {
    Inicializar = 0,
    Desligar = 1,
    AntesDeReiniciar = 2,
    DepoisDeReiniciar = 3,
};

using CallbackDispositivo = void (*)(int32_t evento);
using EventoRender = void (*)(int32_t idEvento);
using EventoRenderComDado = void (*)(int32_t idEvento, void* dado);

// IUnityGraphics: acesso generico ao dispositivo grafico.
struct TabelaGraficos {
    int32_t (*renderizador)();
    void (*registrarCallbackDispositivo)(CallbackDispositivo cb);
    void (*removerCallbackDispositivo)(CallbackDispositivo cb);
    int32_t (*reservarFaixaEventos)(int32_t quantidade);
};

// IUnityLog: mensagens do plugin para o console do hospedeiro.
enum class TipoLog : int32_t { Erro = 0, Aviso = 2, Log = 3, Excecao = 4 };
struct TabelaLog {
    void (*log)(int32_t tipo, const char* mensagem, const char* arquivo, int32_t linha);
};

// GUIDs conhecidos.
constexpr Guid kGuidGraficos{0x7CBA0A9CA4DDB544ULL, 0x8C5AD4926EB17B11ULL};
constexpr Guid kGuidLog{0x9E7507FA5B444D5DULL, 0x92FB979515EA83FCULL};
constexpr Guid kGuidGraficosVulkan{0x95355348D4EF4E11ULL, 0x9789313DFCFFCC87ULL};

} // namespace eruption::compat::unity
