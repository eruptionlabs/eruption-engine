// Painel Cena: imagem do jogo, navegação da câmera de edição, seleção por
// clique, alças de transformação e o indicador de eixos.
#include "editor/Editor.hpp"
#include "editor/Translate.hpp"
#include "editor/EditorTheme.hpp"

#include "core/Engine.hpp"
#include "script/ScriptHost.hpp"

#include <imgui.h>
#include <ImGuizmo.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace eruption {

namespace {

constexpr float kLookSensitivity = 0.0035f;
constexpr float kMaxPitch = 1.553f; // ~89 graus

// Direção do alvo para o olho na convenção orbital da Camera.
Vec3 orbitDir(float yaw, float pitch) {
    return Vec3(std::cos(pitch) * std::cos(yaw), std::sin(pitch), std::cos(pitch) * std::sin(yaw));
}

// Interseção raio x caixa (slab). Devolve a distância de entrada ou -1.
float rayAabb(const Vec3& o, const Vec3& invDir, const Vec3& bmin, const Vec3& bmax) {
    const Vec3 t0 = (bmin - o) * invDir;
    const Vec3 t1 = (bmax - o) * invDir;
    const Vec3 tmin = glm::min(t0, t1), tmax = glm::max(t0, t1);
    const float enter = std::max(std::max(tmin.x, tmin.y), tmin.z);
    const float exit = std::min(std::min(tmax.x, tmax.y), tmax.z);
    if (exit < 0.0f || enter > exit || enter < 0.0f) return -1.0f;
    return enter;
}

} // namespace

void Editor::drawViewport() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    const bool open = ImGui::Begin(trw("Scene###Scene"), nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    if (!open) {
        m_viewportHovered = m_viewportFocused = false;
        ImGui::End();
        return;
    }

    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const uint32_t w = static_cast<uint32_t>(std::max(0.0f, avail.x));
    const uint32_t h = static_cast<uint32_t>(std::max(0.0f, avail.y));
    if (w >= 64 && h >= 64 && (w != m_engine->m_viewportW || h != m_engine->m_viewportH)) {
        if (w != m_pendingW || h != m_pendingH) {
            m_pendingW = w;
            m_pendingH = h;
            m_resizeTimer = 0.0f;
        }
    }
    m_viewportRect[0] = pos.x;
    m_viewportRect[1] = pos.y;
    m_viewportRect[2] = avail.x;
    m_viewportRect[3] = avail.y;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (VkDescriptorSet tex = m_engine->viewportTexture()) {
        ImGui::Image(reinterpret_cast<ImTextureID>(tex), avail);
    } else {
        dl->AddRectFilled(pos, ImVec2(pos.x + avail.x, pos.y + avail.y), IM_COL32(16, 17, 18, 255));
        ImGui::Dummy(avail);
    }

    if (m_play != PlayState::Editing) m_engine->drawGameHudIn(ImGui::GetWindowDrawList(), pos, avail);
    m_engine->drawSpeechBubbles(pos.x, pos.y, avail.x, avail.y, ImGui::GetIO().DeltaTime);
    m_engine->drawGameFx2D(pos.x, pos.y, avail.x, avail.y, ImGui::GetIO().DeltaTime);
    if (m_scripts) m_scripts->drawMessages(pos.x, pos.y, avail.x, avail.y);

    // .glb solto do painel Projeto: entra no chão sob o mouse.
    if (m_play == PlayState::Editing && ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kModelPayload)) {
            const ImVec2 m = ImGui::GetIO().MousePos;
            Vec3 at;
            const std::string path = static_cast<const char*>(p->Data);
            if (groundPointAt((m.x - pos.x) / avail.x, (m.y - pos.y) / avail.y, at)) {
                if (std::filesystem::path(path).extension() == ".png") dropCharacter(path, at);
                else dropModel(path, at);
            }
        }
        ImGui::EndDragDropTarget();
    }

    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;
    const bool inside = mouse.x >= pos.x && mouse.y >= pos.y && mouse.x < pos.x + avail.x && mouse.y < pos.y + avail.y;
    m_viewportHovered = inside && ImGui::IsWindowHovered();
    m_viewportFocused = ImGui::IsWindowFocused();
    // Qualquer clique na cena dá foco a ela (teclado vai para a câmera/jogo).
    if (m_viewportHovered && (ImGui::IsMouseClicked(ImGuiMouseButton_Right) || ImGui::IsMouseClicked(ImGuiMouseButton_Middle)))
        ImGui::SetWindowFocus();

    if (m_play == PlayState::Playing) {
        // O jogo lê teclado e mouse direto; libera os dois enquanto a cena
        // estiver em foco ou sob o mouse.
        if (m_viewportFocused || m_viewportHovered) {
            ImGui::SetNextFrameWantCaptureKeyboard(false);
            ImGui::SetNextFrameWantCaptureMouse(false);
        }
        dl->AddRect(pos, ImVec2(pos.x + avail.x, pos.y + avail.y), ImGui::GetColorU32(kAccent), 0.0f, 0, 2.0f);
    } else {
        drawSceneMarkers(pos.x, pos.y, avail.x, avail.y);
        terrainBrushInput(pos.x, pos.y, avail.x, avail.y);
        drawWalkOverlay(pos.x, pos.y, avail.x, avail.y);
        drawTerrainBrush(pos.x, pos.y, avail.x, avail.y);
        drawSelectionBounds(pos.x, pos.y, avail.x, avail.y);
        drawGizmo(pos.x, pos.y, avail.x, avail.y);
        drawViewCube(pos.x, pos.y, avail.x, avail.y);
        if (m_play == PlayState::Paused) {
            dl->AddRect(pos, ImVec2(pos.x + avail.x, pos.y + avail.y), IM_COL32(230, 190, 60, 255), 0.0f, 0, 2.0f);
            dl->AddText(ImVec2(pos.x + 10, pos.y + 8), IM_COL32(230, 190, 60, 255), "PAUSED");
        }

        // Arrastar no vazio (fora das alças, sem Alt): retângulo de seleção.
        const bool gizmoBusy = ImGuizmo::IsUsing() || m_gizmoWasUsing;
        if (m_tool != Tool::Terrain && m_viewportHovered && !io.KeyAlt && !gizmoBusy && !ImGuizmo::IsOver() &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            m_boxStart = mouse;
            m_boxSelecting = true;
        }
        if (m_boxSelecting) {
            if (gizmoBusy) {
                m_boxSelecting = false;
            } else if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                const ImVec2 d(mouse.x - m_boxStart.x, mouse.y - m_boxStart.y);
                if (d.x * d.x + d.y * d.y >= 16.0f) {
                    dl->AddRectFilled(m_boxStart, mouse, ImGui::GetColorU32(ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.12f)));
                    dl->AddRect(m_boxStart, mouse, ImGui::GetColorU32(kAccent), 0.0f, 0, 1.5f);
                }
            } else {
                m_boxSelecting = false;
                const ImVec2 d(mouse.x - m_boxStart.x, mouse.y - m_boxStart.y);
                if (d.x * d.x + d.y * d.y >= 16.0f)
                    boxSelect(m_boxStart, mouse, pos.x, pos.y, avail.x, avail.y, io.KeyCtrl || io.KeyShift);
            }
        }

        // Clique simples (sem arrastar, fora das alças) seleciona.
        if (m_tool != Tool::Terrain && m_viewportHovered && !io.KeyAlt && !gizmoBusy &&
            ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
            ImGui::GetMouseDragDelta(ImGuiMouseButton_Left).x * ImGui::GetMouseDragDelta(ImGuiMouseButton_Left).x +
                ImGui::GetMouseDragDelta(ImGuiMouseButton_Left).y * ImGui::GetMouseDragDelta(ImGuiMouseButton_Left).y < 16.0f &&
            !ImGuizmo::IsOver()) {
            pickAt((mouse.x - pos.x) / avail.x, (mouse.y - pos.y) / avail.y);
        }
    }

    if (m_engine->currentMapName().empty()) {
        const char* msg = tr("No map loaded");
        const ImVec2 ts = ImGui::CalcTextSize(msg);
        ImGui::SetCursorScreenPos(ImVec2(pos.x + (avail.x - ts.x) * 0.5f, pos.y + avail.y * 0.45f));
        ImGui::TextUnformatted(msg);
        const char* btn = tr("Open Map...  (Ctrl+O)");
        const float bw = ImGui::CalcTextSize(btn).x + 20.0f;
        ImGui::SetCursorScreenPos(ImVec2(pos.x + (avail.x - bw) * 0.5f, ImGui::GetCursorScreenPos().y + 6));
        if (ImGui::Button(btn, ImVec2(bw, 0))) m_commands.run("file.open_map");
    }

    if (m_showWelcome && m_play == PlayState::Editing) drawWelcome();
    ImGui::End();
}

