#include "utils/CSSParser.hpp"
#include "utils/AssetVfs.hpp"
#include <fstream>
#include <sstream>
#include <cctype>
#include <algorithm>

namespace eruption {

std::string CSSParser::trim(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) ++start;
    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
    return s.substr(start, end - start);
}

std::vector<std::string> CSSParser::split(const std::string& s, char delim) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == delim) {
            if (!cur.empty()) {
                out.push_back(trim(cur));
                cur.clear();
            }
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(trim(cur));
    return out;
}

bool CSSParser::load(const std::string& path) {
    std::string text; // disco ou pacote .ecp do projeto
    if (!vfs::readText(path, text)) return false;
    parse(text);
    reindex();
    return true;
}

bool CSSParser::loadFromString(const std::string& content) {
    parse(content);
    reindex();
    return true;
}

void CSSParser::parse(const std::string& content) {
    m_rules.clear();

    std::string s = content;

    // Remove comentarios /* ... */
    size_t pos = 0;
    while ((pos = s.find("/*", pos)) != std::string::npos) {
        size_t end = s.find("*/", pos + 2);
        if (end == std::string::npos) {
            s.erase(pos);
            break;
        }
        s.erase(pos, end - pos + 2);
    }

    pos = 0;
    while (pos < s.size()) {
        size_t blockStart = s.find('{', pos);
        if (blockStart == std::string::npos) break;

        std::string selectorList = trim(s.substr(pos, blockStart - pos));
        size_t blockEnd = s.find('}', blockStart + 1);
        if (blockEnd == std::string::npos) break;

        std::string body = s.substr(blockStart + 1, blockEnd - blockStart - 1);
        CSSRule rule;

        // Declaracoes: o `;` dentro de parenteses ou aspas (url(...), var(--a, x))
        // nao separa nada.
        std::vector<std::string> decls;
        {
            std::string cur;
            int paren = 0;
            char quote = 0;
            for (char c : body) {
                if (quote) {
                    if (c == quote) quote = 0;
                } else if (c == '"' || c == '\'') {
                    quote = c;
                } else if (c == '(') {
                    ++paren;
                } else if (c == ')' && paren > 0) {
                    --paren;
                } else if (c == ';' && paren == 0) {
                    decls.push_back(trim(cur));
                    cur.clear();
                    continue;
                }
                cur += c;
            }
            decls.push_back(trim(cur));
        }
        for (const std::string& decl : decls) {
            size_t colon = decl.find(':');
            if (colon == std::string::npos) continue;
            std::string prop = trim(decl.substr(0, colon));
            std::string value = trim(decl.substr(colon + 1));
            if (!prop.empty()) {
                rule.properties[prop] = value;
            }
        }

        // `.a, .b { ... }`: uma regra por seletor da lista.
        if (!rule.properties.empty()) {
            for (const std::string& selector : split(selectorList, ',')) {
                if (selector.empty()) continue;
                CSSRule one = rule;
                one.selector = selector;
                m_rules.push_back(std::move(one));
            }
        }

        pos = blockEnd + 1;
    }

    expandClasses();
}

