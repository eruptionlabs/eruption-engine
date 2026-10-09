#pragma once

#include "renderer/VulkanContext.hpp"
#include "math/Types.hpp"

#include <array>
#include <vector>

namespace eruption {

// AMD FSR 3.1 (upscaler temporal), com host proprio em Vulkan. Os passes de
// GPU sao os GLSL do SDK v1.1.4 (third_party/fsr3, MIT), compilados pelo
// CMake em shaders/fsr3/<passe>_{hdr,ldr}.comp.spv. Todo o lado de CPU (as
// imagens internas, as constantes, a ordem dos passes, o ping-pong de
// historico) foi escrito aqui a partir do codigo de referencia do SDK.
//
// Entradas, todas na resolucao de RENDER e em SHADER_READ_ONLY_OPTIMAL:
//   - cor rasterizada COM jitter (linear HDR se hdr=true, ja' tonemapeada se
//     hdr=false);
//   - depth 0..1 nao invertido (o do G-buffer);
//   - motion vectors do CameraMotion (UV, atual -> anterior, sem jitter).
// Saida: RGBA16F na resolucao de DISPLAY, mesmo espaco de cor da entrada,
// deixada em SHADER_READ_ONLY_OPTIMAL ao fim de dispatch().
class Fsr3Upscaler {
public:
    struct FrameParams {
        Vec2 jitterPx{0.0f};       // deslocamento do conteudo em pixels de render (x dir., y baixo)
        float nearZ = 0.1f, farZ = 1000.0f;
        float fovY = 1.0f;         // radianos, vertical
        float frameTimeMs = 16.6f;
        float sharpness = 0.0f;    // 0 = sem RCAS; (0,1] = RCAS ligado
        float preExposure = 1.0f;
        float metersPerUnit = 1.0f;
        bool reset = false;        // corte de camera / teleporte
        bool debugView = false;    // sobrepoe a visao de debug do SDK na saida
    };

    // true se o device tem o que os passes exigem (ver VulkanContext::fsrSupported).
    static bool supported(const VulkanContext* ctx) { return ctx && ctx->fsrSupported(); }

    bool init(VulkanContext* ctx, bool hdr, uint32_t renderW, uint32_t renderH,
              uint32_t displayW, uint32_t displayH);
    void shutdown();
    // Recria tudo nos tamanhos novos. O proximo dispatch e' um reset.
    void resize(uint32_t renderW, uint32_t renderH, uint32_t displayW, uint32_t displayH);
    // As views precisam ser rechamadas sempre que o dono as recriar.
    // `reactive` (opcional, R8 0..1 em SHADER_READ_ONLY): onde o FSR nao deve
    // confiar no historico (ex.: sprites redesenhados depois por cima).
    // write=false so' guarda as views (o resize logo depois escreve os sets).
    void bindInputs(VkImageView color, VkImageView depth, VkImageView motion,
                    VkImageView reactive = VK_NULL_HANDLE, bool write = true);

    void dispatch(VkCommandBuffer cmd, const FrameParams& params);

    VkImage outputImage() const { return m_output.image; }
    VkImageView outputView() const { return m_output.view; }
    static constexpr VkFormat kOutputFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    uint32_t renderWidth() const { return m_renderW; }
    uint32_t renderHeight() const { return m_renderH; }
    uint32_t displayWidth() const { return m_displayW; }
    uint32_t displayHeight() const { return m_displayH; }
    bool hdr() const { return m_hdr; }

    // Fases de jitter que o FSR espera para esta razao de escala (8 * r^2).
    static uint32_t jitterPhaseCount(uint32_t renderW, uint32_t displayW);

