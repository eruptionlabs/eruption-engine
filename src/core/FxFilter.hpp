#pragma once
// Busca nos paineis de efeitos (F2): uma caixa no topo filtra os controles por nome ou por descricao.
//
// Camada fina por cima do ImGui. Fora do escopo do painel (ou com a caixa vazia) cada funcao repassa
// a chamada sem mudar nada. Com texto na caixa, dentro do escopo, o controle que nao bate fica
// escondido (junto com titulos, separadores, abas e secoes, que viram ruido sem os controles).
// O texto digitado vale por palavra: todas precisam aparecer no nome ou na descricao do controle,
// sem diferenciar maiuscula de minuscula.
//
// A descricao de um controle vem de fx::describe(nome, texto), gerada a partir do tooltip dele.

#include <imgui.h>

#include <cctype>
#include <cstdarg>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace eruption::fx {

constexpr int kBuf = 128;

struct State {
    char text[kBuf] = "";
    bool scope = false;        // dentro do painel que aceita busca
    bool active = false;       // escopo ligado e caixa com texto, fixo durante o quadro
    bool lastSkipped = false;  // o ultimo controle foi escondido
    int matched = 0;           // controles mostrados neste quadro
    std::vector<std::string> tokens;
    std::unordered_map<std::string, std::string> desc;
};

inline State& st() {
    static State s;
    return s;
}

inline std::string lower(const char* s) {
    std::string r;
    for (; s && *s; ++s) r += static_cast<char>(std::tolower(static_cast<unsigned char>(*s)));
    return r;
}

// "Nome##id" -> "Nome".
inline std::string visibleLabel(const char* label) {
    std::string l = label ? label : "";
    const size_t p = l.find("##");
    return p == std::string::npos ? l : l.substr(0, p);
}

inline void describe(const char* label, const char* text) {
    st().desc[visibleLabel(label)] = text ? text : "";
}

inline bool matches(const char* label) {
    State& s = st();
    const std::string name = visibleLabel(label);
    std::string hay = lower(name.c_str());
    if (auto it = s.desc.find(name); it != s.desc.end()) hay += " " + lower(it->second.c_str());
    for (const std::string& t : s.tokens)
        if (hay.find(t) == std::string::npos) return false;
    return true;
}

// Caixa de busca. Chamar antes de beginScope(), com o escopo desligado.
inline void drawBox() {
    State& s = st();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##fxfilter", "Buscar efeito: nome ou descricao (ex.: displacement, shadow, bloom)", s.text, kBuf);
}

inline void beginScope() {
    State& s = st();
    s.tokens.clear();
    const std::string t = lower(s.text);
    size_t i = 0;
    while (i < t.size()) {
        while (i < t.size() && std::isspace(static_cast<unsigned char>(t[i]))) ++i;
        size_t j = i;
        while (j < t.size() && !std::isspace(static_cast<unsigned char>(t[j]))) ++j;
        if (j > i) s.tokens.push_back(t.substr(i, j - i));
        i = j;
    }
    s.scope = true;
    s.active = !s.tokens.empty();
    s.matched = 0;
    s.lastSkipped = false;
}

inline void endScope() {
    State& s = st();
    const bool none = s.active && s.matched == 0;
    s.scope = false;
    s.active = false;
    s.lastSkipped = false;
    if (none) ImGui::TextDisabled("Nenhum controle encontrado.");
}

inline bool active() { return st().active; }

// true quando o controle deve ficar escondido neste quadro.
inline bool skip(const char* label) {
    State& s = st();
    if (!s.active) {
        s.lastSkipped = false;
        return false;
    }
    s.lastSkipped = !matches(label);
    if (!s.lastSkipped) ++s.matched;
    return s.lastSkipped;
}

// ---- controles ----
template <class... A> bool SliderFloat(const char* l, A&&... a) { return skip(l) ? false : ImGui::SliderFloat(l, std::forward<A>(a)...); }
template <class... A> bool SliderInt(const char* l, A&&... a) { return skip(l) ? false : ImGui::SliderInt(l, std::forward<A>(a)...); }
template <class... A> bool Checkbox(const char* l, A&&... a) { return skip(l) ? false : ImGui::Checkbox(l, std::forward<A>(a)...); }
template <class... A> bool Combo(const char* l, A&&... a) { return skip(l) ? false : ImGui::Combo(l, std::forward<A>(a)...); }
template <class... A> bool ColorEdit3(const char* l, A&&... a) { return skip(l) ? false : ImGui::ColorEdit3(l, std::forward<A>(a)...); }
template <class... A> bool DragFloat(const char* l, A&&... a) { return skip(l) ? false : ImGui::DragFloat(l, std::forward<A>(a)...); }
template <class... A> bool RadioButton(const char* l, A&&... a) { return skip(l) ? false : ImGui::RadioButton(l, std::forward<A>(a)...); }
template <class... A> bool InputFloat(const char* l, A&&... a) { return skip(l) ? false : ImGui::InputFloat(l, std::forward<A>(a)...); }
template <class... A> bool InputInt(const char* l, A&&... a) { return skip(l) ? false : ImGui::InputInt(l, std::forward<A>(a)...); }
template <class... A> bool InputText(const char* l, A&&... a) { return skip(l) ? false : ImGui::InputText(l, std::forward<A>(a)...); }
template <class... A> bool DragInt(const char* l, A&&... a) { return skip(l) ? false : ImGui::DragInt(l, std::forward<A>(a)...); }
template <class... A> bool ColorEdit4(const char* l, A&&... a) { return skip(l) ? false : ImGui::ColorEdit4(l, std::forward<A>(a)...); }
template <class... A> bool Button(const char* l, A&&... a) { return skip(l) ? false : ImGui::Button(l, std::forward<A>(a)...); }