void Editor::drawWelcome() {
    const float x = m_viewportRect[0], y = m_viewportRect[1], h = m_viewportRect[3];
    const float cardW = 470.0f;
    ImGui::SetCursorScreenPos(ImVec2(x + 14, y + h - 214));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.08f, 0.085f, 0.09f, 0.92f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 10));
    if (ImGui::BeginChild("##welcome", ImVec2(cardW, 200), ImGuiChildFlags_Border | ImGuiChildFlags_AlwaysUseWindowPadding)) {
        ImGui::TextColored(kAccent, "%s", tr("Getting started"));
        ImGui::Separator();
        ImGui::BulletText("%s", tr("Hold the right mouse button to look; add WASD to fly."));
        ImGui::BulletText("%s", tr("Click an object to select it; drag the arrows to move."));
        ImGui::BulletText("%s", tr("W / E / R switch between move, rotate and scale."));
        ImGui::BulletText("%s", tr("F5 runs the game here; stopping restores the scene."));
        ImGui::BulletText("%s", tr("Lost? Ctrl+Shift+P searches every command."));
        ImGui::Spacing();
        if (ImGui::Button(tr("Got it"))) m_showWelcome = false;
        ImGui::SameLine();
        if (ImGui::Button(tr("Show all shortcuts"))) m_showShortcuts = true;
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

void Editor::updateCamera(float dt) {
    const ImGuiIO& io = ImGui::GetIO();
    Camera& cam = m_engine->camera();
    const ImVec2 md = io.MouseDelta;

    // Sinal do giro horizontal: mouse para a direita vira a vista para a
    // direita, qualquer que seja a orientação do eixo de mundo.
    const float yaw = cam.orbitYaw(), pitch = cam.orbitPitch();
    const Vec3 dForwardDYaw(std::cos(pitch) * std::sin(yaw), 0.0f, -std::cos(pitch) * std::cos(yaw));
    const float yawSign = glm::dot(dForwardDYaw, cam.right()) >= 0.0f ? 1.0f : -1.0f;

    if (m_viewportHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && !io.KeyAlt) m_flying = true;
    if (m_flying && !ImGui::IsMouseDown(ImGuiMouseButton_Right)) m_flying = false;

    if (m_flying) {
        // Gira em volta do olho: o olho fica parado e o alvo anda.
        const Vec3 eye = cam.position();
        const float dist = cam.orbitDistance();
        const float ny = yaw + yawSign * md.x * kLookSensitivity;
        const float np = std::clamp(pitch + md.y * kLookSensitivity, -kMaxPitch, kMaxPitch);
        cam.setOrbitTarget(eye - orbitDir(ny, np) * dist);
        cam.setOrbit(ny, np, dist);

        if (io.MouseWheel != 0.0f)
            m_flySpeed = std::clamp(m_flySpeed * std::pow(1.2f, io.MouseWheel), 1.0f, 2000.0f);
        Vec3 move(0.0f);
        const Vec3 fwd = cam.forward(), right = cam.right();
        if (ImGui::IsKeyDown(ImGuiKey_W)) move += fwd;
        if (ImGui::IsKeyDown(ImGuiKey_S)) move -= fwd;
        if (ImGui::IsKeyDown(ImGuiKey_D)) move += right;
        if (ImGui::IsKeyDown(ImGuiKey_A)) move -= right;
        if (ImGui::IsKeyDown(ImGuiKey_E)) move.y += 1.0f;
        if (ImGui::IsKeyDown(ImGuiKey_Q)) move.y -= 1.0f;
        if (glm::dot(move, move) > 0.0f) {
            const float speed = m_flySpeed * (io.KeyShift ? 3.0f : 1.0f);
            cam.moveTarget(glm::normalize(move) * speed * dt);
        }
        return;
    }

    // Arrastos que começam dentro da cena continuam mesmo saindo dela.
    static int dragButton = -1;
    if (m_viewportHovered && dragButton < 0) {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) dragButton = ImGuiMouseButton_Middle;
        else if (io.KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) dragButton = ImGuiMouseButton_Left;
        else if (io.KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) dragButton = ImGuiMouseButton_Right;
    }
    if (dragButton >= 0 && !ImGui::IsMouseDown(dragButton)) dragButton = -1;
    m_cameraDrag = dragButton >= 0;

    const float dist = cam.orbitDistance();
    if (dragButton == ImGuiMouseButton_Middle) {
        // Arrastar a cena: o ponto sob o mouse acompanha o cursor.
        const float viewH = std::max(1.0f, m_viewportRect[3]);
        const float unitsPerPixel = 2.0f * dist * std::tan(glm::radians(cam.fov()) * 0.5f) / viewH;
        cam.moveTarget((-cam.right() * md.x + cam.up() * md.y) * unitsPerPixel);
    } else if (dragButton == ImGuiMouseButton_Left) {
        cam.setOrbit(yaw - yawSign * md.x * kLookSensitivity,
                     std::clamp(pitch + md.y * kLookSensitivity, -kMaxPitch, kMaxPitch), dist);
    } else if (dragButton == ImGuiMouseButton_Right) {
        cam.setOrbit(yaw, pitch, std::max(0.5f, dist * std::pow(1.006f, md.x - md.y)));
    }

    if (m_viewportHovered && io.MouseWheel != 0.0f && !m_cameraDrag)
        cam.setOrbit(yaw, pitch, std::max(0.5f, dist * std::pow(0.88f, io.MouseWheel)));
}

