#pragma once

// Espelho do dispositivo Vulkan nulo (VulkanNull.cpp). O motor sobe malhas e
// texturas como no desktop (staging mapeado + vkCmdCopy*); o driver nulo
// copia esses bytes na hora: buffer de GPU vira uma copia na memoria do wasm,
// imagem vira textura GL. O desenho do navegador (core/EngineWeb.cpp) le
// daqui, pelos mesmos handles que o renderer Vulkan guarda.
#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>

namespace eruption::web {

// Bytes de um buffer: a copia (buffer de GPU) ou a memoria mapeada (buffer
// visivel ao host, escrito pelo motor a cada quadro). Nulo se nunca recebeu
// dado. `version` muda a cada copia para dentro do buffer.
const uint8_t* bufferBytes(VkBuffer buffer, size_t* size = nullptr, uint64_t* version = nullptr);

// Textura GL (GLuint) de uma imagem/view, ou 0 se nenhum texel chegou ainda.
// `target` recebe GL_TEXTURE_2D, GL_TEXTURE_2D_ARRAY ou GL_TEXTURE_3D.
unsigned imageTexture(VkImage image, unsigned* target = nullptr);
unsigned viewTexture(VkImageView view, unsigned* target = nullptr);

// Slot de textura (bindless ou camada) -> view, registrado por quem atribui
// o slot (BindlessDescriptor, TextureLayers).
void setSlotView(uint32_t slot, VkImageView view);
unsigned slotTexture(uint32_t slot, unsigned* target = nullptr);
// Tamanho do nivel 0 da imagem do slot; falso se o slot nao tem imagem.
bool slotExtent(uint32_t slot, uint32_t* width, uint32_t* height);

} // namespace eruption::web
