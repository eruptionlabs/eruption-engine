#include "editor/VfxGraphEditor.hpp"

#include "editor/EditorTheme.hpp"
#include "editor/Translate.hpp"

#include <imgui.h>
#include <imnodes.h>

#include <algorithm>
#include <cstdio>
#include <vector>

namespace eruption {

namespace {

// Pinos: entrada = no*16 + pino; saida = no*16 + 8 + pino.
int inPin(int node, int pin) { return node * 16 + pin; }
int outPin(int node, int pin) { return node * 16 + 8 + pin; }

const char* kCategories[] = {"Input", "Constant", "Math", "Vector", "Output"};

} // namespace

void VfxGraphEditor::init() {
    if (m_ctx) return;
    m_ctx = ImNodes::CreateContext();
    ImNodes::SetCurrentContext(m_ctx);
    ImNodes::StyleColorsDark();
    ImNodesStyle& st = ImNodes::GetStyle();
    st.Colors[ImNodesCol_TitleBarSelected] = ImGui::ColorConvertFloat4ToU32(kAccent);
    st.Colors[ImNodesCol_LinkSelected] = ImGui::ColorConvertFloat4ToU32(kAccent);
}

void VfxGraphEditor::shutdown() {
    if (m_ctx) ImNodes::DestroyContext(m_ctx);
    m_ctx = nullptr;
}

bool VfxGraphEditor::drawNode(VfxGraphNode& n, const VfxGraph& g) {
    const VfxGraphNodeType* t = vfxGraphType(n.op);
    if (!t) return false;
    bool changed = false;
    const bool output = std::string(t->category) == "Output";
    if (output) ImNodes::PushColorStyle(ImNodesCol_TitleBar, IM_COL32(150, 45, 30, 255));
    ImNodes::BeginNode(n.id);
    ImNodes::BeginNodeTitleBar();
    ImGui::TextUnformatted(tr(t->label));
    ImNodes::EndNodeTitleBar();
    ImGui::PushID(n.id);
    ImGui::PushItemWidth(70.0f);
    // Valor proprio do no (constantes, semente do aleatorio).
    if (n.op == "const_float") changed |= ImGui::DragFloat("##v", &n.value.x, 0.01f);
    else if (n.op == "const_vec3") {
        ImGui::PushItemWidth(150.0f);
        changed |= ImGui::DragFloat3("##v", &n.value.x, 0.01f);
        ImGui::PopItemWidth();
    } else if (n.op == "const_color") {
        changed |= ImGui::ColorEdit4("##v", &n.value.x, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR |
                                                            ImGuiColorEditFlags_NoInputs);
    } else if (n.op == "in_random") {
        int seed = int(n.value.x);
        if (ImGui::DragInt(tr("seed"), &seed, 0.2f, 0, 1023)) {
            n.value.x = float(std::clamp(seed, 0, 1023));
            changed = true;
        }
    }
    for (int i = 0; i < t->inputs; ++i) {
        ImNodes::BeginInputAttribute(inPin(n.id, i));
        bool linked = false;
        for (const auto& l : g.links)
            if (l.toNode == n.id && l.toPin == i) linked = true;
        if (linked) {
            ImGui::TextUnformatted(t->inputNames[i]);
        } else {
            // Entrada solta: o valor fica no no (componente i).
            ImGui::PushID(i);
            changed |= ImGui::DragFloat(t->inputNames[i], &n.value[i], 0.01f);
            ImGui::PopID();
        }
        ImNodes::EndInputAttribute();
    }
    for (int o = 0; o < t->outputs; ++o) {
        ImNodes::BeginOutputAttribute(outPin(n.id, o));
        ImGui::Indent(40.0f);
        ImGui::TextUnformatted(t->outputNames[o]);
        ImNodes::EndOutputAttribute();
    }
    ImGui::PopItemWidth();
    ImGui::PopID();
    ImNodes::EndNode();
    if (output) ImNodes::PopColorStyle();
    return changed;
}

bool VfxGraphEditor::draw(const char* title, const std::string& key, VfxGraph& g, bool* open) {
    if (!*open) return false;
    if (!m_ctx) init();
    bool changed = false;
    ImNodes::SetCurrentContext(m_ctx);
    if (key != m_key) {
        // Outro grafo: os nos dele entram nas posicoes salvas.
        m_key = key;
        m_placed.clear();
        m_status.clear();
    }
    ImGui::SetNextWindowSize(ImVec2(900, 560), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(title, open)) {
        ImGui::End();
        return false;
    }
    // Situacao da compilacao (o mesmo compilador do motor).
    const VfxGraphCompiled c = compileVfxGraph(g, 0, 0);
    if (!c.error.empty()) ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "%s", c.error.c_str());
    else
        ImGui::TextDisabled(tr("%zu instructions | spawn %u, force %u, color %u, size %u"), c.code.size(),
                            c.spawnVelocity.length, c.force.length, c.color.length, c.size.length);
    ImGui::SameLine();
    ImGui::TextDisabled("%s", tr("| right-click: add node, Delete: remove"));

