#pragma once

#include <cstdint>
#include <string>
#include <functional>

struct GLFWwindow;

namespace eruption {

class Window {
public:
    using ResizeCallback = std::function<void(int width, int height)>;
    using CloseCallback = std::function<void()>;

    Window() = default;
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    bool init(int width, int height, const std::string& title);
    // PNG do icone (antes de init); vazio ou ilegivel = o icone do motor.
    void setIconFile(const std::string& path) { m_iconFile = path; }
    void maximize();
    // Tela cheia em tempo de execucao (Alt+Enter, window.set_fullscreen). Liga:
    // guarda posicao/tamanho da janela e vai para o modo nativo do monitor
    // primario; desliga: volta ao que foi guardado. A troca dispara o callback
    // de framebuffer, que recria a swapchain pelo caminho normal de resize.
    void setFullscreen(bool on);
    void toggleFullscreen() { setFullscreen(!isFullscreen()); }
    bool isFullscreen() const;
    void shutdown();

    bool shouldClose() const;
    void pollEvents() const;
    void swapBuffers() const;

    void setResizeCallback(ResizeCallback cb);
    void setCloseCallback(CloseCallback cb);

    int width() const { return m_width; }
    int height() const { return m_height; }
    float aspectRatio() const { return static_cast<float>(m_width) / static_cast<float>(m_height); }

    GLFWwindow* handle() const { return m_window; }

    bool isMouseButtonDown(int button) const;
    void getMousePos(double& x, double& y) const;
    float getMouseScroll();

private:
    GLFWwindow* m_window = nullptr;
    int m_width = 0;
    int m_height = 0;
    ResizeCallback m_resizeCb;
    CloseCallback m_closeCb;
    float m_scrollOffset = 0.0f;
    // Janela a que voltar ao sair da tela cheia (ver setFullscreen).
    int m_winX = 100, m_winY = 100, m_winW = 1280, m_winH = 720;

    static void framebufferResizeCallback(GLFWwindow* window, int width, int height);
    static void scrollCallback(GLFWwindow* window, double xoffset, double yoffset);
    std::string m_iconFile;
};

} // namespace eruption
