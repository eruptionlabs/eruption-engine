#version 450
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

layout(location = 0) in vec2 v_uv;

layout(set = 0, binding = 0) uniform sampler2D u_worldPos;
layout(set = 0, binding = 1) uniform sampler2D u_cloudColor;

layout(push_constant) uniform PushConstants {
    vec2 u_worldMin;
    vec2 u_worldMax;
    float u_blend;
    float u_pad;
} pc;

layout(location = 0) out vec4 o_color;

void main() {
    vec4 cloud = texture(u_cloudColor, v_uv);

    // If the ray-march wrote nothing, paint a strong magenta tint so it is
    // unmistakable that the debug view is active but no cloud was generated.
    if (cloud.a < 0.001) {
        o_color = vec4(1.0, 0.0, 1.0, pc.u_blend);
        return;
    }

    // Show the raw cloud color scaled up so faint clouds are visible.
    o_color = vec4(cloud.rgb * 2.0, pc.u_blend);
}
