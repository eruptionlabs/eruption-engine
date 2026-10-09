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
uint32_t AudioSystem::play(const std::string&, float, bool) { return 0; }
uint32_t AudioSystem::playAt(const std::string&, const Vec3&, float, bool, float) { return 0; }
void AudioSystem::stop(uint32_t, float) {}
void AudioSystem::stopAll() {}
bool AudioSystem::playing(uint32_t) const { return false; }
void AudioSystem::music(const std::string& file, float, float) { m_musicFile = file; }
void AudioSystem::stopMusic(float) { m_musicFile.clear(); }
void AudioSystem::setMasterVolume(float volume) { m_master = volume; }
void AudioSystem::update(const Vec3&, const Vec3&, const Vec3&) {}
size_t AudioSystem::activeCount() const { return 0; }
uint32_t AudioSystem::start(const std::string&, float, bool, bool, const Vec3&, float, bool) { return 0; }

} // namespace eruption
