// Folha de eventos: regras "quando / se / faça" lidas e gravadas como um
// arquivo .luau comum na pasta de scripts.
#include "editor/Editor.hpp"
#include "editor/EditorBlocks.hpp"
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
        if (c.def->params[i].type == ParamType::Text || c.def->params[i].type == ParamType::Model ||
            c.def->params[i].type == ParamType::Character)
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
            case ParamType::Character: {
                const std::string cur = unquote(code);
                if (ImGui::BeginCombo("##ch", cur.empty() ? "(choose)" : cur.c_str())) {
                    for (const auto& ch : m_engine->characters()) {
                        if (ch.removed) continue;
                        if (ImGui::Selectable(ch.name.c_str(), cur == ch.name)) { code = quote(ch.name); changed = true; }
                    }
                    ImGui::EndCombo();
                }
                if (m_engine->characters().empty())
                    ImGui::SetItemTooltip("Drag a PNG from the Project panel into the scene to add a character.");
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
    {
        // Mesma folha em duas vistas: linhas (planilha) ou blocos de encaixar.
        int view = m_rules.blocks ? 1 : 0;
        ImGui::SetNextItemWidth(120);
        const char* views[] = {lang == UiLanguage::Portuguese ? "Folha" : "Sheet",
                               lang == UiLanguage::Portuguese ? "Blocos" : "Blocks"};
        if (ImGui::Combo("##view", &view, views, 2)) m_rules.blocks = view == 1;
    }
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
    if (m_rules.blocks) {
        ImGui::BeginChild("##blocks", ImVec2(ImGui::GetContentRegionAvail().x - codeW, 0));
        drawRuleBlocks();
        ImGui::EndChild();
        if (m_rules.showCode) {
            ImGui::SameLine();
            ImGui::BeginChild("##code", ImVec2(0, 0), ImGuiChildFlags_Border, ImGuiWindowFlags_HorizontalScrollbar);
            ImGui::TextUnformatted(m_rules.sheet.generate().c_str());
            ImGui::EndChild();
        }
        ImGui::EndChild();
        ImGui::End();
        return;
    }
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

// ---------------------------------------------------------------- blocos
// A mesma folha de regras desenhada como blocos de encaixar: o evento é o
// "chapéu" de cima, as condições ficam na barra "se", as ações empilhadas
// dentro. Arrastar da paleta encaixa; arrastar um bloco o move; soltar na
// paleta apaga. O arquivo continua o mesmo Luau.

namespace {

constexpr const char* kBlockPayload = "ERUPTION_RULEBLOCK";

struct BlockDrag {
    const RuleDef* def; // da paleta
    int rule, list, index; // bloco existente (list: 0 evento, 1 condição, 2 ação, 3 senão)
};

} // namespace

bool Editor::ruleBlock(RuleCall& call, int kind, ImU32 color, int rule, int list, int index) {
    // Devolve true quando o bloco pede para ser apagado.
    const std::string text = sentence(call, m_lang);
    const ImVec2 ts = ImGui::CalcTextSize(text.c_str());
    const ImVec2 pad(10, kind == 0 ? 9 : 6);
    const ImVec2 size(std::max(ts.x + pad.x * 2, 120.0f), ts.y + pad.y * 2);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::PushID(rule * 4096 + list * 1024 + index);
    ImGui::InvisibleButton("##blk", size);
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 col = call.def ? color : IM_COL32(120, 120, 125, 255);
    blockShape(dl, p0, ImVec2(p0.x + size.x, p0.y + size.y), hovered ? darker(col, 1.12f) : col, kind);
    dl->AddText(ImVec2(p0.x + pad.x, p0.y + pad.y), IM_COL32(255, 255, 255, 255), text.c_str());
    if (ImGui::BeginDragDropSource()) {
        BlockDrag d{nullptr, rule, list, index};
        ImGui::SetDragDropPayload(kBlockPayload, &d, sizeof(d));
        ImGui::TextUnformatted(text.c_str());
        ImGui::EndDragDropSource();
    }
    // Clique (soltar sem arrastar) edita; apertar e arrastar move o bloco.
    const ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsItemDeactivated() && hovered && io.MouseDragMaxDistanceSqr[0] < io.MouseDragThreshold * io.MouseDragThreshold)
        ImGui::OpenPopup("##blkedit");
    bool remove = false;
    if (ImGui::BeginPopup("##blkedit")) {
        ImGui::TextUnformatted(text.c_str());
        ImGui::Separator();
        if (call.def) {
            if (rulesParamEditor(call)) rulesCommit("Edit rule");
            if (call.def->kind == RuleKind::Condition && ImGui::Checkbox(ruleUi("negate", m_lang), &call.negate))
                rulesCommit("Edit rule");
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

void Editor::drawRuleBlocks() {
    auto& rules = m_rules.sheet.rules;
    const UiLanguage lang = m_lang;
    // Recebe um bloco arrastado e o encaixa em (rule, list, index); index -1 = fim.
    auto acceptInto = [&](int rule, int list, int index) -> bool {
        const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kBlockPayload);
        if (!p) return false;
        const BlockDrag d = *static_cast<const BlockDrag*>(p->Data);
        auto listOf = [&](int r, int l) -> std::vector<RuleCall>* {
            if (r < 0 || r >= int(rules.size())) return nullptr;
            return l == 1 ? &rules[size_t(r)].conditions : l == 2 ? &rules[size_t(r)].actions
                         : l == 3 ? &rules[size_t(r)].otherwise : nullptr;
        };
        RuleCall moving;
        if (d.def) {
            moving = RuleSheet::makeCall(*d.def);
        } else {
            auto* src = listOf(d.rule, d.list);
            if (!src || d.index < 0 || d.index >= int(src->size())) return false;
            moving = (*src)[size_t(d.index)];
            src->erase(src->begin() + d.index);
            if (d.rule == rule && d.list == list && index > d.index) --index;
        }
        // Condição só entra no "se"; ação só em "faça"/"senão".
        const bool isCond = moving.def && moving.def->kind == RuleKind::Condition;
        if (isCond != (list == 1)) {
            if (!d.def) rulesLoad(m_rules.file); // desfaz a retirada
            return false;
        }
        auto* dst = listOf(rule, list);
        if (!dst) return false;
        if (index < 0 || index > int(dst->size())) index = int(dst->size());
        dst->insert(dst->begin() + index, moving);
        rulesCommit("Move block");
        return true;
    };

    // Paleta à esquerda (também apaga: soltar um bloco aqui).
    ImGui::BeginChild("##palette", ImVec2(250, 0), ImGuiChildFlags_Border | ImGuiChildFlags_ResizeX);
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##bsearch", ruleUi("search", lang), m_rules.search, sizeof(m_rules.search));
    const char* lastCategory = nullptr;
    for (const auto& def : ruleCatalog()) {
        std::string label = sentence(RuleSheet::makeCall(def), lang);
        std::string lower = label, query = m_rules.search;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
        std::transform(query.begin(), query.end(), query.begin(), [](unsigned char c) { return std::tolower(c); });
        if (!query.empty() && lower.find(query) == std::string::npos) continue;
        if (!lastCategory || std::strcmp(lastCategory, def.category) != 0) {
            lastCategory = def.category;
            ImGui::TextColored(kAccent, "%s", categoryName(def.category, lang));
        }
        const ImU32 col = def.kind == RuleKind::Trigger ? kEventColor : def.kind == RuleKind::Condition ? kConditionColor
                                                                                                         : kActionColor;
        const int kind = def.kind == RuleKind::Trigger ? 0 : def.kind == RuleKind::Condition ? 2 : 1;
        const ImVec2 ts = ImGui::CalcTextSize(label.c_str());
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const ImVec2 size(std::min(ImGui::GetContentRegionAvail().x, ts.x + 20), ts.y + 10);
        ImGui::PushID(&def);
        ImGui::InvisibleButton("##pal", size);
        blockShape(ImGui::GetWindowDrawList(), p0, ImVec2(p0.x + size.x, p0.y + size.y), col, kind);
        ImGui::GetWindowDrawList()->PushClipRect(p0, ImVec2(p0.x + size.x - 4, p0.y + size.y), true);
        ImGui::GetWindowDrawList()->AddText(ImVec2(p0.x + 10, p0.y + 5), IM_COL32(255, 255, 255, 255), label.c_str());
        ImGui::GetWindowDrawList()->PopClipRect();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", label.c_str());
        if (ImGui::BeginDragDropSource()) {
            BlockDrag d{&def, -1, -1, -1};
            ImGui::SetDragDropPayload(kBlockPayload, &d, sizeof(d));
            ImGui::TextUnformatted(label.c_str());
            ImGui::EndDragDropSource();
        }
        ImGui::PopID();
        ImGui::Dummy(ImVec2(0, 2));
    }
    ImGui::EndChild();
    if (ImGui::BeginDragDropTarget()) {
        // Soltar na paleta = apagar o bloco arrastado.
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kBlockPayload)) {
            const BlockDrag d = *static_cast<const BlockDrag*>(p->Data);
            if (!d.def && d.rule >= 0 && d.rule < int(rules.size())) {
                Rule& r = rules[size_t(d.rule)];
                if (d.list == 0) rules.erase(rules.begin() + d.rule);
                else {
                    auto& v = d.list == 1 ? r.conditions : d.list == 2 ? r.actions : r.otherwise;
                    if (d.index >= 0 && d.index < int(v.size())) v.erase(v.begin() + d.index);
                }
                rulesCommit("Delete block");
            }
        }
        ImGui::EndDragDropTarget();
    }

    ImGui::SameLine();
    ImGui::BeginChild("##canvas", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    int removeRule = -1;
    for (size_t ri = 0; ri < rules.size(); ++ri) {
        Rule& r = rules[ri];
        ImGui::PushID(int(ri));
        if (r.raw) {
            ImGui::TextDisabled("%s", r.code.c_str());
            ImGui::PopID();
            ImGui::Dummy(ImVec2(0, 10));
            continue;
        }
        if (ruleBlock(r.trigger, 0, kEventColor, int(ri), 0, 0)) removeRule = int(ri);
        // Condições: barra "se" com as condições em linha; aceita condição.
        const ImVec2 cStart = ImGui::GetCursorScreenPos();
        const bool hasIf = !r.conditions.empty();
        {
            ImGui::BeginGroup();
            ImGui::AlignTextToFramePadding();
            if (hasIf) ImGui::Indent(14);
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(hasIf ? kConditionColor : ImGui::GetColorU32(ImGuiCol_TextDisabled)),
                               "%s", lang == UiLanguage::Portuguese ? (hasIf ? "se" : "+ se") : (hasIf ? "if" : "+ if"));
            if (!hasIf && ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", lang == UiLanguage::Portuguese ? "Solte uma condição aqui" : "Drop a condition here");
            int removeCond = -1;
            for (size_t ci = 0; ci < r.conditions.size(); ++ci) {
                ImGui::SameLine();
                if (ruleBlock(r.conditions[ci], 2, kConditionColor, int(ri), 1, int(ci))) removeCond = int(ci);
                if (ImGui::BeginDragDropTarget()) { acceptInto(int(ri), 1, int(ci)); ImGui::EndDragDropTarget(); }
                if (ci + 1 < r.conditions.size()) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", lang == UiLanguage::Portuguese ? "e" : "and");
                }
            }
            if (hasIf) {
                ImGui::SameLine();
                ImGui::TextDisabled("+");
                ImGui::Unindent(14);
            }
            ImGui::EndGroup();
            if (ImGui::BeginDragDropTarget()) { acceptInto(int(ri), 1, -1); ImGui::EndDragDropTarget(); }
            if (removeCond >= 0) {
                r.conditions.erase(r.conditions.begin() + removeCond);
                rulesCommit("Delete block");
            }
        }
        // Ações (faça) e senão, recuadas dentro do bloco "se".
        auto stack = [&](std::vector<RuleCall>& list, int listId, const char* title) {
            ImGui::Indent(22);
            ImGui::TextDisabled("%s", title);
            int removeAt = -1;
            for (size_t ai = 0; ai < list.size(); ++ai) {
                if (ruleBlock(list[ai], 1, kActionColor, int(ri), listId, int(ai))) removeAt = int(ai);
                if (ImGui::BeginDragDropTarget()) { acceptInto(int(ri), listId, int(ai)); ImGui::EndDragDropTarget(); }
                ImGui::Dummy(ImVec2(0, 1));
            }
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::Button(lang == UiLanguage::Portuguese ? "+ solte uma ação" : "+ drop an action", ImVec2(150, 0));
            ImGui::PopStyleColor();
            if (ImGui::BeginDragDropTarget()) { acceptInto(int(ri), listId, -1); ImGui::EndDragDropTarget(); }
            ImGui::Unindent(22);
            if (removeAt >= 0) {
                list.erase(list.begin() + removeAt);
                rulesCommit("Delete block");
            }
        };
        stack(r.actions, 2, ruleUi("do", lang));
        if (hasIf || !r.otherwise.empty()) stack(r.otherwise, 3, ruleUi("else", lang));
        // Braço do "se" abraçando as ações (só quando há condição).
        if (hasIf) {
            const ImVec2 cEnd = ImGui::GetCursorScreenPos();
            dl->AddRectFilled(ImVec2(cStart.x, cStart.y), ImVec2(cStart.x + 8, cEnd.y), kConditionColor, 3.0f);
            dl->AddRectFilled(ImVec2(cStart.x, cEnd.y), ImVec2(cStart.x + 60, cEnd.y + 6), kConditionColor, 3.0f);
        }
        ImGui::Dummy(ImVec2(0, 18));
        ImGui::PopID();
    }
    if (removeRule >= 0) {
        rules.erase(rules.begin() + removeRule);
        rulesCommit("Delete rule");
    }
    // Área livre: evento solto aqui vira regra nova.
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::Button(lang == UiLanguage::Portuguese ? "+ solte um evento aqui para criar uma regra"
                                                 : "+ drop an event here to start a rule",
                  ImVec2(std::max(320.0f, ImGui::GetContentRegionAvail().x), 60));
    ImGui::PopStyleColor();
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kBlockPayload)) {
            const BlockDrag d = *static_cast<const BlockDrag*>(p->Data);
            if (d.def && d.def->kind == RuleKind::Trigger) {
                Rule nr;
                nr.trigger = RuleSheet::makeCall(*d.def);
                rules.push_back(std::move(nr));
                rulesCommit("Add rule");
            }
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::EndChild();
}

} // namespace eruption
