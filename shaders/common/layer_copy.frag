#version 450
// Copia uma textura para uma camada do array (TextureLayers): o triângulo de
// tela cheia cobre a camada e o mip da fonte sai das derivadas. Fonte sRGB:
// a amostra chega linear e volta codificada, porque o array é UNORM e a
// decodificação fica no shader que lê (common/slot_textures.glsl).
layout(set = 0, binding = 0) uniform sampler2D u_source;
layout(push_constant) uniform Push { uint encodeSrgb; } push;
layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

void main() {
    vec4 c = texture(u_source, fragUV);
    if (push.encodeSrgb != 0u) {
        vec3 l = clamp(c.rgb, 0.0, 1.0);
        c.rgb = mix(l * 12.92, 1.055 * pow(l, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), l));
    }
    outColor = c;
}
