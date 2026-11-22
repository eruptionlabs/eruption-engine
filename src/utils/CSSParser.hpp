#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>

namespace eruption {

struct CSSRule {
    std::string selector;
    std::unordered_map<std::string, std::string> properties;
    // Especificidade fixa (ver CSSSelector::specificity); -1 = calcular do
    // seletor. A regra que `--class` injeta no id usa a de uma classe, para
    // `.linha:hover` e `.linha.marcada` ganharem dela.
    int specificity = -1;
};

// Seletor composto de UM elemento: `#id`, `.a.b`, `tag.cls`, `#id.cls:hover`.
// Sem combinadores (espaco, `>`, `,` ja' separado na leitura) e so' o pseudo
// `:hover`; qualquer outra coisa deixa `valid` falso e a regra nunca casa.
struct CSSSelector {
    std::string id, tag;
    std::vector<std::string> classes;
    bool hover = false;
    bool valid = false;
    // Peso da cascata: id 10000, classe e pseudo 100 cada, tag 1.
    int specificity() const;
};

// Funcao que devolve o valor de uma propriedade personalizada (`--nome`) ou
// nullptr. Usada por `CSSParser::resolveVars`.
using CSSVarLookup = std::function<const std::string*(const std::string&)>;

class CSSParser {
public:
    bool load(const std::string& path);
    bool loadFromString(const std::string& content);

    const CSSRule* findRule(const std::string& selector) const;
    std::vector<const CSSRule*> findRules(const std::string& selector) const;

    // Retorna uma regra mesclada de todas as regras do seletor.
    // Propriedades definidas em regras posteriores sobrescrevem as anteriores
    // (comportamento CSS de cascata).
    CSSRule mergedRule(const std::string& selector) const;
    // A mesma regra mesclada, sem copia: ponteiro para o indice por seletor
    // (nullptr se o seletor nao tem propriedade). Vale ate' a proxima mudanca
    // das regras.
    const CSSRule* mergedRulePtr(const std::string& selector) const;

    const std::vector<CSSRule>& rules() const { return m_rules; }
    // Troca o conjunto de regras (o UiDocument monta o seu a cada mudanca de
    // estado, sem reler o arquivo).
    void setRules(std::vector<CSSRule> rules) { m_rules = std::move(rules); reindex(); }

    static CSSSelector parseSelector(const std::string& selector);

    // Substitui `var(--x)` e `var(--x, reserva)` em `value` (dentro de calc()
    // tambem: e' troca de texto). A reserva pode ter parenteses e outro var().
    // Falso se uma variavel nao existe e nao ha' reserva, ou se ha' ciclo: a
    // declaracao deve ser descartada, como em CSS.
    static bool resolveVars(const std::string& value, const CSSVarLookup& lookup, std::string& out, int depth = 0);
    // Propriedades personalizadas de `:root`, em ordem de leitura.
    std::unordered_map<std::string, std::string> rootVars() const;
    // Resolve var() em todas as regras so' com `:root` e as variaveis da
    // propria regra (quem nao tem arvore de elementos, como a HUD).
    void resolveRootVars();

    static std::string trim(const std::string& s);
    static std::vector<std::string> split(const std::string& s, char delim);

private:
    void parse(const std::string& content);

    // Expande `--class` depois da leitura: ver o comentario da definicao.
    void expandClasses();
    // Refaz m_merged; chamado por toda funcao que muda m_rules.
    void reindex();
    std::vector<CSSRule> m_rules;
    // Regra mesclada por seletor. A interface consulta o estilo de cada
    // elemento varias vezes por quadro (caixa, pais, cor, fonte); varrer a
    // lista inteira a cada consulta custava ~24 ms por quadro numa HUD de
    // ~1500 elementos e derrubava o jogo de 60 para ~25 fps.
    std::unordered_map<std::string, CSSRule> m_merged;
};

} // namespace eruption
