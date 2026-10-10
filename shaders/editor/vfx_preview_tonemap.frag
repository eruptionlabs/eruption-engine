#version 450
// Previa de efeitos do editor: HDR -> imagem da interface (Reinhard + gama).
layout(set = 0, binding = 0) uniform sampler2D u_hdr;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
void main() {
    vec3 c = texture(u_hdr, v_uv).rgb;
    c = c / (1.0 + c);
    o_color = vec4(pow(c, vec3(1.0 / 2.2)), 1.0);
}
