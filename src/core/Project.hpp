#pragma once

// PROJETO DE JOGO. O motor abre de dois modos: um mapa solto (como sempre
// fez, tudo saindo da pasta do repositorio) ou um PROJETO - uma pasta fora do
// repositorio, com o manifesto project.eruption e todo o conteudo do jogo
// dentro (mapas, sprites, cenas, prefabs, scripts, sons, animacoes, efeitos).
//
// Com um projeto aberto o conteudo do jogo vem SO' do projeto: o motor nao
// enxerga mais os mapas e sprites do repositorio. O que continua saindo do
// repositorio e' o que pertence ao motor (icone da janela, idiomas, shaders,
// configuracao grafica padrao, sprite de reserva) - esses passam por
// project::engine(), nao por project::game().
//
// Sem projeto aberto as duas funcoes dao no mesmo lugar (a pasta do
// executavel/repositorio), e nada muda pra quem abre um mapa direto.

#include <filesystem>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <vector>

namespace eruption {

class Project {
public:
    // Manifesto <projeto>/project.eruption (JSON).
    struct Manifest {
        std::string name;             // nome do jogo
        std::string version = "0.1.0";
        std::string author;
        std::string description;
        std::string startupMap;       // mapa que abre com o projeto
        bool hasSpawn = false;        // posicao inicial do jogador
        float spawn[3] = {0.0f, 0.0f, 0.0f};
        // ENQUADRAMENTO DA CAMERA do jogo (bloco "camera" do manifesto, todas
        // as chaves opcionais): fov, yaw, pitch, distance. Quem nao vem fica
        // com o padrao do motor (math/Camera.hpp). E' aqui que um jogo diz
        // como se ve', em vez de corrigir a camera por script a cada mapa.
        std::optional<float> cameraFov, cameraYaw, cameraPitch, cameraDistance;
        // "keep_inside_map": true - a camera que segue o jogador para antes de
        // o QUADRO passar da borda do mapa (o jogador sai do centro perto da
        // borda, como num RPG 2D). Sem isso so' o alvo fica dentro do mapa, e
        // meia tela mostra o vazio alem da borda.
        bool cameraKeepInsideMap = false;
        // "dev_tools": true - o projeto e' uma BANCADA (demo/benchmark do
        // motor): no modo jogo continua com os menus F1..F12, o painel de
        // controle, o overlay F3, as barras e o minimapa. Jogo de verdade
        // deixa de fora e sai limpo, como o exportado.
        bool devTools = false;
    };

    static Project& instance();

    // Abre a pasta (precisa ter project.eruption). Erro vai em *err.
    bool open(const std::filesystem::path& dir, std::string* err = nullptr);
    // Cria a pasta, as subpastas de conteudo e o manifesto, e abre.
    bool create(const std::filesystem::path& dir, const std::string& name, std::string* err = nullptr);
    bool saveManifest(std::string* err = nullptr);
    void close();

    bool isOpen() const { return m_open; }
    const std::filesystem::path& root() const { return m_root; }
    const std::filesystem::path& engineRoot() const { return m_engineRoot; }
    Manifest& manifest() { return m_manifest; }
    const Manifest& manifest() const { return m_manifest; }

    // Mapas do projeto (pastas assets/external/<mapa>/<mapa>.glb).
    std::vector<std::string> maps() const;

    // Pasta do manifesto a partir de --project/ERUPTION_PROJECT/
    // data/launcher_config.json ("project"). Vazio = nenhum pedido.
    // *explicitlyAsked = veio da linha de comando ou do ambiente (ai' um erro
    // de abertura e' fatal); falso = so' lembranca do ultimo projeto aberto.
    static std::filesystem::path requested(int argc, char** argv, bool* explicitlyAsked = nullptr);
    // Guarda o projeto aberto em data/launcher_config.json pra proxima vez.
    static void rememberLastOpened(const std::filesystem::path& dir);
    // Blocos "spawn" e "camera" de um JSON (o manifesto, ou o
    // data/launcher_config.json do jogo exportado, que os leva copiados).
    static void readFraming(const nlohmann::json& j, Manifest& m);

    static constexpr const char* kManifestName = "project.eruption";
    // Subpastas de conteudo criadas por create() (e esperadas pelo editor).
    static const std::vector<std::string>& contentDirs();

private:
    Project();
    bool m_open = false;
    std::filesystem::path m_root;
    std::filesystem::path m_engineRoot;
    Manifest m_manifest;
};

namespace project {

// Conteudo do jogo: vem do projeto aberto; sem projeto, do repositorio.
std::filesystem::path game(const std::string& rel);
// Arquivo do proprio motor: sempre do repositorio/instalacao.
std::filesystem::path engine(const std::string& rel);
// Caminho CITADO por conteudo do jogo (cena, prefab, script, manifesto):
// relativo vale a partir da pasta do projeto; absoluto passa como esta'.
std::filesystem::path gameRef(const std::string& path);
// Configuracao do motor que o jogo pode SOBREPOR (data/postprocess.json,
// data/char_config.json, data/diorama.json): o arquivo de mesmo caminho no
// projeto vence; sem ele, o caminho volta como veio (o padrao do motor). A
// exportacao ja' resolve assim (projeto primeiro), entao o jogo exportado leva
// o mesmo visual.
std::string config(const std::string& rel);

inline bool isOpen() { return Project::instance().isOpen(); }
inline const std::filesystem::path& root() { return Project::instance().root(); }
inline const std::filesystem::path& engineRoot() { return Project::instance().engineRoot(); }

} // namespace project
} // namespace eruption
