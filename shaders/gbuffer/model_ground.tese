#version 450
#extension GL_EXT_nonuniform_qualifier : enable
#extension GL_GOOGLE_include_directive : enable
// Chao (ModelRenderer, preset tessellation_ground_geomorph): niveis em
// potencia de 2 + geomorph. Ver model_tesc.glsl.
#define ERUPTION_TESS_GEOMORPH
#include "gbuffer/model_tese.glsl"
