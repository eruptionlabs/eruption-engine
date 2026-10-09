// Painel Cena: imagem do jogo, navegação da câmera de edição, seleção por
// clique, alças de transformação e o indicador de eixos.
#include "editor/Editor.hpp"
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
    const bool open = ImGui::Begin("Scene###Scene", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
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

    if (m_scripts) m_scripts->drawMessages(pos.x, pos.y, avail.x, avail.y);

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
        drawSelectionBounds(pos.x, pos.y, avail.x, avail.y);
        drawGizmo(pos.x, pos.y, avail.x, avail.y);
        drawViewCube(pos.x, pos.y, avail.x, avail.y);
        if (m_play == PlayState::Paused) {
            dl->AddRect(pos, ImVec2(pos.x + avail.x, pos.y + avail.y), IM_COL32(230, 190, 60, 255), 0.0f, 0, 2.0f);
            dl->AddText(ImVec2(pos.x + 10, pos.y + 8), IM_COL32(230, 190, 60, 255), "PAUSED");
        }

        // Clique simples (sem arrastar, fora das alças) seleciona.
        const bool gizmoBusy = ImGuizmo::IsUsing() || m_gizmoWasUsing;
        if (m_viewportHovered && !io.KeyAlt && !gizmoBusy && ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
            ImGui::GetMouseDragDelta(ImGuiMouseButton_Left).x * ImGui::GetMouseDragDelta(ImGuiMouseButton_Left).x +
                ImGui::GetMouseDragDelta(ImGuiMouseButton_Left).y * ImGui::GetMouseDragDelta(ImGuiMouseButton_Left).y < 16.0f &&
            !ImGuizmo::IsOver()) {
            pickAt((mouse.x - pos.x) / avail.x, (mouse.y - pos.y) / avail.y);
        }
    }

    if (m_engine->currentMapName().empty()) {
        const char* msg = "No map loaded";
        const ImVec2 ts = ImGui::CalcTextSize(msg);
        ImGui::SetCursorScreenPos(ImVec2(pos.x + (avail.x - ts.x) * 0.5f, pos.y + avail.y * 0.45f));
        ImGui::TextUnformatted(msg);
        const char* btn = "Open Map...  (Ctrl+O)";
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
        ImGui::TextColored(kAccent, "Getting started");
        ImGui::Separator();
        ImGui::BulletText("Hold the right mouse button to look; add WASD to fly.");
        ImGui::BulletText("Click an object to select it; drag the arrows to move.");
        ImGui::BulletText("W / E / R switch between move, rotate and scale.");
        ImGui::BulletText("F5 runs the game here; stopping restores the scene.");
        ImGui::BulletText("Lost? Ctrl+Shift+P searches every command.");
        ImGui::Spacing();
        if (ImGui::Button("Got it")) m_showWelcome = false;
        ImGui::SameLine();
        if (ImGui::Button("Show all shortcuts")) m_showShortcuts = true;
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
    if (m_selection.kind == SelectionKind::Model) {
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
    } else {
        return;
    }
    Camera& cam = m_engine->camera();
    const float fitDist = radius / std::sin(glm::radians(cam.fov()) * 0.5f);
    cam.setOrbitTarget(center);
    cam.setOrbit(cam.orbitYaw(), cam.orbitPitch(), std::max(2.0f, fitDist * 1.1f));
}

void Editor::pickAt(float u, float v) {
    const Camera& cam = m_engine->camera();
    const Mat4 inv = glm::inverse(cam.viewProjNoJitter());
    const Vec2 ndc(u * 2.0f - 1.0f, v * 2.0f - 1.0f);
    Vec4 pn = inv * Vec4(ndc, 0.0f, 1.0f);
    Vec4 pf = inv * Vec4(ndc, 1.0f, 1.0f);
    const Vec3 origin = Vec3(pn) / pn.w;
    const Vec3 dir = glm::normalize(Vec3(pf) / pf.w - origin);
    const Vec3 invDir(1.0f / (std::abs(dir.x) > 1e-8f ? dir.x : 1e-8f),
                      1.0f / (std::abs(dir.y) > 1e-8f ? dir.y : 1e-8f),
                      1.0f / (std::abs(dir.z) > 1e-8f ? dir.z : 1e-8f));

    Selection best;
    float bestT = FLT_MAX;
    const auto& lights = m_engine->m_deferredLighting.getPointLights();
    for (size_t i = 0; i < lights.size(); ++i) {
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
    const auto& insts = m_engine->modelRenderer().getInstances();
    for (size_t i = 0; i < insts.size(); ++i) {
        if (!insts[i].enabled) continue;
        const float t = rayAabb(origin, invDir, insts[i].worldAabbMin, insts[i].worldAabbMax);
        if (t >= 0.0f && t < bestT) {
            bestT = t;
            best = {SelectionKind::Model, static_cast<int>(i)};
        }
    }
    select(best);
}

void Editor::drawGizmo(float x, float y, float w, float h) {
    const bool usable = m_play == PlayState::Editing && m_tool != Tool::Select && selectionHasTransform();
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
    }
    if (using_) setSelectionTransform(model);
    if (!using_ && m_gizmoWasUsing) pushTransformEdit(m_gizmoSelection, m_gizmoBefore, selectionTransform());
    m_gizmoWasUsing = using_;
}

void Editor::drawSelectionBounds(float x, float y, float w, float h) {
    if (m_selection.kind != SelectionKind::Model || !selectionHasTransform()) return;
    const ModelInstance& inst = m_engine->modelRenderer().getInstances()[static_cast<size_t>(m_selection.index)];
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
    const ImU32 col = ImGui::GetColorU32(ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.9f));
    for (const auto& e : kEdges) dl->AddLine(p[e[0]], p[e[1]], col, 1.5f);
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
        ImGui::SetTooltip("Click an axis to look along it");
}

} // namespace eruption
