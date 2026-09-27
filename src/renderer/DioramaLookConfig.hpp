#pragma once

// SHIM TEMPORARIO.
//
// A struct foi renomeada para LookConfig (renderer/LookConfig.hpp): diorama nao
// e' o que ela e', e' UM dos presets dela. Este cabecalho existe so' para
// src/core/Engine.* e src/renderer/PostProcessor.*, que ainda usam o nome
// antigo e estao com trabalho em voo de OUTRO AGENTE - trocar o nome la' agora
// atropelaria mudanca nao commitada dele (regra de ouro, secao 0 do
// PROTOCOLO.cai).
//
// PARA REMOVER: troque `DioramaLookConfig` por `LookConfig` e o include por
// "renderer/LookConfig.hpp" em Engine.hpp, Engine.cpp, PostProcessor.hpp,
// PostProcessor.cpp e game/PlayerController.cpp; depois apague este arquivo.

#include "renderer/LookConfig.hpp"

namespace eruption {
using DioramaLookConfig = LookConfig;
}
