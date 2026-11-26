#pragma once
// ESTADO DO CHAO SOBRE A VEGETACAO: o estado de uma celula (neve, geada, molhado,
// queimado; core/GroundState.hpp) e' uma QUALIDADE que muda a substancia de
// arvore, arbusto e capim, nao so' a cor. Galho carregado de neve fica denso e
// quase nao balanca; capim coberto fica parado; mato queimado e' seco e
// quebradico e sacode mais.
//
// Esta e' a LOGICA PURA (sem GPU, sem Engine): a tabela por tipo, lida de
// data/ground_state.json, e a mistura pela cobertura. A CPU amostra a cobertura
// na raiz de cada instancia vegetal (1 leitura por instancia, sem descritor novo)
// e entrega o resultado ao vertex shader pelo buffer de instancias
// (ModelRenderer::updateGroundVegetation).
#include <string>

namespace eruption::ground {

// Efeito de UM tipo com cobertura cheia (1.0 = sem efeito, nos fatores).
struct VegEffect {
    float ampScale = 1.0f;      // x amplitude do balanco (vento do clima, rajada, swirl)
    float freqScale = 1.0f;     // x frequencia do balanco (galho pesado oscila devagar)
    float densityBoost = 0.0f;  // copa mais cheia: baixa o limiar do alfa-teste em 0..1 (negativo = rarefeita)
    float albedoLift = 0.0f;    // 0..1: quanto a folha clareia rumo ao branco-neve
};

// Canais da imagem do estado, na ordem do shader: 0 queimado, 1 geada, 2 molhado, 3 neve.
// O gelo sobre a agua (ice_water) divide o canal da geada e nao tem planta por cima:
// o tipo nao tem linha propria na tabela.
inline constexpr int kVegChannels = 4;

class VegetationTable {
public:
    // Le `text` (JSON, chave "vegetation": {"snow": {...}, ...}). So' sobrepoe o que o
    // arquivo declara; false se nao for JSON valido (a tabela fica como estava).
    bool loadFromJson(const std::string& text);
    // Mistura os efeitos pela cobertura (0..1 por canal): fatores multiplicam cada um
    // na proporcao da cobertura, densidade e clareamento somam. Sem cobertura: neutro.
    VegEffect blend(const float cover[kVegChannels]) const;
    // Tudo neutro (tabela nao carregada)? Entao o motor nem varre as instancias.
    bool neutral() const;
    const VegEffect& channel(int c) const { return m_ch[c]; }
    void setChannel(int c, const VegEffect& e) { m_ch[c] = e; }

private:
    VegEffect m_ch[kVegChannels];
};

// Nome do tipo -> canal (0..3); -1 se nao tem linha na tabela. "ice_water" cai na geada.
int vegChannelForName(const std::string& name);

} // namespace eruption::ground
