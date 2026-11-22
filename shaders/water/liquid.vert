#version 450

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aTexCoord;

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec2 vTexCoord;
layout(location = 2) out vec3 vNormal;
layout(location = 3) out vec3 vTangent;
layout(location = 4) out vec3 vBitangent;
// Nivel desta agua (o aberto e cada mapa vizinho tem o seu), para a espessura.
layout(location = 5) flat out float vLevel;

// Igual ao do liquid.frag, campo a campo: no WebGL2 os dois estagios ligam o mesmo bloco pelo nome.
layout(set = 1, binding = 0) uniform WaterUBO {
    vec4 baseColorDeep;
    vec4 baseColorShallow;
    vec4 waterParams;         // x=transparency, y=refractionStrength, z=reflectivity, w=roughness
    vec4 normalParams;        // x=scale, y=waveSpeed, z=strength, w=lamina da praia (1/0)
    vec4 waveParams;          // x=amplitude, y=frequency, z=speed, w=unused
    vec4 foamParams;          // x=edgeDepth, y=contactStrength, z=enable, w=unused
    vec4 skyTop;
    vec4 skyHorizon;
    vec4 sunDirIntensity;
    vec4 sunColor;
    vec4 ambientColor;
    vec4 cameraPos;
    vec4 waterLevelAndPlanes; // x=waterLevel, y=nearPlane, z=farPlane, w=time
    vec4 screenSize;          // x=viewportW, y=viewportH, z=cameraPitch, w=unused
    vec4 waveDirAmp[3];
    vec4 waveSpeedSteep[3];   // x=speed, y=steepness; zw = nuvens do ceu: [0] cobertura, escala; [1] velocidade, maciez; [2] tempo do ceu
    ivec4 textureSlots;       // x=screenTex, y=depthTex, z=foamMask, w=waterTex
    ivec4 skyboxSlot;         // x=skyboxTex, y=forca das nuvens no reflexo (milesimos), z=nuvens do ceu ligadas, w=unused
    mat4 invViewProj;
    vec4 extraParams;         // x=absorptionCoeff, y=maxPxScale, z=enableCaustics, w=unused
    vec4 foamSurfaceParams;   // x=foamScale, y=foamSpeed, z=foamRoughness, w=enableSurfaceFoam
    vec4 normalAdvanced;      // x=normalOctaves, y=lavaEmissive, zw=unused
    vec4 causticsParams;      // x=intensity, y=depthAttenuation, z=lava flowSpeed, w=lava crustWiden
    vec4 weatherParams;       // x=sunOcclusion, y=moonOcclusion, z=stormDarken, w=rainIntensity
    vec4 lavaCrust;           // xyz=tinta da crosta, w=deslocamento da abertura (crustCoverage)
    vec4 styleParams;         // x=diorama (0/1), y=forca do reflexo, z=brilhos, w=aneis de chuva
    vec4 styleParams2;        // x=espuma de margem, y=nivel do SSPR, z=distorcao do reflexo, w=clareza
    vec4 styleDeep;           // xyz=cor funda, w=inclinacao das ondas
    ivec4 styleSlots;         // x=slot do reflexo planar (-1 = sem), y=slot do reflexo espelhado (-1 = sem)
} ubo;

layout(push_constant) uniform Push {
    mat4 viewProj;
    ivec4 debugMask;   // x = bits de A/B do menu, y = diagnostico de cobertura
} pc;

vec3 gerstnerWave(vec2 pos, vec2 dir, float amp, float freq, float speed, float steepness, float t) {
    float phase = dot(dir, pos) * freq + speed * t;
    float c = cos(phase);
    float s = sin(phase);
    float Q = steepness;
    return vec3(Q * amp * dir.x * c, amp * s, Q * amp * dir.y * c);
}

vec3 gerstnerTangentX(vec2 pos, vec2 dir, float amp, float freq, float speed, float steepness, float t) {
    float phase = dot(dir, pos) * freq + speed * t;
    float c = cos(phase);
    float s = sin(phase);
    float wa = freq * amp;
    float Q = steepness;
    return vec3(1.0 - Q * dir.x * dir.x * wa * s, dir.x * wa * c, -Q * dir.x * dir.y * wa * s);
}