    enum Pass : uint32_t {
        PrepareInputs, LumaPyramid, ShadingChangePyramid, ShadingChange,
        PrepareReactivity, LumaInstability, Accumulate, Rcas, DebugView, PassCount
    };

private:
    struct Tex {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation alloc = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;            // todos os mips
        std::array<VkImageView, 6> mipViews{};        // so' SPD_MIPS: um nivel por view
        VkFormat format = VK_FORMAT_UNDEFINED;
        uint32_t width = 0, height = 0, mips = 1;
    };
    // O que cada binding de cada passe le/escreve, resolvido por paridade.
    enum class Res : uint32_t {
        InColor, InDepth, InMotion, InExposure, InReactive, InTransparency,
        DilatedMotion, DilatedDepth, ReconPrevDepth, Intermediate16, CurrentLuma, PreviousLuma,
        SpdAtomic, FrameInfo, SpdMip0, SpdMip1, SpdMip2, SpdMip3, SpdMip4, SpdMip5, SpdMipsAll,
        FarthestMip1, ShadingChangeImg, DilatedReactive, NewLocks, AccumSrv, AccumUav,
        LumaHistSrv, LumaHistUav, UpscaledSrv, UpscaledUav, LanczosLut, Output,
        Cb0, CbSpd, CbRcas
    };
    struct Binding {
        uint32_t binding;
        VkDescriptorType type;
        Res res;
    };
    struct PassObjects {
        std::vector<Binding> bindings;
        VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkPipeline pipelineAlt = VK_NULL_HANDLE; // accumulate com nitidez
        std::array<VkDescriptorSet, 2> sets{};    // [paridade]
    };

    bool createStatic();
    void destroyStatic();
    bool createSizeDependent();
    void destroySizeDependent();
    bool createTex(Tex& t, uint32_t w, uint32_t h, VkFormat fmt, uint32_t mips = 1);
    void destroyTex(Tex& t);
    void initialUploadsAndClears();
    void writeSets();
    VkImageView viewFor(Res r, uint32_t parity) const;
    VkImageLayout layoutFor(Res r) const;

    VulkanContext* m_ctx = nullptr;
    bool m_hdr = true;
    uint32_t m_renderW = 0, m_renderH = 0, m_displayW = 0, m_displayH = 0;
    VkImageView m_inColor = VK_NULL_HANDLE, m_inDepth = VK_NULL_HANDLE, m_inMotion = VK_NULL_HANDLE;
    VkImageView m_inReactive = VK_NULL_HANDLE;

    // Imagens internas (nomes = recursos do SDK).
    std::array<Tex, 2> m_accum, m_luma, m_lumaHist, m_upscaled;
    Tex m_intermediate16, m_shadingChange, m_newLocks, m_spdMips, m_farthestMip1, m_spdAtomic;
    Tex m_dilatedReactive, m_lanczosLut, m_defaultReactive, m_defaultExposure, m_frameInfo;
    Tex m_dilatedDepth, m_dilatedMotion, m_reconPrevDepth, m_output;
    VkImageLayout m_outputLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkSampler m_pointClamp = VK_NULL_HANDLE, m_linearClamp = VK_NULL_HANDLE;
    std::array<PassObjects, PassCount> m_passes{};
    VkDescriptorPool m_pool = VK_NULL_HANDLE;

    // UBO dinamico: [frame em voo][cb0 | spd | rcas], cada um alinhado.
    VkBuffer m_ubo = VK_NULL_HANDLE;
    VmaAllocation m_uboAlloc = VK_NULL_HANDLE;
    uint8_t* m_uboMapped = nullptr;
    VkDeviceSize m_uboStride = 256;

    // Estado entre frames (espelha FfxFsr3UpscalerContext_Private).
    struct Constants {
        int32_t renderSize[2];
        int32_t previousFrameRenderSize[2];
        int32_t upscaleSize[2];
        int32_t previousFrameUpscaleSize[2];
        int32_t maxRenderSize[2];
        int32_t maxUpscaleSize[2];
        float deviceToViewDepth[4];
        float jitterOffset[2];
        float previousFrameJitterOffset[2];
        float motionVectorScale[2];
        float downscaleFactor[2];
        float motionVectorJitterCancellation[2];
        float tanHalfFOV;
        float jitterPhaseCount;
        float deltaTime;
        float deltaPreExposure;
        float viewSpaceToMetersFactor;
        float frameIndex;
        float velocityFactor;
        float reactivenessScale;
        float shadingChangeScale;
        float accumulationAddedPerFrame;
        float minDisocclusionAccumulation;
    };
    static_assert(sizeof(Constants) == 148, "cbFSR3Upscaler (std140)");
    Constants m_c{};
    bool m_firstExecution = true;
    uint32_t m_resourceFrameIndex = 0;
    float m_preExposure = 0.0f, m_prevPreExposure = 0.0f;
};

} // namespace eruption
