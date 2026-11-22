// Variaveis CSS (`--x`, `var()`), seletores compostos e lista de seletores do
// CSSParser. O estado de elemento (:hover, classes do script) mora no
// UiDocument e depende do ImGui; aqui so' a parte pura que ele usa.
//
//   g++ -std=c++17 -I src tests/test_css_vars.cpp src/utils/CSSParser.cpp ...
#include "utils/CSSParser.hpp"

#include <cstdio>
#include <string>

using namespace eruption;

static int g_falhas = 0;

static void checa(bool ok, const char* titulo) {
    std::printf("  [%s] %s\n", ok ? "ok" : "FALHA", titulo);
    if (!ok) ++g_falhas;
}

static std::string resolve(const CSSParser& css, const std::string& v, const char* resultadoSeFalha = "<falha>") {
    const auto root = css.rootVars();
    const CSSVarLookup lk = [&](const std::string& k) -> const std::string* {
        auto it = root.find(k);
        return it == root.end() ? nullptr : &it->second;
    };
    std::string out;
    return CSSParser::resolveVars(v, lk, out) ? out : std::string(resultadoSeFalha);
}

int main() {
    std::printf("=== variaveis e seletores ===\n");
    CSSParser css;
    css.loadFromString(R"(
        :root { --accent: #e8c77a; --gap: 8px; --ref: var(--accent); --a: var(--b); --b: var(--a); }
        .row, .item { background: var(--accent); }
        #x { width: calc(100% - var(--gap, 2px)); content: "a;b"; }
        #y.sel:hover { color: #fff; }
    )");

    checa(resolve(css, "var(--accent)") == "#e8c77a", "var() simples");
    checa(resolve(css, "calc(100% - var(--gap))") == "calc(100% - 8px)", "var() dentro de calc()");
    checa(resolve(css, "var(--nope, #123456)") == "#123456", "reserva quando falta");
    checa(resolve(css, "var(--nope, var(--gap))") == "8px", "reserva com outro var()");
    checa(resolve(css, "var(--nope, calc(1px + 2px))") == "calc(1px + 2px)", "reserva com parenteses");
    checa(resolve(css, "var(--nope)") == "<falha>", "sem variavel nem reserva: descarta");
    checa(resolve(css, "var(--ref)") == "#e8c77a", "variavel que aponta para variavel");
    checa(resolve(css, "var(--a)") == "<falha>", "ciclo nao trava");
    checa(resolve(css, "1px 2px") == "1px 2px", "sem var() passa intacto");

    checa(css.findRule(".row") && css.findRule(".item"), "lista de seletores vira uma regra por seletor");
    checa(css.mergedRule("#x").properties["content"] == "\"a;b\"", ";" " dentro de aspas nao separa");

    const CSSSelector a = CSSParser::parseSelector("#y.sel:hover");
    checa(a.valid && a.id == "y" && a.classes.size() == 1 && a.classes[0] == "sel" && a.hover, "#id.classe:hover");
    const CSSSelector b = CSSParser::parseSelector("bar.a.b");
    checa(b.valid && b.tag == "bar" && b.classes.size() == 2, "tag.a.b");
    checa(!CSSParser::parseSelector(".a .b").valid, "combinador de descendente nao e' suportado");
    checa(!CSSParser::parseSelector(".a:focus").valid, "so' :hover");
    checa(!CSSParser::parseSelector(":root").valid, ":root nao e' de elemento");
    checa(CSSParser::parseSelector("#y.sel:hover").specificity() > CSSParser::parseSelector(".sel:hover").specificity(),
          "id pesa mais que classe");
    checa(CSSParser::parseSelector(".r:hover").specificity() > CSSParser::parseSelector(".r").specificity(),
          ":hover pesa como classe");

    CSSParser hud;
    hud.loadFromString(":root { --c: #112233; } #h { color: var(--c); border-color: var(--zz); }");
    hud.resolveRootVars();
    checa(hud.mergedRule("#h").properties["color"] == "#112233", "resolveRootVars troca o valor");
    checa(hud.mergedRule("#h").properties.count("border-color") == 0, "resolveRootVars descarta o invalido");

    std::printf("%s\n", g_falhas == 0 ? "TODOS OS TESTES PASSARAM" : "HOUVE FALHA");
    return g_falhas == 0 ? 0 : 1;
}
