#pragma once

#include "renderer/VulkanContext.hpp"
#include "renderer/BindlessDescriptor.hpp"
#include "formats/TerrainParser.hpp"
#include <vector>

namespace eruption {

// POPUP DE PREVIEW DE TEXTURA (F1 -> Object Manager -> Textures, clicar num
// item). Renderiza uma UV-sphere, tesselada, com a textura clicada, numa
// imagem OFFSCREEN PROPRIA - nao entra no mapa, nao mexe em instancia
// nenhuma, nao recarrega nada. Trocar de textura e' so' reescrever o
// texIndex/pbrIndex do buffer de vertice (mapeado, ~100 KB) e pronto, no
// mesmo frame. Reusa model.vert/tesc/tese SEM MODIFICACAO (o deslocamento
// que aparece aqui e' o mesmo, byte a byte, que o chao real calcula) - so'
// o fragment e' proprio (sphere_preview.frag), porque model.frag escreve
// no G-buffer de 5 anexos da cena principal, e reusar aquilo exigiria um
// G-buffer + passe de composicao inteiros so' pra mostrar uma esfera.
class SpherePreview {
public:
    bool init(VulkanContext* ctx, BindlessDescriptor* bindless,
              VkDescriptorSetLayout frameUboLayout, VkDescriptorSet frameUboSet);
    void shutdown();

    // dispScale: mesmo campo que ModelMeshGPU::dispScale (push.uvScale.w) -
    // 0 = superficie nunca desloca. Passa o valor REAL da categoria do
    // material (dispScaleForCategory() em PbrMaterialProfile.hpp) - antes
    // disto o preview usava 1.0 fixo pra' qualquer textura clicada, entao
    // uma pedra ou madeira no popup deslocava MAIS que a mesma textura no
    // mapa (autor: "o chao continua diferente do preview").
    void setMaterial(uint32_t texIndex, uint32_t pbrIndex, float dispScale, uint32_t normalIndex = 0);

    bool active() const { return m_active; }
    void setActive(bool on) { m_active = on; }
    void toggle() { m_active = !m_active; }

    // cameraPos: mundo real da camera principal. O deslocamento no shader
    // (model.tese) le' a distancia ate' u_cameraPos do FrameUBO COMPARTILHADO
    // da cena principal pra decidir a amplitude pela curva de distancia -
    // sem colocar a esfera PERTO da camera de verdade, a curva julgaria
    // "longe" e o deslocamento sairia zero ou fraco.
    void render(VkCommandBuffer cmd, float dt, const Vec3& cameraPos);

    void* imguiTextureId() const { return m_imguiTextureId; }
    uint32_t size() const { return kSize; }
    const std::string& materialLabel() const { return m_materialLabel; }
    void setMaterialLabel(const std::string& s) { m_materialLabel = s; }

    // CAMERA DO POPUP, ORBITAVEL (pedido do autor: "clico ela nao move, tem
    // que poder mover" / "so' da pra ver de perto, fica ruim"). A esfera
    // continua girando sozinha (m_rotation) - isto e' so' a camera que olha
    // pra ela, arrastavel com o botao esquerdo e com zoom por scroll,
    // chamado de ImGui.cpp enquanto o mouse esta' sobre a imagem.
    void orbitDrag(float dYawPixels, float dPitchPixels);
    void zoom(float wheelDelta);
    // Pose fixa da camera do popup + giro automatico liga/desliga - pra'
    // captura repetivel (bancada) e pra' comparar formas no mesmo angulo.
    void setOrbitDegrees(float yawDeg, float pitchDeg) {
        m_camYaw = yawDeg * 0.01745329252f;
        m_camPitch = pitchDeg * 0.01745329252f;
    }
    void setOrbitDistance(float d) { m_camDist = d < kMinDist ? kMinDist : (d > kMaxDist ? kMaxDist : d); }
    // UV por unidade de mundo da textura no mapa (0 = desconhecida, usa a UV
    // nativa da forma). Liga/desliga a escala do mundo no popup.
    void setWorldUvPerUnit(float v) { m_worldUvPerUnit = v; }
    void setWorldScale(bool on) { m_worldScale = on; }
    bool worldScale() const { return m_worldScale; }
    bool hasWorldUvPerUnit() const { return m_worldUvPerUnit > 0.0f; }
    void setSpin(bool on) { m_spin = on; if (!on) m_rotation = 0.0f; }
    bool spin() const { return m_spin; }