// ---- estado do ultimo controle (tooltips e eventos so' valem para um controle mostrado) ----
template <class... A> bool IsItemHovered(A&&... a) { return !st().lastSkipped && ImGui::IsItemHovered(std::forward<A>(a)...); }
inline bool IsItemActive() { return !st().lastSkipped && ImGui::IsItemActive(); }
inline bool IsItemDeactivatedAfterEdit() { return !st().lastSkipped && ImGui::IsItemDeactivatedAfterEdit(); }
inline void SetTooltip(const char* fmt, ...) IM_FMTARGS(1);
inline void SetTooltip(const char* fmt, ...) {
    if (st().lastSkipped) return;
    va_list ap;
    va_start(ap, fmt);
    ImGui::SetTooltipV(fmt, ap);
    va_end(ap);
}
inline void SetItemTooltip(const char* fmt, ...) IM_FMTARGS(1);
inline void SetItemTooltip(const char* fmt, ...) {
    if (st().lastSkipped) return;
    va_list ap;
    va_start(ap, fmt);
    ImGui::SetItemTooltipV(fmt, ap);
    va_end(ap);
}

// ---- texto e layout: somem enquanto a busca filtra ----
inline void Text(const char* fmt, ...) IM_FMTARGS(1);
inline void Text(const char* fmt, ...) {
    if (st().active) return;
    va_list ap;
    va_start(ap, fmt);
    ImGui::TextV(fmt, ap);
    va_end(ap);
}
inline void TextColored(const ImVec4& c, const char* fmt, ...) IM_FMTARGS(2);
inline void TextColored(const ImVec4& c, const char* fmt, ...) {
    if (st().active) return;
    va_list ap;
    va_start(ap, fmt);
    ImGui::TextColoredV(c, fmt, ap);
    va_end(ap);
}
inline void TextDisabled(const char* fmt, ...) IM_FMTARGS(1);
inline void TextDisabled(const char* fmt, ...) {
    if (st().active) return;
    va_list ap;
    va_start(ap, fmt);
    ImGui::TextDisabledV(fmt, ap);
    va_end(ap);
}
inline void TextWrapped(const char* fmt, ...) IM_FMTARGS(1);
inline void TextWrapped(const char* fmt, ...) {
    if (st().active) return;
    va_list ap;
    va_start(ap, fmt);
    ImGui::TextWrappedV(fmt, ap);
    va_end(ap);
}
inline void Separator() { if (!st().active) ImGui::Separator(); }
inline void Spacing() { if (!st().active) ImGui::Spacing(); }
template <class... A> void SameLine(A&&... a) { if (!st().active) ImGui::SameLine(std::forward<A>(a)...); }

// ---- abas e secoes: com a busca ativa o conteudo aparece direto, sem abas nem cabecalhos ----
template <class... A> bool BeginTabBar(const char* id, A&&... a) { return st().active ? true : ImGui::BeginTabBar(id, std::forward<A>(a)...); }
inline void EndTabBar() { if (!st().active) ImGui::EndTabBar(); }
template <class... A> bool BeginTabItem(const char* l, A&&... a) { return st().active ? true : ImGui::BeginTabItem(l, std::forward<A>(a)...); }
inline void EndTabItem() { if (!st().active) ImGui::EndTabItem(); }
template <class... A> bool CollapsingHeader(const char* l, A&&... a) { return st().active ? true : ImGui::CollapsingHeader(l, std::forward<A>(a)...); }
template <class... A> bool TreeNode(const char* l, A&&... a) { return st().active ? true : ImGui::TreeNode(l, std::forward<A>(a)...); }
template <class... A> bool TreeNodeEx(const char* l, A&&... a) { return st().active ? true : ImGui::TreeNodeEx(l, std::forward<A>(a)...); }
inline void TreePop() { if (!st().active) ImGui::TreePop(); }

} // namespace eruption::fx