// Seletor de CLASSE. Sem ele, uma interface com lista escreve a mesma regra
// uma vez por indice: numa interface de quatro fichas, `#hp-1` e `#hp-2` sao
// identicos de ponta a ponta menos o `--parent`, e sao dez linhas cada. Quatro
// personagens, cinco listas, e o arquivo chega a 760 linhas em que quase tudo
// e' copia. Mudar a cor da barra de vida vira quatro ediçoes que tem de
// combinar, e o dia em que uma nao combina ninguem percebe.
//
// Com classe:
//     .barra-hp { --widget: bar; width: 60%; --fill-color: #d84040; ... }
//     #hp-1 { --class: barra-hp; --parent: #ficha-1; }
//
// COMO: as propriedades da classe entram como uma regra do PROPRIO id,
// inserida logo ANTES da primeira regra escrita a mao daquele id. Como
// `mergedRule` mescla na ordem do vetor e o que vem depois vence, o id
// continua mandando na classe — que e' a cascata que o autor espera de CSS.
// E inserir no lugar, em vez de no topo do arquivo, mantem a ordem de
// aparicao: ela e' o criterio de desempate do z-index no UiDocument, entao
// jogar as regras para o topo reordenaria o desenho de quem empata em z. E como a expansao acontece aqui, no
// fim da leitura, tudo que consulta o parser depois (CSSLayout, UiDocument,
// o Caldera) herda classe sem saber que ela existe.
//
// Uma classe que peca outra classe nao e' resolvida: uma volta so'. CSS de
// verdade tambem nao encadeia classe, quem encadeia e' o HTML.
void CSSParser::expandClasses() {
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> classes;
    for (const auto& rule : m_rules) {
        if (rule.selector.size() < 2 || rule.selector[0] != '.' || rule.selector.find_first_of(".:# ", 1) != std::string::npos) continue;
        auto& dst = classes[rule.selector.substr(1)];
        for (const auto& kv : rule.properties) dst[kv.first] = kv.second;
    }
    if (classes.empty()) return;

    // Qual classe cada id pede. Entre regras do mesmo id, a ultima vence.
    std::vector<std::string> ordem;
    std::unordered_map<std::string, std::string> pedido;
    for (const auto& rule : m_rules) {
        if (rule.selector.empty() || rule.selector[0] != '#' || rule.selector.find_first_of(".:# ", 1) != std::string::npos) continue;
        const auto it = rule.properties.find("--class");
        if (it == rule.properties.end()) continue;
        if (pedido.find(rule.selector) == pedido.end()) ordem.push_back(rule.selector);
        pedido[rule.selector] = it->second;
    }
    if (ordem.empty()) return;

    std::vector<CSSRule> base;
    base.reserve(ordem.size());
    for (const std::string& selector : ordem) {
        CSSRule rule;
        rule.selector = selector;
        rule.specificity = 100; // pesa como classe: o id e os estados da classe a vencem
        std::string lista = pedido[selector];
        // Aceita `--class: barra-hp` e `--class: "barra-hp"`, porque o resto
        // do arquivo usa aspas em `--image` e o autor vai misturar.
        lista.erase(std::remove(lista.begin(), lista.end(), '"'), lista.end());
        for (const std::string& nome : split(lista, ' ')) {
            const auto it = classes.find(nome);
            if (it == classes.end()) continue;
            for (const auto& kv : it->second) rule.properties[kv.first] = kv.second;
        }
        rule.properties.erase("--class");
        if (!rule.properties.empty()) base.push_back(std::move(rule));
    }

    std::unordered_map<std::string, size_t> pendente;
    for (size_t i = 0; i < base.size(); ++i) pendente[base[i].selector] = i;
    std::vector<CSSRule> saida;
    saida.reserve(m_rules.size() + base.size());
    for (CSSRule& rule : m_rules) {
        const auto it = pendente.find(rule.selector);
        if (it != pendente.end()) {
            saida.push_back(std::move(base[it->second]));
            pendente.erase(it);
        }
        saida.push_back(std::move(rule));
    }
    m_rules = std::move(saida);
}

int CSSSelector::specificity() const {
    return (id.empty() ? 0 : 10000) + int(classes.size() + (hover ? 1 : 0)) * 100 + (tag.empty() ? 0 : 1);
}

CSSSelector CSSParser::parseSelector(const std::string& selector) {
    CSSSelector out;
    auto ident = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_'; };
    size_t i = 0;
    const size_t n = selector.size();
    auto readIdent = [&](std::string& dst) {
        const size_t b = i;
        while (i < n && ident(selector[i])) ++i;
        dst = selector.substr(b, i - b);
        return i > b;
    };
    if (n == 0) return out;
    if (ident(selector[0])) {
        if (!readIdent(out.tag)) return out;
    }
    while (i < n) {
        const char c = selector[i++];
        std::string name;
        if (c == '#') {
            if (!readIdent(out.id)) return out;
        } else if (c == '.') {
            if (!readIdent(name)) return out;
            out.classes.push_back(name);
        } else if (c == ':') {
            if (!readIdent(name) || name != "hover") return out;
            out.hover = true;
        } else {
            return out;
        }
    }
    out.valid = true;
    return out;
}

