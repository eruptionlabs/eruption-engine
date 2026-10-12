#pragma once

// VISUALIZADOR DE MODELO do editor: um .glb (ou um nó dele) sozinho, numa
// imagem própria, com a malha, o LOD e as texturas de verdade do motor e luz
// de estúdio fixa. Câmera orbital própria, nível de LOD escolhido à mão,
// aramado, normais, UV e as animações de modelos com esqueleto.
#include "math/Types.hpp"
#include "renderer/VulkanContext.hpp"

#include <string>
#include <vector>

namespace eruption {

class BindlessDescriptor;
class ModelRenderer;

class ModelPreview {
public:
    bool init(VulkanContext* ctx, BindlessDescriptor* bindless, ModelRenderer* models,
              VkDescriptorSetLayout frameUboLayout, VkDescriptorSet frameUboSet);
    void shutdown();

    // O que mostrar: um asset já carregado no ModelRenderer; node < 0 = todos.
    bool open(const std::string& assetPath, int node);
    void close();
    bool hasTarget() const { return !m_nodes.empty(); }
    const std::string& assetPath() const { return m_path; }
    int node() const { return m_node; }

    // Uma vez por frame, no command buffer do frame (fora de passe de render).
    void render(VkCommandBuffer cmd, float dt);
    // Tamanho da imagem (recria os alvos quando muda).
    void setSize(uint32_t w, uint32_t h);
    void* texture() const { return m_texture; }
    uint32_t width() const { return m_w; }
    uint32_t height() const { return m_h; }

    // Câmera.
    void orbit(float dx, float dy);
    void pan(float dx, float dy);
    void zoom(float wheel);
    void frame();

    enum class Mode { Lit = 0, Color = 1, Normals = 2, Uv = 3 };
    enum class Lod { Original = 0, Near = 1, Far = 2, Shadow = 3 };
    Mode mode = Mode::Lit;
    Lod lod = Lod::Original;
    bool wireframe = false;
    bool spin = false;
    bool alphaCut = true;
    float lightYaw = 40.0f, lightPitch = 50.0f; // graus
    bool wireframeSupported() const { return m_linePipeline != VK_NULL_HANDLE; }

    // Animações (modelo com esqueleto).
    std::vector<std::string> animations() const { return m_animations; }
    int animation = -1;
    float animationSpeed = 1.0f;

    // Números do que está na tela.
    struct Stats {
        size_t nodes = 0;
        uint64_t trisOriginal = 0, trisNear = 0, trisFar = 0, trisShadow = 0, trisDrawn = 0;
        float switchNear = 0.0f, switchFar = 0.0f;
        Vec3 size{0.0f};
        bool skinned = false, foliage = false, ground = false, shadowOpaque = true;
    };
    Stats stats() const;
    float cameraDistance() const { return m_dist; }

private:
    struct Node { uint32_t mesh; Mat4 matrix; int skin = -1; };
    // Layout do EngInstance de model_vert.glsl (prevModel só é lido com velocidade).
    struct GpuInstance { Mat4 model; Vec4 uvTranslateRot; Vec4 uvScaleDisp; Mat4 prevModel; };
    static constexpr uint32_t kMaxNodes = 512;

    bool createPipelines();
    bool createInstances();
    void destroyTargets();
    bool createTargets();
    void computeBounds();

    VulkanContext* m_ctx = nullptr;
    BindlessDescriptor* m_bindless = nullptr;
    ModelRenderer* m_models = nullptr;
    VkDescriptorSetLayout m_frameUboLayout = VK_NULL_HANDLE;
    VkDescriptorSet m_frameUboSet = VK_NULL_HANDLE;

    VkPipelineLayout m_layout = VK_NULL_HANDLE;
    VkPipeline m_fillPipeline = VK_NULL_HANDLE;
    VkPipeline m_linePipeline = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_instLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    VkDescriptorSet m_instSets[VulkanContext::MAX_FRAMES_IN_FLIGHT] = {};
    VkBuffer m_instBuffer = VK_NULL_HANDLE;
    VmaAllocation m_instAlloc = VK_NULL_HANDLE;
    GpuInstance* m_instMapped = nullptr;

    uint32_t m_w = 0, m_h = 0, m_wantW = 640, m_wantH = 480;
    VkImage m_color = VK_NULL_HANDLE, m_depth = VK_NULL_HANDLE;
    VmaAllocation m_colorAlloc = VK_NULL_HANDLE, m_depthAlloc = VK_NULL_HANDLE;
    VkImageView m_colorView = VK_NULL_HANDLE, m_depthView = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;
    void* m_texture = nullptr;
    bool m_colorReadable = false; // já passou por um frame (layout de leitura)

    std::string m_path;
    int m_node = -1;
    bool m_flipY = false;
    std::vector<Node> m_nodes;
    std::vector<std::string> m_animations;
    int m_playing = -2;
    float m_playingSpeed = 1.0f;
    Vec3 m_boundsMin{0.0f}, m_boundsMax{0.0f};
    Vec3 m_target{0.0f};
    float m_yaw = 0.6f, m_pitch = 0.35f, m_dist = 10.0f;
    float m_spinAngle = 0.0f;
};

} // namespace eruption
