#include "net/LinkSim.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace eruption {

bool LinkSimConfig::active() const {
    return lossPercent > 0.0f || latencyMs > 0.0f || jitterMs > 0.0f || duplicatePercent > 0.0f || reorderPercent > 0.0f;
}

bool LinkSimConfig::parse(const std::string& text, LinkSimConfig& out, std::string* error) {
    LinkSimConfig c;
    std::stringstream ss(text);
    std::string item;
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    while (std::getline(ss, item, ',')) {
        if (item.empty()) continue;
        const size_t eq = item.find('=');
        if (eq == std::string::npos) return fail("sem '=' em '" + item + "'");
        const std::string key = item.substr(0, eq);
        const std::string value = item.substr(eq + 1);
        char* end = nullptr;
        const float v = std::strtof(value.c_str(), &end);
        if (value.empty() || *end != '\0' || !(v >= 0.0f)) return fail("valor inválido em '" + item + "'");
        if (key == "loss" || key == "dup" || key == "reorder") {
            if (v > 100.0f) return fail("porcentagem acima de 100 em '" + item + "'");
            (key == "loss" ? c.lossPercent : key == "dup" ? c.duplicatePercent : c.reorderPercent) = v;
        } else if (key == "lat" || key == "jitter") {
            if (v > 10000.0f) return fail("atraso acima de 10 s em '" + item + "'");
            (key == "lat" ? c.latencyMs : c.jitterMs) = v;
        } else if (key == "burst") {
            c.burstLength = std::max(1.0f, v);
        } else {
            return fail("chave desconhecida '" + key + "'");
        }
    }
    out = c;
    return true;
}

std::string LinkSimConfig::toString() const {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "loss=%g,burst=%g,lat=%g,jitter=%g,dup=%g,reorder=%g", lossPercent, burstLength,
                  latencyMs, jitterMs, duplicatePercent, reorderPercent);
    return buf;
}

LinkSim::LinkSim(PacketLink& inner, const LinkSimConfig& config, uint32_t seed)
    : m_inner(inner), m_config(config), m_rng(seed) {}

bool LinkSim::dropNext() {
    std::uniform_real_distribution<double> u(0.0, 1.0);
    const double p = m_config.lossPercent / 100.0;
    if (p <= 0.0) return false;
    if (m_config.burstLength <= 1.0f) return u(m_rng) < p;
    // Gilbert–Elliott de dois estados: no ruim perde tudo, no bom nada. Sair
    // do ruim tem chance 1/L (rajada média L); entrar é escolhido para a perda
    // média dar p: p_entrar = p * p_sair / (1 - p).
    const double leave = 1.0 / m_config.burstLength;
    const double enter = p >= 1.0 ? 1.0 : std::min(1.0, p * leave / (1.0 - p));
    m_badState = m_badState ? u(m_rng) >= leave : u(m_rng) < enter;
    return m_badState;
}

double LinkSim::delayFor(bool reorder) {
    std::uniform_real_distribution<double> u(-1.0, 1.0);
    double ms = m_config.latencyMs + m_config.jitterMs * u(m_rng);
    if (reorder) ms += 25.0 + 25.0 * u(m_rng);
    return std::max(0.0, ms) / 1000.0;
}

void LinkSim::send(const NetAddress& to, const uint8_t* data, int bytes) {
    if (bytes <= 0) return;
    ++m_sent;
    if (dropNext()) {
        ++m_dropped;
        return;
    }
    std::uniform_real_distribution<double> u(0.0, 100.0);
    const int copies = u(m_rng) < m_config.duplicatePercent ? 2 : 1;
    for (int k = 0; k < copies; ++k) {
        const bool reorder = u(m_rng) < m_config.reorderPercent;
        m_pending.push_back(Pending{m_now + delayFor(reorder), to, std::vector<uint8_t>(data, data + bytes)});
    }
}

void LinkSim::update(double time) {
    m_now = time;
    // Solta em ordem de horário; quem ganhou menos atraso passa na frente.
    std::stable_sort(m_pending.begin(), m_pending.end(), [](const Pending& a, const Pending& b) { return a.due < b.due; });
    size_t sent = 0;
    while (sent < m_pending.size() && m_pending[sent].due <= time) {
        const Pending& p = m_pending[sent++];
        m_inner.send(p.to, p.data.data(), static_cast<int>(p.data.size()));
    }
    m_pending.erase(m_pending.begin(), m_pending.begin() + static_cast<long>(sent));
    m_inner.update(time);
}

} // namespace eruption
