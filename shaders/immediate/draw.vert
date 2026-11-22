#version 450
// Desenho imediato de script (world.draw_strip / world.draw_sprite): vertice
// ja' no mundo, cor RGBA por vertice. Sem constante de especializacao (celulares
// fracos); push constant so' a viewProj (64 bytes).
layout(location = 0) in vec3 inPos;
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec4 inColor;
layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vColor;
layout(push_constant) uniform PushConstants { mat4 viewProj; } pc;
void main() {
    gl_Position = pc.viewProj * vec4(inPos, 1.0);
    vUV = inUV;
    vColor = inColor;
}
