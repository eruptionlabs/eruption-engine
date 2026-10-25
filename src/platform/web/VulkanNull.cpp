// Dispositivo Vulkan nulo do navegador. Nao existe Vulkan no navegador, mas o
// motor inteiro (carga de mapa, sprites, scripts, objetos) passa por
// renderers que criam recursos de GPU. Em vez de espalhar guardas por cada
// um, o build web liga contra um "driver" que aceita tudo e nao desenha nada:
// cada vkCreate* devolve um handle falso, a memoria mapeada e' memoria comum
// do wasm, e as consultas descrevem uma GPU modesta sem descriptor indexing
// (o caminho de camadas, o mesmo da spec web). O quadro de verdade sai pelo
// caminho WebGL2 (core/EngineWeb.cpp); os renderers migram para a RHI um a um.
//
// Aqui ficam so' as funcoes cuja resposta importa. As outras vem de
// tools/web/vulkan_stub.py (VK_SUCCESS, handle falso no ultimo parametro).
#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace {

uintptr_t s_nextHandle = 0x100000;

template <class H>
H fakeHandle() {
    s_nextHandle += 64;
    return reinterpret_cast<H>(s_nextHandle);
}

struct Memory {
    VkDeviceSize size = 0;
    void* host = nullptr; // so' existe depois do primeiro vkMapMemory
};
std::unordered_map<VkDeviceMemory, Memory> s_memory;
std::unordered_map<VkBuffer, VkDeviceSize> s_bufferSize;
std::unordered_map<VkImage, VkDeviceSize> s_imageSize;
uint32_t s_swapIndex = 0;
constexpr uint32_t kSwapImages = 3;
VkImage s_swapImages[kSwapImages] = {};

// Formatos comprimidos (BC, ETC2, ASTC) ficam sem suporte: o motor sobe a
// textura sem compressao, que e' o que o WebGL2 recebe.
bool compressedFormat(VkFormat f) {
    return (f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK);
}

bool depthFormat(VkFormat f) {
    return f == VK_FORMAT_D16_UNORM || f == VK_FORMAT_D32_SFLOAT || f == VK_FORMAT_D24_UNORM_S8_UINT ||
           f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_X8_D24_UNORM_PACK32;
}

template <class T>
VkResult fillList(const T* src, uint32_t n, uint32_t* count, T* out) {
    if (!out) {
        *count = n;
        return VK_SUCCESS;
    }
    const uint32_t k = *count < n ? *count : n;
    for (uint32_t i = 0; i < k; ++i) out[i] = src[i];
    *count = k;
    return k < n ? VK_INCOMPLETE : VK_SUCCESS;
}

VkExtensionProperties extension(const char* name) {
    VkExtensionProperties e{};
    std::strncpy(e.extensionName, name, VK_MAX_EXTENSION_NAME_SIZE - 1);
    e.specVersion = 1;
    return e;
}

} // namespace

// Para o stub gerado (vulkan_stub.py): mesmo contador de handles.
void* eruptionVkFakeHandle() { return fakeHandle<void*>(); }

