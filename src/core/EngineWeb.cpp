// Caminho do navegador (spec 10, base WebGL2). O init roda inteiro sobre o
// dispositivo Vulkan nulo (platform/web/VulkanNull.cpp), entao mapa, scripts
// e objetos sobem como no desktop; o quadro sai daqui, em GLES 3.0 (WebGL2).
// Os renderers de cena entram um a um pela RHI (src/rhi/); ate' la' o quadro
// e' a interface sobre um fundo liso.
#if defined(__EMSCRIPTEN__)

#include "core/Engine.hpp"
#include "core/Input.hpp"
#include "core/Logger.hpp"
#include "core/TouchRouter.hpp"
#include "utils/ImageUtils.hpp"

#include <GLES3/gl3.h>
#include <GLFW/glfw3.h>
#include <emscripten.h>
#include <emscripten/html5.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <stb_image.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// A pagina de teste recebe o quadro da captura (ver ERUPTION_TEST_SCREENSHOT_FRAME).
EM_JS(void, webTestFrame, (int frame), { if (Module.onTestFrame) Module.onTestFrame(frame); });

// O GLFW do Emscripten converte so' o primeiro dedo em botao esquerdo: tira
// esses ouvintes do canvas para o toque passar pelo TouchRouter, como no celular.
EM_JS(void, webDropGlfwTouch, (), {
    const c = Module.canvas;
    if (!c || typeof GLFW === "undefined") return;
    c.removeEventListener("touchmove", GLFW.onMousemove, true);
    c.removeEventListener("touchstart", GLFW.onMouseButtonDown, true);
    c.removeEventListener("touchcancel", GLFW.onMouseButtonUp, true);
    c.removeEventListener("touchend", GLFW.onMouseButtonUp, true);
});

// Seletor CSS do canvas do motor (Module.canvas), para as funcoes html5.h; da'
// um id ao canvas se a pagina nao deu.
EM_JS(char*, webCanvasSelector, (), {
    const c = Module.canvas;
    if (!c) return 0;
    if (!c.id) c.id = "eruption-canvas";
    return stringToNewUTF8("#" + c.id);
});

