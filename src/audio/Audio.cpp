#include "audio/Audio.hpp"

#include "core/Logger.hpp"
#include "core/Project.hpp"

#include <miniaudio.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <unordered_map>
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

ma_uint64 fadeMs(float seconds) { return ma_uint64(std::max(seconds, 0.0f) * 1000.0f); }

ma_attenuation_model attenuationModel(AudioSystem::Falloff f) {
    switch (f) {
        case AudioSystem::Falloff::Inverse: return ma_attenuation_model_inverse;
        case AudioSystem::Falloff::Exponential: return ma_attenuation_model_exponential;
        default: return ma_attenuation_model_linear;
    }
}

// Gravação de bancada (ERUPTION_TEST_AUDIO_OUT): WAV float 32 escrito à mão (a
// miniaudio é compilada sem o codificador). O cabeçalho é refeito no fim.
struct WavOut {
    std::FILE* f = nullptr;
    uint32_t channels = 2, rate = 48000;
    uint64_t frames = 0;
    void header() {
        const uint32_t data = uint32_t(frames * channels * 4);
        const uint32_t riff = 36 + data, fmtSize = 16, byteRate = rate * channels * 4;
        const uint16_t format = 3, ch = uint16_t(channels), align = uint16_t(channels * 4), bits = 32;
        std::fseek(f, 0, SEEK_SET);
        std::fwrite("RIFF", 1, 4, f); std::fwrite(&riff, 4, 1, f); std::fwrite("WAVEfmt ", 1, 8, f);
        std::fwrite(&fmtSize, 4, 1, f); std::fwrite(&format, 2, 1, f); std::fwrite(&ch, 2, 1, f);
        std::fwrite(&rate, 4, 1, f); std::fwrite(&byteRate, 4, 1, f); std::fwrite(&align, 2, 1, f);
        std::fwrite(&bits, 2, 1, f); std::fwrite("data", 1, 4, f); std::fwrite(&data, 4, 1, f);
        std::fseek(f, 0, SEEK_END);
    }
};
} // namespace

struct AudioSystem::Impl {
    ma_context context{};
    bool hasContext = false;
    ma_engine engine{};
    bool ready = false;
    struct Voice {
        uint32_t id = 0;
        ma_sound* sound = nullptr;
        ma_sound* next = nullptr; // laço emendado na introdução (música)
        bool started = false;
        ma_uint64 startAt = 0;    // quadro da mistura em que começa (agendado); antes dele nao "toca"
        ma_uint64 nextAt = 0;     // idem, o laço
    };
    std::vector<Voice> voices;
    uint32_t nextId = 1;
    std::unordered_map<std::string, ma_sound_group*> groups;
    std::vector<std::string> preloaded;                  // caminhos registrados no gerenciador
    // Uma fonte por preload ainda decodificando: o registro nao diz quando
    // acaba; a fonte do mesmo arquivo divide o buffer e responde MA_BUSY.
    std::vector<ma_resource_manager_data_source*> decoding;
    std::unordered_map<std::string, ma_uint64> musicPos; // onde cada faixa parou (retomar)
    std::string musicLoop;     // arquivo do laço da música atual ("" = sem introdução)
    bool musicLoopOnly = false; // a música atual começou direto no laço (retomada)
    WavOut wav;
    double wavDebt = 0.0;
    Voice* find(uint32_t id) {
        for (auto& v : voices)
            if (v.id == id) return &v;
        return nullptr;
    }
    ma_sound_group* group(const std::string& name) {
        if (name.empty()) return nullptr;
        auto it = groups.find(name);
        if (it != groups.end()) return it->second;
        auto* g = new ma_sound_group;
        if (ma_sound_group_init(&engine, 0, nullptr, g) != MA_SUCCESS) {
            delete g;
            return nullptr;
        }
        groups.emplace(name, g);
        return g;
    }
    static void release(Voice& v) {
        ma_sound_uninit(v.sound);
        delete v.sound;
        if (v.next) {
            ma_sound_uninit(v.next);
            delete v.next;
        }
        v.sound = v.next = nullptr;
    }
};

AudioSystem::AudioSystem() : m(std::make_unique<Impl>()) {}
AudioSystem::~AudioSystem() { shutdown(); }

