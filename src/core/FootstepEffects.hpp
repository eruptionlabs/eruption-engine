#pragma once
// ESTADO DOS EFEITOS DE PASSO. O motor so' da' pecas genericas; que material
// faz o que e' logica do jogo (script): characters.on_step entrega o chao sob
// o pe', e world.footprint / world.dust tocam o efeito com os parametros dados.
// Aqui: o anel de efeitos vivos (limite fixo, os mais velhos saem), os efeitos
// do VFX ja' montados por parametros, e a lista de chao do mapa.
#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace eruption {

class FootstepEffects {
public:
    struct Live {
        uint32_t id = 0;
        double expire = 0.0;
        bool print = false;
    };
    static constexpr size_t kMaxPrints = 64; // marcas vivas ao mesmo tempo
    static constexpr size_t kMaxDust = 24;   // poeiras vivas ao mesmo tempo
    static constexpr size_t kMaxVariants = 512; // efeitos distintos montados

    void tick(float dt) {
        clock += double(dt);
        live.erase(std::remove_if(live.begin(), live.end(), [&](const Live& l) { return l.expire < clock; }),
                   live.end());
    }
    // Mapa novo: o VFX foi limpo, os ids vivos e a lista de chao caducam.
    void onMapLoaded() {
        live.clear();
        ground.clear();
        groundEpoch = 0xFFFFFFFFu;
        effectNames.clear();
    }

    std::vector<Live> live;
    std::vector<uint32_t> ground;     // instancias de chao do mapa
    uint32_t groundEpoch = 0xFFFFFFFFu;
    std::unordered_map<std::string, std::string> effectNames; // chave dos parametros -> efeito do VFX
    double clock = 0.0;               // s
};

} // namespace eruption