void Editor::cameraLookDir(const Vec3& forward) {
    Camera& cam = m_engine->camera();
    const Vec3 f = glm::normalize(forward);
    const float pitch = std::clamp(std::asin(std::clamp(-f.y, -1.0f, 1.0f)), -kMaxPitch, kMaxPitch);
    const float yaw = (std::abs(f.x) + std::abs(f.z) > 1e-4f) ? std::atan2(-f.z, -f.x) : cam.orbitYaw();
    cam.setOrbit(yaw, pitch, cam.orbitDistance());
}

void Editor::frameSelection() {
    Vec3 center(0.0f);
    float radius = 1.0f;
    if (const auto list = selectionList(); list.size() > 1) {
        // Grupo: enquadra a caixa de todos.
        Vec3 lo(FLT_MAX), hi(-FLT_MAX);
        for (const auto& s : list) {
            if (s.kind == SelectionKind::Model) {
                const ModelInstance& inst = m_engine->modelRenderer().getInstances()[static_cast<size_t>(s.index)];
                lo = glm::min(lo, inst.worldAabbMin);
                hi = glm::max(hi, inst.worldAabbMax);
            } else {
                const Vec3 p(transformOf(s)[3]);
                lo = glm::min(lo, p - Vec3(5.0f));
                hi = glm::max(hi, p + Vec3(5.0f));
            }
        }
        center = (lo + hi) * 0.5f;
        radius = std::max(1.0f, glm::length(hi - lo) * 0.5f);
    } else if (m_selection.kind == SelectionKind::Model) {
        const auto& insts = m_engine->modelRenderer().getInstances();
        if (m_selection.index < 0 || m_selection.index >= static_cast<int>(insts.size())) return;
        const ModelInstance& inst = insts[static_cast<size_t>(m_selection.index)];
        center = (inst.worldAabbMin + inst.worldAabbMax) * 0.5f;
        radius = std::max(0.5f, glm::length(inst.worldAabbMax - inst.worldAabbMin) * 0.5f);
    } else if (m_selection.kind == SelectionKind::Light) {
        const auto& lights = m_engine->m_deferredLighting.getPointLights();
        if (m_selection.index < 0 || m_selection.index >= static_cast<int>(lights.size())) return;
        center = lights[static_cast<size_t>(m_selection.index)].position;
        radius = 4.0f;
    } else if (m_selection.kind == SelectionKind::Character && selectionHasTransform()) {
        center = m_engine->characters()[static_cast<size_t>(m_selection.index)].position + Vec3(0.0f, 12.0f, 0.0f);
        radius = 20.0f;
    } else {
        return;
    }
    Camera& cam = m_engine->camera();
    const float fitDist = radius / std::sin(glm::radians(cam.fov()) * 0.5f);
    cam.setOrbitTarget(center);
    cam.setOrbit(cam.orbitYaw(), cam.orbitPitch(), std::max(2.0f, fitDist * 1.1f));
}

void Editor::viewportRay(float u, float v, Vec3& origin, Vec3& dir) const {
    const Mat4 inv = glm::inverse(m_engine->camera().viewProjNoJitter());
    const Vec2 ndc(u * 2.0f - 1.0f, v * 2.0f - 1.0f);
    const Vec4 pn = inv * Vec4(ndc, 0.0f, 1.0f);
    const Vec4 pf = inv * Vec4(ndc, 1.0f, 1.0f);
    origin = Vec3(pn) / pn.w;
    dir = glm::normalize(Vec3(pf) / pf.w - origin);
}

