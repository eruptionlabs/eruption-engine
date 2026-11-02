// Dispositivo Vulkan nulo do navegador. Nao existe Vulkan no navegador, mas o
// motor inteiro (carga de mapa, sprites, scripts, objetos) passa por
// renderers que criam recursos de GPU. Em vez de espalhar guardas por cada
// um, o build web liga contra um "driver" que aceita tudo e nao desenha nada:
// cada vkCreate* devolve um handle falso, a memoria mapeada e' memoria comum
// do wasm, e as consultas descrevem uma GPU modesta sem descriptor indexing
// (o caminho de camadas, o mesmo da spec web). O quadro de verdade sai pelo
// caminho WebGL2 (core/EngineWeb.cpp); os renderers migram para a RHI um a um.
//
// O driver tambem espelha o que o motor sobe (platform/web/WebMirror.hpp):
// vkCmdCopyBuffer guarda os bytes do buffer de destino, vkCmdCopyBufferToImage
// sobe os texels numa textura GL. As copias acontecem na gravacao do comando:
// o motor so' grava a copia depois de preencher o staging.
//
// Aqui ficam so' as funcoes cuja resposta importa. As outras vem de
// tools/web/vulkan_stub.py (VK_SUCCESS, handle falso no ultimo parametro).
#include "platform/web/WebMirror.hpp"

#include <GLES3/gl3.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

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

struct Buffer {
    VkDeviceSize size = 0;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    std::vector<uint8_t> copy; // buffer de GPU que recebeu vkCmdCopyBuffer
    uint64_t version = 0;
};
std::unordered_map<VkBuffer, Buffer> s_buffers;

struct Image {
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 1, height = 1, depth = 1, layers = 1, mips = 1;
    GLuint tex = 0;
    GLenum target = GL_TEXTURE_2D;
    uint32_t levelsWritten = 0; // bit por mip que recebeu texel
    bool mipsPending = false;   // mips gerados por blit no Vulkan: glGenerateMipmap na 1a leitura
};
std::unordered_map<VkImage, Image> s_images;
std::unordered_map<VkImageView, VkImage> s_views;
std::unordered_map<uint32_t, VkImageView> s_slots;
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

// Formato GL de um VkFormat (so' os que o motor sobe sem compressao).
struct GlFormat {
    GLenum internal = 0, format = 0, type = 0;
    uint32_t bytes = 0;
    bool bgra = false; // o GLES nao tem BGRA: troca R e B na subida
    bool filterable = true;
};

GlFormat glFormat(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8G8B8A8_UNORM: return {GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 4};
    case VK_FORMAT_R8G8B8A8_SRGB: return {GL_SRGB8_ALPHA8, GL_RGBA, GL_UNSIGNED_BYTE, 4};
    case VK_FORMAT_B8G8R8A8_UNORM: return {GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 4, true};
    case VK_FORMAT_B8G8R8A8_SRGB: return {GL_SRGB8_ALPHA8, GL_RGBA, GL_UNSIGNED_BYTE, 4, true};
    case VK_FORMAT_R8_UNORM: return {GL_R8, GL_RED, GL_UNSIGNED_BYTE, 1};
    case VK_FORMAT_R8G8_UNORM: return {GL_RG8, GL_RG, GL_UNSIGNED_BYTE, 2};
    case VK_FORMAT_R16_SFLOAT: return {GL_R16F, GL_RED, GL_HALF_FLOAT, 2};
    case VK_FORMAT_R16G16_SFLOAT: return {GL_RG16F, GL_RG, GL_HALF_FLOAT, 4};
    case VK_FORMAT_R16G16B16A16_SFLOAT: return {GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, 8};
    case VK_FORMAT_R32_SFLOAT: return {GL_R32F, GL_RED, GL_FLOAT, 4, false, false};
    case VK_FORMAT_R32G32_SFLOAT: return {GL_RG32F, GL_RG, GL_FLOAT, 8, false, false};
    case VK_FORMAT_R32G32B32A32_SFLOAT: return {GL_RGBA32F, GL_RGBA, GL_FLOAT, 16, false, false};
    default: return {};
    }
}

uint8_t* bufferData(VkBuffer b) {
    const auto it = s_buffers.find(b);
    if (it == s_buffers.end()) return nullptr;
    Buffer& buf = it->second;
    if (!buf.copy.empty()) return buf.copy.data();
    const auto mem = s_memory.find(buf.memory);
    if (mem == s_memory.end() || !mem->second.host) return nullptr;
    return static_cast<uint8_t*>(mem->second.host) + buf.offset;
}

