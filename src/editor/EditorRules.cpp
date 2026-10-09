// Folha de eventos: regras "quando / se / faça" lidas e gravadas como um
// arquivo .luau comum na pasta de scripts.
#include "editor/Editor.hpp"
#include "editor/EditorTheme.hpp"

#include "core/Engine.hpp"
#include "renderer/WeatherTypes.hpp"
#include "script/ScriptHost.hpp"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>

namespace eruption {

namespace {

namespace fs = std::filesystem;

const ImU32 kEventColor = IM_COL32(255, 60, 40, 255);
const ImU32 kConditionColor = IM_COL32(70, 140, 230, 255);
const ImU32 kActionColor = IM_COL32(110, 190, 90, 255);

std::string readText(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string unquote(const std::string& code) {
    if (code.size() >= 2 && code.front() == '"' && code.back() == '"') return code.substr(1, code.size() - 2);
    return code;
}

std::string quote(const std::string& text) {
    std::string out = "\"";
    for (char c : text) {
        if (c == '"' || c == '\\') out += '\\';
        if (c == '\n') { out += "\\n"; continue; }
        out += c;
    }
    return out + "\"";
}

std::string formatNumber(double v) {
    char buf[32];
    if (std::abs(v - std::round(v)) < 1e-6) std::snprintf(buf, sizeof(buf), "%.0f", v);
    else std::snprintf(buf, sizeof(buf), "%g", v);
    return buf;
}

bool isNumberLiteral(const std::string& s) {
    if (s.empty()) return false;
    char* end = nullptr;
    std::strtod(s.c_str(), &end);
    return end && *end == '\0';
}

// Frase do catálogo com os valores no lugar de {1}, {2}...
std::string sentence(const RuleCall& c, UiLanguage lang) {
    if (!c.def) return c.code;
    std::string text = ruleText(*c.def, lang);
    for (size_t i = 0; i < c.args.size(); ++i) {
        const std::string key = "{" + std::to_string(i + 1) + "}";
        const size_t at = text.find(key);
        if (at == std::string::npos) continue;
        std::string value = ruleArgDisplay(c.def->params[i].type, c.args[i], lang);
        if (value.empty()) value = "?";
        if (c.def->params[i].type == ParamType::Text || c.def->params[i].type == ParamType::Model)
            value = "\"" + value + "\"";
        text.replace(at, key.size(), value);
    }
    const size_t op = text.find("{op}");
    if (op != std::string::npos) {
        static const struct { const char* op; const char* en; const char* pt; } kOps[] = {
            {"==", "is", "é"}, {"~=", "is not", "não é"}, {"<", "is less than", "é menor que"},
            {"<=", "is at most", "é no máximo"}, {">", "is greater than", "é maior que"},
            {">=", "is at least", "é no mínimo"}};
        std::string word = c.op;
        for (const auto& o : kOps)
            if (c.op == o.op) word = lang == UiLanguage::Portuguese ? o.pt : o.en;
        text.replace(op, 4, word);
    }
    if (c.negate) text = std::string(ruleUi("not", lang)) + ": " + text;
    return text;
}

} // namespace

void Editor::rulesLoad(const fs::path& file) {
    m_rules.file = file;
    m_rules.error.clear();
    std::error_code ec;
    m_rules.mtime = fs::last_write_time(file, ec);
    m_rules.savedText = readText(file);
    if (!m_rules.sheet.parse(m_rules.savedText, m_rules.error)) m_rules.sheet = RuleSheet{};
}

void Editor::rulesCommit(const std::string& label) {
    const std::string before = m_rules.savedText;
    const std::string after = m_rules.sheet.generate();
    if (before == after) return;
    const fs::path file = m_rules.file;
    auto write = [this, file](const std::string& text) {
        std::ofstream(file, std::ios::binary) << text;
        if (m_rules.file == file) rulesLoad(file);
    };
    write(after);
    m_undo.push({label, [write, before] { write(before); }, [write, after] { write(after); }});
}

bool Editor::rulesParamEditor(RuleCall& call) {
    bool changed = false;
    if (!call.def) return false;
    const UiLanguage lang = m_lang;
    if (call.def->kind == RuleKind::Condition && call.def->id != std::string(kVarCompareId))
        changed |= ImGui::Checkbox(ruleUi("negate", lang), &call.negate);

    for (size_t i = 0; i < call.args.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        const ParamType type = call.def->params[i].type;
        std::string& code = call.args[i];
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("{%zu}", i + 1);
        ImGui::SameLine(44);
        ImGui::SetNextItemWidth(260);
        switch (type) {
            case ParamType::Text:
            case ParamType::Variable:
            case ParamType::Model: {
                char buf[256];
                std::snprintf(buf, sizeof(buf), "%s", unquote(code).c_str());
                if (ImGui::InputText("##s", buf, sizeof(buf))) { code = quote(buf); changed = true; }
                if (type == ParamType::Model) {
                    // Sugestões com os nomes do mapa que contêm o texto digitado.
                    const auto& insts = m_engine->modelRenderer().getInstances();
                    int shown = 0;
                    const std::string typed = buf;
                    if (!typed.empty() && ImGui::BeginListBox("##models", ImVec2(260, 90))) {
                        for (const auto& inst : insts) {
                            if (inst.name.find(typed) == std::string::npos || inst.name == typed) continue;
                            if (ImGui::Selectable(inst.name.c_str())) { code = quote(inst.name); changed = true; }
                            if (++shown >= 40) break;
                        }
                        ImGui::EndListBox();
                    }
                }
                break;
            }
            case ParamType::Key: {
                static const char* kKeys[] = {"space", "enter", "escape", "tab", "left", "right", "up", "down",
                                              "shift", "ctrl", "alt", "A", "B", "C", "D", "E", "F", "G", "H", "I",
                                              "J", "K", "L", "M", "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W",
                                              "X", "Y", "Z", "0", "1", "2", "3", "4", "5", "6", "7", "8", "9"};
                const std::string cur = unquote(code);
                if (ImGui::BeginCombo("##k", cur.c_str())) {
                    for (const char* k : kKeys)
                        if (ImGui::Selectable(k, cur == k)) { code = quote(k); changed = true; }
                    ImGui::EndCombo();
                }
                break;
            }
            case ParamType::Weather: {
                const std::string cur = unquote(code);
                if (ImGui::BeginCombo("##w", cur.c_str())) {
                    for (uint32_t t = 0; t < WeatherTypeCount; ++t) {
                        const char* name = weatherTypeName(static_cast<WeatherType>(t));
                        if (ImGui::Selectable(name, cur == name)) { code = quote(name); changed = true; }
                    }
                    ImGui::EndCombo();
                }
                break;
            }
            case ParamType::Number:
            case ParamType::Seconds:
            case ParamType::Hour:
            case ParamType::Light: {
                if (!isNumberLiteral(code)) {
                    char buf[256];
                    std::snprintf(buf, sizeof(buf), "%s", code.c_str());
                    if (ImGui::InputText("##expr", buf, sizeof(buf))) { code = buf; changed = true; }
                    break;
                }
                float v = static_cast<float>(std::strtod(code.c_str(), nullptr));
                bool edited = false;
                if (type == ParamType::Hour) edited = ImGui::SliderFloat("##h", &v, 0.0f, 24.0f, "%.1f h");
                else if (type == ParamType::Light) {
                    int li = static_cast<int>(v);
                    edited = ImGui::InputInt("##l", &li);
                    v = static_cast<float>(std::max(1, li));
                } else edited = ImGui::DragFloat("##n", &v, type == ParamType::Seconds ? 0.05f : 0.5f, 0.0f, 0.0f, "%g");
                if (edited) { code = formatNumber(v); changed = true; }
                break;
            }
            case ParamType::Value: {
                const bool isBool = code == "true" || code == "false";
                const bool isText = code.size() >= 2 && code.front() == '"';
                int kind = isBool ? 2 : (isText ? 1 : 0);
                const char* kinds[] = {lang == UiLanguage::Portuguese ? "número" : "number",
                                       lang == UiLanguage::Portuguese ? "texto" : "text",
                                       lang == UiLanguage::Portuguese ? "sim/não" : "yes/no"};
                ImGui::SetNextItemWidth(90);
                if (ImGui::Combo("##vk", &kind, kinds, 3)) {
                    code = kind == 0 ? "0" : (kind == 1 ? "\"\"" : "true");
                    changed = true;
                }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(160);
                if (kind == 0) {
                    float v = static_cast<float>(std::strtod(code.c_str(), nullptr));
                    if (ImGui::DragFloat("##vn", &v, 0.5f, 0.0f, 0.0f, "%g")) { code = formatNumber(v); changed = true; }
                } else if (kind == 1) {
                    char buf[256];
                    std::snprintf(buf, sizeof(buf), "%s", unquote(code).c_str());
                    if (ImGui::InputText("##vs", buf, sizeof(buf))) { code = quote(buf); changed = true; }
                } else {
                    bool b = code == "true";
                    if (ImGui::Checkbox("##vb", &b)) { code = b ? "true" : "false"; changed = true; }
                }
                break;
            }
            case ParamType::Offset: {
                float v[3] = {0, 0, 0};
                if (code.rfind("vector.create(", 0) == 0)
                    std::sscanf(code.c_str() + 14, "%f , %f , %f", &v[0], &v[1], &v[2]);
                if (ImGui::DragFloat3("##v", v, 0.5f, 0.0f, 0.0f, "%g")) {
                    code = "vector.create(" + formatNumber(v[0]) + ", " + formatNumber(v[1]) + ", " + formatNumber(v[2]) + ")";
                    changed = true;
                }
                break;
            }
        }
        ImGui::PopID();
    }
    if (call.def->id == std::string(kVarCompareId)) {
        static const char* kOps[] = {"==", "~=", "<", "<=", ">", ">="};
        ImGui::SetNextItemWidth(80);
        if (ImGui::BeginCombo("##op", call.op.c_str())) {
            for (const char* o : kOps)
                if (ImGui::Selectable(o, call.op == o)) { call.op = o; changed = true; }
            ImGui::EndCombo();
        }
    }
    return changed;
}

bool Editor::rulesChip(RuleCall& call, int id, ImU32 color) {
    // Devolve true quando o usuário pede para apagar.
    const std::string text = sentence(call, m_lang);
    ImGui::PushID(id);
    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.5f);
    ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(call.def ? color : IM_COL32(120, 120, 125, 255)));
    if (ImGui::Button(text.c_str())) ImGui::OpenPopup("##edit");
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
    bool remove = false;
    if (ImGui::BeginPopup("##edit")) {
        ImGui::TextUnformatted(text.c_str());
        ImGui::Separator();
        if (call.def) {
            if (rulesParamEditor(call)) rulesCommit("Edit rule");
        } else {
            ImGui::TextDisabled("%s", ruleUi("raw", m_lang));
        }
        ImGui::Separator();
        if (ImGui::Button(ruleUi("delete", m_lang))) {
            remove = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
    return remove;
}

void Editor::rulesAddPopup() {
    if (m_rules.openAdd) {
        ImGui::OpenPopup("##addrule");
        m_rules.openAdd = false;
        m_rules.search[0] = '\0';
    }
    ImGui::SetNextWindowSize(ImVec2(420, 380), ImGuiCond_Appearing);
    if (!ImGui::BeginPopup("##addrule")) return;
    const RuleKind kind = m_rules.addList == 3 ? RuleKind::Trigger
                        : (m_rules.addList == 0 ? RuleKind::Condition : RuleKind::Action);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##search", ruleUi("search", m_lang), m_rules.search, sizeof(m_rules.search));
    ImGui::BeginChild("##list");
    const char* lastCategory = nullptr;
    for (const auto& def : ruleCatalog()) {
        if (def.kind != kind) continue;
        std::string label = sentence(RuleSheet::makeCall(def), m_lang);
        std::string lower = label, query = m_rules.search;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
        std::transform(query.begin(), query.end(), query.begin(), [](unsigned char c) { return std::tolower(c); });
        if (!query.empty() && lower.find(query) == std::string::npos) continue;
        if (!lastCategory || std::strcmp(lastCategory, def.category) != 0) {
            lastCategory = def.category;
            ImGui::TextColored(kAccent, "%s", categoryName(def.category, m_lang));
        }
        if (ImGui::Selectable(label.c_str())) {
            RuleCall call = RuleSheet::makeCall(def);
            auto& rules = m_rules.sheet.rules;
            if (m_rules.addList == 3) {
                Rule r;
                r.trigger = call;
                rules.push_back(std::move(r));
            } else if (m_rules.addRule >= 0 && m_rules.addRule < static_cast<int>(rules.size())) {
                Rule& r = rules[static_cast<size_t>(m_rules.addRule)];
                (m_rules.addList == 0 ? r.conditions : m_rules.addList == 1 ? r.actions : r.otherwise).push_back(call);
            }
            rulesCommit("Add to rule sheet");
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::EndChild();
    ImGui::EndPopup();
}

void Editor::drawRules() {
    if (!ImGui::Begin("Rules###Rules", &m_showRules)) { ImGui::End(); return; }
    const UiLanguage lang = m_lang;

    // Folhas = arquivos .luau da pasta de scripts.
    ImGui::BeginChild("##sheets", ImVec2(200, 0), ImGuiChildFlags_Border | ImGuiChildFlags_ResizeX);
    if (m_scripts) {
        std::error_code ec;
        std::vector<fs::path> files;
        for (const auto& e : fs::directory_iterator(m_scripts->dir(), ec))
            if (e.path().extension() == ".luau" && e.path().filename().string().find(".d.luau") == std::string::npos)
                files.push_back(e.path());
        std::sort(files.begin(), files.end());
        for (const auto& f : files)
            if (ImGui::Selectable(f.stem().string().c_str(), f == m_rules.file)) rulesLoad(f);
        ImGui::Separator();
        ImGui::TextDisabled("%s", ruleUi("new_name", lang));
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##newname", m_rules.newName, sizeof(m_rules.newName));
        if (ImGui::Button(ruleUi("new_sheet", lang), ImVec2(-1, 0))) {
            std::string base = m_rules.newName[0] ? m_rules.newName : "rules";
            fs::path p = m_scripts->dir() / (base + ".luau");
            for (int i = 2; fs::exists(p); ++i) p = m_scripts->dir() / (base + "_" + std::to_string(i) + ".luau");
            std::ofstream(p, std::ios::binary) << RuleSheet{}.generate();
            rulesLoad(p);
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();

    ImGui::BeginChild("##sheet");
    // Barra: idioma, código, VS Code.
    int langIdx = lang == UiLanguage::Portuguese ? 1 : 0;
    const char* langs[] = {"English", "Português"};
    ImGui::SetNextItemWidth(120);
    if (ImGui::Combo("##lang", &langIdx, langs, 2)) m_lang = langIdx == 1 ? UiLanguage::Portuguese : UiLanguage::English;
    ImGui::SameLine();
    ImGui::Checkbox(ruleUi("show_code", lang), &m_rules.showCode);
    if (!m_rules.file.empty()) {
        ImGui::SameLine();
        if (ImGui::Button(ruleUi("edit_code", lang))) codeOpen(m_rules.file);
        ImGui::SameLine();
        if (ImGui::Button(ruleUi("open_vscode", lang))) openInExternalEditor(m_rules.file, 1);
        ImGui::SameLine();
        ImGui::TextDisabled("%s", m_rules.file.filename().string().c_str());
    }
    ImGui::Separator();

    if (m_rules.file.empty()) {
        ImGui::TextDisabled("%s", ruleUi("no_sheet", lang));
        ImGui::EndChild();
        ImGui::End();
        return;
    }
    // Mudou fora do editor (VS Code, IA): recarrega.
    std::error_code ec;
    if (fs::exists(m_rules.file, ec) && fs::last_write_time(m_rules.file, ec) != m_rules.mtime) rulesLoad(m_rules.file);
    if (!m_rules.error.empty()) {
        ImGui::TextColored(ImVec4(0.95f, 0.38f, 0.35f, 1.0f), "%s", ruleUi("syntax_error", lang));
        ImGui::TextWrapped("%s", m_rules.error.c_str());
        ImGui::EndChild();
        ImGui::End();
        return;
    }

    const float codeW = m_rules.showCode ? std::min(420.0f, ImGui::GetContentRegionAvail().x * 0.36f) : 0.0f;
    ImGui::BeginChild("##rows", ImVec2(ImGui::GetContentRegionAvail().x - codeW, 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_HorizontalScrollbar);
    auto& rules = m_rules.sheet.rules;
    if (rules.empty()) ImGui::TextDisabled("%s", ruleUi("empty", lang));
    const float colW = ImGui::GetContentRegionAvail().x * 0.48f;
    int removeRule = -1, moveRule = -1, moveDir = 0, dupRule = -1;
    for (size_t ri = 0; ri < rules.size(); ++ri) {
        Rule& r = rules[ri];
        ImGui::PushID(static_cast<int>(ri));
        const ImVec2 rowStart = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->ChannelsSplit(2);
        dl->ChannelsSetCurrent(1);
        ImGui::BeginGroup();
        ImGui::TextDisabled("%zu", ri + 1);
        ImGui::SameLine(28);
        if (r.raw) {
            ImGui::BeginGroup();
            ImGui::TextDisabled("%s", ruleUi("raw", lang));
            ImGui::TextUnformatted(r.code.c_str());
            ImGui::EndGroup();
        } else {
            // QUANDO / SE
            ImGui::BeginGroup();
            if (rulesChip(r.trigger, 1, kEventColor)) removeRule = static_cast<int>(ri);
            int removeCond = -1;
            for (size_t ci = 0; ci < r.conditions.size(); ++ci)
                if (rulesChip(r.conditions[ci], 100 + static_cast<int>(ci), kConditionColor)) removeCond = static_cast<int>(ci);
            if (removeCond >= 0) {
                r.conditions.erase(r.conditions.begin() + removeCond);
                rulesCommit("Remove condition");
            }
            if (ImGui::SmallButton(ruleUi("add_condition", lang))) {
                m_rules.addRule = static_cast<int>(ri);
                m_rules.addList = 0;
                m_rules.openAdd = true;
            }
            ImGui::EndGroup();
            // FAÇA / SENÃO
            ImGui::SameLine(28 + colW);
            ImGui::BeginGroup();
            int removeAct = -1, removeElse = -1;
            for (size_t ai = 0; ai < r.actions.size(); ++ai)
                if (rulesChip(r.actions[ai], 200 + static_cast<int>(ai), kActionColor)) removeAct = static_cast<int>(ai);
            if (ImGui::SmallButton(ruleUi("add_action", lang))) {
                m_rules.addRule = static_cast<int>(ri);
                m_rules.addList = 1;
                m_rules.openAdd = true;
            }
            if (!r.conditions.empty()) {
                if (!r.otherwise.empty()) ImGui::TextDisabled("%s", ruleUi("else", lang));
                for (size_t ei = 0; ei < r.otherwise.size(); ++ei)
                    if (rulesChip(r.otherwise[ei], 300 + static_cast<int>(ei), kActionColor)) removeElse = static_cast<int>(ei);
                ImGui::SameLine();
                if (ImGui::SmallButton(ruleUi("add_else", lang))) {
                    m_rules.addRule = static_cast<int>(ri);
                    m_rules.addList = 2;
                    m_rules.openAdd = true;
                }
            }
            if (removeAct >= 0) {
                r.actions.erase(r.actions.begin() + removeAct);
                rulesCommit("Remove action");
            }
            if (removeElse >= 0) {
                r.otherwise.erase(r.otherwise.begin() + removeElse);
                rulesCommit("Remove action");
            }
            ImGui::EndGroup();
        }
        ImGui::EndGroup();
        // Fundo da linha e menu de contexto.
        const ImVec2 rowEnd(ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x, ImGui::GetItemRectMax().y + 6);
        dl->ChannelsSetCurrent(0);
        dl->AddRectFilled(ImVec2(rowStart.x - 4, rowStart.y - 4), rowEnd,
                          ri % 2 ? IM_COL32(40, 41, 44, 255) : IM_COL32(46, 47, 50, 255), 5.0f);
        dl->ChannelsMerge();
        if (ImGui::BeginPopupContextItem("##rowmenu")) {
            if (ImGui::MenuItem(ruleUi("move_up", lang), nullptr, false, ri > 0)) { moveRule = static_cast<int>(ri); moveDir = -1; }
            if (ImGui::MenuItem(ruleUi("move_down", lang), nullptr, false, ri + 1 < rules.size())) { moveRule = static_cast<int>(ri); moveDir = 1; }
            if (ImGui::MenuItem(ruleUi("duplicate", lang))) dupRule = static_cast<int>(ri);
            if (ImGui::MenuItem(ruleUi("delete", lang))) removeRule = static_cast<int>(ri);
            ImGui::EndPopup();
        }
        ImGui::Dummy(ImVec2(0, 8));
        ImGui::PopID();
    }
    if (removeRule >= 0) {
        rules.erase(rules.begin() + removeRule);
        rulesCommit("Remove rule");
    } else if (moveRule >= 0) {
        std::swap(rules[static_cast<size_t>(moveRule)], rules[static_cast<size_t>(moveRule + moveDir)]);
        rulesCommit("Move rule");
    } else if (dupRule >= 0) {
        rules.insert(rules.begin() + dupRule + 1, rules[static_cast<size_t>(dupRule)]);
        rulesCommit("Duplicate rule");
    }
    if (ImGui::Button(ruleUi("add_rule", lang))) {
        m_rules.addRule = -1;
        m_rules.addList = 3;
        m_rules.openAdd = true;
    }
    rulesAddPopup();
    ImGui::EndChild();

    if (m_rules.showCode) {
        ImGui::SameLine();
        ImGui::BeginChild("##code", ImVec2(0, 0), ImGuiChildFlags_Border, ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::TextDisabled("%s  (%s)", ruleUi("code", lang), m_rules.file.filename().string().c_str());
        ImGui::Separator();
        ImGui::TextUnformatted(m_rules.savedText.c_str());
        ImGui::EndChild();
    }
    ImGui::EndChild();
    ImGui::End();
}

} // namespace eruption