namespace eruption {

namespace {

// Backbuffer (de baixo para cima no GL) em PNG, de cima para baixo.
void writeWebScreenshot(const std::string& path, int w, int h) {
    if (w <= 0 || h <= 0) return;
    std::vector<uint8_t> px(size_t(w) * size_t(h) * 4), flipped(px.size());
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    const size_t row = size_t(w) * 4;
    for (int y = 0; y < h; ++y) std::memcpy(&flipped[size_t(y) * row], &px[size_t(h - 1 - y) * row], row);
    if (ImageUtils::writePNG(path, w, h, 4, flipped.data())) ERUPTION_LOG_WARN("[TEST] captura gravada em %s", path.c_str());
    else ERUPTION_LOG_ERROR("[TEST] nao deu para gravar a captura em %s", path.c_str());
}

std::string s_canvas = "#canvas";

// Os callbacks que o motor registrou na janela (Input): o toque chega neles
// como o mouse chegaria.
GLFWwindow* s_touchWindow = nullptr;
GLFWcursorposfun s_cursorCb = nullptr;
GLFWmousebuttonfun s_buttonCb = nullptr;
GLFWscrollfun s_scrollCb = nullptr;

TouchRouter& webTouchRouter() {
    static TouchRouter router({
        [](double x, double y) { if (s_cursorCb) s_cursorCb(s_touchWindow, x, y); },
        [](int b, int action) { if (s_buttonCb) s_buttonCb(s_touchWindow, b, action, 0); },
        [](double dy) { if (s_scrollCb) s_scrollCb(s_touchWindow, 0.0, dy); },
        [](float x, float y) { ImGui_ImplGlfw_CursorPosCallback(s_touchWindow, x, y); },
        [](int action) { ImGui_ImplGlfw_MouseButtonCallback(s_touchWindow, GLFW_MOUSE_BUTTON_LEFT, action, 0); },
        [] {
            int w = 0, h = 0;
            emscripten_get_canvas_element_size(s_canvas.c_str(), &w, &h);
            return h;
        },
    });
    return router;
}

// Toque em pixels CSS -> pixels do canvas (a janela que o motor ve').
EM_BOOL onWebTouch(int type, const EmscriptenTouchEvent* e, void*) {
    int w = 0, h = 0;
    double cw = 0.0, ch = 0.0;
    emscripten_get_canvas_element_size(s_canvas.c_str(), &w, &h);
    emscripten_get_element_css_size(s_canvas.c_str(), &cw, &ch);
    const double sx = cw > 0.0 ? w / cw : 1.0, sy = ch > 0.0 ? h / ch : 1.0;
    const auto t = static_cast<int64_t>(emscripten_get_now() * 1e6);
    TouchRouter& router = webTouchRouter();
    for (int i = 0; i < e->numTouches; ++i) {
        const EmscriptenTouchPoint& p = e->touches[i];
        if (!p.isChanged) continue;
        const auto x = static_cast<float>(p.targetX * sx), y = static_cast<float>(p.targetY * sy);
        switch (type) {
            case EMSCRIPTEN_EVENT_TOUCHSTART: router.down(static_cast<int>(p.identifier), x, y, t); break;
            case EMSCRIPTEN_EVENT_TOUCHMOVE: router.move(static_cast<int>(p.identifier), x, y); break;
            case EMSCRIPTEN_EVENT_TOUCHEND: router.up(static_cast<int>(p.identifier), x, y, t, false); break;
            default: router.up(static_cast<int>(p.identifier), x, y, t, true); break;
        }
    }
    return EM_TRUE; // sem rolagem/zoom da pagina e sem o mouse sintetizado
}

} // namespace

void Engine::initImGuiWebBackend() {
    ERUPTION_LOG_WARN("[WEB] %s | %s", reinterpret_cast<const char*>(glGetString(GL_VERSION)),
                      reinterpret_cast<const char*>(glGetString(GL_RENDERER)));
    ImGui_ImplGlfw_InitForOpenGL(m_window.handle(), false); // como no desktop: Input repassa os eventos ao ImGui
    ImGui_ImplOpenGL3_Init("#version 300 es");
    s_touchWindow = m_window.handle();
    if (char* sel = webCanvasSelector()) {
        s_canvas = sel;
        std::free(sel);
    }
    s_cursorCb = glfwSetCursorPosCallback(s_touchWindow, nullptr);
    glfwSetCursorPosCallback(s_touchWindow, s_cursorCb);
    s_buttonCb = glfwSetMouseButtonCallback(s_touchWindow, nullptr);
    glfwSetMouseButtonCallback(s_touchWindow, s_buttonCb);
    s_scrollCb = glfwSetScrollCallback(s_touchWindow, nullptr);
    glfwSetScrollCallback(s_touchWindow, s_scrollCb);
    webDropGlfwTouch();
    emscripten_set_touchstart_callback(s_canvas.c_str(), nullptr, EM_FALSE, onWebTouch);
    emscripten_set_touchmove_callback(s_canvas.c_str(), nullptr, EM_FALSE, onWebTouch);
    emscripten_set_touchend_callback(s_canvas.c_str(), nullptr, EM_FALSE, onWebTouch);
    emscripten_set_touchcancel_callback(s_canvas.c_str(), nullptr, EM_FALSE, onWebTouch);
}

void* Engine::webUiTexture(const std::string& path) {
    int w = 0, h = 0, n = 0;
    unsigned char* pixels = stbi_load(path.c_str(), &w, &h, &n, STBI_rgb_alpha);
    if (!pixels) return nullptr;
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    stbi_image_free(pixels);
    return reinterpret_cast<void*>(static_cast<uintptr_t>(tex));
}

void Engine::renderWeb() {
    // O quadro do contexto Vulkan roda no dispositivo nulo: limpezas adiadas e
    // slot do quadro seguem como no desktop, e o que o motor sobe por comando
    // (o splat que chega nos vertices do chao) passa pelo espelho.
    const bool frame = m_vulkan.beginFrame();
    if (frame) m_modelRenderer.flushVertexPatches(m_vulkan.currentCmdBuf());

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    renderImGui();
    renderLoadingProgress();
    ImGui::Render();

    int w = 0, h = 0;
    glfwGetFramebufferSize(m_window.handle(), &w, &h);
    glViewport(0, 0, w, h);
    // Fundo: o horizonte do ciclo de dia (o ceu procedural fica para o marco 2).
    const Vec3 sky = glm::pow(glm::max(m_dayNightCycle.getSkyHorizonColor(), Vec3(0.0f)), Vec3(1.0f / 2.2f));
    glClearColor(sky.r, sky.g, sky.b, 1.0f);
    glClearDepthf(1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (m_currentMap) renderWebScene();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    if (frame) m_vulkan.endFrame();

    // Contrato com a casca: onReady no primeiro quadro desenhado, quando ela
    // tira o painel de carga.
    if (!m_webReady) {
        m_webReady = true;
        EM_ASM({ if (Module.onReady) Module.onReady(); }, 0);
    }

    // ERUPTION_TEST_SCREENSHOT_FRAME=N (bancada): no quadro N a pagina recebe
    // Module.onTestFrame(N) ainda dentro do quadro (o canvas so' e' valido ate'
    // o fim desta tarefa: a pagina le com toDataURL ali) e o laco para. Com
    // --screenshot <arquivo> (padrao: quadro 600, como no desktop) o PNG sai
    // tambem no FS do modulo, lido do backbuffer, para a pagina pegar com
    // FS.readFile e comparar com o desktop.
    ++m_framesCount;
    static const int stopFrame = [this] {
        const char* e = std::getenv("ERUPTION_TEST_SCREENSHOT_FRAME");
        if (e && std::atoi(e) > 0) return std::atoi(e);
        return m_screenshotOnLoad.empty() ? 0 : 600;
    }();
    if (stopFrame > 0 && m_framesCount == stopFrame) {
        ERUPTION_LOG_WARN("[TEST] quadro %d desenhado, laco parado para a captura", stopFrame);
        if (!m_screenshotOnLoad.empty()) writeWebScreenshot(m_screenshotOnLoad, w, h);
        webTestFrame(stopFrame);
        emscripten_pause_main_loop();
    }
}

} // namespace eruption

#endif
