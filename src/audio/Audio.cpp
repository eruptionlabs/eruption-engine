#include "audio/Audio.hpp"

#include "core/Logger.hpp"
#include "core/Project.hpp"

#include <miniaudio.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <vector>

namespace eruption {

namespace {
// O motor usa mão esquerda; a miniaudio, direita. Espelhar X troca a mão (o som
// da direita sai na direita).
inline void maVec(const Vec3& v, float& x, float& y, float& z) {
    x = -v.x;
    y = v.y;
    z = v.z;
}
} // namespace

struct AudioSystem::Impl {
    ma_context context{};
    bool hasContext = false;
    ma_engine engine{};
    bool ready = false;
    struct Voice {
        uint32_t id = 0;
        ma_sound* sound = nullptr;
        bool started = false;
    };
    std::vector<Voice> voices;
    uint32_t nextId = 1;
    Voice* find(uint32_t id) {
        for (auto& v : voices)
            if (v.id == id) return &v;
        return nullptr;
    }
};

AudioSystem::AudioSystem() : m(std::make_unique<Impl>()) {}
AudioSystem::~AudioSystem() { shutdown(); }

bool AudioSystem::init() {
    if (m->ready) return true;
    ma_engine_config cfg = ma_engine_config_init();
    const char* mode = std::getenv("ERUPTION_AUDIO");
    if (mode && std::strcmp(mode, "off") == 0) return false;
    if (mode && std::strcmp(mode, "null") == 0) {
        // Dispositivo mudo: consome o som no tempo real, sem sair nada.
        const ma_backend backends[] = {ma_backend_null};
        if (ma_context_init(backends, 1, nullptr, &m->context) == MA_SUCCESS) {
            m->hasContext = true;
            cfg.pContext = &m->context;
        }
    }
    if (ma_engine_init(&cfg, &m->engine) != MA_SUCCESS) {
        ERUPTION_LOG_WARN("Audio: no sound device; the game runs without sound");
        if (m->hasContext) ma_context_uninit(&m->context);
        m->hasContext = false;
        return false;
    }
    m->ready = true;
    ma_engine_listener_set_world_up(&m->engine, 0, 0.0f, 1.0f, 0.0f);
    ERUPTION_LOG_INFO("Audio: %u Hz, %u channels", ma_engine_get_sample_rate(&m->engine), ma_engine_get_channels(&m->engine));
    return true;
}

void AudioSystem::shutdown() {
    if (!m || !m->ready) return;
    for (auto& v : m->voices) {
        ma_sound_uninit(v.sound);
        delete v.sound;
    }
    m->voices.clear();
    ma_engine_uninit(&m->engine);
    if (m->hasContext) ma_context_uninit(&m->context);
    m->hasContext = false;
    m->ready = false;
    m_musicId = 0;
    m_musicFile.clear();
}

bool AudioSystem::available() const { return m->ready; }

uint32_t AudioSystem::start(const std::string& file, float volume, bool loop, bool spatial, const Vec3& pos, float range,
                            bool stream) {
    if (!m->ready || file.empty()) return 0;
    std::error_code ec;
    // Som citado por cena/script: relativo sai da pasta do projeto.
    const std::string path = project::gameRef(file).string();
    if (!std::filesystem::exists(path, ec)) {
        ERUPTION_LOG_WARN("Audio: file not found: %s", path.c_str());
        return 0;
    }
    auto* sound = new ma_sound;
    // Efeito: decodificado uma vez e guardado (o cache da miniaudio reaproveita);
    // música: lida aos poucos. Os dois carregam em segundo plano.
    const ma_uint32 flags = (stream ? MA_SOUND_FLAG_STREAM : MA_SOUND_FLAG_DECODE) | MA_SOUND_FLAG_ASYNC;
    if (ma_sound_init_from_file(&m->engine, path.c_str(), flags, nullptr, nullptr, sound) != MA_SUCCESS) {
        ERUPTION_LOG_WARN("Audio: can't play %s (wav, ogg, mp3 or flac)", file.c_str());
        delete sound;
        return 0;
    }
    ma_sound_set_volume(sound, volume);
    ma_sound_set_looping(sound, loop ? MA_TRUE : MA_FALSE);
    ma_sound_set_spatialization_enabled(sound, spatial ? MA_TRUE : MA_FALSE);
    if (spatial) {
        float x, y, z;
        maVec(pos, x, y, z);
        ma_sound_set_position(sound, x, y, z);
        ma_sound_set_attenuation_model(sound, ma_attenuation_model_linear);
        ma_sound_set_min_distance(sound, range * 0.1f);
        ma_sound_set_max_distance(sound, range);
    }
    ma_sound_start(sound);
    const uint32_t id = m->nextId++;
    m->voices.push_back({id, sound, true});
    return id;
}

uint32_t AudioSystem::play(const std::string& file, float volume, bool loop) {
    return start(file, volume, loop, false, Vec3(0.0f), 0.0f, false);
}

uint32_t AudioSystem::playAt(const std::string& file, const Vec3& position, float volume, bool loop, float range) {
    return start(file, volume, loop, true, position, std::max(range, 1.0f), false);
}

void AudioSystem::stop(uint32_t id, float fadeSeconds) {
    Impl::Voice* v = m->find(id);
    if (!v) return;
    if (fadeSeconds > 0.0f) ma_sound_stop_with_fade_in_milliseconds(v->sound, ma_uint64(fadeSeconds * 1000.0f));
    else ma_sound_stop(v->sound);
}

void AudioSystem::stopAll() {
    for (auto& v : m->voices) ma_sound_stop(v.sound);
    m_musicId = 0;
    m_musicFile.clear();
}

bool AudioSystem::playing(uint32_t id) const {
    for (const auto& v : m->voices)
        if (v.id == id) return ma_sound_is_playing(v.sound) == MA_TRUE;
    return false;
}

void AudioSystem::music(const std::string& file, float volume, float fadeSeconds) {
    if (!m->ready) return;
    if (file == m_musicFile && playing(m_musicId)) return;
    stopMusic(fadeSeconds);
    m_musicId = start(file, volume, true, false, Vec3(0.0f), 0.0f, true);
    if (!m_musicId) return;
    m_musicFile = file;
    if (fadeSeconds > 0.0f)
        if (Impl::Voice* v = m->find(m_musicId))
            ma_sound_set_fade_in_milliseconds(v->sound, 0.0f, volume, ma_uint64(fadeSeconds * 1000.0f));
}

void AudioSystem::stopMusic(float fadeSeconds) {
    if (m_musicId) stop(m_musicId, fadeSeconds);
    m_musicId = 0;
    m_musicFile.clear();
}

void AudioSystem::setMasterVolume(float volume) {
    m_master = std::max(0.0f, volume);
    if (m->ready) ma_engine_set_volume(&m->engine, m_master);
}

void AudioSystem::update(const Vec3& listener, const Vec3& forward, const Vec3& up) {
    if (!m->ready) return;
    float x, y, z;
    maVec(listener, x, y, z);
    ma_engine_listener_set_position(&m->engine, 0, x, y, z);
    maVec(forward, x, y, z);
    ma_engine_listener_set_direction(&m->engine, 0, x, y, z);
    maVec(up, x, y, z);
    ma_engine_listener_set_world_up(&m->engine, 0, x, y, z);
    // Solta o que acabou (ou parou com a passagem suave).
    for (size_t i = 0; i < m->voices.size();) {
        Impl::Voice& v = m->voices[i];
        if (!ma_sound_is_playing(v.sound) && (ma_sound_at_end(v.sound) || v.id != m_musicId)) {
            ma_sound_uninit(v.sound);
            delete v.sound;
            m->voices.erase(m->voices.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
}

size_t AudioSystem::activeCount() const { return m->voices.size(); }

} // namespace eruption
