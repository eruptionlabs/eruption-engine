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
