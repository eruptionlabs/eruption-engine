// CALIBRACAO DE ALBEDO (preset: albedo_knee / albedo_ceiling; knee <= 0 desliga).
// Textura pintada para um motor sem iluminacao traz a luz embutida: calcada
// de albedo linear ~0,9 vira branco sob o sol. Faixas fisicas de dieletrico
// (Filament: sRGB 50-240 estrito; Unreal: concreto 0,51, neve fresca 0,81)
// ficam bem abaixo disso. Acima do joelho a LUMINANCIA linear e' comprimida
// suavemente (exponencial) para no maximo `ceiling`; a croma (razao entre
// canais) e' preservada, entao o matiz nao muda. Abaixo do joelho o albedo
// passa intacto. Chamada na escrita do G-buffer (modelo e terreno): todos os
// passes de luz, e o reflexo da agua, leem o mesmo valor ja' calibrado.
vec3 albedoCalibrate(vec3 srgb, vec2 kneeCeiling) {
    const float knee = kneeCeiling.x;
    if (knee <= 0.0) return srgb;
    const float ceil_ = max(kneeCeiling.y, knee + 0.01);
    vec3 c = clamp(srgb, 0.0, 1.0);
    vec3 lin = mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
    const float y = dot(lin, vec3(0.2126, 0.7152, 0.0722));
    if (y <= knee) return srgb;
    const float y2 = knee + (ceil_ - knee) * (1.0 - exp(-(y - knee) / (ceil_ - knee)));
    lin *= y2 / y;
    const vec3 lo = lin * 12.92;
    const vec3 hi = pow(lin, vec3(1.0 / 2.4)) * 1.055 - vec3(0.055);
    return mix(lo, hi, step(vec3(0.0031308), lin));
}
