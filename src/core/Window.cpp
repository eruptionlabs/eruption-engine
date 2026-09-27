#include "core/Window.hpp"

#include <cstdlib>
#include <string>
#include "core/Logger.hpp"
#include <GLFW/glfw3.h>
#include <stb_image.h>
#include "Icons.hpp"

namespace eruption {

Window::~Window() {
    shutdown();
}

bool Window::init(int width, int height, const std::string& title) {
    if (!glfwInit()) {
        ERUPTION_LOG_FATAL("Failed to initialize GLFW");
        return false;
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

    GLFWmonitor* monitor = glfwGetPrimaryMonitor();
    const GLFWvidmode* mode = glfwGetVideoMode(monitor);
    int nativeW = mode ? mode->width : width;
    int nativeH = mode ? mode->height : height;

    // TELA CHEIA EXCLUSIVA (ERUPTION_FULLSCREEN=1).
    //
    // Motivo medido: num display com compositor, a janela que nao esta' visivel
    // e' ESTRANGULADA - e nao e' a engine. Provado com glxgears no mesmo
    // display: 215,4 FPS no primeiro intervalo e 1,000 FPS no segundo. Isso
    // inviabiliza medir FPS de verdade a partir de uma sessao de terminal.
    //
    // Passar o monitor ao glfwCreateWindow faz o compositor UNREDIRECT a
    // janela (ela deixa de passar pela composicao), que e' a saida padrao para
    // esse throttle. Serve tambem como modo de jogo de verdade, nao so' para
    // medicao - uma game engine precisa de tela cheia.
    const bool wantFullscreen = [] {
        const char* e = std::getenv("ERUPTION_FULLSCREEN");
        return e && *e && std::string(e) != "0";
    }();
    m_window = glfwCreateWindow(nativeW, nativeH, title.c_str(),
                                wantFullscreen ? monitor : nullptr, nullptr);
    if (m_window && monitor && !wantFullscreen) {
        glfwSetWindowPos(m_window, 0, 0);
    }
    if (wantFullscreen) {
        ERUPTION_LOG_WARN("Janela em TELA CHEIA exclusiva (ERUPTION_FULLSCREEN)");
    }
    if (!m_window) {
        ERUPTION_LOG_FATAL("Failed to create GLFW window");
        glfwTerminate();
        return false;
    }

    // Set Window Icons
    GLFWimage images[6];
    int count = 0;

    auto load_icon = [&](const uint8_t* data, size_t len) {
        int w, h, channels;
        unsigned char* pixels = stbi_load_from_memory(data, static_cast<int>(len), &w, &h, &channels, 4);
        if (pixels) {
            images[count].width = w;
            images[count].height = h;
            images[count].pixels = pixels;
            count++;
        }
    };

    load_icon(icons::eruption_v3_16_png, icons::eruption_v3_16_png_len);
    load_icon(icons::eruption_v3_32_png, icons::eruption_v3_32_png_len);
    load_icon(icons::eruption_v3_64_png, icons::eruption_v3_64_png_len);
    load_icon(icons::eruption_v3_128_png, icons::eruption_v3_128_png_len);
    load_icon(icons::eruption_v3_256_png, icons::eruption_v3_256_png_len);
    load_icon(icons::eruption_v3_512_png, icons::eruption_v3_512_png_len);

    if (count > 0) {
        glfwSetWindowIcon(m_window, count, images);
        for (int i = 0; i < count; i++) {
            stbi_image_free(images[i].pixels);
        }
    }

    glfwGetFramebufferSize(m_window, &m_width, &m_height);

    glfwSetWindowUserPointer(m_window, this);
    glfwSetFramebufferSizeCallback(m_window, framebufferResizeCallback);
    glfwSetScrollCallback(m_window, scrollCallback);

    return true;
}

void Window::maximize() {
    if (m_window) glfwMaximizeWindow(m_window);
}

void Window::shutdown() {
    if (m_window) {
        glfwDestroyWindow(m_window);
        m_window = nullptr;
    }
    glfwTerminate();
}

bool Window::shouldClose() const {
    return m_window ? glfwWindowShouldClose(m_window) : true;
}

void Window::pollEvents() const {
    glfwPollEvents();
}

void Window::swapBuffers() const {
    // Swap buffers is handled by Vulkan present, not GLFW
    // This function is kept for API completeness
    (void)this;
}

void Window::setResizeCallback(ResizeCallback cb) {
    m_resizeCb = std::move(cb);
}

void Window::setCloseCallback(CloseCallback cb) {
    m_closeCb = std::move(cb);
}

void Window::framebufferResizeCallback(GLFWwindow* window, int width, int height) {
    Window* self = static_cast<Window*>(glfwGetWindowUserPointer(window));
    if (self) {
        self->m_width = width;
        self->m_height = height;
        if (self->m_resizeCb) {
            self->m_resizeCb(width, height);
        }
    }
}

bool Window::isMouseButtonDown(int button) const {
    return m_window && glfwGetMouseButton(m_window, button) == GLFW_PRESS;
}

void Window::getMousePos(double& x, double& y) const {
    if (m_window) {
        glfwGetCursorPos(m_window, &x, &y);
    } else {
        x = y = 0.0;
    }
}

float Window::getMouseScroll() {
    float offset = m_scrollOffset;
    m_scrollOffset = 0.0f;
    return offset;
}

void Window::scrollCallback(GLFWwindow* window, double xoffset, double yoffset) {
    (void)xoffset;
    Window* self = static_cast<Window*>(glfwGetWindowUserPointer(window));
    if (self) {
        self->m_scrollOffset += static_cast<float>(yoffset);
    }
}

} // namespace eruption
