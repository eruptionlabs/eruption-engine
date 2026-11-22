#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace eruption {

// Exportação do jogo (File > Export Game): parte dos mapas escolhidos e copia
// só o que eles usam (grafo de dependência) para uma pasta que roda sozinha.
//
// Nós do grafo: a pasta do mapa (assets/external/<mapa>), os arquivos de cena
// (assets/scenes/<mapa>.*), o clima do mapa, os scripts, e todo caminho entre
// aspas nesses arquivos que exista no disco (modelos acrescentados, prefabs e
// o modelo deles, sprites com o .spr ao lado, camadas de terreno, folhas de
// interface .css e as imagens que elas citam). Efeitos de partícula entram
// pelo nome citado ("tocha" -> assets/effects/tocha.vfx.json, ou o modelo do
// motor em data/vfx). String interpolada do Luau leva os arquivos da pasta que
// casam com o modelo (`retratos/{nome}.png` -> retratos/*.png). Mais os
// arquivos fixos do motor (configurações, shaders compilados, sprite padrão).
// Limite: caminho montado por concatenação não aparece no texto; para esses,
// o project.eruption aceita "export": { "include": [globs], "exclude": [globs] }
// (relativos à raiz do projeto; '*' por componente, "**" em qualquer profundidade).
struct ExportOptions {
    std::filesystem::path projectRoot; // pasta do jogo (projeto aberto)
    // Pasta do motor, quando o projeto esta' fora dela: o que e' do motor
    // (shaders compilados, configuracoes padrao, icone, sprite de reserva)
    // sai daqui. Vazia = tudo mora na mesma pasta (modo sem projeto).
    std::filesystem::path engineRoot;
    std::vector<std::string> maps; // o primeiro é o mapa inicial
    std::filesystem::path outDir;
    std::filesystem::path binary;  // executável a levar (o enxuto, se existir)
};

struct ExportReport {
    size_t files = 0;
    uint64_t bytes = 0;
    std::vector<std::string> missing; // citado em algum arquivo mas não existe
    size_t compiled = 0;               // .luau que foram como bytecode
    size_t neighbors = 0;              // mapas vizinhos (mundo contínuo) que foram junto
    size_t excluded = 0;               // tirados por export.exclude do projeto
    size_t groundPacked = 0;           // chao de mapa (.glb + .ter) que foi como .relevo
    std::vector<std::string> groundKept; // chao que nao empacotou: "<arquivo>: motivo"
    std::vector<std::string> blocked;  // arquivo de modelo pedido pelo grafo: recusado, nao copiado
    std::string error;
};

ExportReport exportGame(const ExportOptions& options);

// Módulos opcionais do motor, lidos de cmake/modules.json (a mesma tabela
// que o CMake usa para opção, fontes e implementação muda). O editor detecta
// no projeto quais o jogo usa, e o programa enxuto leva só esses.
struct ModuleInfo {
    std::string name, option, label, summary;
    bool defaultOn = true;
    bool exportable = true; // false: o jogo exportado nunca leva (ex.: modelo não exportado)
    std::vector<std::string> scriptGlobals; // API de script que liga o módulo (`audio.play`)
    std::vector<std::string> glbExtensions; // extensao glTF que liga o modulo (declarada no JSON do .glb/.gltf)
    std::vector<std::string> scriptCalls;   // chamada especifica que liga o modulo (`world.set_ambient`)
    std::vector<std::string> sceneFields;   // campo de cena que liga o módulo (`sounds = {...}`)
};
std::vector<ModuleInfo> loadModuleTable(const std::filesystem::path& engineRoot, std::string* error = nullptr);
bool moduleUsedByProject(const ModuleInfo& m, const std::filesystem::path& projectRoot);

// Perfil de build do jogo exportado: opção CMake -> ligada.
struct BuildProfile {
    std::map<std::string, bool> modules;
};
// Perfil com que build-export/ foi configurado (false se ainda não existe).
bool builtProfile(const std::filesystem::path& buildDir, const std::vector<ModuleInfo>& table, BuildProfile& out);
std::string profileCMakeFlags(const std::vector<ModuleInfo>& table, const BuildProfile& p);

// ---------------------------------------------------------------------------
// Filtro da API de script (ver src/script/ApiFilter.hpp)
// ---------------------------------------------------------------------------

// Funcoes da API que os scripts do projeto usam. `universe` e' o que o motor
// registra (as entradas ERUPTION_API_FN dos fontes de src/script/), `used` o que
// o projeto chama ("global.funcao"), mais o que o export_keep.json forca.
struct ApiUsage {
    std::map<std::string, std::vector<std::string>> universe; // global -> funcoes
    std::set<std::string> used;
    std::vector<std::string> notes; // acesso dinamico, globais passadas adiante, export_keep.json
    bool valid = false;             // false: nao achou os fontes do motor (nada e' cortado)
};
ApiUsage analyzeApiUse(const std::filesystem::path& engineRoot, const std::filesystem::path& projectRoot);
// build/<perfil>/eruption_api_used.h: um #define ERUPTION_API_USE_<global>_<funcao> 1 por funcao usada.
bool writeApiUsedHeader(const ApiUsage& usage, const std::filesystem::path& header);

struct ExportReportInfo {
    ApiUsage api;
    std::vector<ModuleInfo> modules;
    BuildProfile profile;
    std::vector<bool> moduleUsed;
    std::filesystem::path binary;        // o que foi exportado
    std::filesystem::path baselineBinary; // para "antes" (vazio = nao informado)
    std::filesystem::path linkLog;       // saida do linker com --print-gc-sections (opcional)
    std::vector<std::string> blocked;    // arquivos de modelo recusados
    bool apiFiltered = false;            // o programa foi compilado com o filtro
};
// Grava <outDir>/export_report.txt.
void writeExportReport(const std::filesystem::path& outDir, const ExportReportInfo& info);

} // namespace eruption
