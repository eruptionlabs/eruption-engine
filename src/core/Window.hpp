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
    void maximize();
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

    static void framebufferResizeCallback(GLFWwindow* window, int width, int height);
    static void scrollCallback(GLFWwindow* window, double xoffset, double yoffset);
};

} // namespace eruption
