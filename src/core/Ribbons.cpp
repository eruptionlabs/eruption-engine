// Faixas por spline da cena (estrada, trilha, rio raso): montadas e assentadas
// no chao quando entram, consultadas pelos scripts (world.ribbon_query).
#include "core/Engine.hpp"
#include "core/Logger.hpp"

namespace eruption {

bool Engine::addRibbon(const RibbonDef& def, bool fromScript, std::string& error) {
    if (def.name.empty()) {
        error = "ribbon without a name";
        return false;
    }
    SceneRibbon r;
    r.def = def;
    r.fromScript = fromScript;
    RibbonParams params;
    params.closed = def.closed;
    params.step = def.step;
    params.uvLength = def.uvLength;
    if (!r.ribbon.build(def.pointsXZ, def.widths, params, error)) {
        error = def.name + ": " + error;
        return false;
    }
    // Assenta: a altura de cada vertice da faixa vem do chao, para a consulta
    // devolver a superficie. A malha em si ainda nao e' desenhada.
    std::vector<RibbonVertex> vertices;
    std::vector<uint32_t> indices;
    r.ribbon.buildMesh([this](float x, float z) { return sampleTerrainHeight(x, z); }, def.lift, vertices, indices);
    // O hash vai no log: dois aparelhos conferem que montaram a mesma faixa.
    ERUPTION_LOG_WARN("[RIBBON] %s: %zu amostras, %.1f m, hash %016llx", def.name.c_str(), r.ribbon.samples().size(),
                      static_cast<double>(r.ribbon.length()), static_cast<unsigned long long>(r.ribbon.hash()));
    for (SceneRibbon& o : m_ribbons)
        if (o.def.name == def.name) {
            o = std::move(r);
            return true;
        }
    m_ribbons.push_back(std::move(r));
    return true;
}

const Ribbon* Engine::findRibbon(const std::string& name) const {
    for (const SceneRibbon& r : m_ribbons)
        if (r.def.name == name) return &r.ribbon;
    return nullptr;
}

void Engine::clearRibbons() { m_ribbons.clear(); }

} // namespace eruption