Editor::ViewLayer Editor::layerOfInstance(uint32_t index) const {
    if (m_engine->isRegionInstance(index)) return LayerNeighbors;
    if (m_engine->isScatterInstance(index)) return LayerPainted;
    return index < m_original.count ? LayerMap : LayerAdded;
}

void Editor::applyLayerVisibility() {
    std::vector<uint32_t> show, hide;
    const auto& insts = m_engine->modelRenderer().getInstances();
    for (uint32_t i = 0; i < insts.size(); ++i) {
        // O chão fica sempre (sem ele não há onde clicar nem pintar), menos
        // isolando: aí só o que foi isolado aparece.
        const bool visible = !m_isolated.empty()
                                 ? std::binary_search(m_isolated.begin(), m_isolated.end(), i)
                                 : m_engine->modelRenderer().isGroundMesh(insts[i].meshIndex) || m_layerShow[layerOfInstance(i)];
        if (insts[i].viewHidden == visible) (visible ? show : hide).push_back(i);
    }
    if (!show.empty()) m_engine->modelRenderer().setViewHidden(show, false);
    if (!hide.empty()) m_engine->modelRenderer().setViewHidden(hide, true);
    m_engine->hideCharactersInView = !m_isolated.empty() || !m_layerShow[LayerCharacters];
    m_engine->hideMapEffectsInView = !m_isolated.empty();
}

void Editor::toggleIsolate() {
    if (!m_isolated.empty()) {
        m_isolated.clear();
        applyLayerVisibility();
        return;
    }
    std::vector<Selection> sel = m_extraSelection;
    sel.push_back(m_selection);
    for (const Selection& s : sel)
        if (s.kind == SelectionKind::Model && s.index >= 0) m_isolated.push_back(static_cast<uint32_t>(s.index));
    if (m_isolated.empty()) return;
    std::sort(m_isolated.begin(), m_isolated.end());
    m_isolated.erase(std::unique(m_isolated.begin(), m_isolated.end()), m_isolated.end());
    applyLayerVisibility();
    const auto& insts = m_engine->modelRenderer().getInstances();
    const Vec3 c = insts[m_isolated.front()].worldCenter;
    char msg[160];
    std::snprintf(msg, sizeof(msg), "Isolated %zu model(s) (%s at %.0f, %.0f, %.0f). Press / again to show everything.",
                  m_isolated.size(), insts[m_isolated.front()].name.c_str(), c.x, c.y, c.z);
    log(LogLevel::Info, msg);
    frameSelection();
}

int Editor::raycastModels(const Vec3& origin, const Vec3& dir, float& hitT, int ignore, bool skipScatter) const {
    const Vec3 invDir(1.0f / (std::abs(dir.x) > 1e-8f ? dir.x : 1e-8f),
                      1.0f / (std::abs(dir.y) > 1e-8f ? dir.y : 1e-8f),
                      1.0f / (std::abs(dir.z) > 1e-8f ? dir.z : 1e-8f));
    const ModelRenderer& mr = m_engine->modelRenderer();
    const auto& insts = mr.getInstances();
    // Caixas atingidas, da mais perto para a mais longe; os triângulos só são
    // testados enquanto a caixa começa antes do melhor acerto.
    std::vector<std::pair<float, uint32_t>> boxes;
    for (size_t i = 0; i < insts.size(); ++i) {
        if (!insts[i].enabled || insts[i].viewHidden || static_cast<int>(i) == ignore) continue;
        if (skipScatter && m_engine->isScatterInstance(static_cast<uint32_t>(i))) continue;
        const float t = rayAabb(origin, invDir, insts[i].worldAabbMin, insts[i].worldAabbMax);
        if (t >= 0.0f) boxes.push_back({t, static_cast<uint32_t>(i)});
    }
    std::sort(boxes.begin(), boxes.end());
    int best = -1;
    hitT = FLT_MAX;
    for (const auto& [boxT, i] : boxes) {
        if (boxT > hitT) break;
        const float t = mr.raycastInstance(i, origin, dir);
        if (t >= 0.0f && t < hitT) {
            hitT = t;
            best = static_cast<int>(i);
        }
    }
    return best;
}

bool Editor::floorBelow(const Vec3& from, int ignoreModel, float& floorY, bool skipScatter) const {
    const Vec3 down(0.0f, -1.0f, 0.0f);
    float t = FLT_MAX, tt = 0.0f;
    raycastModels(from, down, t, ignoreModel, skipScatter);
    if (m_engine->raycastTerrain(from, down, std::min(t, 50000.0f), tt)) t = std::min(t, tt);
    if (t == FLT_MAX) return false;
    floorY = from.y - t;
    return true;
}

void Editor::snapSelectionToFloor(bool pivot) {
    if (m_play != PlayState::Editing || !selectionHasTransform()) return;
    const Mat4 before = selectionTransform();
    Mat4 after = before;
    if (m_selection.kind == SelectionKind::Model) {
        const ModelInstance& inst = m_engine->modelRenderer().getInstances()[static_cast<size_t>(m_selection.index)];
        // Do alto do objeto para baixo, sem contar ele mesmo: acha o chão
        // mesmo que o objeto esteja afundado.
        const Vec3 c = (inst.worldAabbMin + inst.worldAabbMax) * 0.5f;
        float floorY = 0.0f;
        if (pivot) {
            // O ponto de origem do objeto no chão (árvores com tronco enterrado).
            const Vec3 p(before[3]);
            if (!floorBelow(Vec3(p.x, std::max(p.y, inst.worldAabbMax.y) + 0.01f, p.z), m_selection.index, floorY)) return;
            after[3].y = floorY;
        } else {
            if (!floorBelow(Vec3(c.x, inst.worldAabbMax.y + 0.01f, c.z), m_selection.index, floorY)) return;
            after[3].y += floorY - inst.worldAabbMin.y;
        }
    } else {
        // Personagem e luz: o ponto em si vai para o chão (a luz fica 2 acima).
        const Vec3 p(before[3]);
        float floorY = 0.0f;
        if (!floorBelow(p + Vec3(0.0f, 1000.0f, 0.0f), -1, floorY)) return;
        after[3].y = floorY + (m_selection.kind == SelectionKind::Light ? 2.0f : 0.0f);
    }
    setSelectionTransform(after);
    pushTransformEdit(m_selection, before, after);
}

