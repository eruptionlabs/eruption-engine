#include "utils/CSSParser.hpp"
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
    std::ifstream file(path);
    if (!file.is_open()) return false;
    std::stringstream ss;
    ss << file.rdbuf();
    parse(ss.str());
    return true;
}

bool CSSParser::loadFromString(const std::string& content) {
    parse(content);
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

        std::string selector = trim(s.substr(pos, blockStart - pos));
        size_t blockEnd = s.find('}', blockStart + 1);
        if (blockEnd == std::string::npos) break;

        std::string body = s.substr(blockStart + 1, blockEnd - blockStart - 1);
        CSSRule rule;
        rule.selector = selector;

        std::vector<std::string> decls = split(body, ';');
        for (const std::string& decl : decls) {
            size_t colon = decl.find(':');
            if (colon == std::string::npos) continue;
            std::string prop = trim(decl.substr(0, colon));
            std::string value = trim(decl.substr(colon + 1));
            if (!prop.empty()) {
                rule.properties[prop] = value;
            }
        }

        if (!rule.selector.empty() && !rule.properties.empty()) {
            m_rules.push_back(std::move(rule));
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
        if (rule.selector.size() < 2 || rule.selector[0] != '.') continue;
        auto& dst = classes[rule.selector.substr(1)];
        for (const auto& kv : rule.properties) dst[kv.first] = kv.second;
    }
    if (classes.empty()) return;

    // Qual classe cada id pede. Entre regras do mesmo id, a ultima vence.
    std::vector<std::string> ordem;
    std::unordered_map<std::string, std::string> pedido;
    for (const auto& rule : m_rules) {
        if (rule.selector.empty() || rule.selector[0] != '#') continue;
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
    CSSRule merged;
    merged.selector = selector;
    for (const auto& rule : m_rules) {
        if (rule.selector == selector) {
            for (const auto& kv : rule.properties) {
                merged.properties[kv.first] = kv.second;
            }
        }
    }
    return merged;
}

} // namespace eruption