// Troca de texto, com o cuidado de achar o `)` certo: a reserva pode ter
// parenteses (`var(--a, calc(1px + 2px))`). `depth` corta ciclo (a: var(--b),
// b: var(--a)) e cadeias absurdas.
bool CSSParser::resolveVars(const std::string& value, const CSSVarLookup& lookup, std::string& out, int depth) {
    if (depth > 8) return false;
    if (value.find("var(") == std::string::npos) {
        out = value;
        return true;
    }
    std::string res;
    size_t i = 0;
    while (i < value.size()) {
        const size_t at = value.find("var(", i);
        if (at == std::string::npos) {
            res.append(value, i, std::string::npos);
            break;
        }
        // "var(" no meio de um nome (ex.: "xvar(") nao e' funcao.
        if (at > 0 && (std::isalnum(static_cast<unsigned char>(value[at - 1])) || value[at - 1] == '-' || value[at - 1] == '_')) {
            res.append(value, i, at + 4 - i);
            i = at + 4;
            continue;
        }
        res.append(value, i, at - i);
        size_t j = at + 4;
        int paren = 1;
        size_t comma = std::string::npos;
        while (j < value.size() && paren > 0) {
            if (value[j] == '(') ++paren;
            else if (value[j] == ')') --paren;
            else if (value[j] == ',' && paren == 1 && comma == std::string::npos) comma = j;
            if (paren > 0) ++j;
        }
        if (paren != 0) return false; // sem fechar
        const size_t nameEnd = comma == std::string::npos ? j : comma;
        const std::string name = trim(value.substr(at + 4, nameEnd - at - 4));
        const std::string* found = name.size() > 2 && name[0] == '-' && name[1] == '-' ? lookup(name) : nullptr;
        std::string sub;
        bool ok = false;
        if (found) {
            ok = resolveVars(*found, lookup, sub, depth + 1);
        }
        if (!ok && comma != std::string::npos) {
            ok = resolveVars(trim(value.substr(comma + 1, j - comma - 1)), lookup, sub, depth + 1);
        }
        if (!ok) return false;
        res += sub;
        i = j + 1;
    }
    out = std::move(res);
    return true;
}

std::unordered_map<std::string, std::string> CSSParser::rootVars() const {
    std::unordered_map<std::string, std::string> vars;
    for (const CSSRule& r : m_rules) {
        if (r.selector != ":root") continue;
        for (const auto& kv : r.properties) {
            if (kv.first.compare(0, 2, "--") == 0) vars[kv.first] = kv.second;
        }
    }
    return vars;
}

void CSSParser::resolveRootVars() {
    const auto root = rootVars();
    for (CSSRule& r : m_rules) {
        const auto own = r.properties;
        const CSSVarLookup lookup = [&](const std::string& n) -> const std::string* {
            auto it = own.find(n);
            if (it != own.end()) return &it->second;
            it = root.find(n);
            return it != root.end() ? &it->second : nullptr;
        };
        for (auto it = r.properties.begin(); it != r.properties.end();) {
            std::string out;
            if (it->second.find("var(") == std::string::npos) {
                ++it;
            } else if (resolveVars(it->second, lookup, out)) {
                it->second = std::move(out);
                ++it;
            } else {
                it = r.properties.erase(it);
            }
        }
    }
    reindex();
}

const CSSRule* CSSParser::findRule(const std::string& selector) const {
    const CSSRule* last = nullptr;
    for (const auto& rule : m_rules) {
        if (rule.selector == selector) last = &rule;
    }
    return last;
}

std::vector<const CSSRule*> CSSParser::findRules(const std::string& selector) const {
    std::vector<const CSSRule*> out;
    for (const auto& rule : m_rules) {
        if (rule.selector == selector) out.push_back(&rule);
    }
    return out;
}

CSSRule CSSParser::mergedRule(const std::string& selector) const {
    if (const CSSRule* r = mergedRulePtr(selector)) return *r;
    CSSRule merged;
    merged.selector = selector;
    return merged;
}

const CSSRule* CSSParser::mergedRulePtr(const std::string& selector) const {
    auto it = m_merged.find(selector);
    return it == m_merged.end() ? nullptr : &it->second;
}

void CSSParser::reindex() {
    m_merged.clear();
    for (const auto& rule : m_rules) {
        if (rule.properties.empty()) continue;
        CSSRule& merged = m_merged[rule.selector];
        merged.selector = rule.selector;
        for (const auto& kv : rule.properties) merged.properties[kv.first] = kv.second;
    }
}

} // namespace eruption
