// Módulo de som desligado (ERUPTION_MODULE_AUDIO=OFF): o jogo não tem som e
// não leva a miniaudio. Mesma interface; nada toca.
#include "audio/Audio.hpp"

namespace eruption {

struct AudioSystem::Impl {};

AudioSystem::AudioSystem() = default;
AudioSystem::~AudioSystem() = default;
bool AudioSystem::init() { return false; }
void AudioSystem::shutdown() {}
bool AudioSystem::available() const { return false; }
void AudioSystem::setSuspended(bool) {}
uint32_t AudioSystem::play(const std::string&, const PlayOptions&) { return 0; }
uint32_t AudioSystem::playAt(const std::string&, const Vec3&, const PlayOptions&) { return 0; }
uint32_t AudioSystem::play(const std::string&, float, bool) { return 0; }
uint32_t AudioSystem::playAt(const std::string&, const Vec3&, float, bool, float) { return 0; }
void AudioSystem::stop(uint32_t, float) {}
void AudioSystem::stopAll() {}
bool AudioSystem::playing(uint32_t) const { return false; }
void AudioSystem::setVolume(uint32_t, float, float) {}
void AudioSystem::setPitch(uint32_t, float) {}
bool AudioSystem::preload(const std::string&) { return false; }
size_t AudioSystem::pendingLoads() { return 0; }
void AudioSystem::music(const std::string& file, float, float, const std::string&, bool) { m_musicFile = file; }
void AudioSystem::stopMusic(float) { m_musicFile.clear(); }
void AudioSystem::setMusicVolume(float volume, float) { m_musicVolume = volume; }
void AudioSystem::setMasterVolume(float volume) { m_master = volume; }
void AudioSystem::setGroupVolume(const std::string&, float, float) {}
void AudioSystem::resetMix() {
    m_master = 1.0f;
    m_musicVolume = 1.0f;
}
void AudioSystem::update(const Vec3&, const Vec3&, const Vec3&, float) {}
size_t AudioSystem::activeCount() const { return 0; }
uint32_t AudioSystem::start(const std::string&, const PlayOptions&, bool, const Vec3&, bool, uint64_t) { return 0; }

} // namespace eruption
