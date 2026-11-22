// Modulo de efeitos de particula desligado (ERUPTION_MODULE_VFX=OFF): o jogo
// nao usa vfx.*, efeitos de mapa nem particulas de ambiente, e o programa nao
// leva o simulador na GPU (pipelines, buffers, grafo). Mesma interface:
// init() falha, que o motor ja' trata ("[VFX] indisponivel"), e nada desenha,
// entao nenhum descritor do sistema chega a ser usado (o FSR tem mascara
// reativa propria de reserva; o pos pula a distorcao quando a vista e' nula).
#include "renderer/vfx/VfxSystem.hpp"

namespace eruption {

bool VfxSystem::init(const InitInfo&) { return false; }
void VfxSystem::shutdown() {}
void VfxSystem::setBudget(uint32_t capacity, float) { m_capacity = capacity; }
const VfxEffectDesc* VfxSystem::effect(const std::string&) { return nullptr; }
void VfxSystem::setEffect(const std::string&, const VfxEffectDesc&) {}
void VfxSystem::forgetEffect(const std::string&) {}
uint32_t VfxSystem::spawn(const std::string&, const Mat4&) { return 0; }
void VfxSystem::setColor(uint32_t, const Vec3&, bool) {}
void VfxSystem::setVision(uint32_t, bool) {}
void VfxSystem::setEmitterOffset(uint32_t, const std::string&, const Vec3&) {}
void VfxSystem::setTransform(uint32_t, const Mat4&) {}
void VfxSystem::setRateScale(uint32_t, float) {}
void VfxSystem::stop(uint32_t, bool) {}
void VfxSystem::restart(uint32_t) {}
bool VfxSystem::isPlaying(uint32_t) const { return false; }
float VfxSystem::instanceTime(uint32_t) const { return 0.0f; }
void VfxSystem::clear() {}
void VfxSystem::setGroundHeights(const std::vector<float>&, uint32_t, uint32_t, Vec2, float) {}
void VfxSystem::update(float, const Camera&, VkExtent2D) {}
void VfxSystem::simulate(VkCommandBuffer, VkImageView) {}
void VfxSystem::drawLowRes(VkCommandBuffer, VkImageView, VkExtent2D) {}
void VfxSystem::draw(VkCommandBuffer, VkImage, VkImageView, VkImageView, VkExtent2D) {}
void VfxSystem::drawMeshes(VkCommandBuffer, VkImageView, VkExtent2D, const Mat4&, const Mat4&) {}
void VfxSystem::drawMeshShadows(VkCommandBuffer, const Mat4&) {}
void VfxSystem::collectLights(const Vec3&, size_t, std::vector<Light>& out) const { out.clear(); }
void VfxSystem::endFrame(VkCommandBuffer) {}
bool VfxSystem::resize(uint32_t, uint32_t) { return false; }
void VfxSystem::debugDump() {}

} // namespace eruption
