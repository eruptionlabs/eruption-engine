// Seletor de classe do CSSParser: `--class: nome` puxa as propriedades de
// `.nome` para dentro do id, com o id mandando no empate.
//
// O que se verifica, e por que cada um importa:
//   1. a classe chega ao id                 — e' a funcao
//   2. o id vence a classe                  — e' a cascata que o autor espera
//   3. varias classes, a ultima vence       — ordem de leitura, como em CSS
//   4. classe inexistente nao apaga nada    — erro de digitacao nao mata a tela
//   5. a ORDEM DO ARQUIVO nao muda          — ela desempata o z-index do
//                                             UiDocument; se mudar, o desenho
//                                             reordena sozinho
//   6. classe que pede classe nao encadeia  — limite assumido, nao acidente
//
//   g++ -std=c++17 -I src tests/test_css_classes.cpp src/utils/CSSParser.cpp
#include "utils/CSSParser.hpp"

#include <cstdio>
#include <string>

using namespace eruption;

static int g_falhas = 0;

static void checa(bool ok, const char* titulo) {
    std::printf("  [%s] %s\n", ok ? "ok" : "FALHA", titulo);
    if (!ok) ++g_falhas;
}

static std::string prop(const CSSParser& css, const char* selector, const char* nome) {
    const CSSRule r = css.mergedRule(selector);
    const auto it = r.properties.find(nome);
    return it != r.properties.end() ? it->second : std::string("<ausente>");
}

int main() {
    std::printf("=== seletor de classe ===\n");

    CSSParser css;
    css.loadFromString(R"(
        .barra { --widget: bar; width: 60%; --fill-color: #d84040; }
        .magra { width: 20%; }
        .encadeia { --class: barra; color: #fff; }

        #hp-1 { --class: barra; --parent: #ficha-1; }
        #hp-2 { --class: barra; --parent: #ficha-2; width: 45%; }
        #mp-1 { --class: barra magra; --parent: #ficha-1; }
        #sem  { --class: "nao-existe"; width: 10%; }
        #puro { width: 99%; }
        #neto { --class: encadeia; }
    )");

    checa(prop(css, "#hp-1", "--widget") == "bar", "a classe chega ao id");
    checa(prop(css, "#hp-1", "--fill-color") == "#d84040", "a classe traz tudo que tem");
    checa(prop(css, "#hp-1", "width") == "60%", "o que so' a classe define vale");
    checa(prop(css, "#hp-1", "--parent") == "#ficha-1", "o que so' o id define vale");

    checa(prop(css, "#hp-2", "width") == "45%", "o id vence a classe");
    checa(prop(css, "#hp-2", "--widget") == "bar", "... sem derrubar o resto dela");

    checa(prop(css, "#mp-1", "width") == "20%", "duas classes: a ultima vence");
    checa(prop(css, "#mp-1", "--widget") == "bar", "... e a primeira continua valendo");

    checa(prop(css, "#sem", "width") == "10%", "classe inexistente nao apaga o id");
    checa(prop(css, "#puro", "width") == "99%", "id sem classe passa intacto");

    // Encadear classe nao e' suportado: `.encadeia` pede `.barra`, e o que
    // chega a #neto e' so' o que `.encadeia` escreve com as proprias maos.
    checa(prop(css, "#neto", "color") == "#fff", "classe sem --class propria funciona");
    checa(prop(css, "#neto", "--widget") == "<ausente>", "classe nao encadeia classe");
    // `--class` CONTINUA no resultado de proposito: e' a declaracao do autor,
    // e e' por ela que uma ferramenta (o Caldera) sabe a que classe o elemento
    // pertence para oferecer editar a classe em vez do elemento. O que nao
    // pode vazar e' o `--class` DA CLASSE: `.encadeia` pede `.barra`, e se
    // esse pedido chegasse a #neto o elemento diria pertencer a uma classe que
    // o autor nunca escreveu nele.
    checa(prop(css, "#neto", "--class") == "encadeia", "--class fica, e e' o do autor");
    checa(prop(css, "#hp-1", "--class") == "barra", "--class do id sobrevive a expansao");

    // A ordem de aparicao dos ids no vetor de regras e' o criterio de
    // desempate do z-index la' no UiDocument. A expansao nao pode mexer nela.
    const char* esperado[] = {"#hp-1", "#hp-2", "#mp-1", "#sem", "#puro", "#neto"};
    size_t i = 0;
    bool ordem_ok = true;
    std::string ultimo;
    for (const CSSRule& r : css.rules()) {
        if (r.selector.empty() || r.selector[0] != '#') continue;
        if (r.selector == ultimo) continue;             // classe + regra do mesmo id
        ultimo = r.selector;
        if (i >= 6 || r.selector != esperado[i]) { ordem_ok = false; break; }
        ++i;
    }
    checa(ordem_ok && i == 6, "a ordem do arquivo nao muda");

    std::printf("%s\n", g_falhas == 0 ? "TODOS OS TESTES PASSARAM" : "HOUVE FALHA");
    return g_falhas == 0 ? 0 : 1;
}
