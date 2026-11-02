#pragma once

// Particulas do navegador (WebGL2 nao tem compute): a mesma simulacao do
// vfx_emit.comp + vfx_sim.comp, na CPU. A emissao continua no VfxSystem
// (update() conta quem nasce e monta os emissores por slot, como no
// desktop); aqui so' nasce, anda, morre e vira quad ordenado para o desenho
// em core/EngineWebScene.cpp. Pool em SoA, sem chamada virtual por particula.
//
// Fora daqui (aproximado ou ausente): colisao com a profundidade e com a
// grade do chao, sub-emissores, grafos, particulas de malha (nao desenham),
// fita (vira billboard) e o poligono recortado (quad inteiro).

#include "math/Types.hpp"
#include "renderer/vfx/VfxSystem.hpp"

#include <cstdint>
#include <vector>

namespace eruption {

// Vertice do quad de uma particula (4 por particula, 18 floats).
struct WebVfxVertex {
    float pos[3];    // mundo
    float uv[4];     // quadro atual (xy) e o seguinte (zw) da folha
    float color[4];  // rgb linear ja' com luz e emissivo, a com o fade de perto
    float misc[4];   // x mistura do proximo quadro, y unidade de textura (-1 = disco), z borda suave (mundo), w profundidade (w do clip)
    float corner[3]; // xy canto do quad (-1..1, disco procedural), z mistura (0 aditiva, 1 premultiplicada, 2 alfa)
};

struct WebVfxView {
    Mat4 viewProj{1.0f}; // so' a linha do w (profundidade de vista) e' usada
    Vec3 camPos{0.0f};
    Vec3 camRight{1.0f, 0.0f, 0.0f};
    Vec3 camUp{0.0f, 1.0f, 0.0f};
    float nearPlane = 0.1f;
    float proj11 = 1.0f;   // teto de tamanho na tela
    Vec4 sunDir{0.0f, -1.0f, 0.0f, 1.0f}; // xyz direcao em que a luz viaja, w intensidade
    Vec3 sunColor{1.0f};
    Vec4 ambient{0.5f, 0.55f, 0.6f, 0.5f}; // xyz cor do ceu, w intensidade
};

class WebVfxSim {
public:
    // Um frame: nasce o que os pedidos mandam e simula todo o pool.
    void step(const VfxEmitterGpu* emitters, uint32_t slotCount, const std::vector<VfxSpawnRequestGpu>& requests,
              float dt, float time, uint32_t capacity);
    // Quads de tras para frente (a mistura alfa precisa do fundo primeiro).
    // `quadTex` recebe o slot bindless de cada quad (0xFFFFFFFF = disco).
    // Os de meia resolucao (lowRes: fumaca, poeira) vem primeiro; devolve
    // quantos sao.
    uint32_t buildQuads(const VfxEmitterGpu* emitters, uint32_t slotCount, const WebVfxView& view,
                        std::vector<WebVfxVertex>& verts, std::vector<uint32_t>& quadTex);
    void clear();
    uint32_t alive() const { return uint32_t(m_age.size()); }

private:
    void spawnOne(const VfxEmitterGpu& e, uint32_t slot, const Vec3& origin, uint32_t seed, float preAge);
    void kill(size_t i);

    // Pool em SoA: uma entrada por particula viva (remocao por troca com a ultima).
    std::vector<Vec3> m_pos, m_vel;
    std::vector<float> m_age, m_life, m_size, m_rot, m_angVel, m_startFrame;
    std::vector<uint32_t> m_slot, m_gen, m_seed;
    // Ordenacao do desenho (reaproveitada entre frames).
    std::vector<float> m_sortKey;
    std::vector<uint32_t> m_order;
};

} // namespace eruption