    // ESFERA OU CUBO (pedido do autor: comparar com chao real - superficie
    // curva confunde "curvatura da esfera" com "deslocamento de verdade".
    // O cubo tem face PLANA, igual o chao: se o chao fica reto e o cubo
    // mostra relevo com a MESMA textura, o problema e' no chao, nao na
    // textura/gain). Reaplica texIndex/pbrIndex/dispScale atuais na malha
    // nova - trocar de forma nao deveria perder o material selecionado.
    enum class Shape { Sphere, Cube };
    void setShape(Shape s);
    Shape shape() const { return m_shape; }

private:
    static constexpr uint32_t kSize = 384;
    static constexpr int kStacks = 32;
    static constexpr int kSlices = 32;
    static constexpr float kRadius = 2.0f;
    // Subdivisao por face: (N+1)^2*6 vertices e N*N*6*6 indices tem que caber
    // no buffer ja' alocado pra' esfera (1089 v / 6144 i) - N=10 da' 726 v /
    // 3600 i, com folga, sem precisar realocar buffer ao trocar de forma.
    static constexpr int kCubeGrid = 10;

    void buildSphereGeometry();
    void buildCubeGeometry();
    void uploadVertices();
    void uploadIndices();
    void applyCurrentMaterialToVertices();
    bool createTargets();
    bool createInstanceBuffer();
    bool createPipeline();

    VulkanContext* m_ctx = nullptr;
    BindlessDescriptor* m_bindless = nullptr;
    VkDescriptorSetLayout m_frameUboLayout = VK_NULL_HANDLE;
    VkDescriptorSet m_frameUboSet = VK_NULL_HANDLE;

    VkImage m_colorImage = VK_NULL_HANDLE;
    VmaAllocation m_colorAlloc = VK_NULL_HANDLE;
    VkImageView m_colorView = VK_NULL_HANDLE;
    VkImage m_depthImage = VK_NULL_HANDLE;
    VmaAllocation m_depthAlloc = VK_NULL_HANDLE;
    VkImageView m_depthView = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;
    void* m_imguiTextureId = nullptr;
    bool m_firstFrame = true; // primeira transicao parte de UNDEFINED de verdade

    std::vector<TerrainVertex> m_vertices;
    std::vector<uint32_t> m_indices;
    VkBuffer m_vertexBuffer = VK_NULL_HANDLE;
    VmaAllocation m_vertexAlloc = VK_NULL_HANDLE;
    void* m_vertexMapped = nullptr;
    VkBuffer m_indexBuffer = VK_NULL_HANDLE;
    VmaAllocation m_indexAlloc = VK_NULL_HANDLE;

    struct GpuInstance { Mat4 model; Vec4 uvTranslateRot; Vec4 uvScaleDisp; };
    VkBuffer m_instBuffer = VK_NULL_HANDLE;
    VmaAllocation m_instAlloc = VK_NULL_HANDLE;
    void* m_instMapped = nullptr;
    VkDescriptorSetLayout m_instLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_ownPool = VK_NULL_HANDLE;
    VkDescriptorSet m_instSet = VK_NULL_HANDLE;

    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkPipeline m_tessPipeline = VK_NULL_HANDLE;

    float m_rotation = 0.0f;
    bool m_spin = true;
    float m_worldUvPerUnit = 0.0f;
    bool m_worldScale = true;
    // Orbita da camera do popup - independente da rotacao da esfera acima
    // (aquela e' a MALHA girando sozinha; isto e' o OLHO do popup, que o
    // usuario controla). Distancia default mais generosa que a versao fixa
    // anterior (era ~4,9 u, ficava colado na esfera).
    float m_camYaw = 0.7853981634f;   // 45 graus
    float m_camPitch = 0.4363323f;    // 25 graus
    float m_camDist = 8.0f;
    static constexpr float kMinDist = 3.0f;
    static constexpr float kMaxDist = 30.0f;
    std::string m_materialLabel;
    float m_dispScale = 1.0f;
    uint32_t m_curTexIndex = 0;
    uint32_t m_curPbrIndex = 0;
    uint32_t m_curNormalIndex = 0;
    Shape m_shape = Shape::Sphere;
    bool m_active = false;
    bool m_initialized = false;
};

} // namespace eruption
