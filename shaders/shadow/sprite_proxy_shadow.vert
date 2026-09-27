#version 450
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

// ============================================================================
// Sprite Proxy Shadow — Vertex Shader
// ============================================================================
// Renders a simple AABB proxy geometry for nearby sprites into the shadow map.
// This is used for high-quality shadow-map shadows at close range (0-10m).
// ============================================================================

layout(location = 0) in vec3 a_position;

layout(location = 1) in vec4 i_modelRow0;
layout(location = 2) in vec4 i_modelRow1;
layout(location = 3) in vec4 i_modelRow2;
layout(location = 4) in vec4 i_modelRow3;

layout(push_constant) uniform PushConstants {
    mat4 lightSpaceMatrix;
};

void main() {
    mat4 model = mat4(
        i_modelRow0,
        i_modelRow1,
        i_modelRow2,
        i_modelRow3
    );
    gl_Position = lightSpaceMatrix * model * vec4(a_position, 1.0);
}
