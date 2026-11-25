// RAJADAS LOCAIS na sombra (world.wind_burst): a MESMA rotacao em torno da base
// de gbuffer/model_vert.glsl applyGusts, para a sombra acompanhar a copa que
// gira. Quem inclui declara no push constant (offsets 84..123) as duas rajadas
// mais fortes do quadro, empacotadas por ModelRenderer::setShadowGusts:
//   gA = half2(raio, forca de agora)     gB = half2(frente 0..1, duracao s)
//   gC = unorm4x8(largura da frente, forca atras, idade/duracao, 0)
//   gX, gZ = centro (float).
// Sem tremor da copa (nao se ve na sombra). Mantenha as contas IGUAIS as do
// model_vert.glsl; forca 0 = rajada vazia.
vec3 shadowGustOne(vec3 worldPos, float h, vec2 origin, uint a, uint b, uint c, float gx, float gz) {
    const vec2 rs = unpackHalf2x16(a);
    if (rs.y <= 0.0 || rs.x <= 0.0) return worldPos;
    const vec2 fd = unpackHalf2x16(b);
    const vec4 sh = unpackUnorm4x8(c);
    const vec2 d = origin - vec2(gx, gz);
    const float dist = length(d);
    const float fall = 1.0 - smoothstep(0.0, rs.x, dist);
    const float front = (dist / rs.x - fd.x) / max(sh.x, 1e-3);
    const float onFront = exp(-front * front);
    const float tArr = pow(min(dist / rs.x, 1.0), 1.6667);
    const float tau = max(sh.z - tArr, 0.0) * fd.y;
    const float behind = smoothstep(0.0, 0.04, sh.z - tArr);
    const float spring = behind * exp(-tau * 1.3) * sin(tau * 9.0);
    const float k = rs.y * fall * ((sh.y + (1.0 - sh.y) * onFront) + 0.7 * spring);
    const float theta = clamp(k, -1.7, 1.7) * 0.3 * (1.0 - exp(-h / 12.0));
    const vec2 dir = dist > 1e-3 ? d / dist : vec2(0.0);
    worldPos.xz += dir * (h * sin(theta));
    worldPos.y += h * (cos(theta) - 1.0);
    return worldPos;
}
