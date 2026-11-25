// Logica pura das afeccoes do lugar (ver Affects.hpp): tabela, escolha da
// reacao, anel de afeccoes e anel de rajadas.
#include "core/Affects.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>

namespace eruption::affects {

namespace {

using json = nlohmann::json;

float num(const json& j, const char* key, float def) {
    auto it = j.find(key);
    return it != j.end() && it->is_number() ? it->get<float>() : def;
}
bool flag(const json& j, const char* key, bool def) {
    auto it = j.find(key);
    return it != j.end() && it->is_boolean() ? it->get<bool>() : def;
}
std::string text(const json& j, const char* key, const std::string& def) {
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : def;
}
void rgb(const json& j, const char* key, float out[3]) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_array() || it->size() < 3) return;
    for (int i = 0; i < 3; ++i)
        if ((*it)[size_t(i)].is_number()) out[i] = (*it)[size_t(i)].get<float>();
}
void range2(const json& j, const char* key, float out[2]) {
    auto it = j.find(key);
    if (it == j.end()) return;
    if (it->is_number()) out[0] = out[1] = it->get<float>();
    else if (it->is_array() && it->size() >= 2 && (*it)[0].is_number() && (*it)[1].is_number()) {
        out[0] = (*it)[0].get<float>();
        out[1] = (*it)[1].get<float>();
    }
}
uint32_t count(const json& j, const char* key, uint32_t def) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_number()) return def;
    return uint32_t(std::clamp(it->get<float>(), 0.0f, 64.0f));
}

// Os dois ficam iguais ate' o JSON dar o "color_end".
void colors(const json& j, float color[3], float colorEnd[3]) {
    rgb(j, "color", color);
    for (int i = 0; i < 3; ++i) colorEnd[i] = color[i];
    rgb(j, "color_end", colorEnd);
}

void quality(const json& j, int& lo, int& hi) {
    lo = int(num(j, "min_quality", float(lo)));
    hi = int(num(j, "max_quality", float(hi)));
}

