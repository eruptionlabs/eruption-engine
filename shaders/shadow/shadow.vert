#version 450
#extension GL_EXT_nonuniform_qualifier : enable

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec2 inTexCoord;
layout(location = 2) in vec3 inNormal;
layout(location = 3) in uint inTexIndex;
layout(location = 4) in uint inMatId;
layout(location = 5) in uint inColor;

layout(location = 0) out vec2 outTexCoord;
layout(location = 1) out flat uint outTexIndex;

layout(push_constant) uniform PushConstants {
    mat4 mvp;
};

void main() {
    gl_Position = mvp * vec4(inPosition, 1.0);
    outTexCoord = inTexCoord;
    outTexIndex = inTexIndex;
}
