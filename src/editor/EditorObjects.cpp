// Inspetor > Object: o que a cena declara sobre o objeto selecionado (modelo
// ou personagem) - pai, etiquetas e scripts de objeto. Fica no arquivo de
// cena (objects["nome"] = { parent = ..., tags = {...}, scripts = {...} }).
#include "editor/Editor.hpp"
#include "editor/Translate.hpp"

#include "core/Engine.hpp"
#include "editor/EditorTheme.hpp"

#include <imgui.h>

#include <cstdio>
#include <cstring>
#include <sstream>

namespace eruption {

namespace {

// "a, b ,c" -> {"a", "b", "c"} (vazios saem).
std::vector<std::string> splitList(const std::string& text) {
    std::vector<std::string> out;
    std::stringstream ss(text);
    std::string item;
    while (std::getline(ss, item, ',')) {
        const size_t a = item.find_first_not_of(" \t");
        const size_t b = item.find_last_not_of(" \t");
        if (a != std::string::npos) out.push_back(item.substr(a, b - a + 1));
    }
    return out;
}

std::string joinList(const std::vector<std::string>& items) {
    std::string out;
    for (const auto& s : items) out += (out.empty() ? "" : ", ") + s;
    return out;
}

SceneObjects::Authored authoredFor(const SceneObjects& so, const std::string& name) {
    for (const auto& a : so.authored())
        if (a.name == name) return a;
    SceneObjects::Authored a;
    a.name = name;
    return a;
}

// Linha de propriedade: rótulo à esquerda, campo ocupando o resto (como no
// resto do inspetor).
void propertyLabel(const char* label) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(tr(label));
    ImGui::SameLine(96.0f);
    ImGui::SetNextItemWidth(-28.0f);
}

// Campo de texto que só vale com Enter (o arquivo de cena muda uma vez).
bool listField(const char* id, const std::string& current, std::string& out) {
    char buf[512];
    std::snprintf(buf, sizeof(buf), "%s", current.c_str());
    if (!ImGui::InputText(id, buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue)) return false;
    out = buf;
    return out != current;
}

} // namespace

void Editor::declareObject(const SceneObjects::Authored& a) {
    // No Play a cena não se mexe (desfazer no meio do jogo arrancaria o que o
    // jogo fez); vale de novo depois do Stop.
    if (m_play != PlayState::Editing) {
        log(LogLevel::Warning, "Object changes are undone and redone with the game stopped.");
        return;
    }
    m_engine->sceneObjects().author(*m_engine, a);
    m_engine->sceneObjects().restoreAuthored(*m_engine);
}

void Editor::authorObject(const SceneObjects::Authored& before, const SceneObjects::Authored& after) {
    declareObject(after);
    m_undo.push({"Object " + after.name, [this, before] { declareObject(before); }, [this, after] { declareObject(after); }});
}

void Editor::drawObjectInspector() {
    SceneObjects& so = m_engine->sceneObjects();
    so.refresh(*m_engine);
    const ObjectId id = objectOfSelection(m_selection);
    if (!so.world().alive(id) || !ImGui::CollapsingHeader(tr("Object"), ImGuiTreeNodeFlags_DefaultOpen)) return;

    const std::string name = so.sceneName(id);
    const SceneObjects::Authored before = authoredFor(so, name);
    SceneObjects::Authored after = before;
    bool changed = false;
    // No Play o que vale é o jogo; a cena se edita parado.
    ImGui::BeginDisabled(m_play != PlayState::Editing);

    propertyLabel("Path");
    ImGui::TextWrapped("%s", so.pathOf(id).c_str());
    ImGui::SetItemTooltip("%s", tr("Scripts find it with objects.find(\"this/path\")."));

    propertyLabel("Parent");
    std::string text;
    if (listField("##parent", before.parent, text)) {
        // Só um objeto que existe e que não esteja dentro deste.
        const ObjectId parent = text.empty() ? ObjectId{} : so.findSceneName(text);
        if (!text.empty() && (!parent.valid() || parent == id || so.world().isAncestor(id, parent))) {
            log(LogLevel::Warning, "There is no object \"" + text + "\" that can hold this one.");
        } else {
            after.parent = text;
            changed = true;
        }
    }
    ImGui::SetItemTooltip("%s", tr("Name of another object (or a path like house/door). It carries this one when it "
                                   "moves. Empty = no parent. Press Enter."));

    propertyLabel("Tags");
    if (listField("##tags", joinList(before.tags), text)) {
        after.tags = splitList(text);
        changed = true;
    }
    ImGui::SetItemTooltip("%s", tr("Free words separated by commas; scripts ask obj:has_tag(\"...\") and "
                                   "objects.with_tag(\"...\"). Press Enter."));

    propertyLabel("Scripts");
    if (listField("##scripts", joinList(before.scripts), text)) {
        after.scripts = splitList(text);
        changed = true;
    }
    ImGui::SetItemTooltip("%s", tr("Scripts of this object, separated by commas: assets/scripts/<name>.luau returning "
                                   "{ start = function(self) end, update = function(self, dt) end }. They start with Play. "
                                   "Press Enter."));

    ImGui::EndDisabled();
    if (changed && m_play == PlayState::Editing) authorObject(before, after);
}

// ---------------------------------------------------------------- hierarquia

