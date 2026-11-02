#pragma once

#include <cstdint>
#include <vector>

// Interface fina de GPU com a forma do WebGPU (a API mais restrita dos alvos):
// texturas, samplers, grupos de ligação imutáveis, pipelines fechadas com os
// formatos dos alvos, passes com alvos explícitos e sem barreiras na mão. O
// backend Vulkan (RhiVulkan.cpp) traduz para os mesmos comandos que os
// renderers gravavam direto; o backend WebGPU implementa a mesma interface.
//
// Estado das texturas: cada textura tem um "estado de repouso" (amostrável se
// tem Sampled; senão o de alvo). O passe leva os alvos do repouso para alvo e
// devolve ao repouso no fim, como o WebGPU faz por baixo. Textura de fora (a
// imagem da swapchain, alvos de renderers ainda não migrados) entra por
// wrapVulkan com o estado em que os outros a deixam.
//
// Dados por draw (push): setPush. No Vulkan é push constant; no WebGPU, que
// não tem, vira um anel de uniform com offset dinâmico.

#include <vulkan/vulkan.h>

namespace eruption {
class VulkanContext;
}

namespace eruption::rhi {

enum class Format : uint8_t {
    Undefined,
    RGBA8Unorm,
    RGBA8Srgb,
    BGRA8Unorm,
    BGRA8Srgb,
    RGBA16Float,
    RG16Float,
    R16Float,
    R32Float,
    R8Unorm,
    RG8Unorm,
    RGB10A2Unorm,
    RG11B10Float,
    Depth32Float,
};

enum TextureUsage : uint32_t {
    kUsageSampled = 1u << 0,
    kUsageStorage = 1u << 1,
    kUsageRenderTarget = 1u << 2,
    kUsageCopySrc = 1u << 3,
    kUsageCopyDst = 1u << 4,
};

enum ShaderStage : uint32_t {
    kStageVertex = 1u << 0,
    kStageFragment = 1u << 1,
    kStageCompute = 1u << 2,
};

enum class Filter : uint8_t { Nearest, Linear };
enum class AddressMode : uint8_t { Repeat, ClampToEdge, MirroredRepeat };
enum class LoadOp : uint8_t { DontCare, Clear, Load };
enum class StoreOp : uint8_t { Store, Discard };
enum class CullMode : uint8_t { None, Front, Back };
enum class Topology : uint8_t { TriangleList, TriangleStrip, LineList };

// Objetos opacos: cada backend define a struct.
struct Texture;
struct Sampler;
struct BindGroupLayout;
struct BindGroup;
struct RenderPipeline;

struct TextureDesc {
    uint32_t width = 1, height = 1;
    Format format = Format::RGBA8Unorm;
    uint32_t usage = kUsageSampled;
};

struct SamplerDesc {
    Filter magFilter = Filter::Linear, minFilter = Filter::Linear;
    Filter mipFilter = Filter::Nearest;
    AddressMode address = AddressMode::ClampToEdge;
    float minLod = 0.0f, maxLod = 0.0f;
};

// Tipo de ligação. TextureSampler é a textura e o sampler juntos (um
// combined image sampler no Vulkan; no WebGPU vira duas ligações).
enum class BindingType : uint8_t { TextureSampler };

struct BindGroupLayoutEntry {
    uint32_t binding = 0;
    BindingType type = BindingType::TextureSampler;
    uint32_t stages = kStageFragment;
};

struct BindGroupEntry {
    uint32_t binding = 0;
    Texture* texture = nullptr;
    Sampler* sampler = nullptr;
};

struct RenderPipelineDesc {
    // Caminhos dos .spv, relativos a shaders/ (ShaderCompiler::loadSPIRV).
    const char* vertexShader = nullptr;
    const char* fragmentShader = nullptr;
    // Constantes de especialização do shader_lite no fragmento
    // (VulkanContext::liteSpecialization).
    bool liteSpecialization = false;
    std::vector<BindGroupLayout*> bindGroups;
    uint32_t pushSize = 0;
    uint32_t pushStages = kStageFragment;
    Topology topology = Topology::TriangleList;
    CullMode cull = CullMode::None;
    std::vector<Format> colorFormats;
    Format depthFormat = Format::Undefined;
};

struct ColorTarget {
    Texture* texture = nullptr;
    LoadOp load = LoadOp::DontCare;
    StoreOp store = StoreOp::Store;
    float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
};

struct RenderPassDesc {
    std::vector<ColorTarget> colors;
    uint32_t width = 0, height = 0; // área e viewport
};

// Gravação de comandos de um quadro. No Vulkan embrulha o VkCommandBuffer do
// quadro (os renderers ainda não migrados gravam no mesmo).
class CommandList {
public:
    void beginRenderPass(const RenderPassDesc& desc);
    void endRenderPass();
    void setPipeline(RenderPipeline* pipeline);
    void setBindGroup(uint32_t index, BindGroup* group);
    void setPush(const void* data, uint32_t size);
    void draw(uint32_t vertexCount, uint32_t instanceCount = 1, uint32_t firstVertex = 0);

    explicit CommandList(VkCommandBuffer cmd) : m_cmd(cmd) {}
    VkCommandBuffer vk() const { return m_cmd; }

private:
    VkCommandBuffer m_cmd = VK_NULL_HANDLE;
    RenderPipeline* m_pipeline = nullptr;
    std::vector<Texture*> m_passTargets;
};

class Device {
public:
    Texture* createTexture(const TextureDesc& desc);
    void destroy(Texture* texture);
    Sampler* createSampler(const SamplerDesc& desc);
    void destroy(Sampler* sampler);
    BindGroupLayout* createBindGroupLayout(const std::vector<BindGroupLayoutEntry>& entries);
    void destroy(BindGroupLayout* layout);
    BindGroup* createBindGroup(BindGroupLayout* layout, const std::vector<BindGroupEntry>& entries);
    void destroy(BindGroup* group);
    RenderPipeline* createRenderPipeline(const RenderPipelineDesc& desc);
    void destroy(RenderPipeline* pipeline);

    // Espera a GPU terminar tudo (resize, recriação de alvos).
    void waitIdle();
    Format swapchainFormat() const;

    explicit Device(VulkanContext* ctx) : m_ctx(ctx) {}
    VulkanContext* vulkan() const { return m_ctx; }
    // Textura de fora da RHI (não é destruída pela RHI; destroy só solta o
    // embrulho). `rest` = layout em que o resto do motor a deixa e espera de
    // volta (SHADER_READ_ONLY para entrada amostrada, COLOR_ATTACHMENT para a
    // imagem da swapchain no fim do quadro).
    Texture* wrapVulkan(VkImage image, VkImageView view, Format format, uint32_t width, uint32_t height,
                        VkImageLayout rest);
    static Format fromVulkan(VkFormat format);
    static VkFormat toVulkan(Format format);

private:
    VulkanContext* m_ctx = nullptr;
};

} // namespace eruption::rhi
