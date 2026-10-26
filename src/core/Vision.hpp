#pragma once

#include "math/Types.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace eruption {

// Visao de equipe: o mapa fica sempre aberto, mas fora do alcance das fontes
// (personagens, sentinelas) ele escurece e o que se move ali some. As fontes
// sao discos no plano XZ; as zonas mudam a regra num disco:
//   block  - so' uma fonte que esta' DENTRO da zona ve o que esta' dentro;
//   reveal - dentro da zona tudo e' visivel, sem fonte.
// visible() e' a regra exata e vale no mapa inteiro (minimapa, esconder
// personagem). raster() escreve a mesma regra, com borda suave e meia-vida,
// numa janela de texels que a composicao amostra para escurecer a tela.
class Vision {
public:
    void setEnabled(bool on) { m_enabled = on; }
    bool enabled() const { return m_enabled; }
    // Fracao do brilho que sobra fora da visao (0..1). Nunca preto de fato:
    // abaixo de 0,05 a tela deixa de ler o terreno.
    void setDim(float dim);
    float dim() const { return m_dim; }
    // Meia-vida (s) com que cada texel segue o alvo; 0 = sem atraso.
    void setFade(float halfLife) { m_fade = halfLife > 0.0f ? halfLife : 0.0f; }
    // So' as fontes desta equipe contam (uma textura so', a da equipe local).
    void setTeam(int team) { m_team = team; }
    int team() const { return m_team; }
    // Lado da janela em unidades de mundo e texels por lado.
    void setWindow(float size, uint32_t texels);
    float windowSize() const { return m_window; }
    uint32_t texels() const { return m_res; }

    // O mesmo id move a fonte (ou a zona) em vez de criar outra.
    void setSource(const std::string& id, const Vec2& pos, float radius, int team);
    bool removeSource(const std::string& id);
    void setZone(const std::string& id, const Vec2& pos, float radius, bool reveal);
    bool removeZone(const std::string& id);
    size_t sourceCount() const { return m_sources.size(); }
    size_t zoneCount() const { return m_zones.size(); }
    // Volta ao padrao (desligada, sem fontes nem zonas); a janela fica.
    void reset();

    // Regra exata, sem borda nem atraso: o raio da fonte e' a linha de 50%
    // da borda desenhada. Desligada, tudo e' visivel.
    bool visible(const Vec2& p) const;

    // Janela centrada em `center` (encaixada na grade de texels, para a
    // borda nao tremer quando a camera anda) e um passo de meia-vida de `dt`.
    // Texels que a janela acabou de cobrir entram ja' no alvo.
    void raster(const Vec2& center, float dt);
    const std::vector<uint8_t>& image() const { return m_image; }
    // Canto (x, z) minimo da janela do ultimo raster.
    Vec2 origin() const { return m_origin; }
    // Mapa inteiro (minimapa), pela regra exata e sem meia-vida: n x n texels
    // de `size` u a partir de `origin`, com o ESCURO de cada um (alfa de uma
    // camada preta: (1 - dim)(1 - visao)); fora de `keep` (xmin, zmin, xmax,
    // zmax) fica 0.
    void rasterShade(const Vec2& origin, float size, uint32_t n, const Vec4& keep, std::vector<uint8_t>& out) const;

private:
    struct Source { Vec2 pos; float radius; int team; int zone; };
    struct Zone { Vec2 pos; float radius; bool reveal; };

    int blockZoneAt(const Vec2& p) const;
    void refreshSourceZones();
    float edgeWidth(float radius, float texel) const;
    void rasterTarget(const Vec2& origin, float texel, uint32_t n, std::vector<float>& target,
                      std::vector<int16_t>& zoneMap) const;

    bool m_enabled = false;
    float m_dim = 0.35f;
    float m_fade = 0.15f;
    int m_team = 1;
    float m_window = 1024.0f;
    uint32_t m_res = 256;

    // Planas (o raster e o visible() percorrem tudo a cada quadro); o mapa de
    // id so' serve para mover e remover.
    std::vector<Source> m_sources;
    std::vector<std::string> m_sourceIds;
    std::unordered_map<std::string, size_t> m_sourceIndex;
    std::vector<Zone> m_zones;
    std::vector<std::string> m_zoneIds;
    std::unordered_map<std::string, size_t> m_zoneIndex;

    std::vector<float> m_target;
    std::vector<float> m_current;
    std::vector<int16_t> m_zoneMap;
    std::vector<uint8_t> m_image;
    // Rascunho do rasterShade (const por fora: so' memoria reaproveitada).
    mutable std::vector<float> m_mapTarget;
    mutable std::vector<int16_t> m_mapZones;
    Vec2 m_origin = Vec2(0.0f);
    bool m_hasHistory = false;
};

} // namespace eruption
