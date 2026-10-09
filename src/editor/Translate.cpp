#include "editor/Translate.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <unordered_map>

namespace eruption {

namespace {

UiLanguage g_lang = UiLanguage::English;
std::unordered_map<std::string, std::string> g_table;   // inglês -> tradução
std::unordered_map<std::string, std::string> g_results; // texto pedido -> texto pronto

void loadTable(UiLanguage lang) {
    g_table.clear();
    g_results.clear();
    if (lang != UiLanguage::Portuguese) return;
    std::ifstream f("data/lang/pt-BR.json", std::ios::binary);
    const nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
    if (!j.is_object()) return;
    for (auto it = j.begin(); it != j.end(); ++it)
        if (it.value().is_string()) g_table[it.key()] = it.value().get<std::string>();
}

const char* translate(const char* english, bool window) {
    if (g_lang == UiLanguage::English || !english || !*english) return english;
    const std::string key = std::string(window ? "\x01" : "") + english;
    auto done = g_results.find(key);
    if (done != g_results.end()) return done->second.c_str();
    // Separa "Texto##id" / "Texto###id": só o texto se traduz.
    const std::string all = english;
    const size_t hash = all.find("##");
    const std::string visible = hash == std::string::npos ? all : all.substr(0, hash);
    const std::string id = hash == std::string::npos ? std::string() : all.substr(hash);
    auto it = g_table.find(visible);
    std::string out;
    if (it == g_table.end()) {
        out = all;
    } else {
        out = it->second;
        if (!id.empty()) out += id;
        else if (window) out += "###" + visible; // mesma janela em qualquer idioma
    }
    return g_results.emplace(key, std::move(out)).first->second.c_str();
}

} // namespace

void setUiLanguage(UiLanguage lang) {
    if (lang == g_lang && (lang == UiLanguage::English || !g_table.empty())) return;
    g_lang = lang;
    loadTable(lang);
}

UiLanguage uiLanguage() { return g_lang; }

const char* tr(const char* english) { return translate(english, false); }
const char* trw(const char* english) { return translate(english, true); }

} // namespace eruption
