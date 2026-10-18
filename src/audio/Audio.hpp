#pragma once

#include "math/Types.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace eruption {

// Som do jogo (miniaudio): efeitos, som num ponto do mundo (mais baixo longe e
// do lado certo) e música com troca suave. Arquivos wav, ogg, mp3 ou flac,
// carregados em segundo plano na primeira vez e guardados para as próximas.
// ERUPTION_AUDIO=null: toca num dispositivo mudo (testes sem barulho).
// ERUPTION_TEST_AUDIO_OUT=<arquivo.wav> (bancada): sem dispositivo; a mistura
// avança pelo dt de cada frame e é gravada no arquivo (com
// ERUPTION_TEST_FIXED_DT sai igual a cada rodada).
class AudioSystem {
public:
    // Queda com a distância (som num ponto do mundo).
    enum class Falloff { Linear, Inverse, Exponential };
    struct PlayOptions {
        float volume = 1.0f;
        float pitch = 1.0f;          // 2 = uma oitava acima (e mais rápido)
        bool loop = false;
        float range = 300.0f;        // distância em que some (som num ponto)
        float minDistance = -1.0f;   // até aqui toca cheio; < 0 = 10% do alcance
        Falloff falloff = Falloff::Linear;
        std::string group;           // barramento com volume próprio ("" = nenhum)
    };

    AudioSystem();
    ~AudioSystem();
    bool init();
    void shutdown();
    bool available() const;

    // Devolve um número para mudar ou parar depois (0 = não tocou).
    uint32_t play(const std::string& file, const PlayOptions& options);
    uint32_t playAt(const std::string& file, const Vec3& position, const PlayOptions& options);
    uint32_t play(const std::string& file, float volume = 1.0f, bool loop = false);
    uint32_t playAt(const std::string& file, const Vec3& position, float volume = 1.0f, bool loop = false,
                    float range = 300.0f);
    void stop(uint32_t id, float fadeSeconds = 0.0f);
    void stopAll();
    bool playing(uint32_t id) const;
    // Muda um som que já toca, sem recomeçar (fade em segundos).
    void setVolume(uint32_t id, float volume, float fadeSeconds = 0.0f);
    void setPitch(uint32_t id, float pitch);
    // Decodifica o arquivo em segundo plano e o guarda: o primeiro toque sai
    // na hora. Falso se o arquivo não existe.
    bool preload(const std::string& file);

    // Uma música por vez; trocar faz a passagem suave entre as duas. Com
    // `loopFile`, `file` é a introdução e o laço emenda nela sem lacuna.
    // `resume`: volta do ponto em que esta faixa parou da última vez.
    void music(const std::string& file, float volume = 1.0f, float fadeSeconds = 1.0f,
               const std::string& loopFile = std::string(), bool resume = false);
    void stopMusic(float fadeSeconds = 1.0f);
    void setMusicVolume(float volume, float fadeSeconds = 0.0f);
    const std::string& currentMusic() const { return m_musicFile; }

    void setMasterVolume(float volume);
    float masterVolume() const { return m_master; }
    // Barramento: todo som tocado com `group` passa por ele (criado no uso).
    void setGroupVolume(const std::string& group, float volume, float fadeSeconds = 0.0f);
    // Volumes de volta ao da abertura: geral, musica e todos os grupos em 1.
    void resetMix();

    // Uma vez por frame: ouvinte na câmera; solta os sons que acabaram. `dt`
    // só conta na gravação de bancada (ERUPTION_TEST_AUDIO_OUT).
    void update(const Vec3& listener, const Vec3& forward, const Vec3& up, float dt = 0.0f);
    // Sons tocando agora (para a telemetria e os testes).
    size_t activeCount() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m;
    std::string m_musicFile;
    uint32_t m_musicId = 0;
    float m_musicVolume = 1.0f;
    float m_master = 1.0f;
    uint32_t start(const std::string& file, const PlayOptions& options, bool spatial, const Vec3& pos, bool stream,
                   uint64_t startFrame);
};

} // namespace eruption
