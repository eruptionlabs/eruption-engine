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
class AudioSystem {
public:
    AudioSystem();
    ~AudioSystem();
    bool init();
    void shutdown();
    bool available() const;

    // Devolve um número para parar depois (0 = não tocou).
    uint32_t play(const std::string& file, float volume = 1.0f, bool loop = false);
    uint32_t playAt(const std::string& file, const Vec3& position, float volume = 1.0f, bool loop = false,
                    float range = 300.0f);
    void stop(uint32_t id, float fadeSeconds = 0.0f);
    void stopAll();
    bool playing(uint32_t id) const;

    // Uma música por vez; trocar faz a passagem suave entre as duas.
    void music(const std::string& file, float volume = 1.0f, float fadeSeconds = 1.0f);
    void stopMusic(float fadeSeconds = 1.0f);
    const std::string& currentMusic() const { return m_musicFile; }

    void setMasterVolume(float volume);
    float masterVolume() const { return m_master; }

    // Uma vez por frame: ouvinte na câmera; solta os sons que acabaram.
    void update(const Vec3& listener, const Vec3& forward, const Vec3& up);
    // Sons tocando agora (para a telemetria e os testes).
    size_t activeCount() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m;
    std::string m_musicFile;
    uint32_t m_musicId = 0;
    float m_master = 1.0f;
    uint32_t start(const std::string& file, float volume, bool loop, bool spatial, const Vec3& pos, float range,
                   bool stream);
};

} // namespace eruption