bool parseBlock(const json& j, Block& b, std::string* err) {
    const std::string type = text(j, "type", "");
    if (type == "decal") {
        b.type = Block::Type::Decal;
        DecalBlock& d = b.decal;
        d.texture = text(j, "texture", d.texture);
        d.additive = text(j, "blend", "alpha") == "add";
        colors(j, d.color, d.colorEnd);
        d.alpha = std::clamp(num(j, "alpha", d.alpha), 0.0f, 1.0f);
        d.size = std::clamp(num(j, "size", d.size), 0.01f, 4.0f);
        d.sizeJitter = std::clamp(num(j, "size_jitter", d.sizeJitter), 0.0f, 1.0f);
        d.sizeStart = std::clamp(num(j, "size_start", d.sizeStart), 0.05f, 4.0f);
        d.sizeEnd = std::clamp(num(j, "size_end", d.sizeEnd), 0.05f, 4.0f);
        d.count = std::max(1u, count(j, "count", d.count));
        d.scatter = std::clamp(num(j, "scatter", d.scatter), 0.0f, 2.0f);
        d.life = std::clamp(num(j, "life", d.life), 0.005f, 2.0f);
        d.fadeIn = std::clamp(num(j, "fade_in", d.fadeIn), 0.0f, 1.0f);
        d.hold = std::clamp(num(j, "hold", d.hold), 0.0f, 1.0f);
        d.delay = std::clamp(num(j, "delay", d.delay), 0.0f, 1.0f);
        d.directional = flag(j, "directional", d.directional);
        d.atlas = uint32_t(std::clamp(num(j, "atlas", float(d.atlas)), 1.0f, 4.0f));
        d.lit = std::clamp(num(j, "lit", d.lit), 0.0f, 1.0f);
        d.strengthAlpha = std::clamp(num(j, "strength_alpha", d.strengthAlpha), 0.0f, 1.0f);
        quality(j, d.minQuality, d.maxQuality);
    } else if (type == "particles") {
        b.type = Block::Type::Particles;
        ParticleBlock& p = b.particles;
        p.texture = text(j, "texture", p.texture);
        p.mesh = text(j, "mesh", p.mesh);
        p.additive = text(j, "blend", "alpha") == "add";
        colors(j, p.color, p.colorEnd);
        p.alpha = std::clamp(num(j, "alpha", p.alpha), 0.0f, 1.0f);
        p.count = count(j, "count", p.count);
        range2(j, "life", p.life);
        range2(j, "speed", p.speed);
        range2(j, "size", p.size);
        p.sizeEnd = std::clamp(num(j, "size_end", p.sizeEnd), 0.05f, 8.0f);
        p.gravity = num(j, "gravity", p.gravity);
        p.drag = std::max(num(j, "drag", p.drag), 0.0f);
        p.spread = std::clamp(num(j, "spread", p.spread), 0.0f, 1.0f);
        p.areaRadius = std::clamp(num(j, "area", p.areaRadius), 0.0f, 2.0f);
        p.burstTime = std::clamp(num(j, "burst_time", p.burstTime), 0.0f, 1.0f);
        p.rate = std::clamp(num(j, "rate", p.rate), 0.0f, 200.0f);
        p.duration = std::clamp(num(j, "duration", p.duration), 0.0f, 2.0f);
        p.collide = flag(j, "collide", p.collide);
        p.lit = std::clamp(num(j, "lit", p.lit), 0.0f, 1.0f);
        p.outward = flag(j, "outward", p.outward);
        p.distortion = std::clamp(num(j, "distortion", p.distortion), 0.0f, 0.25f);
        p.distortionRadial = std::clamp(num(j, "distortion_radial", p.distortionRadial), 0.0f, 1.0f);
        if (auto lj = j.find("light"); lj != j.end() && lj->is_object()) {
            p.light = flag(*lj, "enabled", true);
            rgb(*lj, "color", p.lightColor);
            p.lightIntensity = std::clamp(num(*lj, "intensity", p.lightIntensity), 0.0f, 40.0f);
            p.lightRadius = std::clamp(num(*lj, "radius", p.lightRadius), 0.05f, 8.0f);
            p.lightFlicker = std::clamp(num(*lj, "flicker", p.lightFlicker), 0.0f, 1.0f);
            p.lightHeight = std::clamp(num(*lj, "height", p.lightHeight), 0.0f, 4.0f);
        }
        if (p.life[0] <= 0.0f || p.life[1] < p.life[0]) { if (err) *err = "particles: life invalido"; return false; }
        quality(j, p.minQuality, p.maxQuality);
    } else if (type == "raise") {
        b.type = Block::Type::Raise;
        RaiseBlock& r = b.raise;
        r.mesh = text(j, "mesh", r.mesh);
        rgb(j, "color", r.color);
        r.count = count(j, "count", r.count);
        r.ring = std::clamp(num(j, "ring", r.ring), 0.0f, 2.0f);
        r.size = std::clamp(num(j, "size", r.size), 0.01f, 2.0f);
        r.rise = std::clamp(num(j, "rise", r.rise), 0.0f, 2.0f);
        r.riseTime = std::clamp(num(j, "rise_time", r.riseTime), 0.01f, 0.9f);
        r.life = std::clamp(num(j, "life", r.life), 0.005f, 2.0f);
        r.lit = std::clamp(num(j, "lit", r.lit), 0.0f, 1.0f);
        quality(j, r.minQuality, r.maxQuality);
    } else if (type == "wet") {
        b.type = Block::Type::Wet;
        WetBlock& w = b.wet;
        w.amount = std::clamp(num(j, "amount", w.amount), 0.0f, 1.0f);
        w.radius = std::clamp(num(j, "radius", w.radius), 0.05f, 4.0f);
        w.dry = std::clamp(num(j, "dry", w.dry), 0.01f, 4.0f);
        w.mud = flag(j, "mud", w.mud);
    } else if (type == "wind") {
        b.type = Block::Type::Wind;
        WindBlock& w = b.wind;
        w.strength = std::clamp(num(j, "strength", w.strength), 0.0f, 4.0f);
        w.radius = std::clamp(num(j, "radius", w.radius), 0.1f, 8.0f);
        w.duration = std::clamp(num(j, "duration", w.duration), 0.001f, 2.0f);
        w.durationMin = std::clamp(num(j, "duration_min", w.durationMin), 0.1f, 30.0f);
        w.shape.width = std::clamp(num(j, "front_width", w.shape.width), 0.03f, 1.0f);
        w.shape.floor = std::clamp(num(j, "floor", w.shape.floor), 0.0f, 1.0f);
        w.shape.shake = std::clamp(num(j, "shake", w.shape.shake), 0.0f, 1.0f);
        w.shape.lift = std::clamp(num(j, "lift", w.shape.lift), 0.0f, 1.0f);
    } else if (type == "ripple") {
        b.type = Block::Type::Ripple;
        RippleBlock& r = b.ripple;
        r.radius = std::clamp(num(j, "radius", r.radius), 0.05f, 4.0f);
        r.life = std::clamp(num(j, "life", r.life), 0.2f, 6.0f);
        r.strength = std::clamp(num(j, "strength", r.strength), 0.0f, 2.0f);
        r.rings = uint32_t(std::clamp(num(j, "rings", float(r.rings)), 1.0f, 4.0f));
    } else if (type == "ground") {
        b.type = Block::Type::Ground;
        GroundBlock& g = b.ground;
        g.kind = text(j, "kind", g.kind);
        if (g.kind != "scorch" && g.kind != "frost" && g.kind != "wet" && g.kind != "snow" && g.kind != "ice_water") {
            if (err) *err = "ground: kind desconhecido '" + g.kind + "'";
            return false;
        }
        g.amount = std::clamp(num(j, "amount", g.amount), 0.0f, 1.0f);
        g.radius = std::clamp(num(j, "radius", g.radius), 0.05f, 4.0f);
        g.duration = std::clamp(num(j, "duration", g.duration), 0.02f, 20.0f);
        g.fade = std::clamp(num(j, "fade", g.fade), 0.0f, 1.0f);
    } else {
        if (err) *err = "bloco de tipo desconhecido: '" + type + "'";
        return false;
    }
    return true;
}

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

} // namespace

