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