bool Editor::groundPointAt(float u, float v, Vec3& out) const {
    Vec3 origin, dir;
    viewportRay(u, v, origin, dir);
    float t = FLT_MAX, tt = 0.0f;
    raycastModels(origin, dir, t);
    if (m_engine->raycastTerrain(origin, dir, std::min(t, 50000.0f), tt)) t = std::min(t, tt);
    if (t < FLT_MAX) {
        out = origin + dir * t;
        return true;
    }
    // Nada atingido: plano y = 0 à frente da câmera.
    if (dir.y < -1e-4f) {
        out = origin + dir * (-origin.y / dir.y);
        return true;
    }
    return false;
}

void Editor::pickAt(float u, float v) {
    Vec3 origin, dir;
    viewportRay(u, v, origin, dir);
    Selection best;
    float bestT = FLT_MAX;
    const int model = raycastModels(origin, dir, bestT);
    // Objetos espalhados pelo pincel não se selecionam um a um; camada travada também não.
    if (model >= 0 && !m_engine->isScatterInstance(static_cast<uint32_t>(model)) &&
        !m_layerLock[layerOfInstance(static_cast<uint32_t>(model))])
        best = {SelectionKind::Model, model};
    // Personagens: cilindro em pé do tamanho de uma pessoa.
    const auto& chars = m_engine->characters();
    const bool charsClickable = m_layerShow[LayerCharacters] && !m_layerLock[LayerCharacters];
    for (size_t i = 0; charsClickable && i < chars.size(); ++i) {
        if (chars[i].removed || chars[i].parked || (chars[i].fromNeighbor && m_layerLock[LayerNeighbors])) continue;
        const Vec3 base = chars[i].position;
        const float radius = 6.0f, height = 28.0f;
        // Ponto do raio mais perto do eixo vertical (no plano XZ).
        const Vec2 o2(origin.x - base.x, origin.z - base.z), d2(dir.x, dir.z);
        const float dd = glm::dot(d2, d2);
        if (dd < 1e-8f) continue;
        const float t = -glm::dot(o2, d2) / dd;
        if (t <= 0.0f || t >= bestT) continue;
        const Vec3 p = origin + dir * t;
        if (glm::length(Vec2(p.x - base.x, p.z - base.z)) > radius || p.y < base.y || p.y > base.y + height) continue;
        bestT = t;
        best = {SelectionKind::Character, static_cast<int>(i)};
    }
    const bool markersClickable = m_layerShow[LayerMarkers] && !m_layerLock[LayerMarkers];
    // Áreas: raio contra a caixa (no espaço dela, cubo de -0,5 a 0,5).
    const auto& areas = m_engine->areas();
    for (size_t i = 0; markersClickable && i < areas.size(); ++i) {
        const auto& a = areas[i];
        if (a.removed || a.parked || a.fromNeighbor) continue;
        const Mat4 inv = glm::inverse(a.matrix());
        const Vec3 o = Vec3(inv * Vec4(origin, 1.0f)), d = Vec3(inv * Vec4(dir, 0.0f));
        const Vec3 invD(1.0f / (std::abs(d.x) > 1e-8f ? d.x : 1e-8f), 1.0f / (std::abs(d.y) > 1e-8f ? d.y : 1e-8f),
                        1.0f / (std::abs(d.z) > 1e-8f ? d.z : 1e-8f));
        const float t = rayAabb(o, invD, Vec3(-0.5f), Vec3(0.5f));
        // Dentro da caixa (t = 0) não pega: clicar no chão dentro dela continua escolhendo o chão.
        if (t > 0.0f && t < bestT) {
            bestT = t;
            best = {SelectionKind::Area, static_cast<int>(i)};
        }
    }
    if (markersClickable && m_engine->hasPlayerStart()) {
        const Vec3 oc = m_engine->playerStart() - origin;
        const float t = glm::dot(oc, dir);
        const float d2 = glm::dot(oc, oc) - t * t;
        const float r = std::max(2.0f, 0.02f * t);
        if (t > 0.0f && t < bestT && d2 < r * r) {
            bestT = t;
            best = {SelectionKind::Spawn, 0};
        }
    }
    const auto& sounds = m_engine->soundEmitters();
    for (size_t i = 0; markersClickable && i < sounds.size(); ++i) {
        if (sounds[i].removed || sounds[i].fromNeighbor || sounds[i].parked) continue;
        const Vec3 oc = sounds[i].position - origin;
        const float t = glm::dot(oc, dir);
        if (t <= 0.0f) continue;
        const float d2 = glm::dot(oc, oc) - t * t;
        const float pickR = std::max(1.5f, 0.014f * t);
        if (d2 < pickR * pickR && t < bestT) {
            bestT = t;
            best = {SelectionKind::Sound, static_cast<int>(i)};
        }
    }
    const auto& lights = m_engine->m_deferredLighting.getPointLights();
    for (size_t i = 0; markersClickable && i < lights.size(); ++i) {
        const Vec3 oc = lights[i].position - origin;
        const float t = glm::dot(oc, dir);
        if (t <= 0.0f) continue;
        const float d2 = glm::dot(oc, oc) - t * t;
        const float pickR = std::max(1.0f, 0.012f * t);
        if (d2 < pickR * pickR && t < bestT) {
            bestT = t;
            best = {SelectionKind::Light, static_cast<int>(i)};
        }
    }
    // Ctrl/Shift: soma ou tira da seleção; clique no vazio com eles não limpa.
    if (ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeyShift) {
        if (best.kind != SelectionKind::None) toggleSelect(best);
    } else {
        select(best);
    }
}

