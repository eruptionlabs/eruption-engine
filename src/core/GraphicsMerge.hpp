#pragma once
// data/graphics.json do PROJETO e' parcial: mesclado sobre o do motor por
// JSON merge patch (objeto mescla chave a chave, o resto troca), e
// "every_preset": {...} vale para todos os presets. A mesma regra serve ao
// motor (Engine, ao abrir o projeto) e ao export (o jogo exportado abre sem
// projeto, entao leva o arquivo ja' mesclado).

#include <nlohmann/json.hpp>

namespace eruption {

inline void mergeGraphicsPatch(nlohmann::json& root, nlohmann::json patch) {
    const nlohmann::json every = patch.contains("every_preset") ? patch["every_preset"] : nlohmann::json();
    patch.erase("every_preset");
    root.merge_patch(patch);
    if (every.is_object() && root.contains("presets") && root["presets"].is_object())
        for (auto& [name, preset] : root["presets"].items()) preset.merge_patch(every);
}

} // namespace eruption
