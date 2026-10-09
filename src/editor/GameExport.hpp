#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace eruption {

// Exportação do jogo (File > Export Game): parte dos mapas escolhidos e copia
// só o que eles usam (grafo de dependência) para uma pasta que roda sozinha.
//
// Nós do grafo: a pasta do mapa (assets/external/<mapa>), os arquivos de cena
// (assets/scenes/<mapa>.*), o clima do mapa, os scripts, e todo caminho entre
// aspas nesses arquivos que exista no disco (modelos acrescentados, prefabs e
// o modelo deles, sprites com o .spr ao lado, camadas de terreno). Mais os
// arquivos fixos do motor (configurações, shaders compilados, sprite padrão).
// Limite: caminhos montados em tempo de execução não aparecem no texto.
struct ExportOptions {
    std::filesystem::path projectRoot;
    std::vector<std::string> maps; // o primeiro é o mapa inicial
    std::filesystem::path outDir;
    std::filesystem::path binary;  // executável a levar (o enxuto, se existir)
};

struct ExportReport {
    size_t files = 0;
    uint64_t bytes = 0;
    std::vector<std::string> missing; // citado em algum arquivo mas não existe
    std::string error;
};

ExportReport exportGame(const ExportOptions& options);

} // namespace eruption
