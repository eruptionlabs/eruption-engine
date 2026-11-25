#version 450
// Desenho imediato de script (world.draw_strip / world.draw_sprite / world.draw_mesh):
// vertice ja' no mundo, cor RGBA por vertice, normal opcional (zero = sem normal,
// o fragmento usa a da face). Sem constante de especializacao (celulares
// fracos); push constant do vertice so' a viewProj (64 bytes), os outros 64 sao
// do fragmento (material).
layout(location = 0) in vec3 inPos;
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec4 inColor;
layout(location = 3) in vec3 inNormal;
layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vColor;
layout(location = 2) out vec3 vNormal;
layout(location = 3) out vec3 vWorld;
layout(push_constant) uniform PushConstants { mat4 viewProj; } pc;
void main() {
    gl_Position = pc.viewProj * vec4(inPos, 1.0);
    vUV = inUV;
    vColor = inColor;
    vNormal = inNormal;
    vWorld = inPos;
}