bool AudioSystem::init() {
    if (m->ready) return true;
    ma_engine_config cfg = ma_engine_config_init();
    const char* mode = std::getenv("ERUPTION_AUDIO");
    const char* wavPath = std::getenv("ERUPTION_TEST_AUDIO_OUT");
    if (mode && std::strcmp(mode, "off") == 0) return false;
    if (wavPath && *wavPath) {
        // Sem dispositivo: a mistura so' anda quando update() a le'.
        cfg.noDevice = MA_TRUE;
        cfg.channels = m->wav.channels;
        cfg.sampleRate = m->wav.rate;
    } else if (mode && std::strcmp(mode, "null") == 0) {
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
    if (wavPath && *wavPath) {
        m->wav.f = std::fopen(wavPath, "wb");
        if (m->wav.f) m->wav.header();
        ERUPTION_LOG_WARN("Audio: gravando a mistura em %s (%s)", wavPath, m->wav.f ? "ok" : "FALHOU");
    }
    ERUPTION_LOG_INFO("Audio: %u Hz, %u channels", ma_engine_get_sample_rate(&m->engine), ma_engine_get_channels(&m->engine));
    return true;
}

void AudioSystem::shutdown() {
    if (!m || !m->ready) return;
    for (auto& v : m->voices) Impl::release(v);
    m->voices.clear();
    for (auto& [name, g] : m->groups) {
        ma_sound_group_uninit(g);
        delete g;
    }
    m->groups.clear();
    ma_resource_manager* rm = ma_engine_get_resource_manager(&m->engine);
    for (auto* ds : m->decoding) {
        ma_resource_manager_data_source_uninit(ds);
        delete ds;
    }
    m->decoding.clear();
    for (const auto& path : m->preloaded) ma_resource_manager_unregister_file(rm, path.c_str());
    m->preloaded.clear();
    if (m->wav.f) {
        m->wav.header();
        std::fclose(m->wav.f);
        m->wav.f = nullptr;
    }
    ma_engine_uninit(&m->engine);
    if (m->hasContext) ma_context_uninit(&m->context);
    m->hasContext = false;
    m->ready = false;
    m_musicId = 0;
    m_musicFile.clear();
}

bool AudioSystem::available() const { return m->ready; }

// Caminho em disco de um som citado por cena/script (relativo = pasta do
// projeto); vazio se nao existe.
static std::string soundPath(const std::string& file) {
    std::error_code ec;
    const std::string path = project::gameRef(file).string();
    if (std::filesystem::exists(path, ec)) return path;
    ERUPTION_LOG_WARN("Audio: file not found: %s", path.c_str());
    return std::string();
}

// Comprimento (na taxa do arquivo) de um som aberto em stream. O stream da
// miniaudio le pelo VFS, e o Ogg Vorbis por ali fica no modo "push", que nao
// sabe o comprimento (devolve 0): toda introdução .ogg caia no "sem laço" e
// repetia do começo. Um decodificador aberto direto no arquivo usa o modo
// "pull", que acha o fim pela ultima pagina sem decodificar o resto.
static void streamLength(const std::string& path, ma_uint64& len, ma_uint32& rate) {
    if (path.empty()) return;
    ma_decoder decoder;
    if (ma_decoder_init_file(path.c_str(), nullptr, &decoder) != MA_SUCCESS) return;
    ma_uint64 frames = 0;
    ma_uint32 fileRate = 0;
    if (ma_decoder_get_length_in_pcm_frames(&decoder, &frames) == MA_SUCCESS &&
        ma_decoder_get_data_format(&decoder, nullptr, nullptr, &fileRate, nullptr, 0) == MA_SUCCESS && frames > 0 &&
        fileRate > 0) {
        len = frames;
        rate = fileRate;
    }
    ma_decoder_uninit(&decoder);
}

uint32_t AudioSystem::start(const std::string& file, const PlayOptions& o, bool spatial, const Vec3& pos, bool stream,
                            uint64_t startFrame) {
    if (!m->ready || file.empty()) return 0;
    const std::string path = soundPath(file);
    if (path.empty()) return 0;
    auto* sound = new ma_sound;
    // Efeito: decodificado uma vez e guardado (o cache da miniaudio reaproveita);
    // música: lida aos poucos. Os dois carregam em segundo plano.
    // Agendado (startFrame): aberto ja' pronto, para tocar no quadro certo.
    ma_uint32 flags = stream ? ma_uint32(MA_SOUND_FLAG_STREAM) : ma_uint32(MA_SOUND_FLAG_DECODE);
    if (!startFrame) flags |= ma_uint32(MA_SOUND_FLAG_ASYNC);
    if (ma_sound_init_from_file(&m->engine, path.c_str(), flags, m->group(o.group), nullptr, sound) != MA_SUCCESS) {
        ERUPTION_LOG_WARN("Audio: can't play %s (wav, ogg, mp3 or flac)", file.c_str());
        delete sound;
        return 0;
    }
    ma_sound_set_volume(sound, o.volume);
    ma_sound_set_pitch(sound, std::max(o.pitch, 0.01f));
    ma_sound_set_looping(sound, o.loop ? MA_TRUE : MA_FALSE);
    ma_sound_set_spatialization_enabled(sound, spatial ? MA_TRUE : MA_FALSE);
    if (spatial) {
        float x, y, z;
        maVec(pos, x, y, z);
        ma_sound_set_position(sound, x, y, z);
        const float range = std::max(o.range, 1.0f);
        ma_sound_set_attenuation_model(sound, attenuationModel(o.falloff));
        ma_sound_set_min_distance(sound, o.minDistance >= 0.0f ? std::min(o.minDistance, range) : range * 0.1f);
        ma_sound_set_max_distance(sound, range);
    }
    if (startFrame) ma_sound_set_start_time_in_pcm_frames(sound, startFrame);
    ma_sound_start(sound);
    const uint32_t id = m->nextId++;
    m->voices.push_back({id, sound, nullptr, true, startFrame, 0});
    return id;
}

uint32_t AudioSystem::play(const std::string& file, const PlayOptions& options) {
    return start(file, options, false, Vec3(0.0f), false, 0);
}

uint32_t AudioSystem::playAt(const std::string& file, const Vec3& position, const PlayOptions& options) {
    return start(file, options, true, position, false, 0);
}

uint32_t AudioSystem::play(const std::string& file, float volume, bool loop) {
    PlayOptions o;
    o.volume = volume;
    o.loop = loop;
    return play(file, o);
}

uint32_t AudioSystem::playAt(const std::string& file, const Vec3& position, float volume, bool loop, float range) {
    PlayOptions o;
    o.volume = volume;
    o.loop = loop;
    o.range = range;
    return playAt(file, position, o);
}

void AudioSystem::stop(uint32_t id, float fadeSeconds) {
    Impl::Voice* v = m->find(id);
    if (!v) return;
    for (ma_sound* snd : {v->sound, v->next}) {
        if (!snd) continue;
        if (fadeSeconds > 0.0f) ma_sound_stop_with_fade_in_milliseconds(snd, fadeMs(fadeSeconds));
        else ma_sound_stop(snd);
    }
}

void AudioSystem::setSuspended(bool on) {
    if (!m->ready) return;
    if (on) ma_engine_stop(&m->engine);
    else ma_engine_start(&m->engine);
}

void AudioSystem::stopAll() {
    for (auto& v : m->voices) {
        ma_sound_stop(v.sound);
        if (v.next) ma_sound_stop(v.next);
    }
    m_musicId = 0;
    m_musicFile.clear();
}

bool AudioSystem::playing(uint32_t id) const {
    for (const auto& v : m->voices)
        if (v.id == id) return ma_sound_is_playing(v.sound) == MA_TRUE || (v.next && ma_sound_is_playing(v.next) == MA_TRUE);
    return false;
}

void AudioSystem::setVolume(uint32_t id, float volume, float fadeSeconds) {
    Impl::Voice* v = m->find(id);
    if (!v) return;
    volume = std::max(volume, 0.0f);
    for (ma_sound* snd : {v->sound, v->next}) {
        if (!snd) continue;
        // Fade a partir do volume atual (-1): sem salto no começo da rampa.
        if (fadeSeconds > 0.0f) ma_sound_set_fade_in_milliseconds(snd, -1.0f, volume, fadeMs(fadeSeconds));
        else {
            ma_sound_set_fade_in_milliseconds(snd, -1.0f, volume, 0);
            ma_sound_set_volume(snd, volume);
        }
    }
}

void AudioSystem::setPitch(uint32_t id, float pitch) {
    if (Impl::Voice* v = m->find(id)) ma_sound_set_pitch(v->sound, std::max(pitch, 0.01f));
}

bool AudioSystem::preload(const std::string& file) {
    if (!m->ready || file.empty()) return false;
    const std::string path = soundPath(file);
    if (path.empty()) return false;
    if (std::find(m->preloaded.begin(), m->preloaded.end(), path) != m->preloaded.end()) return true;
    // Registrado, o arquivo fica decodificado no gerenciador ate' o fim: o
    // ma_sound_init_from_file do primeiro toque so' aponta para ele.
    const ma_uint32 flags = MA_RESOURCE_MANAGER_DATA_SOURCE_FLAG_DECODE | MA_RESOURCE_MANAGER_DATA_SOURCE_FLAG_ASYNC;
    ma_resource_manager* rm = ma_engine_get_resource_manager(&m->engine);
    if (ma_resource_manager_register_file(rm, path.c_str(), flags) != MA_SUCCESS) return false;
    m->preloaded.push_back(path);
    auto* ds = new ma_resource_manager_data_source{};
    if (ma_resource_manager_data_source_init(rm, path.c_str(), flags, nullptr, ds) == MA_SUCCESS)
        m->decoding.push_back(ds);
    else
        delete ds;
    return true;
}

size_t AudioSystem::pendingLoads() {
    if (!m || !m->ready) return 0;
    auto& d = m->decoding;
    for (size_t i = 0; i < d.size();) {
        if (ma_resource_manager_data_source_result(d[i]) == MA_BUSY) {
            ++i;
            continue;
        }
        ma_resource_manager_data_source_uninit(d[i]);
        delete d[i];
        d[i] = d.back();
        d.pop_back();
    }
    return d.size();
}

void AudioSystem::music(const std::string& file, float volume, float fadeSeconds, const std::string& loopFile,
                        bool resume) {
    if (!m->ready) return;
    if (file == m_musicFile && playing(m_musicId)) return;
    stopMusic(fadeSeconds);
    m_musicVolume = std::max(volume, 0.0f);
    PlayOptions o;
    o.volume = m_musicVolume;
    o.loop = loopFile.empty();
    o.group = "music";
    // Retomar o laço direto quando a faixa ja' tinha passado da introdução.
    const auto loopPos = resume && !loopFile.empty() ? m->musicPos.find(loopFile) : m->musicPos.end();
    const bool startInLoop = loopPos != m->musicPos.end();
    if (startInLoop) o.loop = true;
    // INTRODUÇÃO -> LAÇO: dois sons agendados no relogio da mistura. A
    // introdução abre ja' pronta (o tamanho dela e' preciso agora) e o laço
    // entra no quadro exato em que ela acaba, com a repetição nativa do stream.
    // (Cadeia de fontes da miniaudio foi medida antes: ela so' repete pela
    // fonte cabeça, voltando ao inicio por busca, e o stream abre um buraco de
    // alguns ms em toda volta.)
    const bool chained = !loopFile.empty() && !startInLoop;
    const uint64_t t0 = chained ? ma_engine_get_time_in_pcm_frames(&m->engine) + 1024 : 0;
    m_musicId = start(startInLoop ? loopFile : file, o, false, Vec3(0.0f), true, t0);
    if (!m_musicId) return;
    m_musicFile = file;
    m->musicLoop = loopFile;
    m->musicLoopOnly = startInLoop;
    Impl::Voice* v = m->find(m_musicId);
    if (chained) {
        ma_uint64 introPos = 0;
        if (resume) {
            if (auto it = m->musicPos.find(file); it != m->musicPos.end()) {
                introPos = it->second;
                ma_sound_seek_to_pcm_frame(v->sound, introPos);
            }
        }
        ma_uint64 len = 0;
        ma_uint32 rate = 0;
        ma_sound_get_length_in_pcm_frames(v->sound, &len);
        ma_sound_get_data_format(v->sound, nullptr, nullptr, &rate, nullptr, 0);
        if (len == 0) streamLength(soundPath(file), len, rate);
        const std::string loopPath = soundPath(loopFile);
        auto* next = new ma_sound;
        if (len > introPos && rate > 0 && !loopPath.empty() &&
            ma_sound_init_from_file(&m->engine, loopPath.c_str(), MA_SOUND_FLAG_STREAM, m->group("music"), nullptr,
                                    next) == MA_SUCCESS) {
            const double engineRate = ma_engine_get_sample_rate(&m->engine);
            ma_sound_set_volume(next, m_musicVolume);
            ma_sound_set_looping(next, MA_TRUE);
            ma_sound_set_spatialization_enabled(next, MA_FALSE); // música nao tem lugar no mundo
            v->nextAt = t0 + ma_uint64(double(len - introPos) * engineRate / rate + 0.5);
            ma_sound_set_start_time_in_pcm_frames(next, v->nextAt);
            ma_sound_start(next);
            v->next = next;
        } else {
            delete next;
            // Sem o laço (arquivo nao achado ou comprimento da introdução
            // desconhecido no stream), a introdução repete - avisar, senao a
            // emenda errada passa por "o laço voltou para o começo".
            ERUPTION_LOG_WARN("Audio: laco %s nao emendado em %s (comprimento %llu, taxa %u); a introducao repete",
                              loopFile.c_str(), file.c_str(), static_cast<unsigned long long>(len), rate);
            ma_sound_set_looping(v->sound, MA_TRUE);
        }
    }
    if (resume && !chained) {
        const auto it = startInLoop ? loopPos : m->musicPos.find(file);
        if (it != m->musicPos.end()) ma_sound_seek_to_pcm_frame(v->sound, it->second);
    }
    if (fadeSeconds > 0.0f)
        for (ma_sound* snd : {v->sound, v->next})
            if (snd) ma_sound_set_fade_in_milliseconds(snd, 0.0f, m_musicVolume, fadeMs(fadeSeconds));
}

void AudioSystem::stopMusic(float fadeSeconds) {
    if (m_musicId) {
        if (Impl::Voice* v = m->find(m_musicId)) {
            // Onde parou, para um music(..., resume) depois (ex.: a exploração
            // volta da batalha do ponto em que estava). Ja' no laço, a posição
            // e' a do laço.
            ma_uint64 cursor = 0;
            const bool chainedInLoop = v->next && !ma_sound_is_playing(v->sound); // a introdução ja' acabou
            const bool inLoop = m->musicLoopOnly || chainedInLoop;
            ma_sound* src = chainedInLoop ? v->next : v->sound;
            if (ma_sound_get_cursor_in_pcm_frames(src, &cursor) == MA_SUCCESS) {
                if (inLoop) {
                    // Ja' no laço: a retomada pula a introdução.
                    m->musicPos[m->musicLoop] = cursor;
                    m->musicPos.erase(m_musicFile);
                } else {
                    m->musicPos[m_musicFile] = cursor;
                }
            }
        }
        stop(m_musicId, fadeSeconds);
    }
    m_musicId = 0;
    m_musicFile.clear();
}

void AudioSystem::setMusicVolume(float volume, float fadeSeconds) {
    m_musicVolume = std::max(volume, 0.0f);
    if (m_musicId) setVolume(m_musicId, m_musicVolume, fadeSeconds);
}

void AudioSystem::setMasterVolume(float volume) {
    m_master = std::max(0.0f, volume);
    if (m->ready) ma_engine_set_volume(&m->engine, m_master);
}

void AudioSystem::setGroupVolume(const std::string& group, float volume, float fadeSeconds) {
    if (!m->ready) return;
    ma_sound_group* g = m->group(group);
    if (!g) return;
    volume = std::max(volume, 0.0f);
    if (fadeSeconds > 0.0f) ma_sound_group_set_fade_in_milliseconds(g, -1.0f, volume, fadeMs(fadeSeconds));
    else ma_sound_group_set_volume(g, volume);
}

void AudioSystem::resetMix() {
    setMasterVolume(1.0f);
    setMusicVolume(1.0f);
    if (!m->ready) return;
    for (auto& [name, g] : m->groups) ma_sound_group_set_volume(g, 1.0f);
}

void AudioSystem::update(const Vec3& listener, const Vec3& forward, const Vec3& up, float dt) {
    if (!m->ready) return;
    float x, y, z;
    maVec(listener, x, y, z);
    ma_engine_listener_set_position(&m->engine, 0, x, y, z);
    maVec(forward, x, y, z);
    ma_engine_listener_set_direction(&m->engine, 0, x, y, z);
    maVec(up, x, y, z);
    ma_engine_listener_set_world_up(&m->engine, 0, x, y, z);
    // Gravação de bancada: a mistura anda exatamente dt segundos por frame.
    if (m->wav.f && dt > 0.0f) {
        m->wavDebt += double(dt) * m->wav.rate;
        std::vector<float> buf(4096 * m->wav.channels);
        while (m->wavDebt >= 1.0) {
            const ma_uint64 want = std::min<ma_uint64>(ma_uint64(m->wavDebt), 4096);
            ma_uint64 got = 0;
            ma_engine_read_pcm_frames(&m->engine, buf.data(), want, &got);
            if (got == 0) break;
            std::fwrite(buf.data(), sizeof(float) * m->wav.channels, size_t(got), m->wav.f);
            m->wav.frames += got;
            m->wavDebt -= double(got);
        }
    }
    // Solta o que acabou (ou parou com a passagem suave).
    for (size_t i = 0; i < m->voices.size();) {
        Impl::Voice& v = m->voices[i];
        // Agendado e ainda nao chegou a hora: is_playing diz falso, mas vai tocar.
        const ma_uint64 agora = ma_engine_get_time_in_pcm_frames(&m->engine);
        const bool tocando = ma_sound_is_playing(v.sound) || (v.next && ma_sound_is_playing(v.next)) ||
                             agora < v.startAt || (v.next && agora < v.nextAt);
        const bool acabou = v.next ? !tocando : ma_sound_at_end(v.sound);
        if (!tocando && (acabou || v.id != m_musicId)) {
            Impl::release(v);
            m->voices.erase(m->voices.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
}

size_t AudioSystem::activeCount() const { return m->voices.size(); }

} // namespace eruption
