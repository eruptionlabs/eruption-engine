#pragma once
// Backend de ImGui do Android com a mesma interface do imgui_impl_glfw: o
// motor chama estas funcoes; aqui o toque vira mouse (AndroidMain.cpp).
struct GLFWwindow;
bool ImGui_ImplGlfw_InitForVulkan(GLFWwindow* window, bool installCallbacks);
void ImGui_ImplGlfw_Shutdown();
void ImGui_ImplGlfw_NewFrame();
void ImGui_ImplGlfw_KeyCallback(GLFWwindow* window, int key, int scancode, int action, int mods);
void ImGui_ImplGlfw_MouseButtonCallback(GLFWwindow* window, int button, int action, int mods);
void ImGui_ImplGlfw_CursorPosCallback(GLFWwindow* window, double x, double y);
void ImGui_ImplGlfw_ScrollCallback(GLFWwindow* window, double xoffset, double yoffset);
void ImGui_ImplGlfw_CharCallback(GLFWwindow* window, unsigned int c);
