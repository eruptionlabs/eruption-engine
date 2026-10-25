// Caminho do navegador (spec 10, base WebGL2). O init roda inteiro sobre o
// dispositivo Vulkan nulo (platform/web/VulkanNull.cpp), entao mapa, scripts
// e objetos sobem como no desktop; o quadro sai daqui, em GLES 3.0 (WebGL2).
// Os renderers de cena entram um a um pela RHI (src/rhi/); ate' la' o quadro
// e' a interface sobre um fundo liso.
#if defined(__EMSCRIPTEN__)

#include "core/Engine.hpp"
#include "core/Input.hpp"
#include "core/Logger.hpp"

#include <GLES3/gl3.h>
#include <GLFW/glfw3.h>
#include <emscripten.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

namespace eruption {

void Engine::initImGuiWebBackend() {
    ERUPTION_LOG_WARN("[WEB] %s | %s", reinterpret_cast<const char*>(glGetString(GL_VERSION)),
                      reinterpret_cast<const char*>(glGetString(GL_RENDERER)));
    ImGui_ImplGlfw_InitForOpenGL(m_window.handle(), true);
    ImGui_ImplOpenGL3_Init("#version 300 es");
}

void Engine::renderWeb() {
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    renderImGui();
    renderLoadingProgress();
    ImGui::Render();

    int w = 0, h = 0;
    glfwGetFramebufferSize(m_window.handle(), &w, &h);
    glViewport(0, 0, w, h);
    glClearColor(0.07f, 0.06f, 0.06f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    // Contrato com a casca: onReady no primeiro quadro desenhado, quando ela
    // tira o painel de carga.
    if (!m_webReady) {
        m_webReady = true;
        EM_ASM({ if (Module.onReady) Module.onReady(); }, 0);
    }
}

} // namespace eruption

#endif
