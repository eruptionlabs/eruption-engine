#pragma once

// PARTICULAS DE AMBIENTE: o "ar" da cena - poeira que brilha no facho de sol,
// folhas caindo, vaga-lumes de noite, brasas subindo de chao quente. Nao e'
// um sistema novo: cada camada e' um efeito do VfxSystem montado aqui (sem
// arquivo), uma caixa de nascimento presa ao FOCO da camera. A particula,
// depois de nascer, fica no mundo (a camera anda e ela fica para tras e
// morre no fim da vida), entao nada desliza junto com a tela.
//
// Custo: o do VfxSystem para algumas centenas de particulas (um draw para
// tudo). O preset escala a densidade ("ambient_particles", 0 desliga).

#include "math/Types.hpp"

#include <string>
#include <vector>

namespace eruption {

class VfxSystem;

struct AmbientLayer {
    enum class Kind { Dust, Leaves, Fireflies, Embers };
    enum class When { Any, Day, Night };
    Kind kind = Kind::Dust;
    When when = When::Any;
    float density = 1.0f; // multiplica a contagem padrao do tipo
    float radius = 0.0f;  // m, meia largura da caixa em volta do foco (0 = do tipo)
    float height = 0.0f;  // m, altura da caixa acima do foco (0 = do tipo)
    float size = 1.0f;    // multiplica o tamanho
    Vec3 color{1.0f};
    bool hasColor = false; // recolore (mantem o brilho de cada chave)

    // "dust", "leaves", "fireflies", "embers"; false = nome desconhecido.
    static bool kindFromName(const std::string& name, Kind& out);
};

class AmbientParticles {
public:
    // Troca as camadas (as particulas vivas das antigas terminam a vida).
    void setLayers(VfxSystem& vfx, const std::vector<AmbientLayer>& layers);
    const std::vector<AmbientLayer>& layers() const { return m_layers; }
    // Escala do preset grafico ("ambient_particles"; 0 desliga).
    void setDensityScale(float s) { m_presetScale = s; }
    // Por quadro, antes de VfxSystem::update: segue o foco e acende/apaga as
    // camadas de dia/noite (`night` 0..1).
    // Instancia que sumiu (VfxSystem::clear na troca de mapa) e' recriada aqui.
    void update(VfxSystem& vfx, const Vec3& focus, float night);

private:
    struct Live {
        uint32_t id = 0;
        std::string effect;
        float lastScale = -1.0f;
        float yOffset = 0.0f; // m, centro da caixa acima do foco
    };
    void spawnLayer(VfxSystem& vfx, size_t i, const Vec3& focus);

    std::vector<AmbientLayer> m_layers;
    std::vector<Live> m_live;
    float m_presetScale = 1.0f;
    uint32_t m_generation = 0; // nome de efeito novo a cada setLayers
};

} // namespace eruption
