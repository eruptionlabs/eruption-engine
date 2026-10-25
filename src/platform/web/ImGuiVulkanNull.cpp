// Backend Vulkan do ImGui no navegador: nao faz nada. A interface desenha
// pelo backend OpenGL3 (core/EngineWeb.cpp); estas funcoes so' existem porque
// a tela de carga, o minimapa e as texturas de UI do desktop as chamam. O
// backend de verdade leria os dados do OpenGL3 como se fossem dele (estouro
// de heap visto com AddressSanitizer na tela de carga).
#include <imgui.h>
#include <imgui_impl_vulkan.h>

void* eruptionVkFakeHandle(); // platform/web/VulkanNull.cpp

bool ImGui_ImplVulkan_Init(ImGui_ImplVulkan_InitInfo*) { return true; }
void ImGui_ImplVulkan_Shutdown() {}
void ImGui_ImplVulkan_NewFrame() {}
void ImGui_ImplVulkan_RenderDrawData(ImDrawData*, VkCommandBuffer, VkPipeline) {}
bool ImGui_ImplVulkan_CreateFontsTexture() { return true; }
void ImGui_ImplVulkan_DestroyFontsTexture() {}
void ImGui_ImplVulkan_SetMinImageCount(uint32_t) {}
VkDescriptorSet ImGui_ImplVulkan_AddTexture(VkSampler, VkImageView, VkImageLayout) {
    return reinterpret_cast<VkDescriptorSet>(eruptionVkFakeHandle());
}
void ImGui_ImplVulkan_RemoveTexture(VkDescriptorSet) {}