bool Reaction::usesDir() const {
    for (const Block& b : blocks)
        if (b.type == Block::Type::Decal && b.decal.directional) return true;
    return false;
}

bool Table::parse(const std::string& jsonText, std::string* err) {
    json root;
    try {
        root = json::parse(jsonText, nullptr, true, true); // comentarios aceitos
    } catch (const std::exception& e) {
        if (err) *err = std::string("JSON invalido: ") + e.what();
        return false;
    }
    if (!root.is_object()) {
        if (err) *err = "a raiz precisa ser um objeto";
        return false;
    }
    Table t;
    auto strings = [&](const char* key, std::vector<std::string>& out) {
        auto it = root.find(key);
        if (it == root.end() || !it->is_array()) return;
        for (const json& v : *it)
            if (v.is_string()) out.push_back(v.get<std::string>());
    };
    if (auto al = root.find("aliases"); al != root.end() && al->is_object())
        for (auto it = al->begin(); it != al->end(); ++it)
            if (!it.key().empty() && it.key()[0] != '_' && it.value().is_string())
                t.m_aliases[it.key()] = it.value().get<std::string>();
    strings("elements", t.m_elements);
    strings("surfaces", t.m_surfaces);
    if (t.m_elements.empty() || t.m_surfaces.empty()) {
        if (err) *err = "faltam 'elements' ou 'surfaces'";
        return false;
    }
    if (std::find(t.m_surfaces.begin(), t.m_surfaces.end(), "default") == t.m_surfaces.end())
        t.m_surfaces.push_back("default");

    auto rx = root.find("reactions");
    if (rx == root.end() || !rx->is_object()) {
        if (err) *err = "falta 'reactions'";
        return false;
    }
    for (auto it = rx->begin(); it != rx->end(); ++it) {
        if (it.key().empty() || it.key()[0] == '_') continue; // comentario
        Reaction r;
        r.name = it.key();
        auto bl = it.value().find("blocks");
        if (bl == it.value().end() || !bl->is_array()) {
            if (err) *err = "reacao '" + r.name + "' sem 'blocks'";
            return false;
        }
        for (const json& jb : *bl) {
            Block b;
            std::string e2;
            if (!parseBlock(jb, b, &e2)) {
                if (err) *err = "reacao '" + r.name + "': " + e2;
                return false;
            }
            r.blocks.push_back(std::move(b));
        }
        t.m_reactions.emplace(r.name, std::move(r));
    }

    auto mx = root.find("matrix");
    if (mx == root.end() || !mx->is_object()) {
        if (err) *err = "falta 'matrix'";
        return false;
    }
    for (auto it = mx->begin(); it != mx->end(); ++it) {
        if (it.key().empty() || it.key()[0] == '_') continue;
        if (std::find(t.m_elements.begin(), t.m_elements.end(), it.key()) == t.m_elements.end()) {
            if (err) *err = "matriz cita o elemento '" + it.key() + "', que nao esta em 'elements'";
            return false;
        }
        for (auto c = it.value().begin(); c != it.value().end(); ++c) {
            if (!c.value().is_string()) continue;
            if (std::find(t.m_surfaces.begin(), t.m_surfaces.end(), c.key()) == t.m_surfaces.end()) {
                if (err) *err = "matriz cita a superficie '" + c.key() + "', que nao esta em 'surfaces'";
                return false;
            }
            const std::string name = c.value().get<std::string>();
            if (!t.m_reactions.count(name)) {
                if (err) *err = "matriz cita a reacao '" + name + "', que nao existe";
                return false;
            }
            t.m_matrix[it.key()][c.key()] = name;
        }
    }

    auto sh = root.find("surface_hints");
    if (sh != root.end() && sh->is_object())
        for (auto it = sh->begin(); it != sh->end(); ++it) {
            if (it.key().empty() || it.key()[0] == '_' || !it.value().is_array()) continue;
            std::vector<std::string> words;
            for (const json& w : it.value())
                if (w.is_string()) words.push_back(lower(w.get<std::string>()));
            t.m_hints.emplace_back(it.key(), std::move(words));
        }
    *this = std::move(t);
    return true;
}

