#version 450
#extension GL_EXT_nonuniform_qualifier : enable
#extension GL_GOOGLE_include_directive : enable

#include "lighting/pbr_common.glsl"

layout(location = 0) in vec3 inWorldPos;
layout(location = 0) out vec4 outLit;

layout(set = 0, binding = 0) uniform sampler2D gbufferAlbedo;
layout(set = 0, binding = 1) uniform sampler2D gbufferNormal;
layout(set = 0, binding = 2) uniform sampler2D gbufferDepth;
layout(set = 0, binding = 3) uniform sampler2D gbufferPBR;

layout(set = 0, binding = 7) uniform LightingUBO {
    vec4 dirLightDir;       // xyz=direção, w=intensidade
    vec4 dirLightColor;     // xyz=cor RGB, w=ambientIntensity
    vec4 pointLights[128];
    vec4 pointColors[128];
    uint numPointLights;
    float giIntensity;
    float giAmbientFloor;
    uint usePbr;        // 0 = legacy Blinn-Phong, 1 = Cook-Torrance/GGX
    uint pbrDebugMode;  // bitmask: 1=roughness, 2=metallic, 4=normal, 8=wetness, 16=source
    float rainIntensity;
    float snowIntensity;
    float temperatureC;
    float pbrLightScale;
    float sunAngularRadius; // era pad0; raio angular da fonte, em radianos
    uint pad1;
    vec4 ambientSky;
    vec4 ambientGround;
} lights;

layout(push_constant) uniform PushConstants {
    vec3 lightPos;
    float radius;
    vec3 lightColor;
    float intensity;
    uint shadowCubemapIndex;
    // TRES FLOATS, NAO vec3. Em std430 um vec3 alinha em 16 bytes: declarado
    // aqui ele cairia no offset 48 e empurraria invViewProj pra 64 - mas o
    // C++ (renderPointLights) escreve cameraPos em 36..47 e invViewProj em
    // 48. Foi exatamente esse deslocamento de 16 bytes que fez o frag ler
    // as tres matrizes erradas: worldPos reconstruido virava lixo,
    // `dist > radius` descartava tudo e NENHUMA point light aparecia
    // (lava, lanternas de cidade-D, fogueira). Float alinha em 4: 36/40/44.
    float cameraPosX;
    float cameraPosY;
    float cameraPosZ;
    mat4 invViewProj;
    mat4 viewProj;
    mat4 modelMatrix;
};

vec3 reconstructPosition(vec2 uv, float depth) {
    vec4 clip = vec4(uv * 2.0 - 1.0, depth, 1.0);
    vec4 world = invViewProj * clip;
    return world.xyz / world.w;
}

vec3 srgbToLinear(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}
vec3 linearToSrgb(vec3 c) {
    vec3 lo = c * 12.92;
    vec3 hi = pow(c, vec3(1.0 / 2.4)) * 1.055 - vec3(0.055);
    return mix(lo, hi, step(vec3(0.0031308), c));
}

void main() {
    vec3 cameraPos = vec3(cameraPosX, cameraPosY, cameraPosZ);

    vec2 uv = gl_FragCoord.xy / vec2(textureSize(gbufferAlbedo, 0));

    // gbufferAlbedo.a is NOT a reliable occupancy flag: terrain.frag packs
    // vertex AO there instead of a solid/1.0 constant (only model.frag writes
    // 1.0), so checking it for "is there geometry" discarded most terrain
    // pixels. Depth is the real "empty sky" signal everywhere else in the
    // engine (see post_composite.frag's fog gate).
    float depth = texture(gbufferDepth, uv).r;
    if (depth >= 0.99999) discard;
    // Visualizacao crua do G-buffer: so' o passe ambiente escreve (ver
    // ambient.frag). Luz pontual tambem soma, entao sai fora.
    if ((lights.pbrDebugMode & 95u) != 0u) discard;

    vec4 albedo = texture(gbufferAlbedo, uv);
    // Diagnostico (bit 32 de pbrDebugMode, ERUPTION_DEBUG_FLAT_ALBEDO=1): albedo
    // CIANO chapado em tudo - modelo, terreno, sprite - so' a iluminacao e a
    // sombra sobrevivem. Isola cintilacao de sombra de cintilacao de textura.
    if ((lights.pbrDebugMode & 32u) != 0u) albedo.rgb = vec3(0.0, 1.0, 1.0);
    vec3 normal = texture(gbufferNormal, uv).rgb;
    vec4 pbr = texture(gbufferPBR, uv);

    vec3 worldPos = reconstructPosition(uv, depth);

    vec3 toLight = lightPos - worldPos;
    float dist = length(toLight);
    if (dist > radius) discard;
    vec3 L = normalize(toLight);

    vec3 Nraw = normal * 2.0 - 1.0;
    vec3 N = normalize(Nraw);
    if (any(isnan(N)) || any(isinf(N)) || length(Nraw) < 1e-6) {
        N = vec3(0.0, 1.0, 0.0);
    }
    // A direcao de vista sai da CAMERA, nao da origem do mundo.
    //
    // Isto era `normalize(-worldPos)`, que so' estaria certo se a camera
    // estivesse exatamente em (0,0,0). Como nenhum mapa fica na origem, TODO
    // brilho especular de luz pontual estava no lugar errado, e o erro crescia
    // com a distancia do mapa ate' a origem - no parana_field, cujo terreno vai
    // de 0 a 2000 unidades, o vetor de vista chegava a apontar quase 90 graus
    // fora. O reflexo da tocha aparecia do lado errado do objeto.
    vec3 toEye = cameraPos - worldPos;
    vec3 viewDir = (dot(toEye, toEye) > 1e-12) ? normalize(toEye) : vec3(0.0, 0.0, 1.0);

    // Cuspid attenuation (SDD 2.4.2)
    float s = dist / radius;
    float s2 = s * s;
    float att = pow(1.0 - s2, 2.0) / (1.0 + 1.0 * s2);

    float roughness = pbr.g;
    float metallic = pbr.b;

    vec3 lit;
    if (lights.usePbr == 0u) {
        // Legacy fallback
        float NdotL = max(dot(N, L), 0.0);
        lit = albedo.rgb * lightColor * NdotL * intensity * att;
    } else {
        // PBR expects albedo in linear space; convert and tone-map back.
        vec3 albedoLinear = srgbToLinear(albedo.rgb);
        vec3 radiance = pbrCookTorrance(N, viewDir, L, albedoLinear, metallic, roughness, lightColor, intensity * att) * lights.pbrLightScale;
        // Keep the lit image in linear space; post_composite.frag applies the final gamma.
        lit = max(radiance, vec3(0.0));
    }

    outLit = vec4(lit, 1.0);
}
