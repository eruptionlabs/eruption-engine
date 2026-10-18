#version 450
// BORRAO GRANDE DO DoF, passo 2 (e 3): gaussiana separavel em 1/4 da
// resolucao sobre a cor pre-multiplicada de dof_far_down.frag. 13 amostras a
// cada 1,5 texel: raio de ~9 texels aqui, ~36 px na resolucao inteira.
layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D src_tex;

layout(push_constant) uniform PushConstants {
    vec2 dir; // (1,0) horizontal, (0,1) vertical
} push;

// Gaussiana de sigma 2 amostras, normalizada.
const float kW[7] = float[](0.1997, 0.1762, 0.1211, 0.0648, 0.0270, 0.0088, 0.0022);

void main() {
    vec2 step = push.dir * 1.5 / vec2(textureSize(src_tex, 0));
    vec4 acc = texture(src_tex, in_uv) * kW[0];
    for (int i = 1; i < 7; i++) {
        acc += (texture(src_tex, in_uv + step * float(i)) + texture(src_tex, in_uv - step * float(i))) * kW[i];
    }
    out_color = acc;
}
