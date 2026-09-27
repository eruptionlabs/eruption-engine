#version 450

// Fullscreen triangle for the rain-occlusion debug map (no vertex inputs).
layout(location = 0) out vec2 oUV;

void main() {
    vec2 p = vec2(gl_VertexIndex == 2 ? 3.0 : -1.0,
                  gl_VertexIndex == 1 ? 3.0 : -1.0);
    oUV = p * 0.5 + 0.5;
    gl_Position = vec4(p, 0.0, 1.0);
}
