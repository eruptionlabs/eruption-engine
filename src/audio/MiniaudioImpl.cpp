// Implementação da miniaudio (terceiros, compilada sem avisos). Ogg Vorbis pelo
// stb_vorbis que vem junto da miniaudio.
#define STB_VORBIS_HEADER_ONLY
#include <extras/stb_vorbis.c>

#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

#undef STB_VORBIS_HEADER_ONLY
#include <extras/stb_vorbis.c>

#if defined(__EMSCRIPTEN__)
// Navegador: o JS da miniaudio chama Module._ma_device__on_notification_unlocked
// no primeiro gesto (desbloqueio do som; no celular o AudioContext nasce
// suspenso), mas compilada como C++ a funcao sai com nome decorado e o
// Module nunca a tem. Exporta uma ponte em C e liga o nome esperado a ela.
#include <emscripten.h>

extern "C" EMSCRIPTEN_KEEPALIVE void eruption_ma_unlocked(ma_device* device) {
    ma_device__on_notification_unlocked(device);
}

namespace {
const int s_unlockBridge = [] {
    EM_ASM({ Module._ma_device__on_notification_unlocked = (p) => Module._eruption_ma_unlocked(p); });
    return 0;
}();
} // namespace
#endif