bool Table::hasElement(const std::string& e) const {
    const std::string c = canonical(e);
    return std::find(m_elements.begin(), m_elements.end(), c) != m_elements.end();
}

std::string Table::canonical(const std::string& e) const {
    auto it = m_aliases.find(e);
    return it == m_aliases.end() ? e : it->second;
}

const Reaction* Table::select(const std::string& element, const std::string& surface) const {
    auto el = m_matrix.find(canonical(element));
    if (el == m_matrix.end()) return nullptr;
    auto cell = el->second.find(surface);
    if (cell == el->second.end()) cell = el->second.find("default");
    if (cell == el->second.end()) return nullptr;
    auto r = m_reactions.find(cell->second);
    return r == m_reactions.end() ? nullptr : &r->second;
}

std::string Table::surfaceFromName(const std::string& materialOrTexture) const {
    const std::string n = lower(materialOrTexture);
    if (n.empty()) return "default";
    for (const auto& [surface, words] : m_hints)
        for (const std::string& w : words)
            if (!w.empty() && n.find(w) != std::string::npos) return surface;
    return "default";
}

std::string Table::surfaceFromRegion(const std::string& regionType) const {
    return std::find(m_surfaces.begin(), m_surfaces.end(), regionType) != m_surfaces.end() ? regionType : std::string();
}

