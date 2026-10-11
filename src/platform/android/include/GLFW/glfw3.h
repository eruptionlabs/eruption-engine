#pragma once
// Subconjunto da API do GLFW que o motor usa, implementado no Android sobre a
// ANativeWindow e a fila de eventos da NativeActivity (AndroidMain.cpp). Com
// ele o codigo de janela, entrada e superficie Vulkan compila sem #if.
#ifdef GLFW_INCLUDE_VULKAN
#include <vulkan/vulkan.h>
#endif
#include <cstdint>

#define GLFW_TRUE 1
#define GLFW_FALSE 0
#define GLFW_RELEASE 0
#define GLFW_PRESS 1
#define GLFW_REPEAT 2
#define GLFW_JOYSTICK_1 0
#define GLFW_CURSOR 0x00033001
#define GLFW_CURSOR_NORMAL 0x00034001
#define GLFW_CURSOR_HIDDEN 0x00034002
#define GLFW_CURSOR_DISABLED 0x00034003
#define GLFW_RESIZABLE 0x00020003
#define GLFW_CLIENT_API 0x00022001
#define GLFW_NO_API 0
#define GLFW_MOUSE_BUTTON_LEFT 0
#define GLFW_MOUSE_BUTTON_RIGHT 1
#define GLFW_MOUSE_BUTTON_MIDDLE 2

struct GLFWwindow;
struct GLFWmonitor;
struct GLFWvidmode { int width, height, redBits, greenBits, blueBits, refreshRate; };
struct GLFWimage { int width, height; unsigned char* pixels; };
struct GLFWgamepadstate { unsigned char buttons[15]; float axes[6]; };

typedef void (*GLFWkeyfun)(GLFWwindow*, int, int, int, int);
typedef void (*GLFWcharfun)(GLFWwindow*, unsigned int);
typedef void (*GLFWmousebuttonfun)(GLFWwindow*, int, int, int);
typedef void (*GLFWcursorposfun)(GLFWwindow*, double, double);
typedef void (*GLFWscrollfun)(GLFWwindow*, double, double);
typedef void (*GLFWframebuffersizefun)(GLFWwindow*, int, int);

int glfwInit();
void glfwTerminate();
void glfwWindowHint(int hint, int value);
GLFWmonitor* glfwGetPrimaryMonitor();
const GLFWvidmode* glfwGetVideoMode(GLFWmonitor* monitor);
GLFWwindow* glfwCreateWindow(int width, int height, const char* title, GLFWmonitor* monitor, GLFWwindow* share);
void glfwDestroyWindow(GLFWwindow* window);
void glfwSetWindowPos(GLFWwindow* window, int x, int y);
void glfwSetWindowSize(GLFWwindow* window, int w, int h);
void glfwSetWindowTitle(GLFWwindow* window, const char* title);
void glfwSetWindowIcon(GLFWwindow* window, int count, const GLFWimage* images);
void glfwMaximizeWindow(GLFWwindow* window);
int glfwWindowShouldClose(GLFWwindow* window);
void glfwSetWindowShouldClose(GLFWwindow* window, int value);
void glfwPollEvents();
void glfwGetFramebufferSize(GLFWwindow* window, int* width, int* height);
void glfwSetWindowUserPointer(GLFWwindow* window, void* pointer);
void* glfwGetWindowUserPointer(GLFWwindow* window);
void glfwSetInputMode(GLFWwindow* window, int mode, int value);
int glfwGetMouseButton(GLFWwindow* window, int button);
void glfwGetCursorPos(GLFWwindow* window, double* x, double* y);
GLFWkeyfun glfwSetKeyCallback(GLFWwindow* window, GLFWkeyfun cb);
GLFWcharfun glfwSetCharCallback(GLFWwindow* window, GLFWcharfun cb);
GLFWmousebuttonfun glfwSetMouseButtonCallback(GLFWwindow* window, GLFWmousebuttonfun cb);
GLFWcursorposfun glfwSetCursorPosCallback(GLFWwindow* window, GLFWcursorposfun cb);
GLFWscrollfun glfwSetScrollCallback(GLFWwindow* window, GLFWscrollfun cb);
GLFWframebuffersizefun glfwSetFramebufferSizeCallback(GLFWwindow* window, GLFWframebuffersizefun cb);
int glfwJoystickPresent(int jid);
int glfwJoystickIsGamepad(int jid);
const char* glfwGetJoystickName(int jid);
int glfwGetGamepadState(int jid, GLFWgamepadstate* state);
const float* glfwGetJoystickAxes(int jid, int* count);
const unsigned char* glfwGetJoystickButtons(int jid, int* count);
const char** glfwGetRequiredInstanceExtensions(uint32_t* count);
// Android: o sistema tira a janela ao ir para os apps recentes/home. O dono
// da superficie Vulkan registra como solta-la e recria-la; o shim chama na
// thread do motor (glfwPollEvents) e o jogo continua quando a janela volta.
void eruptionAndroidSetSurfaceHooks(void (*release)(void*), int (*restore)(void*), void* user);
#ifdef VK_VERSION_1_0
VkResult glfwCreateWindowSurface(VkInstance instance, GLFWwindow* window, const VkAllocationCallbacks* allocator,
                                 VkSurfaceKHR* surface);
#endif
