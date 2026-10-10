#include "renderer/vfx/VfxEffect.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>

namespace eruption {

using nlohmann::json;

float VfxCurve::sample(float t) const {
    if (keys.empty()) return 1.0f;
    if (t <= keys.front().x) return keys.front().y;
    for (size_t i = 1; i < keys.size(); ++i) {
        if (t <= keys[i].x) {
            const float span = std::max(keys[i].x - keys[i - 1].x, 1e-6f);
            return glm::mix(keys[i - 1].y, keys[i].y, (t - keys[i - 1].x) / span);
        }
    }
    return keys.back().y;
}

Vec4 VfxGradient::sample(float t) const {
    if (keys.empty()) return Vec4(1.0f);
    if (t <= keys.front().t) return keys.front().color;
    for (size_t i = 1; i < keys.size(); ++i) {
        if (t <= keys[i].t) {
            const float span = std::max(keys[i].t - keys[i - 1].t, 1e-6f);
            return glm::mix(keys[i - 1].color, keys[i].color, (t - keys[i - 1].t) / span);
        }
    }
    return keys.back().color;
}

float VfxEffectDesc::maxLifetime() const {
    float m = 0.0f;
    for (const auto& e : emitters) m = std::max(m, e.lifetime.y);
    return m;
}

namespace {

const char* kShapes[] = {"point", "sphere", "box", "cone", "disc"};
const char* kFacings[] = {"camera", "velocity", "horizontal", "ribbon"};
const char* kBlends[] = {"additive", "premultiplied", "alpha"};
const char* kFlipModes[] = {"life", "fps", "random"};

template <size_t N>
uint32_t enumFrom(const json& j, const char* key, const char* (&names)[N], uint32_t def) {
    if (!j.contains(key) || !j[key].is_string()) return def;
    const std::string s = j[key].get<std::string>();
    for (size_t i = 0; i < N; ++i)
        if (s == names[i]) return static_cast<uint32_t>(i);
    return def;
}

Vec2 v2(const json& j, const char* key, Vec2 def) {
    if (!j.contains(key)) return def;
    const json& a = j[key];
    if (a.is_number()) return Vec2(a.get<float>());
    if (a.is_array() && a.size() >= 2) return Vec2(a[0].get<float>(), a[1].get<float>());
    return def;
}

Vec3 v3(const json& j, const char* key, Vec3 def) {
    if (!j.contains(key)) return def;
    const json& a = j[key];
    if (a.is_number()) return Vec3(a.get<float>());
    if (a.is_array() && a.size() >= 3) return Vec3(a[0].get<float>(), a[1].get<float>(), a[2].get<float>());
    return def;
}

template <typename T>
T num(const json& j, const char* key, T def) {
    return (j.contains(key) && (j[key].is_number() || j[key].is_boolean())) ? j[key].get<T>() : def;
}

json arr(Vec2 v) { return json::array({v.x, v.y}); }
json arr(Vec3 v) { return json::array({v.x, v.y, v.z}); }

void readEmitter(const json& j, VfxEmitterDesc& e) {
    e.name = j.value("name", e.name);
    e.enabled = num(j, "enabled", e.enabled);
    e.capacity = std::clamp<uint32_t>(num(j, "capacity", e.capacity), 1u, 1u << 20);
    e.rate = std::max(0.0f, num(j, "rate", e.rate));
    e.ratePerMeter = std::max(0.0f, num(j, "ratePerMeter", e.ratePerMeter));
    e.burstCount = num(j, "burstCount", e.burstCount);
    e.burstInterval = std::max(0.0f, num(j, "burstInterval", e.burstInterval));
    e.startDelay = std::max(0.0f, num(j, "startDelay", e.startDelay));
    e.duration = std::max(0.0f, num(j, "duration", e.duration));
    e.shape = static_cast<VfxShape>(enumFrom(j, "shape", kShapes, uint32_t(e.shape)));
    e.shapeSize = v3(j, "shapeSize", e.shapeSize);
    e.surfaceOnly = num(j, "surfaceOnly", e.surfaceOnly);
    e.coneAngle = num(j, "coneAngle", e.coneAngle);
    e.lifetime = glm::max(v2(j, "lifetime", e.lifetime), Vec2(0.01f));
    e.speed = v2(j, "speed", e.speed);
    e.direction = v3(j, "direction", e.direction);
    e.spread = std::clamp(num(j, "spread", e.spread), 0.0f, 1.0f);
    e.size = glm::max(v2(j, "size", e.size), Vec2(0.0f));
    e.rotation = v2(j, "rotation", e.rotation);
    e.angularVelocity = v2(j, "angularVelocity", e.angularVelocity);
    e.inheritVelocity = num(j, "inheritVelocity", e.inheritVelocity);
    e.gravity = v3(j, "gravity", e.gravity);
    e.drag = std::max(0.0f, num(j, "drag", e.drag));
    e.noiseStrength = num(j, "noiseStrength", e.noiseStrength);
    e.noiseFrequency = num(j, "noiseFrequency", e.noiseFrequency);
    e.noiseSpeed = num(j, "noiseSpeed", e.noiseSpeed);
    e.collidePlane = num(j, "collidePlane", e.collidePlane);
    e.planeY = num(j, "planeY", e.planeY);
    e.collideDepth = num(j, "collideDepth", e.collideDepth);
    e.bounce = std::clamp(num(j, "bounce", e.bounce), 0.0f, 1.0f);
    e.friction = std::clamp(num(j, "friction", e.friction), 0.0f, 1.0f);
    e.collisionLifeLoss = std::clamp(num(j, "collisionLifeLoss", e.collisionLifeLoss), 0.0f, 1.0f);
    if (j.contains("color") && j["color"].is_array()) {
        e.color.keys.clear();
        for (const json& k : j["color"]) {
            VfxGradient::Key key;
            key.t = k.value("t", 0.0f);
            if (k.contains("rgba") && k["rgba"].is_array() && k["rgba"].size() >= 4)
                key.color = Vec4(k["rgba"][0].get<float>(), k["rgba"][1].get<float>(), k["rgba"][2].get<float>(),
                                 k["rgba"][3].get<float>());
            e.color.keys.push_back(key);
        }
        std::sort(e.color.keys.begin(), e.color.keys.end(),
                  [](const VfxGradient::Key& a, const VfxGradient::Key& b) { return a.t < b.t; });
    }
    if (j.contains("sizeOverLife") && j["sizeOverLife"].is_array()) {
        e.sizeOverLife.keys.clear();
        for (const json& k : j["sizeOverLife"])
            if (k.is_array() && k.size() >= 2) e.sizeOverLife.keys.emplace_back(k[0].get<float>(), k[1].get<float>());
        std::sort(e.sizeOverLife.keys.begin(), e.sizeOverLife.keys.end(),
                  [](const Vec2& a, const Vec2& b) { return a.x < b.x; });
    }
    e.blend = static_cast<VfxBlend>(enumFrom(j, "blend", kBlends, uint32_t(e.blend)));
    e.facing = static_cast<VfxFacing>(enumFrom(j, "facing", kFacings, uint32_t(e.facing)));
    e.texture = j.value("texture", e.texture);
    e.flipCols = std::clamp<uint32_t>(num(j, "flipCols", e.flipCols), 1u, 64u);
    e.flipRows = std::clamp<uint32_t>(num(j, "flipRows", e.flipRows), 1u, 64u);
    e.flipMode = static_cast<VfxFlipMode>(enumFrom(j, "flipMode", kFlipModes, uint32_t(e.flipMode)));
    e.flipFps = std::max(0.0f, num(j, "flipFps", e.flipFps));
    e.flipBlend = num(j, "flipBlend", e.flipBlend);
    e.flipRandomStart = num(j, "flipRandomStart", e.flipRandomStart);
    e.stretch = std::max(0.0f, num(j, "stretch", e.stretch));
    e.softDistance = std::max(0.0f, num(j, "softDistance", e.softDistance));
    e.emissive = std::max(0.0f, num(j, "emissive", e.emissive));
    e.lit = std::clamp(num(j, "lit", e.lit), 0.0f, 1.0f);
    e.maxScreenSize = std::clamp(num(j, "maxScreenSize", e.maxScreenSize), 0.01f, 4.0f);
    e.nearFade = std::max(0.0f, num(j, "nearFade", e.nearFade));
    e.lowRes = num(j, "lowRes", e.lowRes);
    if (j.contains("mesh") && j["mesh"].is_string()) e.mesh = j["mesh"].get<std::string>();
    auto readSub = [&](const char* key, VfxSubEmitter& s) {
        if (!j.contains(key) || !j[key].is_object()) return;
        const json& o = j[key];
        s.emitter = o.value("emitter", std::string());
        s.count = std::clamp<uint32_t>(o.value("count", s.count), 1u, 64u);
        s.probability = std::clamp(o.value("probability", s.probability), 0.0f, 1.0f);
        s.inheritVelocity = std::clamp(o.value("inheritVelocity", s.inheritVelocity), 0.0f, 1.0f);
    };
    readSub("onDeath", e.onDeath);
    readSub("onCollision", e.onCollision);
    if (j.contains("graph") && j["graph"].is_object()) {
        const json& g = j["graph"];
        e.graph = VfxGraph{};
        if (g.contains("nodes") && g["nodes"].is_array())
            for (const json& n : g["nodes"]) {
                VfxGraphNode node;
                node.id = n.value("id", 0);
                node.op = n.value("op", std::string());
                if (n.contains("value") && n["value"].is_array() && n["value"].size() >= 4)
                    node.value = Vec4(n["value"][0].get<float>(), n["value"][1].get<float>(), n["value"][2].get<float>(),
                                      n["value"][3].get<float>());
                node.pos = v2(n, "pos", Vec2(0.0f));
                if (vfxGraphType(node.op)) e.graph.nodes.push_back(node);
                e.graph.nextId = std::max(e.graph.nextId, node.id + 1);
            }
        if (g.contains("links") && g["links"].is_array())
            for (const json& l : g["links"]) {
                if (!l.is_array() || l.size() < 4) continue;
                e.graph.links.push_back({l[0].get<int>(), l[1].get<int>(), l[2].get<int>(), l[3].get<int>()});
            }
    }
}

json writeEmitter(const VfxEmitterDesc& e) {
    json j;
    j["name"] = e.name;
    j["enabled"] = e.enabled;
    j["capacity"] = e.capacity;
    j["rate"] = e.rate;
    j["ratePerMeter"] = e.ratePerMeter;
    j["burstCount"] = e.burstCount;
    j["burstInterval"] = e.burstInterval;
    j["startDelay"] = e.startDelay;
    j["duration"] = e.duration;
    j["shape"] = kShapes[uint32_t(e.shape)];
    j["shapeSize"] = arr(e.shapeSize);
    j["surfaceOnly"] = e.surfaceOnly;
    j["coneAngle"] = e.coneAngle;
    j["lifetime"] = arr(e.lifetime);
    j["speed"] = arr(e.speed);
    j["direction"] = arr(e.direction);
    j["spread"] = e.spread;
    j["size"] = arr(e.size);
    j["rotation"] = arr(e.rotation);
    j["angularVelocity"] = arr(e.angularVelocity);
    j["inheritVelocity"] = e.inheritVelocity;
    j["gravity"] = arr(e.gravity);
    j["drag"] = e.drag;
    j["noiseStrength"] = e.noiseStrength;
    j["noiseFrequency"] = e.noiseFrequency;
    j["noiseSpeed"] = e.noiseSpeed;
    j["collidePlane"] = e.collidePlane;
    j["planeY"] = e.planeY;
    j["collideDepth"] = e.collideDepth;
    j["bounce"] = e.bounce;
    j["friction"] = e.friction;
    j["collisionLifeLoss"] = e.collisionLifeLoss;
    json col = json::array();
    for (const auto& k : e.color.keys)
        col.push_back({{"t", k.t}, {"rgba", json::array({k.color.r, k.color.g, k.color.b, k.color.a})}});
    j["color"] = col;
    json sz = json::array();
    for (const Vec2& k : e.sizeOverLife.keys) sz.push_back(arr(k));
    j["sizeOverLife"] = sz;
    j["blend"] = kBlends[uint32_t(e.blend)];
    j["facing"] = kFacings[uint32_t(e.facing)];
    j["texture"] = e.texture;
    j["flipCols"] = e.flipCols;
    j["flipRows"] = e.flipRows;
    j["flipMode"] = kFlipModes[uint32_t(e.flipMode)];
    j["flipFps"] = e.flipFps;
    j["flipBlend"] = e.flipBlend;
    j["flipRandomStart"] = e.flipRandomStart;
    j["stretch"] = e.stretch;
    j["softDistance"] = e.softDistance;
    j["emissive"] = e.emissive;
    j["lit"] = e.lit;
    j["maxScreenSize"] = e.maxScreenSize;
    j["nearFade"] = e.nearFade;
    j["lowRes"] = e.lowRes;
    if (!e.mesh.empty()) j["mesh"] = e.mesh;
    auto writeSub = [&](const char* key, const VfxSubEmitter& s) {
        if (s.emitter.empty()) return;
        j[key] = {{"emitter", s.emitter}, {"count", s.count}, {"probability", s.probability},
                  {"inheritVelocity", s.inheritVelocity}};
    };
    writeSub("onDeath", e.onDeath);
    writeSub("onCollision", e.onCollision);
    if (!e.graph.empty()) {
        json g;
        json nodes = json::array();
        for (const auto& n : e.graph.nodes)
            nodes.push_back({{"id", n.id}, {"op", n.op},
                             {"value", json::array({n.value.x, n.value.y, n.value.z, n.value.w})},
                             {"pos", arr(n.pos)}});
        json links = json::array();
        for (const auto& l : e.graph.links) links.push_back(json::array({l.fromNode, l.fromPin, l.toNode, l.toPin}));
        g["nodes"] = nodes;
        g["links"] = links;
        j["graph"] = g;
    }
    return j;
}

} // namespace

namespace {

bool fromJson(const json& j, VfxEffectDesc& out, std::string* err, const std::string& where) {
    if (j.is_discarded() || !j.is_object()) {
        if (err) *err = "JSON invalido em " + where;
        return false;
    }
    VfxEffectDesc fx;
    fx.name = j.value("name", std::string());
    fx.duration = std::max(0.0f, num(j, "duration", 0.0f));
    fx.loop = num(j, "loop", true);
    if (j.contains("emitters") && j["emitters"].is_array()) {
        for (const json& je : j["emitters"]) {
            VfxEmitterDesc e;
            readEmitter(je, e);
            if (e.color.keys.empty()) e.color.keys.push_back({0.0f, Vec4(1.0f)});
            fx.emitters.push_back(std::move(e));
        }
    }
    if (fx.emitters.empty()) {
        if (err) *err = "efeito sem emissores: " + where;
        return false;
    }
    out = std::move(fx);
    return true;
}

json toJson(const VfxEffectDesc& fx) {
    json j;
    j["name"] = fx.name;
    j["duration"] = fx.duration;
    j["loop"] = fx.loop;
    json em = json::array();
    for (const auto& e : fx.emitters) em.push_back(writeEmitter(e));
    j["emitters"] = em;
    return j;
}

} // namespace

bool loadVfxEffect(const std::string& path, VfxEffectDesc& out, std::string* err) {
    std::ifstream f(path);
    if (!f) {
        if (err) *err = "nao abriu " + path;
        return false;
    }
    return fromJson(json::parse(f, nullptr, false), out, err, path);
}

bool saveVfxEffect(const std::string& path, const VfxEffectDesc& fx, std::string* err) {
    std::ofstream f(path);
    if (!f) {
        if (err) *err = "nao gravou " + path;
        return false;
    }
    f << toJson(fx).dump(2) << "\n";
    return static_cast<bool>(f);
}

std::string vfxEffectToJson(const VfxEffectDesc& fx) { return toJson(fx).dump(); }

bool vfxEffectFromJson(const std::string& text, VfxEffectDesc& out, std::string* err) {
    return fromJson(json::parse(text, nullptr, false), out, err, "texto");
}

} // namespace eruption
