// Janela > Visualizador de modelo: um .glb (ou um nó dele) sozinho, com o
// desenho de verdade do motor (editor/ModelPreview).
#include "editor/Editor.hpp"
#include "editor/Translate.hpp"

#include "core/Engine.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstdio>

namespace eruption {

void Editor::initModelViewer() {
    Engine& e = *m_engine;
    if (!m_modelPreview.init(&e.m_vulkan, &e.m_bindless, &e.m_modelRenderer, e.m_spriteRenderer.frameUboLayout(),
                             e.m_spriteRenderer.frameUboSet()))
        return;
    e.offscreenHook = [this](VkCommandBuffer cmd) {
        if (m_showModelViewer) m_modelPreview.render(cmd, m_engine->timer().deltaTime());
    };
}

void Editor::shutdownModelViewer() {
    if (m_engine) m_engine->offscreenHook = nullptr;
    m_modelPreview.shutdown();
}

bool Editor::openModelViewer(const std::string& path, int node) {
    if (!m_engine->loadModelForPreview(path) || !m_modelPreview.open(path, node)) {
        log(LogLevel::Error, "Model Viewer: could not open " + path);
        return false;
    }
    m_showModelViewer = true;
    m_pendingFocus.push_back("###ModelViewer");
    return true;
}

void Editor::drawModelViewer() {
    if (!m_showModelViewer) return;
    if (m_viewerFullscreen) {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(vp->WorkSize, ImGuiCond_Always);
        ImGui::SetNextWindowFocus();
    } else {
        ImGui::SetNextWindowSize(ImVec2(900, 560), ImGuiCond_FirstUseEver);
    }
    if (!ImGui::Begin(trw("Model Viewer###ModelViewer"), &m_showModelViewer)) {
        ImGui::End();
        return;
    }
    ModelPreview& pv = m_modelPreview;
    const float side = 330.0f;
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float imgW = std::max(64.0f, avail.x - side - 8.0f), imgH = std::max(64.0f, avail.y);

    // Imagem (o tamanho vale já: os alvos são refeitos antes de a interface usá-los).
    pv.setSize(static_cast<uint32_t>(imgW), static_cast<uint32_t>(imgH));
    ImGui::BeginChild("##view", ImVec2(imgW, imgH), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoMove);
    const ImVec2 imgPos = ImGui::GetCursorPos();
    if (pv.texture()) ImGui::Image(reinterpret_cast<ImTextureID>(pv.texture()), ImVec2(imgW, imgH));
    // Área que segura o mouse por cima da imagem: sem ela o arrasto ia para a
    // janela (que se movia junto) em vez da câmera.
    ImGui::SetCursorPos(imgPos);
    ImGui::InvisibleButton("##viewinput", ImVec2(imgW, imgH),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);
    if (ImGui::IsItemActive() || ImGui::IsItemHovered()) {
        const ImGuiIO& io = ImGui::GetIO();
        if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) pv.orbit(io.MouseDelta.x, io.MouseDelta.y);
        if (ImGui::IsItemActive() && (ImGui::IsMouseDown(ImGuiMouseButton_Right) || ImGui::IsMouseDown(ImGuiMouseButton_Middle)))
            pv.pan(-io.MouseDelta.x, io.MouseDelta.y);
        if (ImGui::IsItemHovered() && io.MouseWheel != 0.0f) pv.zoom(io.MouseWheel);
        if (ImGui::IsItemHovered() &&
            (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) || ImGui::IsKeyPressed(ImGuiKey_F)))
            pv.frame();
    }
    if (!pv.hasTarget()) {
        ImGui::SetCursorPos(ImVec2(16, 16));
        ImGui::PushTextWrapPos(imgW - 16);
        ImGui::TextDisabled("%s", tr("Double-click a .glb in the Project panel, or select a model and use "
                                     "\"Open in Model Viewer\" in the Inspector."));
        ImGui::PopTextWrapPos();
    } else {
        ImGui::SetCursorPos(ImVec2(10, imgH - ImGui::GetTextLineHeightWithSpacing() - 6));
        ImGui::TextDisabled("%s", tr("Left drag: orbit   Right drag: pan   Wheel: zoom   F / double-click: frame"));
    }
    ImGui::EndChild();
    ImGui::SameLine();

    ImGui::BeginChild("##side", ImVec2(side, imgH));
    if (pv.hasTarget()) {
        const ModelPreview::Stats st = pv.stats();
        ImGui::TextWrapped("%s", pv.assetPath().c_str());
        if (pv.node() >= 0) {
            const ModelAsset* a = m_engine->modelRenderer().asset(pv.assetPath());
            if (a && pv.node() < static_cast<int>(a->nodes.size()))
                ImGui::TextDisabled("%s %s", tr("Node:"), a->nodes[static_cast<size_t>(pv.node())].name.c_str());
        }
        ImGui::SeparatorText(tr("View"));
        int mode = static_cast<int>(pv.mode);
        ImGui::RadioButton(tr("Lit"), &mode, 0);
        ImGui::SameLine();
        ImGui::RadioButton(tr("Color"), &mode, 1);
        ImGui::SameLine();
        ImGui::RadioButton(tr("Normals"), &mode, 2);
        ImGui::SameLine();
        ImGui::RadioButton("UV", &mode, 3);
        pv.mode = static_cast<ModelPreview::Mode>(mode);
        ImGui::BeginDisabled(!pv.wireframeSupported());
        ImGui::Checkbox(tr("Wireframe"), &pv.wireframe);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::Checkbox(tr("Spin"), &pv.spin);
        ImGui::SameLine();
        ImGui::Checkbox(tr("Alpha cut"), &pv.alphaCut);
        ImGui::SetItemTooltip("%s", tr("Leaves and cards: pixels with little alpha are cut, like in the game."));

        ImGui::SeparatorText(tr("Level of detail (LOD)"));
        int lod = static_cast<int>(pv.lod);
        char label[96];
        auto lodRow = [&](int value, const char* name, uint64_t tris) {
            std::snprintf(label, sizeof(label), "%s##lod%d", name, value);
            ImGui::RadioButton(label, &lod, value);
            ImGui::SameLine(150);
            ImGui::TextDisabled(tr("%llu triangles"), static_cast<unsigned long long>(tris));
        };
        lodRow(0, tr("Original"), st.trisOriginal);
        lodRow(1, tr("Near (lo)"), st.trisNear);
        lodRow(2, tr("Far"), st.trisFar);
        lodRow(3, tr("Shadow"), st.trisShadow);
        pv.lod = static_cast<ModelPreview::Lod>(lod);
        if (st.switchNear > 0.0f) {
            char buf[160];
            std::snprintf(buf, sizeof(buf), tr("In the game: near level from %.0f u, far from %.0f u."), st.switchNear,
                          st.switchFar > 0.0f ? st.switchFar : st.switchNear);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextWrapped("%s", buf);
            ImGui::PopStyleColor();
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextWrapped("%s", tr("No simplified levels: always the original mesh."));
            ImGui::PopStyleColor();
        }
        ImGui::TextDisabled(tr("Camera distance: %.0f u"), pv.cameraDistance());

        ImGui::SeparatorText(tr("Light"));
        ImGui::SliderFloat(tr("Direction"), &pv.lightYaw, -180.0f, 180.0f, "%.0f");
        ImGui::SliderFloat(tr("Height"), &pv.lightPitch, 5.0f, 89.0f, "%.0f");

        const auto anims = pv.animations();
        if (!anims.empty()) {
            ImGui::SeparatorText(tr("Animation"));
            const char* cur = pv.animation >= 0 && pv.animation < static_cast<int>(anims.size())
                                  ? anims[static_cast<size_t>(pv.animation)].c_str()
                                  : tr("(rest pose)");
            if (ImGui::BeginCombo("##anim", cur)) {
                if (ImGui::Selectable(tr("(rest pose)"), pv.animation < 0)) pv.animation = -1;
                for (int k = 0; k < static_cast<int>(anims.size()); ++k)
                    if (ImGui::Selectable(anims[static_cast<size_t>(k)].c_str(), pv.animation == k)) pv.animation = k;
                ImGui::EndCombo();
            }
            ImGui::SliderFloat(tr("Speed"), &pv.animationSpeed, 0.0f, 3.0f, "%.2fx");
        }

        ImGui::SeparatorText(tr("Model"));
        ImGui::Text(tr("Nodes: %zu"), st.nodes);
        ImGui::Text(tr("Size: %.1f x %.1f x %.1f"), st.size.x, st.size.y, st.size.z);
        if (st.foliage) ImGui::BulletText("%s", tr("Foliage (sways in the wind, card LOD)"));
        if (st.ground) ImGui::BulletText("%s", tr("Ground"));
        if (st.skinned) ImGui::BulletText("%s", tr("Skeleton (animated on the GPU)"));
        ImGui::BulletText("%s", st.shadowOpaque ? tr("Solid shadow caster") : tr("Shadow with alpha test"));
        if (ImGui::Button(tr("Frame (F)"))) pv.frame();
    }
    ImGui::EndChild();
    ImGui::End();
}

} // namespace eruption
