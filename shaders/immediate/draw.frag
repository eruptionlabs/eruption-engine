#version 450
// Textura (PNG sRGB, amostrada em linear) vezes a cor do vertice. A cor sai em
// linear/HDR para a imagem iluminada (pode passar de 1); o blend (aditivo ou
// alfa) e' do pipeline.
//
// Material opcional (push constant, flags = 0 e' o comportamento de sempre,
// byte a byte): so' mexe no alfa.
//   bit 0  fresnel: alfa *= pow(1-|N.V|, p) (ou pow(|N.V|, p), o "nucleo");
//   bit 1  erosao: ruido (set 1) rolando em UV; alfa *= limiar suave;
//   bit 2  soft: alfa *= (profundidade da cena - a do fragmento) / dist;
//   bit 3  lit: cor *= sol da cena (N.L) + ambiente hemisferico; mexe na cor, nao no alfa.
layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor;
layout(location = 2) in vec3 vNormal;
layout(location = 3) in vec3 vWorld;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D uTex;
layout(set = 1, binding = 0) uniform sampler2D uNoise;
layout(set = 2, binding = 0) uniform sampler2D uDepth;
// Luz da cena (so' le' quem liga o bit 3), os mesmos numeros do FrameUBO do deferred.
layout(set = 3, binding = 0) uniform SceneLight {
    vec4 sunDir;        // xyz = para onde a luz viaja, w = intensidade
    vec4 sunColor;      // rgb
    vec4 ambientSky;    // rgb = zenite, w = intensidade do GI
    vec4 ambientGround; // rgb
} sc;
layout(push_constant) uniform PushConstants {
    layout(offset = 64) vec4 a; // x potencia do fresnel, y 1 = invertido, z distancia do soft, w flags
    vec4 b;                     // x,y deslocamento do ruido (scroll * tempo), z,w repeticao
    vec4 c;                     // x erosao 0..1, y borda da erosao, z,w = P[2][2], P[3][2] da projecao
    vec4 d;                     // x = P[2][3], yzw posicao da camera
} mat;
// Sem normal no vertice (fitas, quads): a da face, pela derivada da posicao.
vec3 surfaceNormal() {
    const float nl = dot(vNormal, vNormal);
    return nl > 0.25 ? vNormal * inversesqrt(nl) : normalize(cross(dFdx(vWorld), dFdy(vWorld)) + vec3(1e-12));
}
void main() {
    vec4 col = texture(uTex, vUV) * vColor;
    const int fl = int(mat.a.w + 0.5); // uniforme: as derivadas abaixo ficam em fluxo uniforme
    if (fl != 0) {
        if ((fl & 8) != 0) {
            // Mesma conta do deferred: albedo * (sol * N.L * intensidade + hemisferio * GI).
            // Face de tras (malha sem cull): a normal vira para o lado que se ve.
            vec3 n = surfaceNormal();
            if (!gl_FrontFacing) n = -n;
            const vec3 sun = sc.sunColor.rgb * (sc.sunDir.w * max(dot(n, -normalize(sc.sunDir.xyz)), 0.0));
            const vec3 hemi = mix(sc.ambientGround.rgb, sc.ambientSky.rgb, 0.5 + 0.5 * n.y) * sc.ambientSky.w;
            col.rgb *= sun + hemi;
        }
        if ((fl & 1) != 0) {
            const vec3 n = surfaceNormal();
            const float nv = clamp(abs(dot(n, normalize(mat.d.yzw - vWorld))), 0.0, 1.0);
            col.a *= pow(max(mat.a.y > 0.5 ? nv : 1.0 - nv, 1e-4), mat.a.x);
        }
        if ((fl & 2) != 0) {
            const float nz = texture(uNoise, vUV * mat.b.zw + mat.b.xy).r;
            const float soft = max(mat.c.y, 1e-3);
            // erosao 0 = nada some; 1 = tudo some (o limiar anda de -soft ate' 1).
            const float t = mat.c.x * (1.0 + soft);
            col.a *= smoothstep(t - soft, t, nz);
        }
        if ((fl & 4) != 0) {
            // Profundidade de vista (w do clip) da cena no pixel e a deste fragmento.
            const float dz = texelFetch(uDepth, ivec2(gl_FragCoord.xy), 0).r;
            const float wScene = mat.c.w * mat.d.x / (dz * mat.d.x - mat.c.z);
            col.a *= clamp((wScene - 1.0 / gl_FragCoord.w) / max(mat.a.z, 1e-4), 0.0, 1.0);
        }
    }
    outColor = col;
}