void Editor::boxSelect(const ImVec2& a, const ImVec2& b, float x, float y, float w, float h, bool add) {
    const ImVec2 lo(std::min(a.x, b.x), std::min(a.y, b.y)), hi(std::max(a.x, b.x), std::max(a.y, b.y));
    const Mat4 vp = m_engine->camera().viewProjNoJitter();
    auto inside = [&](const Vec3& p) {
        const Vec4 c = vp * Vec4(p, 1.0f);
        if (c.w <= 1e-3f) return false;
        const float sx = x + (c.x / c.w * 0.5f + 0.5f) * w, sy = y + (c.y / c.w * 0.5f + 0.5f) * h;
        return sx >= lo.x && sx <= hi.x && sy >= lo.y && sy <= hi.y;
    };
    std::vector<Selection> hits;
    const ModelRenderer& mr = m_engine->modelRenderer();
    const auto& insts = mr.getInstances();
    for (uint32_t i = 0; i < insts.size(); ++i) {
        const ModelInstance& inst = insts[i];
        // Chão, espalhados (pincel), vizinhos e camadas travadas ficam de fora.
        if (!inst.enabled || inst.viewHidden || mr.isGroundMesh(inst.meshIndex) || m_engine->isScatterInstance(i) ||
            m_layerLock[layerOfInstance(i)])
            continue;
        if (inside((inst.worldAabbMin + inst.worldAabbMax) * 0.5f)) hits.push_back({SelectionKind::Model, static_cast<int>(i)});
    }
    const auto& chars = m_engine->characters();
    if (m_layerShow[LayerCharacters] && !m_layerLock[LayerCharacters])
        for (size_t i = 0; i < chars.size(); ++i)
            if (!chars[i].removed && !chars[i].fromNeighbor && !chars[i].parked && inside(chars[i].position))
                hits.push_back({SelectionKind::Character, static_cast<int>(i)});
    if (m_layerShow[LayerMarkers] && !m_layerLock[LayerMarkers]) {
        const auto& areas = m_engine->areas();
        for (size_t i = 0; i < areas.size(); ++i)
            if (!areas[i].removed && !areas[i].fromNeighbor && inside(areas[i].center))
                hits.push_back({SelectionKind::Area, static_cast<int>(i)});
        const auto& lights = m_engine->m_deferredLighting.getPointLights();
        for (size_t i = 0; i < lights.size(); ++i)
            if (lights[i].enabled && inside(lights[i].position)) hits.push_back({SelectionKind::Light, static_cast<int>(i)});
        if (m_engine->hasPlayerStart() && inside(m_engine->playerStart())) hits.push_back({SelectionKind::Spawn, 0});
        const auto& sounds = m_engine->soundEmitters();
        for (size_t i = 0; i < sounds.size(); ++i)
            if (!sounds[i].removed && !sounds[i].fromNeighbor && inside(sounds[i].position))
                hits.push_back({SelectionKind::Sound, static_cast<int>(i)});
    }
    if (!add) select({});
    for (const auto& s : hits)
        if (!isSelected(s)) toggleSelect(s);
    if (!hits.empty()) log(LogLevel::Info, std::to_string(selectionList().size()) + " selected");
}

void Editor::drawGizmo(float x, float y, float w, float h) {
    const bool usable = m_play == PlayState::Editing && m_tool != Tool::Select && m_tool != Tool::Terrain &&
                        selectionHasTransform();
    if (!usable) {
        if (m_gizmoWasUsing) pushTransformEdit(m_gizmoSelection, m_gizmoBefore, selectionTransform());
        m_gizmoWasUsing = false;
        return;
    }
    const Camera& cam = m_engine->camera();
    Mat4 model = selectionTransform();
    const Vec4 clip = cam.viewProjNoJitter() * Vec4(Vec3(model[3]), 1.0f);
    if (clip.w <= 1e-4f || std::abs(clip.x / clip.w) > 4.0f || std::abs(clip.y / clip.w) > 4.0f) return;

    const Mat4 view = cam.viewMatrix();
    Mat4 proj = cam.projNoJitter();
    proj[1][1] *= -1.0f; // a projeção do motor tem Y para baixo; as alças esperam Y para cima

    ImGuizmo::OPERATION op = ImGuizmo::TRANSLATE;
    float snap[3] = {m_snapMove, m_snapMove, m_snapMove};
    if (m_selection.kind == SelectionKind::Model) {
        if (m_tool == Tool::Rotate) { op = ImGuizmo::ROTATE; snap[0] = snap[1] = snap[2] = m_snapAngle; }
        if (m_tool == Tool::Scale) { op = ImGuizmo::SCALE; snap[0] = snap[1] = snap[2] = m_snapScale; }
    } else if (m_selection.kind == SelectionKind::Area) {
        // Área: gira só em Y; Scale muda o tamanho da caixa.
        if (m_tool == Tool::Rotate) { op = ImGuizmo::ROTATE_Y; snap[0] = snap[1] = snap[2] = m_snapAngle; }
        if (m_tool == Tool::Scale) { op = ImGuizmo::SCALE; snap[0] = snap[1] = snap[2] = m_snapScale; }
    }
    const bool snapping = m_snap || ImGui::GetIO().KeyCtrl;

    ImGuizmo::SetDrawlist();
    ImGuizmo::SetRect(x, y, w, h);
    ImGuizmo::SetGizmoSizeClipSpace(0.16f);
    ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(proj), op,
                         m_localSpace ? ImGuizmo::LOCAL : ImGuizmo::WORLD, glm::value_ptr(model), nullptr,
                         snapping ? snap : nullptr);
    const bool using_ = ImGuizmo::IsUsing();
    if (using_ && !m_gizmoWasUsing) {
        m_gizmoBefore = selectionTransform();
        m_gizmoSelection = m_selection;
        m_gizmoExtraBefore.clear();
        for (const auto& s : m_extraSelection) m_gizmoExtraBefore.push_back(hasTransformOf(s) ? transformOf(s) : Mat4(1.0f));
    }
    if (using_) {
        setSelectionTransform(model);
        // Os outros selecionados seguem a mesma mudança da principal.
        const Mat4 delta = model * glm::inverse(m_gizmoBefore);
        for (size_t i = 0; i < m_extraSelection.size() && i < m_gizmoExtraBefore.size(); ++i)
            if (hasTransformOf(m_extraSelection[i])) setTransformOf(m_extraSelection[i], delta * m_gizmoExtraBefore[i]);
    }
    if (!using_ && m_gizmoWasUsing) {
        m_undo.beginGroup("Transform " + std::to_string(1 + m_extraSelection.size()) + " objects");
        pushTransformEdit(m_gizmoSelection, m_gizmoBefore, selectionTransform());
        for (size_t i = 0; i < m_extraSelection.size() && i < m_gizmoExtraBefore.size(); ++i)
            if (hasTransformOf(m_extraSelection[i]))
                pushTransformEdit(m_extraSelection[i], m_gizmoExtraBefore[i], transformOf(m_extraSelection[i]));
        m_undo.endGroup();
    }
    m_gizmoWasUsing = using_;
}

