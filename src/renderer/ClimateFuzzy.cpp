#include "renderer/ClimateFuzzy.hpp"
#include "core/Logger.hpp"

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sys/stat.h>

namespace eruption {

// -----------------------------------------------------------------------------
// FuzzyRange
// -----------------------------------------------------------------------------

float FuzzyRange::membership(float x) const {
    if (x <= supportMin || x >= supportMax) return 0.0f;
    if (x >= coreMin && x <= coreMax) return 1.0f;
    if (x < coreMin) {
        const float d = coreMin - supportMin;
        if (d <= 1e-6f) return 1.0f;
        const float t = (x - supportMin) / d;
        // smoothstep e nao rampa linear: a rampa linear tem derivada
        // descontinua nos joelhos, e isso aparece como "estalo" no momento em
        // que um clima comeca a entrar na mistura.
        return t * t * (3.0f - 2.0f * t);
    }
    const float d = supportMax - coreMax;
    if (d <= 1e-6f) return 1.0f;
    const float t = (supportMax - x) / d;
    return t * t * (3.0f - 2.0f * t);
}

bool FuzzyRange::isWildcard() const {
    // Cobre praticamente todo o eixo: o tipo declarou que este eixo nao o
    // define.
    return (supportMin <= -0.999f && supportMax >= 0.999f &&
            coreMin <= -0.999f && coreMax >= 0.999f);
}

bool FuzzyRange::coreOverlaps(const FuzzyRange& o) const {
    if (isWildcard() || o.isWildcard()) return true; // nao restringe
    return coreMin <= o.coreMax && o.coreMin <= coreMax;
}

// -----------------------------------------------------------------------------
// ClimateEnvelope
// -----------------------------------------------------------------------------

float ClimateEnvelope::membership(const WeatherSignature& s) const {
    if (!enabled) return 0.0f;
    // PRODUTO e nao minimo. Com o minimo (Mamdani classico) o resultado e'
    // decidido por um eixo so' e os demais nao diferenciam nada: dois climas
    // que so' diferem em vento ficam empatados. O produto faz cada eixo
    // contribuir, que e' o que se quer para desempatar tipos vizinhos.
    float m = temperature.membership(s.thermalEnergy);
    if (m <= 0.0f) return 0.0f;
    m *= humidity.membership(s.humidity);
    if (m <= 0.0f) return 0.0f;
    m *= wind.membership(s.wind);
    if (m <= 0.0f) return 0.0f;
    m *= instability.membership(s.turbulence);
    return m;
}

// Distancia do estado ao NUCLEO do envelope, 0 quando esta' dentro. Eixos
// normalizados para a mesma escala para que temperatura (-1..1) nao pese o
// dobro dos outros (0..1).
static float axisDistance(const FuzzyRange& r, float x, float scale) {
    if (x < r.coreMin) return (r.coreMin - x) * scale;
    if (x > r.coreMax) return (x - r.coreMax) * scale;
    return 0.0f;
}

float ClimateEnvelope::distanceTo(const WeatherSignature& s) const {
    const float dt = axisDistance(temperature, s.thermalEnergy, 0.5f);
    const float dh = axisDistance(humidity,    s.humidity,      1.0f);
    const float dv = axisDistance(wind,        s.wind,          1.0f);
    const float di = axisDistance(instability, s.turbulence,    1.0f);
    return std::sqrt(dt*dt + dh*dh + dv*dv + di*di);
}

WeatherSignature ClimateEnvelope::centroid() const {
    WeatherSignature s;
    s.thermalEnergy = (temperature.coreMin + temperature.coreMax) * 0.5f;
    s.humidity      = (humidity.coreMin + humidity.coreMax) * 0.5f;
    s.wind          = (wind.coreMin + wind.coreMax) * 0.5f;
    s.turbulence    = (instability.coreMin + instability.coreMax) * 0.5f;
    return s;
}

// -----------------------------------------------------------------------------
// ClimateFuzzySet
// -----------------------------------------------------------------------------

namespace {

FuzzyRange parseRange(const nlohmann::json& j, float lo, float hi) {
    FuzzyRange r;
    r.supportMin = lo; r.coreMin = lo; r.coreMax = hi; r.supportMax = hi;
    if (j.is_array() && j.size() >= 4) {
        r.supportMin = j[0].get<float>();
        r.coreMin    = j[1].get<float>();
        r.coreMax    = j[2].get<float>();
        r.supportMax = j[3].get<float>();
    } else if (j.is_array() && j.size() == 2) {
        // Forma curta [min, max]: nucleo igual ao suporte, sem ombro. Util
        // para um eixo que o autor quer duro.
        r.supportMin = r.coreMin = j[0].get<float>();
        r.coreMax = r.supportMax = j[1].get<float>();
    }
    // Ordem monotonica: um trapezio invertido produziria pertinencia negativa
    // ou buracos, e o erro seria silencioso.
    r.coreMin    = glm::max(r.coreMin, r.supportMin);
    r.coreMax    = glm::max(r.coreMax, r.coreMin);
    r.supportMax = glm::max(r.supportMax, r.coreMax);
    return r;
}

long long fileMtime(const std::string& p) {
    struct stat st{};
    if (stat(p.c_str(), &st) != 0) return 0;
    return static_cast<long long>(st.st_mtime);
}

} // namespace

bool ClimateFuzzySet::loadFromFile(const std::string& path) {
    m_path = path;
    std::ifstream f(path);
    if (!f.is_open()) return false;

    nlohmann::json j;
    try { f >> j; }
    catch (const std::exception& e) {
        ERUPTION_LOG_WARN("[CLIMA] %s: JSON invalido (%s); selecao automatica desligada",
                          path.c_str(), e.what());
        return false;
    }
    m_mtime = fileMtime(path);

    const auto types = j.find("types");
    if (types == j.end() || !types->is_object()) {
        ERUPTION_LOG_WARN("[CLIMA] %s sem objeto 'types'", path.c_str());
        return false;
    }

    m_envelopes.clear();
    for (auto it = types->begin(); it != types->end(); ++it) {
        const WeatherType t = weatherTypeFromName(it.key());
        // weatherTypeFromName devolve Clear (nao Count) para nome desconhecido,
        // entao comparar com Count nao detecta erro de digitacao: um JSON
        // inteiro com nomes errados carregaria como 40 envelopes de "clear" e
        // o sistema pareceria funcionar. A checagem confiavel e' o
        // round-trip - se o nome canonico do tipo nao bate com a chave, a
        // chave nao existe. Foi exatamente isso que aconteceu na primeira
        // versao deste arquivo (chaves em CamelCase; a engine usa snake_case).
        if (std::string(weatherTypeName(t)) != it.key()) {
            ERUPTION_LOG_WARN("[CLIMA] tipo desconhecido '%s' ignorado "
                              "(nomes sao snake_case, ex: partly_cloudy)", it.key().c_str());
            continue;
        }
        // Extreme fica FORA da selecao automatica de proposito: tornado,
        // cinza vulcanica e afins nao sao estados de massa de ar, sao eventos.
        // Eles continuam disponiveis na mao, que e' o uso deles.
        if (weatherTypeCategory(t) == WeatherCategory::Extreme) continue;

        const auto& e = it.value();
        ClimateEnvelope env;
        env.type = t;
        env.enabled = e.value("enabled", true);
        env.priority = e.value("priority", 1.0f);
        if (e.contains("temperature")) env.temperature = parseRange(e["temperature"], -1.0f, 1.0f);
        if (e.contains("humidity"))    env.humidity    = parseRange(e["humidity"], 0.0f, 1.0f);
        if (e.contains("wind"))        env.wind        = parseRange(e["wind"], 0.0f, 1.0f);
        if (e.contains("instability")) env.instability = parseRange(e["instability"], 0.0f, 1.0f);
        m_envelopes.push_back(env);
    }

    ERUPTION_LOG_WARN("[CLIMA] %s: %zu envelope(s) carregado(s)", path.c_str(), m_envelopes.size());

    // Validacao na carga, e de novo a cada hot-reload: o autor edita o JSON e
    // ve o resultado sem reiniciar.
    const auto overlaps = findOverlaps();
    if (!overlaps.empty()) {
        ERUPTION_LOG_WARN("[CLIMA] ATENCAO: %zu par(es) de clima com NUCLEO sobreposto.", overlaps.size());
        ERUPTION_LOG_WARN("[CLIMA] Nucleo em comum = existe estado atmosferico onde os DOIS estao");
        ERUPTION_LOG_WARN("[CLIMA] 100%% certos, e a escolha vira empate arbitrario. Suporte pode");
        ERUPTION_LOG_WARN("[CLIMA] (e deve) se sobrepor - e' ele que da' a transicao suave.");
        for (const auto& o : overlaps) {
            ERUPTION_LOG_WARN("[CLIMA]   %s  <->  %s", weatherTypeName(o.a), weatherTypeName(o.b));
        }
    } else {
        ERUPTION_LOG_WARN("[CLIMA] validacao: nenhum nucleo sobreposto.");
    }

    const float gaps = coverageGaps();
    if (gaps > 0.001f) {
        ERUPTION_LOG_WARN("[CLIMA] %.1f%% do espaco atmosferico nao e' coberto por clima nenhum "
                          "(nesses estados a selecao automatica nao tem resposta).", gaps * 100.0f);
    }
    return true;
}

bool ClimateFuzzySet::reloadIfModified() {
    if (m_path.empty()) return false;
    const long long t = fileMtime(m_path);
    if (t == 0 || t == m_mtime) return false;
    ERUPTION_LOG_WARN("[CLIMA] %s mudou, recarregando", m_path.c_str());
    return loadFromFile(m_path);
}

const ClimateEnvelope* ClimateFuzzySet::find(WeatherType t) const {
    for (const auto& e : m_envelopes) if (e.type == t) return &e;
    return nullptr;
}

WeatherType ClimateFuzzySet::classify(const WeatherSignature& s, float* outConfidence) const {
    // DUAS MEDIDAS COM PAPEIS DIFERENTES, e misturar as duas foi um erro que o
    // teste de continuidade pegou:
    //
    //   pertinencia difusa -> ESCOLHE o vencedor. E' ela que sabe desempatar
    //     entre climas cujos suportes se sobrepoem, porque leva em conta o
    //     formato do trapezio em cada eixo.
    //   distancia ao nucleo -> mede a CONFIANCA. E' definida em todo o espaco,
    //     inclusive fora de qualquer envelope, e varia continuamente.
    //
    // A primeira versao devolvia pertinencia quando havia envelope e um valor
    // fixo de 0.5 quando caia no fallback. Isso dava um degrau de 0.5 na
    // fronteira do suporte: a pertinencia chegava a zero ali, e o fallback
    // comecava em meio. Usar a distancia para a confianca elimina o degrau por
    // construcao - ela nao tem descontinuidade em lugar nenhum.
    const ClimateEnvelope* winner = nullptr;
    float bestM = 0.0f;
    float bestPrio = -1.0f;
    for (const auto& e : m_envelopes) {
        const float m = e.membership(s);
        if (m > bestM || (m > 0.0f && glm::abs(m - bestM) < 1e-5f && e.priority > bestPrio)) {
            bestM = m; winner = &e; bestPrio = e.priority;
        }
    }

    if (!winner) {
        // Nenhum envelope cobre este estado, e isso e' NORMAL: sao quatro eixos
        // com ~4 faixas cada, ou seja centenas de combinacoes, contra algumas
        // dezenas de climas nomeados. A atmosfera real tambem nao tem nome para
        // toda combinacao possivel. Faz-se o que um meteorologista faz: pega o
        // mais PROXIMO.
        float bestDist = 1e30f;
        for (const auto& e : m_envelopes) {
            if (!e.enabled) continue;
            const float d = e.distanceTo(s);
            if (d < bestDist || (glm::abs(d - bestDist) < 1e-5f && e.priority > bestPrio)) {
                bestDist = d; winner = &e; bestPrio = e.priority;
            }
        }
    }

    if (!winner) {
        if (outConfidence) *outConfidence = 0.0f;
        return WeatherType::Clear;
    }
    // Confianca = proximidade do nucleo. 1 dentro dele, caindo suave para fora.
    if (outConfidence) *outConfidence = 1.0f / (1.0f + winner->distanceTo(s));
    return winner->type;
}

std::vector<ClimateFuzzySet::Blend> ClimateFuzzySet::classifyBlend(const WeatherSignature& s,
                                                                   int maxTypes) const {
    std::vector<Blend> all;
    all.reserve(m_envelopes.size());
    for (const auto& e : m_envelopes) {
        const float m = e.membership(s);
        if (m > 1e-4f) all.push_back({e.type, m});
    }
    std::sort(all.begin(), all.end(), [](const Blend& a, const Blend& b) { return a.weight > b.weight; });
    if (static_cast<int>(all.size()) > maxTypes) all.resize(maxTypes);

    if (all.empty()) {
        // Mesmo raciocinio do classify: fora de qualquer envelope, o mais
        // proximo responde sozinho.
        float m = 0.0f;
        const WeatherType t = classify(s, &m);
        all.push_back({t, 1.0f});
        return all;
    }
    float sum = 0.0f;
    for (const auto& b : all) sum += b.weight;
    if (sum > 1e-6f) for (auto& b : all) b.weight /= sum;
    return all;
}

std::vector<ClimateOverlap> ClimateFuzzySet::findOverlaps() const {
    std::vector<ClimateOverlap> out;
    for (size_t i = 0; i < m_envelopes.size(); ++i) {
        for (size_t k = i + 1; k < m_envelopes.size(); ++k) {
            const auto& a = m_envelopes[i];
            const auto& b = m_envelopes[k];
            if (!a.enabled || !b.enabled) continue;
            // Ambiguo so' quando os nucleos coincidem em TODOS os eixos. Se
            // divergem em um unico eixo ja' e' possivel distinguir os dois.
            if (a.temperature.coreOverlaps(b.temperature) &&
                a.humidity.coreOverlaps(b.humidity) &&
                a.wind.coreOverlaps(b.wind) &&
                a.instability.coreOverlaps(b.instability)) {
                out.push_back({a.type, b.type});
            }
        }
    }
    return out;
}

float ClimateFuzzySet::coverageGaps(int samplesPerAxis) const {
    if (m_envelopes.empty()) return 1.0f;
    const int n = glm::max(samplesPerAxis, 2);
    int total = 0, uncovered = 0;
    for (int a = 0; a < n; ++a)
    for (int b = 0; b < n; ++b)
    for (int c = 0; c < n; ++c)
    for (int d = 0; d < n; ++d) {
        WeatherSignature s;
        s.thermalEnergy = -1.0f + 2.0f * float(a) / float(n - 1);
        s.humidity      =         1.0f * float(b) / float(n - 1);
        s.wind          =         1.0f * float(c) / float(n - 1);
        s.turbulence    =         1.0f * float(d) / float(n - 1);
        float m = 0.0f;
        classify(s, &m);
        ++total;
        if (m <= 1e-4f) ++uncovered;
    }
    return total ? float(uncovered) / float(total) : 1.0f;
}

} // namespace eruption
