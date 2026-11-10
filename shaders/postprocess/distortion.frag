#version 450
// DISTORCAO DE TELA (calor, agua, onda de choque): desloca a leitura da cena
// pelo buffer que os emissores "distortion" do VFX escreveram
// (vfx_distort.frag; rg = deslocamento em fracao da altura da tela). Roda
// antes do DoF e do bloom, entao o que foi entortado desfoca e brilha junto.

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D scene_tex;
layout(set = 0, binding = 1) uniform sampler2D distort_tex;

layout(push_constant) uniform PushConstants {
    float strength; // preset "distortion" (0 desliga, 1 = como autorado)
    float aspect;   // altura / largura: fracao da altura -> uv em x
} pc;

void main() {
    vec2 d = texture(distort_tex, in_uv).rg * pc.strength;
    // Teto: 10% da tela, para um emissor exagerado nao rasgar a imagem.
    d = clamp(d, vec2(-0.1), vec2(0.1));
    vec2 uv = clamp(in_uv + vec2(d.x * pc.aspect, d.y), vec2(0.0), vec2(1.0));
    out_color = vec4(texture(scene_tex, uv).rgb, 1.0);
}
