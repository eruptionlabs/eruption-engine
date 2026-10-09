// Janela da segunda tela: liga o servidor e mostra o endereço (com o token)
// para abrir no navegador de outro aparelho na mesma rede.
#include "editor/Editor.hpp"

#include "editor/EditorTheme.hpp"

#include <imgui.h>

namespace eruption {

void Editor::drawRemoteWindow() {
    if (!m_showRemote) return;
    ImGui::SetNextWindowSize(ImVec2(520, 300), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Second Screen###SecondScreen", &m_showRemote)) {
        ImGui::End();
        return;
    }
    ImGui::TextWrapped("Edit the game scripts (assets/scripts) from a phone, tablet or another computer on the "
                       "same network. Saving there updates the game here right away.");
    ImGui::Spacing();
    if (!m_remote.running()) {
        ImGui::SetNextItemWidth(120);
        ImGui::InputInt("Port", &m_remotePort);
        m_remotePort = std::clamp(m_remotePort, 1024, 65535);
        if (ImGui::Button("Start")) {
            if (m_remote.start(static_cast<uint16_t>(m_remotePort), m_projectRoot / "assets" / "scripts"))
                log(LogLevel::Info, "Second screen on port " + std::to_string(m_remotePort));
            else
                log(LogLevel::Error, m_remote.error);
        }
        if (!m_remote.error.empty()) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", m_remote.error.c_str());
    } else {
        ImGui::TextColored(kAccent, "Open one of these addresses on the other device:");
        const auto addrs = RemoteServer::localAddresses();
        if (addrs.empty()) ImGui::TextDisabled("No network found.");
        for (const auto& a : addrs) {
            const std::string url = "http://" + a + ":" + std::to_string(m_remote.port()) + "/?t=" + m_remote.token();
            ImGui::PushID(a.c_str());
            ImGui::TextUnformatted(url.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Copy")) ImGui::SetClipboardText(url.c_str());
            ImGui::PopID();
        }
        ImGui::Spacing();
        ImGui::TextDisabled("The address carries a key: only who has it can open the scripts.");
        ImGui::TextDisabled("The code editor in the page comes from the internet; offline it shows a plain text box.");
        if (ImGui::Button("Stop")) m_remote.stop();
    }
    ImGui::End();
}

} // namespace eruption
