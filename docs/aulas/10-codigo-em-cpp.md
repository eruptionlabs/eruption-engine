# Aula 10 — Código em C++ com recarga a quente

O Luau dá conta da lógica do jogo. Quando um pedaço precisa de velocidade (busca de
caminho para muitos personagens, simulação, conta pesada por frame), ele pode ir
para C++. O editor compila e troca o código **com o jogo rodando**: salvou o arquivo,
a versão nova entra em um ou dois segundos, sem parar o Play.

Funciona no editor em Linux, com um compilador C++ instalado (`c++`, `g++` ou
`clang++`; outro com a variável `ERUPTION_CXX`).

## O módulo

Os fontes ficam na pasta `native` do projeto (crie a pasta). Todo `.cpp` dela entra
no mesmo módulo. O único cabeçalho é `include/eruption/native_module.h`, do motor.

```cpp
#include <eruption/native_module.h>
#include <cmath>

namespace {

// Tudo que precisa durar entre frames mora aqui: o motor guarda esta memória
// e ela continua igual quando o código é recarregado.
struct Estado {
    double angulo;
    int voltas;
};

double dobro(EruptionHost*, const double* a, int n) { return n > 0 ? a[0] * 2.0 : 0.0; }
double voltas(EruptionHost* h, const double*, int) { return static_cast<Estado*>(h->state)->voltas; }

void carregar(EruptionHost* h, int recarregado) {
    h->log(recarregado ? "módulo recarregado" : "módulo carregado");
    // Funções para os scripts: native.dobro(21), native.voltas()
    h->expose("dobro", dobro);
    h->expose("voltas", voltas);
}

void atualizar(EruptionHost* h, float dt) {
    auto* e = static_cast<Estado*>(h->state);
    e->angulo += dt;
    if (e->angulo > 6.2831853) {
        e->angulo -= 6.2831853;
        e->voltas++;
    }
    // Gira um modelo em volta de um ponto.
    if (int m = h->findModel("totem")) {
        const float p[3] = {200.0f + 30.0f * float(std::cos(e->angulo)), 4.0f,
                            200.0f + 30.0f * float(std::sin(e->angulo))};
        h->setModelPosition(m, p);
    }
}

const EruptionModule kModulo = {
    ERUPTION_NATIVE_API_VERSION,
    sizeof(Estado),
    carregar,   // load
    nullptr,    // unload
    nullptr,    // start (Play)
    nullptr,    // stop
    atualizar,  // update (todo frame com o jogo rodando)
};

} // namespace

extern "C" ERUPTION_MODULE_EXPORT const EruptionModule* eruption_module(void) { return &kModulo; }
```

Num script Luau:

```lua
on.every(1, function()
    log("voltas: " .. native.voltas() .. ", dobro de 21: " .. native.dobro(21))
end)
```

## O que vale saber

- **Erro de compilação**: a mensagem do compilador aparece no console
  (`arquivo:linha: erro`) e a versão anterior continua rodando até o próximo save.
- **Estado**: só a memória de `host->state` sobrevive à recarga. Variáveis globais do
  módulo voltam ao valor inicial. Se mudar o `struct` de estado, o `load` recebe
  `recarregado = 1` com os bytes antigos: acrescente campos no fim, ou reinicie o Play.
- **Funções para os scripts**: recebem e devolvem números. Registre todas em `load`
  (depois de uma recarga os nomes são registrados de novo).
- **O que o motor oferece** está em `EruptionHost`: console, modelos do mapa (achar,
  posição), posição do jogador, altura do chão e o tempo de jogo.
- **Exportar**: o jogo exportado leva a última versão compilada (o arquivo
  `.eruption/native/game.so`) e não precisa de compilador. Exportar para Android e
  iOS ainda não leva o módulo.
- Um erro grave no C++ (ponteiro inválido) derruba o editor, como em qualquer
  programa nativo; salve a cena antes de testar código novo.