ObjectId Editor::objectOfSelection(const Selection& s) {
    SceneObjects& so = m_engine->sceneObjects();
    const auto index = static_cast<size_t>(s.index);
    switch (s.kind) {
        case SelectionKind::Model: return so.objectOfInstance(static_cast<uint32_t>(s.index));
        case SelectionKind::Character: return so.objectOfCharacter(index);
        case SelectionKind::Light: return so.objectOfPoint(SceneObjects::PointKind::Light, index);
        case SelectionKind::Area: return so.objectOfPoint(SceneObjects::PointKind::Area, index);
        case SelectionKind::Sound: return so.objectOfPoint(SceneObjects::PointKind::Sound, index);
        default: return {};
    }
}

Editor::Selection Editor::selectionOfObject(ObjectId id) {
    SceneObjects& so = m_engine->sceneObjects();
    const auto insts = so.instancesOf(id);
    if (!insts.empty()) return {SelectionKind::Model, static_cast<int>(insts.front())};
    const int c = so.characterOf(id);
    if (c >= 0) return {SelectionKind::Character, c};
    SceneObjects::PointKind kind;
    size_t index = 0;
    if (!so.pointOf(id, kind, index)) return {};
    const int i = static_cast<int>(index);
    if (kind == SceneObjects::PointKind::Light) return {SelectionKind::Light, i};
    if (kind == SceneObjects::PointKind::Area) return {SelectionKind::Area, i};
    return {SelectionKind::Sound, i};
}

// Arrastar um objeto (linha da hierarquia) para cima de outro: vira filho dele.
void Editor::objectDragSource(ObjectId id) {
    if (m_play != PlayState::Editing || !m_engine->sceneObjects().world().alive(id) || !ImGui::BeginDragDropSource())
        return;
    ImGui::SetDragDropPayload("ERUPTION_OBJECT", &id, sizeof(id));
    ImGui::TextUnformatted(m_engine->sceneObjects().world().name(id).c_str());
    ImGui::EndDragDropSource();
}

// `parent` inválido = soltar para tirar o pai (só o título da seção usa).
void Editor::objectDropTarget(ObjectId parent) {
    if (m_play != PlayState::Editing || !ImGui::BeginDragDropTarget()) return;
    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ERUPTION_OBJECT")) {
        // Aplicado depois do desenho da hierarquia: trocar o pai no meio dela
        // desenharia o objeto em dois lugares neste quadro.
        std::memcpy(&m_dropChild, p->Data, sizeof(m_dropChild));
        m_dropParent = parent;
        m_dropPending = true;
    }
    ImGui::EndDragDropTarget();
}

void Editor::applyPendingDrop() {
    if (!m_dropPending) return;
    m_dropPending = false;
    reparentObject(m_dropChild, m_dropParent);
}

void Editor::reparentObject(ObjectId child, ObjectId parent) {
    SceneObjects& so = m_engine->sceneObjects();
    ObjectWorld& w = so.world();
    if (!w.alive(child) || child == parent || (parent.valid() && w.isAncestor(child, parent))) {
        log(LogLevel::Warning, "An object cannot go inside itself.");
        return;
    }
    const std::string name = so.sceneName(child);
    const SceneObjects::Authored before = authoredFor(so, name);
    SceneObjects::Authored after = before;
    after.parent = parent.valid() ? so.sceneName(parent) : std::string();
    if (after.parent != before.parent) authorObject(before, after);
}

void Editor::drawObjectNode(ObjectId id) {
    SceneObjects& so = m_engine->sceneObjects();
    ObjectWorld& w = so.world();
    const Selection s = selectionOfObject(id);
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_OpenOnArrow;
    if (!w.firstChild(id).valid()) flags |= ImGuiTreeNodeFlags_Leaf;
    if (s.kind != SelectionKind::None && isSelected(s)) flags |= ImGuiTreeNodeFlags_Selected;
    ImGui::PushID(static_cast<int>(id.index));
    const bool open = ImGui::TreeNodeEx(w.name(id).c_str(), flags);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen() && s.kind != SelectionKind::None) select(s);
    objectDragSource(id);
    objectDropTarget(id);
    if (open) {
        for (const ObjectId& c : w.children(id)) drawObjectNode(c);
        ImGui::TreePop();
    }
    ImGui::PopID();
}

// Só as famílias (objetos com filhos): os 8 mil modelos soltos já estão em Modelos.
void Editor::drawObjectTree() {
    SceneObjects& so = m_engine->sceneObjects();
    so.refresh(*m_engine);
    ObjectWorld& w = so.world();
    std::vector<ObjectId> roots;
    for (const ObjectId& id : w.all())
        if (!w.parent(id).valid() && w.firstChild(id).valid()) roots.push_back(id);
    char hdr[96];
    std::snprintf(hdr, sizeof(hdr), "%s (%zu)###objtree", tr("Object hierarchy"), roots.size());
    ImGui::SetNextItemOpen(true, ImGuiCond_FirstUseEver);
    const bool open = ImGui::TreeNodeEx(hdr, ImGuiTreeNodeFlags_SpanAvailWidth);
    ImGui::SetItemTooltip("%s", tr("Drag a model or character onto another to put it inside; drop it here to take it out."));
    objectDropTarget({}); // soltar no título: sem pai
    if (!open) return;
    if (roots.empty()) emptyHint();
    for (const ObjectId& id : roots) drawObjectNode(id);
    ImGui::TreePop();
}

void Editor::emptyHint() {
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", tr("No object has children yet. Drag a model onto another one."));
    ImGui::PopTextWrapPos();
}

} // namespace eruption