vec3 gerstnerTangentZ(vec2 pos, vec2 dir, float amp, float freq, float speed, float steepness, float t) {
    float phase = dot(dir, pos) * freq + speed * t;
    float c = cos(phase);
    float s = sin(phase);
    float wa = freq * amp;
    float Q = steepness;
    return vec3(-Q * dir.x * dir.y * wa * s, dir.y * wa * c, 1.0 - Q * dir.y * dir.y * wa * s);
}

void main() {
    vTexCoord = aTexCoord;

    float t            = ubo.waterLevelAndPlanes.w;
    float waterLevel   = ubo.waterLevelAndPlanes.x;
    float globalAmp    = ubo.waveParams.x;
    float globalFreq   = ubo.waveParams.y;
    float globalSpeed  = ubo.waveParams.z;

    // A malha ja' traz o nivel de cada mapa na altura do vertice; o uniforme
    // so' soma o ajuste do menu. (Antes a altura era trocada pelo nivel do
    // mapa aberto, e a agua de um vizinho com lago em outra altura subia ou
    // descia para ele.)
    vec3 pos = aPos;
    pos.y = aPos.y + ubo.waveParams.w;
    vLevel = pos.y;
    vec2 baseXZ = aPos.xz;

    vec3 tangentX = vec3(1.0, 0.0, 0.0);
    vec3 tangentZ = vec3(0.0, 0.0, 1.0);

    // Agua texturada (liquidKind 4, modo classico): uma onda senoidal simples
    // na altura, sem Gerstner nem normal (a cor vem so' da textura).
    if (int(ubo.extraParams.w + 0.5) == 4) {
        pos.y += globalAmp * sin((baseXZ.x + baseXZ.y) * globalFreq + globalSpeed * t);
        vWorldPos = pos;
        vNormal = vec3(0.0, 1.0, 0.0);
        vTangent = vec3(1.0, 0.0, 0.0);
        vBitangent = vec3(0.0, 0.0, 1.0);
        gl_Position = pc.viewProj * vec4(pos, 1.0);
        return;
    }

    for (int i = 0; i < 3; ++i) {
        vec2 dir  = ubo.waveDirAmp[i].xy;
        float amp = ubo.waveDirAmp[i].z * globalAmp;
        float freq = ubo.waveDirAmp[i].w * globalFreq;
        float spd = ubo.waveSpeedSteep[i].x * globalSpeed;
        float Q   = ubo.waveSpeedSteep[i].y;

        pos += gerstnerWave(baseXZ, dir, amp, freq, spd, Q, t);
        tangentX += gerstnerTangentX(baseXZ, dir, amp, freq, spd, Q, t);
        tangentZ += gerstnerTangentZ(baseXZ, dir, amp, freq, spd, Q, t);
    }

    vWorldPos = pos;
    vec3 normal = cross(tangentZ, tangentX);
    float nLen = length(normal);
    vNormal    = (nLen > 1e-5) ? (normal / nLen) : vec3(0.0, 1.0, 0.0);
    // Gram-Schmidt orthonormalize TBN
    vec3 tProj = tangentX - vNormal * dot(tangentX, vNormal);
    float tLen = length(tProj);
    vTangent   = (tLen > 1e-5) ? (tProj / tLen) : vec3(1.0, 0.0, 0.0);
    vBitangent = cross(vNormal, vTangent);

    // NaN/Inf guards: if any TBN vector is corrupted, reset to flat plane
    if (any(isnan(vNormal)) || any(isinf(vNormal))) vNormal = vec3(0.0, 1.0, 0.0);
    if (any(isnan(vTangent)) || any(isinf(vTangent))) vTangent = vec3(1.0, 0.0, 0.0);
    if (any(isnan(vBitangent)) || any(isinf(vBitangent))) vBitangent = vec3(0.0, 0.0, 1.0);

    gl_Position = pc.viewProj * vec4(pos, 1.0);
}
