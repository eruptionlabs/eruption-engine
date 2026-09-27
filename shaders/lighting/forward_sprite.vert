#version 450

layout(location = 0) in vec2 inPos;
layout(location = 1) in vec2 inUV;

// Instance attributes
layout(location = 2) in mat4 inModel;
layout(location = 6) in vec4 inAnchor; // Updated location
layout(location = 7) in vec4 inTexRect;
layout(location = 8) in uint inTexIndex;
layout(location = 9) in uint inNormalTexIndex;
layout(location = 10) in uint inMrahwTexIndex;
layout(location = 11) in uint inFlags;
layout(location = 12) in uint inPaletteIndex;
layout(location = 13) in vec4 inTint;

layout(location = 0) out vec2 outUV;
layout(location = 1) out vec3 outWorldPos;
layout(location = 2) out vec3 outNormal;
layout(location = 3) out flat uint outTexIndex;
layout(location = 4) out vec4 outTint;
layout(location = 5) out flat uint outFlags;

layout(set = 1, binding = 0) uniform FrameUBO {
    mat4 view;
    mat4 projection;
    mat4 viewProjection;
    mat4 inverseView;
    mat4 inverseProjection;
    vec3 cameraPos;
    float time;
    vec2 screenResolution;
    float nearPlane;
    float farPlane;
    uint frameIndex;
    uint debugMode;
    float spriteExposure;
    float giIntensity;
    float giAmbientFloor;
    float spriteTilt;
    float shadowHeightScale;
    float spriteNormalYMix;
    float normalMapScale;
    float normalMapInvertY;
    float normalSmoothing;
    float defaultRoughness;
    float defaultMetallic;
    vec4 sunDir;
    vec4 sunColor;
    vec4 ambientSky;
    vec4 ambientGround;
} frame;

void main() {
    bool billboard = (inFlags & 4) != 0;
    
    vec3 worldPosCenter = vec3(inModel[3][0], inModel[3][1], inModel[3][2]);
    float scaleX = length(vec3(inModel[0]));
    float scaleY = length(vec3(inModel[1]));

    vec3 finalPos;
    vec3 normal;

    if (billboard) {
        // Cylindrical Billboard — pure rotation around vertical axis.
        // The sprite rotates in place to face the camera horizontally,
        // but stays vertically upright (no tilt). This prevents the
        // "joão bobo" wobble while keeping multi-part sprites aligned.
        vec3 toCam = normalize(frame.cameraPos - inAnchor.xyz);
        vec3 worldUp = vec3(0.0, 1.0, 0.0);
        vec3 right;
        if (abs(dot(toCam, worldUp)) > 0.99) {
            right = vec3(1.0, 0.0, 0.0);
        } else {
            right = normalize(cross(worldUp, toCam));
        }
        vec3 up = worldUp;

        vec3 offset = worldPosCenter - inAnchor.xyz;
        finalPos = inAnchor.xyz + 
                   right * (offset.x + inPos.x * scaleX) + 
                   up    * (offset.y + inPos.y * scaleY);

        normal = toCam;
    } else {
        vec3 right = normalize(vec3(inModel[0]));
        vec3 up = normalize(vec3(inModel[1]));
        finalPos = worldPosCenter + (inPos.x * right * scaleX + inPos.y * up * scaleY);
        normal = normalize(cross(right, up));
    }

    gl_Position = frame.viewProjection * vec4(finalPos, 1.0);
    outWorldPos = finalPos;
    outNormal = normal;
    outUV = inTexRect.xy + inUV * (inTexRect.zw);
    outTexIndex = inTexIndex;
    outTint = inTint;
    outFlags = inFlags;
}
