#pragma once
#include <string>
#include <vector>
#include <unordered_map>

namespace eruption {

struct CSSRule {
    std::string selector;
    std::unordered_map<std::string, std::string> properties;
};

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

    const std::vector<CSSRule>& rules() const { return m_rules; }

    static std::string trim(const std::string& s);
    static std::vector<std::string> split(const std::string& s, char delim);

private:
    void parse(const std::string& content);
    std::vector<CSSRule> m_rules;
};

} // namespace eruption