void Editor::drawSceneMarkers(float x, float y, float w, float h) {
    if (!m_layerShow[LayerMarkers]) return;
    // Pontos sem corpo na cena (início do jogador, luzes) ganham um ícone
    // para dar para ver e clicar.
    const Mat4 vp = m_engine->camera().viewProjNoJitter();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto toScreen = [&](const Vec3& p, ImVec2& out) {
        const Vec4 c = vp * Vec4(p, 1.0f);
        if (c.w <= 1e-3f) return false;
        out = ImVec2(x + (c.x / c.w * 0.5f + 0.5f) * w, y + (c.y / c.w * 0.5f + 0.5f) * h);
        return out.x >= x && out.y >= y && out.x <= x + w && out.y <= y + h;
    };
    ImVec2 s;
    if (m_engine->hasPlayerStart() && toScreen(m_engine->playerStart(), s)) {
        const bool sel = m_selection.kind == SelectionKind::Spawn;
        const ImU32 col = sel ? ImGui::GetColorU32(kAccent) : IM_COL32(90, 200, 120, 230);
        dl->AddCircle(s, 9.0f, col, 20, 2.5f);
        dl->AddTriangleFilled(ImVec2(s.x - 5, s.y - 22), ImVec2(s.x + 5, s.y - 22), ImVec2(s.x, s.y - 11), col);
        dl->AddText(ImVec2(s.x + 12, s.y - 8), col, "Start");
    }
    // Áreas de gatilho: caixa em arame com o nome.
    const auto& areas = m_engine->areas();
    for (size_t i = 0; i < areas.size(); ++i) {
        const auto& a = areas[i];
        if (a.removed || a.parked || (a.fromNeighbor && !m_layerShow[LayerNeighbors])) continue;
        const bool sel = m_selection.kind == SelectionKind::Area && m_selection.index == static_cast<int>(i);
        const ImU32 col = sel ? ImGui::GetColorU32(kAccent) : (a.fromNeighbor ? IM_COL32(90, 170, 200, 120)
                                                                              : IM_COL32(90, 200, 230, 210));
        const Mat4 m = a.matrix();
        ImVec2 p[8];
        bool ok = true;
        for (int c = 0; c < 8 && ok; ++c) {
            const Vec3 corner((c & 1) ? 0.5f : -0.5f, (c & 2) ? 0.5f : -0.5f, (c & 4) ? 0.5f : -0.5f);
            const Vec4 cl = vp * (m * Vec4(corner, 1.0f));
            if (cl.w <= 1e-3f) ok = false;
            else p[c] = ImVec2(x + (cl.x / cl.w * 0.5f + 0.5f) * w, y + (cl.y / cl.w * 0.5f + 0.5f) * h);
        }
        if (!ok) continue;
        static const int kEdges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3},
                                          {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
        for (const auto& e : kEdges) dl->AddLine(p[e[0]], p[e[1]], col, sel ? 2.0f : 1.2f);
        if (toScreen(a.center + Vec3(0.0f, a.size.y * 0.5f, 0.0f), s)) dl->AddText(ImVec2(s.x + 4, s.y - 16), col, a.name.c_str());
    }
    // Emissores de som: nota musical e, selecionado, o círculo do alcance no chão.
    const auto& sounds = m_engine->soundEmitters();
    for (size_t i = 0; i < sounds.size(); ++i) {
        const auto& e = sounds[i];
        if (e.removed || e.parked || (e.fromNeighbor && !m_layerShow[LayerNeighbors])) continue;
        const bool sel = isSelected({SelectionKind::Sound, static_cast<int>(i)});
        const ImU32 col = sel ? ImGui::GetColorU32(kAccent) : IM_COL32(150, 200, 255, e.fromNeighbor ? 110 : 220);
        if (sel) {
            ImVec2 prev;
            bool hasPrev = false;
            for (int k = 0; k <= 40; ++k) {
                const float ang = float(k) / 40.0f * 6.2831853f;
                ImVec2 q;
                const bool on = toScreen(e.position + Vec3(std::cos(ang), 0.0f, std::sin(ang)) * e.range, q);
                if (on && hasPrev) dl->AddLine(prev, q, ImGui::GetColorU32(ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.5f)), 1.2f);
                prev = q;
                hasPrev = on;
            }
        }
        if (!toScreen(e.position, s)) continue;
        dl->AddCircle(s, 8.0f, col, 16, 2.0f);
        dl->AddLine(ImVec2(s.x + 1, s.y + 3), ImVec2(s.x + 1, s.y - 5), col, 2.0f);
        dl->AddCircleFilled(ImVec2(s.x - 2, s.y + 3), 2.5f, col);
        dl->AddText(ImVec2(s.x + 11, s.y - 8), col, e.name.c_str());
    }
    const auto& lights = m_engine->m_deferredLighting.getPointLights();
    for (size_t i = 0; i < lights.size(); ++i) {
        if (!toScreen(lights[i].position, s)) continue;
        const bool sel = m_selection.kind == SelectionKind::Light && m_selection.index == static_cast<int>(i);
        const Vec3 c = glm::clamp(lights[i].color, Vec3(0.0f), Vec3(1.0f));
        const ImU32 fill = IM_COL32(int(c.r * 255), int(c.g * 255), int(c.b * 255), lights[i].enabled ? 200 : 60);
        dl->AddCircleFilled(s, 5.0f, fill);
        dl->AddCircle(s, 7.0f, sel ? ImGui::GetColorU32(kAccent) : IM_COL32(20, 20, 20, 160), 12, sel ? 2.5f : 1.0f);
    }
}