// ------------------------------------------------------------------ anel

std::vector<LiveAffect> AffectRing::add(LiveAffect a, uint32_t cap) {
    std::vector<LiveAffect> dropped;
    cap = std::clamp<uint32_t>(cap, 1u, kHardMaxAffects);
    while (m_live.size() >= cap) { // a mais velha e' a primeira
        dropped.push_back(std::move(m_live.front()));
        m_live.erase(m_live.begin());
    }
    m_live.push_back(std::move(a));
    return dropped;
}

std::vector<LiveAffect> AffectRing::tick(double now) {
    std::vector<LiveAffect> gone;
    for (size_t i = 0; i < m_live.size();) {
        if (m_live[i].expire <= now) {
            gone.push_back(std::move(m_live[i]));
            m_live.erase(m_live.begin() + long(i));
        } else {
            ++i;
        }
    }
    return gone;
}

bool AffectRing::stop(uint32_t id, LiveAffect* out) {
    for (size_t i = 0; i < m_live.size(); ++i)
        if (m_live[i].id == id) {
            if (out) *out = std::move(m_live[i]);
            m_live.erase(m_live.begin() + long(i));
            return true;
        }
    return false;
}

// ----------------------------------------------------------------- vento

float gustEnvelope(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    const float attack = std::min(t / 0.06f, 1.0f); // sem estalo no primeiro quadro
    return attack * std::pow(1.0f - t, 1.5f);
}

float gustFront(float t) { return std::pow(std::clamp(t, 0.0f, 1.0f), 0.6f); }

void GustRing::add(uint32_t id, float x, float z, float radius, float strength, float duration, uint32_t cap, bool* dropped,
                   const GustShape& shape) {
    cap = std::clamp<uint32_t>(cap, 1u, kMaxGusts);
    if (dropped) *dropped = false;
    while (m_gusts.size() >= cap) {
        m_gusts.erase(m_gusts.begin());
        if (dropped) *dropped = true;
    }
    Gust g;
    g.id = id;
    g.x = x;
    g.z = z;
    g.radius = std::max(radius, 1e-3f);
    g.strength = std::clamp(strength, 0.0f, 1.0f);
    g.duration = std::max(duration, 0.05f);
    g.shape = shape;
    g.shape.width = std::clamp(g.shape.width, 0.03f, 1.0f);
    g.shape.floor = std::clamp(g.shape.floor, 0.0f, 1.0f);
    g.shape.shake = std::clamp(g.shape.shake, 0.0f, 1.0f);
    g.shape.lift = std::clamp(g.shape.lift, 0.0f, 1.0f);
    m_gusts.push_back(g);
}

void GustRing::tick(float dt) {
    for (Gust& g : m_gusts) g.age += dt;
    m_gusts.erase(std::remove_if(m_gusts.begin(), m_gusts.end(), [](const Gust& g) { return g.age >= g.duration; }),
                  m_gusts.end());
}

bool GustRing::stop(uint32_t id) {
    for (size_t i = 0; i < m_gusts.size(); ++i)
        if (m_gusts[i].id == id) {
            m_gusts.erase(m_gusts.begin() + long(i));
            return true;
        }
    return false;
}

uint32_t GustRing::pack(GustPacked* out) const {
    uint32_t n = 0;
    for (const Gust& g : m_gusts) {
        if (n >= kMaxGusts) break;
        const float t = g.age / g.duration;
        GustPacked& p = out[n++];
        p.a[0] = g.x;
        p.a[1] = g.z;
        p.a[2] = g.radius;
        p.a[3] = g.strength * gustEnvelope(t);
        p.b[0] = gustFront(t);
        p.b[1] = g.shape.width;
        p.b[2] = g.shape.floor;
        p.b[3] = g.shape.shake;
        p.c[0] = g.shape.lift;
        p.c[1] = g.duration;
        p.c[2] = g.age;
        p.c[3] = 0.0f;
    }
    return n;
}

} // namespace eruption::affects
