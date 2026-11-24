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

#include "core/SpriteContact.hpp"

namespace eruption {

class FootstepEffects {
public:
    struct Live {
        uint32_t id = 0;
        double expire = 0.0;
        uint8_t pool = 0;      // kPrint, kDust ou kTrail: cada uma com o seu teto
        std::string owner;     // de quem e' o rastro (teto por personagem)
    };
    enum Pool : uint8_t { kPrint = 0, kDust = 1, kTrail = 2 };
    static constexpr size_t kMaxPrints = 64; // marcas vivas ao mesmo tempo
    static constexpr size_t kMaxDust = 24;   // poeiras vivas ao mesmo tempo
    static constexpr size_t kMaxTrail = 128; // trechos de rastro vivos (arrasto), reserva a parte das pisadas
    static constexpr size_t kMaxTrailPerOwner = 48; // e no maximo isto por personagem
    static constexpr size_t kMaxVariants = 512; // efeitos distintos montados
    static constexpr size_t kMaxMasks = 256;    // formas de contato distintas

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

    // Formas de contato geradas ("contact:<hash>", SpriteContact.hpp): o VFX as
    // le' pelo mesmo caminho das texturas, em Engine::vfxInitInfo.
    std::unordered_map<std::string, contact::Mask> masks;

    std::vector<Live> live;
    std::vector<uint32_t> ground;     // instancias de chao do mapa
    uint32_t groundEpoch = 0xFFFFFFFFu;
    std::unordered_map<std::string, std::string> effectNames; // chave dos parametros -> efeito do VFX
    double clock = 0.0;               // s
};

} // namespace eruption
