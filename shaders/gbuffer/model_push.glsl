// Bloco de push constants dos fragmentos de modelo (model_frag.glsl e model_min.frag): UM texto so' para
// os dois, o layout tem de ser identico ao do vertex e ao de ModelRenderer.
// 96 B (sem a matriz model e o uvTranslateRot, que nenhum shader lia): cabe
// nos 128 B de push constant garantidos (GPU de celular).
layout(push_constant) uniform PushConstants {
    mat4 viewProjection;
    float alpha;
    float metallicScale;  // absolute metallic value from the material profile
    float roughnessScale; // absolute roughness value from the material profile
    float _pad;           // +1 ERUPTION_TEST_MAGENTA_DEBUG, +2 material absoluto, +4 normal da face, +8*n brilho n/8 (nao e' padding!)
    // O fragment so' precisava ate' aqui, mas o bloco de push tem 96 bytes
    // (static_assert em ModelRenderer.cpp) e `uvScale` fecha a conta. Declarar
    // o campo inteiro e' o que permite usar `uvScale.x`, que esta' livre
    // (o vertex le' so' .z e .w), como interruptor do ladrilhamento — sem
    // crescer o bloco, que mudaria o layout que o vertex espera.
    vec4 uvScale;         // x = ladrilhamento estocastico (0/1), y livre, z/w = vertex
} push;
