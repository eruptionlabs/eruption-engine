#version 450

const vec2 verts[3] = vec2[](
    vec2(-1.0, -1.0),
    vec2( 3.0, -1.0),
    vec2(-1.0,  3.0)
);

layout(location = 0) out vec2 fragUV;

void main() {
    vec2 pos = verts[gl_VertexIndex % 3];
    fragUV = pos * 0.5 + 0.5;
    gl_Position = vec4(pos, 1.0, 1.0);
}