void Editor::drawSelectionBounds(float x, float y, float w, float h) {
    for (const auto& sel : selectionList()) {
        if (sel.kind != SelectionKind::Model) continue;
        drawInstanceBounds(static_cast<uint32_t>(sel.index), x, y, w, h, sel == m_selection);
    }
}

void Editor::drawInstanceBounds(uint32_t index, float x, float y, float w, float h, bool primary) {
    const ModelInstance& inst = m_engine->modelRenderer().getInstances()[index];
    const Mat4 vp = m_engine->camera().viewProjNoJitter();
    ImVec2 p[8];
    for (int c = 0; c < 8; ++c) {
        const Vec3 corner((c & 1) ? inst.worldAabbMax.x : inst.worldAabbMin.x,
                          (c & 2) ? inst.worldAabbMax.y : inst.worldAabbMin.y,
                          (c & 4) ? inst.worldAabbMax.z : inst.worldAabbMin.z);
        const Vec4 cl = vp * Vec4(corner, 1.0f);
        if (cl.w <= 1e-3f) return; // caixa cruza o plano da câmera: não desenha
        p[c] = ImVec2(x + (cl.x / cl.w * 0.5f + 0.5f) * w, y + (cl.y / cl.w * 0.5f + 0.5f) * h);
    }
    static const int kEdges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3},
                                      {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 col = ImGui::GetColorU32(ImVec4(kAccent.x, kAccent.y, kAccent.z, primary ? 0.9f : 0.55f));
    for (const auto& e : kEdges) dl->AddLine(p[e[0]], p[e[1]], col, primary ? 1.5f : 1.0f);
}

void Editor::drawViewCube(float x, float y, float w, float /*h*/) {
    // Indicador de eixos no canto: clicar num eixo põe a câmera olhando ao
    // longo dele (vista de cima, de frente, de lado).
    const Camera& cam = m_engine->camera();
    const float r = 34.0f;
    const ImVec2 c(x + w - r - 16.0f, y + r + 16.0f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const bool nearWidget = (mouse.x - c.x) * (mouse.x - c.x) + (mouse.y - c.y) * (mouse.y - c.y) < (r + 12) * (r + 12);
    dl->AddCircleFilled(c, r + 10.0f, nearWidget ? IM_COL32(255, 255, 255, 30) : IM_COL32(0, 0, 0, 50));

    struct Axis { Vec3 dir; ImU32 col; const char* label; };
    const Axis axes[] = {
        {Vec3(1, 0, 0), IM_COL32(230, 70, 70, 255), "X"},
        {Vec3(0, 1, 0), IM_COL32(110, 200, 70, 255), "Y"},
        {Vec3(0, 0, 1), IM_COL32(70, 130, 235, 255), "Z"},
        {Vec3(-1, 0, 0), IM_COL32(150, 60, 60, 255), nullptr},
        {Vec3(0, -1, 0), IM_COL32(80, 130, 55, 255), nullptr},
        {Vec3(0, 0, -1), IM_COL32(55, 90, 160, 255), nullptr},
    };
    const Vec3 right = cam.right(), up = cam.up(), fwd = cam.forward();
    int order[6] = {0, 1, 2, 3, 4, 5};
    // Os eixos que apontam para longe da câmera são desenhados primeiro.
    std::sort(order, order + 6, [&](int a, int b) { return glm::dot(axes[a].dir, fwd) > glm::dot(axes[b].dir, fwd); });
    int clicked = -1;
    for (int k = 0; k < 6; ++k) {
        const Axis& a = axes[order[k]];
        const ImVec2 p(c.x + glm::dot(a.dir, right) * r, c.y - glm::dot(a.dir, up) * r);
        const float br = a.label ? 9.0f : 6.0f;
        if (a.label) dl->AddLine(c, p, a.col, 2.0f);
        const bool hov = (mouse.x - p.x) * (mouse.x - p.x) + (mouse.y - p.y) * (mouse.y - p.y) < br * br;
        if (a.label) dl->AddCircleFilled(p, br, hov ? IM_COL32(255, 255, 255, 255) : a.col);
        else dl->AddCircle(p, br, hov ? IM_COL32(255, 255, 255, 255) : a.col, 0, 2.0f);
        if (a.label) {
            const ImVec2 ts = ImGui::CalcTextSize(a.label);
            dl->AddText(ImVec2(p.x - ts.x * 0.5f, p.y - ts.y * 0.5f), IM_COL32(20, 20, 20, 255), a.label);
        }
        if (hov && m_viewportHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) clicked = order[k];
    }
    if (clicked >= 0) cameraLookDir(-axes[clicked].dir);
    if (nearWidget && m_viewportHovered)
        ImGui::SetTooltip("%s", tr("Click an axis to look along it"));
}

} // namespace eruption