// Destino de uma copia: a memoria mapeada, se o buffer e' visivel ao host;
// senao a copia propria do buffer (do tamanho dele).
uint8_t* bufferTarget(VkBuffer b) {
    const auto it = s_buffers.find(b);
    if (it == s_buffers.end()) return nullptr;
    Buffer& buf = it->second;
    ++buf.version;
    const auto mem = s_memory.find(buf.memory);
    if (buf.copy.empty() && mem != s_memory.end() && mem->second.host)
        return static_cast<uint8_t*>(mem->second.host) + buf.offset;
    if (buf.copy.empty()) buf.copy.assign(size_t(buf.size), 0);
    return buf.copy.data();
}

// Cria a textura GL da imagem na primeira subida (tamanho e mips fixos).
bool ensureTexture(Image& img) {
    if (img.tex) return true;
    const GlFormat g = glFormat(img.format);
    if (!g.internal) return false;
    img.target = img.depth > 1 ? GL_TEXTURE_3D : (img.layers > 1 ? GL_TEXTURE_2D_ARRAY : GL_TEXTURE_2D);
    glGenTextures(1, &img.tex);
    glBindTexture(img.target, img.tex);
    if (img.target == GL_TEXTURE_2D)
        glTexStorage2D(img.target, GLsizei(img.mips), g.internal, GLsizei(img.width), GLsizei(img.height));
    else
        glTexStorage3D(img.target, GLsizei(img.mips), g.internal, GLsizei(img.width), GLsizei(img.height),
                       GLsizei(img.target == GL_TEXTURE_3D ? img.depth : img.layers));
    const bool mip = img.mips > 1;
    glTexParameteri(img.target, GL_TEXTURE_MIN_FILTER,
                    g.filterable ? (mip ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR) : GL_NEAREST);
    glTexParameteri(img.target, GL_TEXTURE_MAG_FILTER, g.filterable ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(img.target, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(img.target, GL_TEXTURE_WRAP_T, GL_REPEAT);
    return true;
}

void uploadRegion(Image& img, const uint8_t* src, const VkBufferImageCopy& r) {
    if (!ensureTexture(img)) return;
    const GlFormat g = glFormat(img.format);
    const uint32_t w = r.imageExtent.width, h = r.imageExtent.height;
    const uint32_t rowTexels = r.bufferRowLength ? r.bufferRowLength : w;
    const uint32_t rows = r.bufferImageHeight ? r.bufferImageHeight : h;
    const uint32_t layers = img.target == GL_TEXTURE_3D ? r.imageExtent.depth : r.imageSubresource.layerCount;
    const size_t bytes = size_t(rowTexels) * rows * layers * g.bytes;
    src += r.bufferOffset;
    std::vector<uint8_t> swapped;
    if (g.bgra) {
        swapped.assign(src, src + bytes);
        for (size_t i = 0; i + 3 < bytes; i += 4) std::swap(swapped[i], swapped[i + 2]);
        src = swapped.data();
    }
    glBindTexture(img.target, img.tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, GLint(rowTexels));
    glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, GLint(rows));
    const GLint level = GLint(r.imageSubresource.mipLevel);
    if (img.target == GL_TEXTURE_2D)
        glTexSubImage2D(img.target, level, r.imageOffset.x, r.imageOffset.y, GLsizei(w), GLsizei(h), g.format, g.type, src);
    else
        glTexSubImage3D(img.target, level, r.imageOffset.x, r.imageOffset.y,
                        img.target == GL_TEXTURE_3D ? r.imageOffset.z : GLint(r.imageSubresource.baseArrayLayer),
                        GLsizei(w), GLsizei(h), GLsizei(layers), g.format, g.type, src);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, 0);
    if (level < 32) img.levelsWritten |= 1u << level;
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
    s_buffers[*out].size = info->size;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyBuffer(VkDevice, VkBuffer b, const VkAllocationCallbacks*) { s_buffers.erase(b); }

VKAPI_ATTR VkResult VKAPI_CALL vkBindBufferMemory(VkDevice, VkBuffer b, VkDeviceMemory m, VkDeviceSize offset) {
    Buffer& buf = s_buffers[b];
    buf.memory = m;
    buf.offset = offset;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkBindBufferMemory2(VkDevice d, uint32_t n, const VkBindBufferMemoryInfo* infos) {
    for (uint32_t i = 0; i < n; ++i) vkBindBufferMemory(d, infos[i].buffer, infos[i].memory, infos[i].memoryOffset);
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateImage(VkDevice, const VkImageCreateInfo* info, const VkAllocationCallbacks*,
                                             VkImage* out) {
    *out = fakeHandle<VkImage>();
    Image& img = s_images[*out];
    img.format = info->format;
    img.width = info->extent.width;
    img.height = info->extent.height;
    img.depth = info->extent.depth;
    img.layers = info->arrayLayers;
    img.mips = info->mipLevels;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyImage(VkDevice, VkImage i, const VkAllocationCallbacks*) {
    const auto it = s_images.find(i);
    if (it == s_images.end()) return;
    if (it->second.tex) glDeleteTextures(1, &it->second.tex);
    s_images.erase(it);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateImageView(VkDevice, const VkImageViewCreateInfo* info,
                                                 const VkAllocationCallbacks*, VkImageView* out) {
    *out = fakeHandle<VkImageView>();
    s_views[*out] = info->image;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyImageView(VkDevice, VkImageView v, const VkAllocationCallbacks*) {
    s_views.erase(v);
}

// ---------------------------------------------------------------- copias (o espelho)

VKAPI_ATTR void VKAPI_CALL vkCmdCopyBuffer(VkCommandBuffer, VkBuffer src, VkBuffer dst, uint32_t n,
                                           const VkBufferCopy* regions) {
    const uint8_t* from = bufferData(src);
    uint8_t* to = bufferTarget(dst);
    if (!from || !to) return;
    for (uint32_t i = 0; i < n; ++i) std::memcpy(to + regions[i].dstOffset, from + regions[i].srcOffset, regions[i].size);
}

VKAPI_ATTR void VKAPI_CALL vkCmdUpdateBuffer(VkCommandBuffer, VkBuffer dst, VkDeviceSize offset, VkDeviceSize size,
                                             const void* data) {
    if (uint8_t* to = bufferTarget(dst)) std::memcpy(to + offset, data, size_t(size));
}

VKAPI_ATTR void VKAPI_CALL vkCmdCopyBufferToImage(VkCommandBuffer, VkBuffer src, VkImage dst, VkImageLayout, uint32_t n,
                                                  const VkBufferImageCopy* regions) {
    const uint8_t* from = bufferData(src);
    const auto it = s_images.find(dst);
    if (!from || it == s_images.end()) return;
    for (uint32_t i = 0; i < n; ++i) uploadRegion(it->second, from, regions[i]);
}

// O motor gera mips por blit de um nivel para o seguinte; no GL basta pedir
// os mips quando a textura for lida.
VKAPI_ATTR void VKAPI_CALL vkCmdBlitImage(VkCommandBuffer, VkImage, VkImageLayout, VkImage dst, VkImageLayout,
                                          uint32_t, const VkImageBlit*, VkFilter) {
    const auto it = s_images.find(dst);
    if (it != s_images.end()) it->second.mipsPending = true;
}

VKAPI_ATTR void VKAPI_CALL vkGetBufferMemoryRequirements(VkDevice, VkBuffer b, VkMemoryRequirements* r) {
    const auto it = s_buffers.find(b);
    r->size = it != s_buffers.end() ? it->second.size : 256;
    r->alignment = 256;
    r->memoryTypeBits = 0x3;
}

VKAPI_ATTR void VKAPI_CALL vkGetImageMemoryRequirements(VkDevice, VkImage i, VkMemoryRequirements* r) {
    // So' para a contabilidade do VMA (imagem de GPU nunca e' mapeada).
    const auto it = s_images.find(i);
    r->size = it == s_images.end() ? 256
                                   : VkDeviceSize(it->second.width) * it->second.height * it->second.depth *
                                         it->second.layers * 8 * (it->second.mips > 1 ? 2 : 1);
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

// ---------------------------------------------------------------- leitura do espelho

namespace eruption::web {

const uint8_t* bufferBytes(VkBuffer buffer, size_t* size, uint64_t* version) {
    const auto it = s_buffers.find(buffer);
    if (it == s_buffers.end()) return nullptr;
    if (size) *size = size_t(it->second.size);
    if (version) *version = it->second.version;
    return bufferData(buffer);
}

unsigned imageTexture(VkImage image, unsigned* target) {
    const auto it = s_images.find(image);
    if (it == s_images.end() || !it->second.tex) return 0;
    Image& img = it->second;
    // Mips por blit, ou so' o nivel 0 subiu numa imagem com mips: o GL gera.
    if (img.mips > 1 && (img.mipsPending || img.levelsWritten == 1u) && glFormat(img.format).filterable) {
        glBindTexture(img.target, img.tex);
        glGenerateMipmap(img.target);
        img.mipsPending = false;
        img.levelsWritten = ~0u;
    }
    if (target) *target = img.target;
    return img.tex;
}

unsigned viewTexture(VkImageView view, unsigned* target) {
    const auto it = s_views.find(view);
    return it == s_views.end() ? 0 : imageTexture(it->second, target);
}

void setSlotView(uint32_t slot, VkImageView view) { s_slots[slot] = view; }

unsigned slotTexture(uint32_t slot, unsigned* target) {
    const auto it = s_slots.find(slot);
    return it == s_slots.end() ? 0 : viewTexture(it->second, target);
}

bool slotExtent(uint32_t slot, uint32_t* width, uint32_t* height) {
    const auto sl = s_slots.find(slot);
    if (sl == s_slots.end()) return false;
    const auto v = s_views.find(sl->second);
    if (v == s_views.end()) return false;
    const auto it = s_images.find(v->second);
    if (it == s_images.end()) return false;
    *width = it->second.width;
    *height = it->second.height;
    return true;
}

} // namespace eruption::web