extern "C" {

// ---------------------------------------------------------------- instancia

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceVersion(uint32_t* v) {
    *v = VK_API_VERSION_1_1;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(const char*, uint32_t* count,
                                                                      VkExtensionProperties* out) {
    const VkExtensionProperties list[] = {extension(VK_KHR_SURFACE_EXTENSION_NAME)};
    return fillList(list, 1, count, out);
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceLayerProperties(uint32_t* count, VkLayerProperties*) {
    *count = 0;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumeratePhysicalDevices(VkInstance, uint32_t* count, VkPhysicalDevice* out) {
    static const VkPhysicalDevice gpu = fakeHandle<VkPhysicalDevice>();
    return fillList(&gpu, 1, count, out);
}

// ---------------------------------------------------------------- GPU

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties(VkPhysicalDevice, VkPhysicalDeviceProperties* p) {
    *p = {};
    p->apiVersion = VK_API_VERSION_1_1;
    p->deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
    std::strncpy(p->deviceName, "WebGL2 (Vulkan nulo)", VK_MAX_PHYSICAL_DEVICE_NAME_SIZE - 1);
    VkPhysicalDeviceLimits& l = p->limits;
    l.maxImageDimension1D = l.maxImageDimension2D = 8192;
    l.maxImageDimension3D = 2048;
    l.maxImageDimensionCube = 8192;
    l.maxImageArrayLayers = 2048;
    l.maxTexelBufferElements = 1u << 27;
    l.maxUniformBufferRange = 65536;
    l.maxStorageBufferRange = 1u << 27;
    l.maxPushConstantsSize = 128;
    l.maxMemoryAllocationCount = 4096;
    l.maxSamplerAllocationCount = 4000;
    l.bufferImageGranularity = 1;
    l.maxBoundDescriptorSets = 8;
    l.maxPerStageDescriptorSamplers = 16;
    l.maxPerStageDescriptorUniformBuffers = 12;
    l.maxPerStageDescriptorStorageBuffers = 8;
    l.maxPerStageDescriptorSampledImages = 16;
    l.maxPerStageDescriptorStorageImages = 4;
    l.maxPerStageDescriptorInputAttachments = 4;
    l.maxPerStageResources = 64;
    l.maxDescriptorSetSamplers = 80;
    l.maxDescriptorSetUniformBuffers = 72;
    l.maxDescriptorSetUniformBuffersDynamic = 8;
    l.maxDescriptorSetStorageBuffers = 24;
    l.maxDescriptorSetStorageBuffersDynamic = 4;
    l.maxDescriptorSetSampledImages = 96;
    l.maxDescriptorSetStorageImages = 24;
    l.maxDescriptorSetInputAttachments = 4;
    l.maxVertexInputAttributes = 16;
    l.maxVertexInputBindings = 16;
    l.maxVertexInputAttributeOffset = 2047;
    l.maxVertexInputBindingStride = 2048;
    l.maxVertexOutputComponents = 64;
    l.maxFragmentInputComponents = 64;
    l.maxFragmentOutputAttachments = 8;
    l.maxComputeSharedMemorySize = 16384;
    l.maxComputeWorkGroupCount[0] = l.maxComputeWorkGroupCount[1] = l.maxComputeWorkGroupCount[2] = 65535;
    l.maxComputeWorkGroupInvocations = 256;
    l.maxComputeWorkGroupSize[0] = l.maxComputeWorkGroupSize[1] = 256;
    l.maxComputeWorkGroupSize[2] = 64;
    l.maxDrawIndexedIndexValue = 0xFFFFFFFFu;
    l.maxDrawIndirectCount = 1u << 30;
    l.maxSamplerLodBias = 16.0f;
    l.maxSamplerAnisotropy = 16.0f;
    l.maxViewports = 1;
    l.maxViewportDimensions[0] = l.maxViewportDimensions[1] = 8192;
    l.viewportBoundsRange[0] = -16384.0f;
    l.viewportBoundsRange[1] = 16384.0f;
    l.minMemoryMapAlignment = 64;
    l.minTexelBufferOffsetAlignment = 256;
    l.minUniformBufferOffsetAlignment = 256;
    l.minStorageBufferOffsetAlignment = 256;
    l.maxFramebufferWidth = l.maxFramebufferHeight = 8192;
    l.maxFramebufferLayers = 1;
    l.framebufferColorSampleCounts = l.framebufferDepthSampleCounts = VK_SAMPLE_COUNT_1_BIT;
    l.sampledImageColorSampleCounts = l.sampledImageDepthSampleCounts = VK_SAMPLE_COUNT_1_BIT;
    l.storageImageSampleCounts = VK_SAMPLE_COUNT_1_BIT;
    l.maxColorAttachments = 8;
    l.maxClipDistances = 8;
    l.timestampComputeAndGraphics = VK_TRUE;
    l.timestampPeriod = 1.0f;
    l.nonCoherentAtomSize = 256;
    l.optimalBufferCopyOffsetAlignment = 4;
    l.optimalBufferCopyRowPitchAlignment = 4;
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties2(VkPhysicalDevice gpu, VkPhysicalDeviceProperties2* p) {
    vkGetPhysicalDeviceProperties(gpu, &p->properties);
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures(VkPhysicalDevice, VkPhysicalDeviceFeatures* f) {
    *f = {};
    f->samplerAnisotropy = VK_TRUE;
    f->independentBlend = VK_TRUE;
    f->imageCubeArray = VK_TRUE;
    f->drawIndirectFirstInstance = VK_TRUE;
    f->multiDrawIndirect = VK_TRUE;
    f->fragmentStoresAndAtomics = VK_TRUE;
    f->fullDrawIndexUint32 = VK_TRUE;
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures2(VkPhysicalDevice gpu, VkPhysicalDeviceFeatures2* f) {
    vkGetPhysicalDeviceFeatures(gpu, &f->features); // a cadeia pNext fica como veio (tudo desligado)
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties* m) {
    *m = {};
    m->memoryHeapCount = 2;
    m->memoryHeaps[0] = {VkDeviceSize(2048) << 20, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT};
    m->memoryHeaps[1] = {VkDeviceSize(1024) << 20, 0};
    m->memoryTypeCount = 2;
    m->memoryTypes[0] = {VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0};
    m->memoryTypes[1] = {VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 1};
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties2(VkPhysicalDevice gpu,
                                                               VkPhysicalDeviceMemoryProperties2* m) {
    vkGetPhysicalDeviceMemoryProperties(gpu, &m->memoryProperties);
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice, uint32_t* count,
                                                                    VkQueueFamilyProperties* out) {
    VkQueueFamilyProperties q{};
    q.queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
    q.queueCount = 1;
    q.timestampValidBits = 64;
    q.minImageTransferGranularity = {1, 1, 1};
    fillList(&q, 1, count, out);
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice, VkFormat f, VkFormatProperties* p) {
    *p = {};
    if (compressedFormat(f)) return;
    VkFormatFeatureFlags all = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
                               VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
                               VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    all |= depthFormat(f) ? VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT
                          : VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT |
                                VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
    p->optimalTilingFeatures = all;
    p->linearTilingFeatures = all;
    p->bufferFeatures = VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT | VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT;
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFormatProperties2(VkPhysicalDevice gpu, VkFormat f,
                                                               VkFormatProperties2* p) {
    vkGetPhysicalDeviceFormatProperties(gpu, f, &p->formatProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(VkPhysicalDevice, const char*, uint32_t* count,
                                                                    VkExtensionProperties* out) {
    const VkExtensionProperties list[] = {extension(VK_KHR_SWAPCHAIN_EXTENSION_NAME)};
    return fillList(list, 1, count, out);
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceLayerProperties(VkPhysicalDevice, uint32_t* count,
                                                                VkLayerProperties*) {
    *count = 0;
    return VK_SUCCESS;
}

// ---------------------------------------------------------------- superficie e swapchain

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice, uint32_t, VkSurfaceKHR,
                                                                    VkBool32* ok) {
    *ok = VK_TRUE;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice, VkSurfaceKHR,
                                                                         VkSurfaceCapabilitiesKHR* c) {
    *c = {};
    c->minImageCount = 2;
    c->maxImageCount = kSwapImages;
    c->currentExtent = {0xFFFFFFFFu, 0xFFFFFFFFu}; // o motor usa o tamanho da janela (o canvas)
    c->minImageExtent = {1, 1};
    c->maxImageExtent = {8192, 8192};
    c->maxImageArrayLayers = 1;
    c->supportedTransforms = c->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    c->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    c->supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                             VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice, VkSurfaceKHR, uint32_t* count,
                                                                    VkSurfaceFormatKHR* out) {
    const VkSurfaceFormatKHR f{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    return fillList(&f, 1, count, out);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice, VkSurfaceKHR,
                                                                         uint32_t* count, VkPresentModeKHR* out) {
    const VkPresentModeKHR m = VK_PRESENT_MODE_FIFO_KHR;
    return fillList(&m, 1, count, out);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetSwapchainImagesKHR(VkDevice, VkSwapchainKHR, uint32_t* count, VkImage* out) {
    if (!s_swapImages[0])
        for (VkImage& img : s_swapImages) img = fakeHandle<VkImage>();
    return fillList(s_swapImages, kSwapImages, count, out);
}

VKAPI_ATTR VkResult VKAPI_CALL vkAcquireNextImageKHR(VkDevice, VkSwapchainKHR, uint64_t, VkSemaphore, VkFence,
                                                     uint32_t* index) {
    *index = s_swapIndex;
    s_swapIndex = (s_swapIndex + 1) % kSwapImages;
    return VK_SUCCESS;
}

// ---------------------------------------------------------------- objetos em lote

VKAPI_ATTR VkResult VKAPI_CALL vkAllocateCommandBuffers(VkDevice, const VkCommandBufferAllocateInfo* info,
                                                        VkCommandBuffer* out) {
    for (uint32_t i = 0; i < info->commandBufferCount; ++i) out[i] = fakeHandle<VkCommandBuffer>();
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkAllocateDescriptorSets(VkDevice, const VkDescriptorSetAllocateInfo* info,
                                                        VkDescriptorSet* out) {
    for (uint32_t i = 0; i < info->descriptorSetCount; ++i) out[i] = fakeHandle<VkDescriptorSet>();
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateGraphicsPipelines(VkDevice, VkPipelineCache, uint32_t n,
                                                         const VkGraphicsPipelineCreateInfo*,
                                                         const VkAllocationCallbacks*, VkPipeline* out) {
    for (uint32_t i = 0; i < n; ++i) out[i] = fakeHandle<VkPipeline>();
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateComputePipelines(VkDevice, VkPipelineCache, uint32_t n,
                                                        const VkComputePipelineCreateInfo*,
                                                        const VkAllocationCallbacks*, VkPipeline* out) {
    for (uint32_t i = 0; i < n; ++i) out[i] = fakeHandle<VkPipeline>();
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPipelineCacheData(VkDevice, VkPipelineCache, size_t* size, void*) {
    *size = 0;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetQueryPoolResults(VkDevice, VkQueryPool, uint32_t, uint32_t, size_t size,
                                                     void* data, VkDeviceSize, VkQueryResultFlags) {
    std::memset(data, 0, size); // tempos de GPU zerados: nada roda na GPU por aqui
    return VK_SUCCESS;
}

// ---------------------------------------------------------------- memoria

VKAPI_ATTR VkResult VKAPI_CALL vkCreateBuffer(VkDevice, const VkBufferCreateInfo* info, const VkAllocationCallbacks*,
                                              VkBuffer* out) {
    *out = fakeHandle<VkBuffer>();
    s_bufferSize[*out] = info->size;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyBuffer(VkDevice, VkBuffer b, const VkAllocationCallbacks*) {
    s_bufferSize.erase(b);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateImage(VkDevice, const VkImageCreateInfo* info, const VkAllocationCallbacks*,
                                             VkImage* out) {
    *out = fakeHandle<VkImage>();
    // So' para a contabilidade do VMA (imagem de GPU nunca e' mapeada).
    s_imageSize[*out] = VkDeviceSize(info->extent.width) * info->extent.height * info->extent.depth *
                        info->arrayLayers * 8 * (info->mipLevels > 1 ? 2 : 1);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyImage(VkDevice, VkImage i, const VkAllocationCallbacks*) { s_imageSize.erase(i); }

VKAPI_ATTR void VKAPI_CALL vkGetBufferMemoryRequirements(VkDevice, VkBuffer b, VkMemoryRequirements* r) {
    const auto it = s_bufferSize.find(b);
    r->size = it != s_bufferSize.end() ? it->second : 256;
    r->alignment = 256;
    r->memoryTypeBits = 0x3;
}

VKAPI_ATTR void VKAPI_CALL vkGetImageMemoryRequirements(VkDevice, VkImage i, VkMemoryRequirements* r) {
    const auto it = s_imageSize.find(i);
    r->size = it != s_imageSize.end() ? it->second : 256;
    r->alignment = 256;
    r->memoryTypeBits = 0x3;
}

VKAPI_ATTR void VKAPI_CALL vkGetBufferMemoryRequirements2(VkDevice d, const VkBufferMemoryRequirementsInfo2* info,
                                                          VkMemoryRequirements2* r) {
    vkGetBufferMemoryRequirements(d, info->buffer, &r->memoryRequirements);
}

VKAPI_ATTR void VKAPI_CALL vkGetImageMemoryRequirements2(VkDevice d, const VkImageMemoryRequirementsInfo2* info,
                                                         VkMemoryRequirements2* r) {
    vkGetImageMemoryRequirements(d, info->image, &r->memoryRequirements);
}

VKAPI_ATTR VkResult VKAPI_CALL vkAllocateMemory(VkDevice, const VkMemoryAllocateInfo* info,
                                                const VkAllocationCallbacks*, VkDeviceMemory* out) {
    *out = fakeHandle<VkDeviceMemory>();
    s_memory[*out].size = info->allocationSize;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkFreeMemory(VkDevice, VkDeviceMemory m, const VkAllocationCallbacks*) {
    const auto it = s_memory.find(m);
    if (it == s_memory.end()) return;
    std::free(it->second.host);
    s_memory.erase(it);
}

// Memoria de verdade so' quando alguem mapeia (staging, UBO): imagem e
// buffer de GPU nunca ocupam o heap do wasm.
VKAPI_ATTR VkResult VKAPI_CALL vkMapMemory(VkDevice, VkDeviceMemory m, VkDeviceSize offset, VkDeviceSize,
                                           VkMemoryMapFlags, void** out) {
    Memory& mem = s_memory[m];
    if (!mem.host) mem.host = std::calloc(1, size_t(mem.size ? mem.size : 256));
    if (!mem.host) return VK_ERROR_OUT_OF_HOST_MEMORY;
    *out = static_cast<uint8_t*>(mem.host) + offset;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkUnmapMemory(VkDevice, VkDeviceMemory) {}

} // extern "C"