    ImNodes::BeginNodeEditor();
    // Menu de nos (botao direito no fundo do editor).
    if (ImNodes::IsEditorHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##addnode");
    if (ImGui::BeginPopup("##addnode")) {
        const ImVec2 at = ImGui::GetMousePosOnOpeningCurrentPopup();
        for (const char* cat : kCategories) {
            if (!ImGui::BeginMenu(tr(cat))) continue;
            for (const auto& t : vfxGraphCatalog()) {
                if (std::string(t.category) != cat) continue;
                if (ImGui::MenuItem(tr(t.label))) {
                    VfxGraphNode n;
                    n.id = g.nextId++;
                    n.op = t.op;
                    if (n.op == "const_color") n.value = Vec4(1.0f);
                    // Saida e' unica: troca a antiga.
                    if (std::string(cat) == "Output")
                        for (auto it = g.nodes.begin(); it != g.nodes.end();)
                            it = (it->op == n.op) ? g.nodes.erase(it) : it + 1;
                    g.nodes.push_back(n);
                    ImNodes::SetNodeScreenSpacePos(n.id, at);
                    m_placed.insert(n.id);
                    changed = true;
                }
            }
            ImGui::EndMenu();
        }
        ImGui::EndPopup();
    }
    for (auto& n : g.nodes) {
        if (!m_placed.count(n.id)) {
            ImNodes::SetNodeGridSpacePos(n.id, ImVec2(n.pos.x, n.pos.y));
            m_placed.insert(n.id);
        }
        changed |= drawNode(n, g);
    }
    for (size_t i = 0; i < g.links.size(); ++i) {
        const auto& l = g.links[i];
        ImNodes::Link(int(i) + 1, outPin(l.fromNode, l.fromPin), inPin(l.toNode, l.toPin));
    }
    ImNodes::MiniMap(0.15f, ImNodesMiniMapLocation_BottomRight);
    ImNodes::EndNodeEditor();

    // Posicoes de volta para o grafo (vao para o arquivo).
    for (auto& n : g.nodes) {
        const ImVec2 p = ImNodes::GetNodeGridSpacePos(n.id);
        if (p.x != n.pos.x || p.y != n.pos.y) {
            n.pos = Vec2(p.x, p.y);
            changed = true;
        }
    }
    // Ligacao nova: uma entrada recebe um fio so'.
    int a = 0, b = 0;
    if (ImNodes::IsLinkCreated(&a, &b)) {
        if ((a & 15) < 8 && (b & 15) >= 8) std::swap(a, b); // de saida para entrada
        if ((a & 15) >= 8 && (b & 15) < 8 && (a >> 4) != (b >> 4)) {
            VfxGraphLink l{a >> 4, (a & 15) - 8, b >> 4, b & 15};
            g.links.erase(std::remove_if(g.links.begin(), g.links.end(),
                                         [&](const VfxGraphLink& x) { return x.toNode == l.toNode && x.toPin == l.toPin; }),
                          g.links.end());
            g.links.push_back(l);
            changed = true;
        }
    }
    int dead = 0;
    if (ImNodes::IsLinkDestroyed(&dead) && dead >= 1 && dead <= int(g.links.size())) {
        g.links.erase(g.links.begin() + (dead - 1));
        changed = true;
    }
    // Delete: apaga nos e fios selecionados.
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Delete)) {
        std::vector<int> nodes(size_t(std::max(0, ImNodes::NumSelectedNodes())));
        if (!nodes.empty()) ImNodes::GetSelectedNodes(nodes.data());
        std::vector<int> links(size_t(std::max(0, ImNodes::NumSelectedLinks())));
        if (!links.empty()) ImNodes::GetSelectedLinks(links.data());
        std::sort(links.rbegin(), links.rend());
        for (int id : links)
            if (id >= 1 && id <= int(g.links.size())) g.links.erase(g.links.begin() + (id - 1));
        for (int id : nodes) {
            g.nodes.erase(std::remove_if(g.nodes.begin(), g.nodes.end(), [&](const VfxGraphNode& n) { return n.id == id; }),
                          g.nodes.end());
            g.links.erase(std::remove_if(g.links.begin(), g.links.end(),
                                         [&](const VfxGraphLink& l) { return l.fromNode == id || l.toNode == id; }),
                          g.links.end());
        }
        if (!nodes.empty() || !links.empty()) {
            ImNodes::ClearNodeSelection();
            ImNodes::ClearLinkSelection();
            changed = true;
        }
    }
    ImGui::End();
    return changed;
}

} // namespace eruption
