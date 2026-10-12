#include "renderer/SkinRig.hpp"

#include <algorithm>

namespace eruption {

namespace {

// Valor do canal no tempo t: procura o intervalo e interpola.
Vec4 sample(const SkinRig::Channel& c, float t) {
    if (c.times.empty() || c.values.empty()) return Vec4(0.0f);
    const size_t n = std::min(c.times.size(), c.values.size());
    if (t <= c.times.front() || n == 1) return c.values.front();
    if (t >= c.times[n - 1]) return c.values[n - 1];
    const size_t hi = static_cast<size_t>(std::upper_bound(c.times.begin(), c.times.begin() + n, t) - c.times.begin());
    const size_t lo = hi - 1;
    if (c.step) return c.values[lo];
    const float span = c.times[hi] - c.times[lo];
    const float f = span > 0.0f ? (t - c.times[lo]) / span : 0.0f;
    if (c.path == 1) {
        const glm::quat a(c.values[lo].w, c.values[lo].x, c.values[lo].y, c.values[lo].z);
        const glm::quat b(c.values[hi].w, c.values[hi].x, c.values[hi].y, c.values[hi].z);
        const glm::quat q = glm::slerp(a, b, f);
        return Vec4(q.x, q.y, q.z, q.w);
    }
    return glm::mix(c.values[lo], c.values[hi], f);
}

// Posição, rotação e escala locais de cada nó (a de repouso onde a animação
// não tem canal).
void localPose(const SkinRig& rig, int anim, float time, std::vector<Vec3>& t, std::vector<glm::quat>& r,
               std::vector<Vec3>& s) {
    const size_t n = rig.nodes.size();
    t.resize(n);
    r.resize(n);
    s.resize(n);
    for (size_t i = 0; i < n; ++i) {
        t[i] = rig.nodes[i].t;
        r[i] = rig.nodes[i].r;
        s[i] = rig.nodes[i].s;
    }
    if (anim < 0 || anim >= static_cast<int>(rig.animations.size())) return;
    for (const SkinRig::Channel& c : rig.animations[static_cast<size_t>(anim)].channels) {
        if (c.node < 0 || c.node >= static_cast<int>(n) || c.path > 2) continue; // pesos de morph: morphWeights
        const Vec4 v = sample(c, time);
        const size_t k = static_cast<size_t>(c.node);
        if (c.path == 0) t[k] = Vec3(v);
        else if (c.path == 1) r[k] = glm::normalize(glm::quat(v.w, v.x, v.y, v.z));
        else s[k] = Vec3(v);
    }
}

} // namespace

int SkinRig::findAnimation(const std::string& name) const {
    for (size_t i = 0; i < animations.size(); ++i)
        if (animations[i].name == name) return static_cast<int>(i);
    return -1;
}

void SkinRig::pose(int anim, float time, std::vector<glm::mat4>& globals) const {
    blendPose(anim, time, anim, time, 1.0f, globals);
}

void SkinRig::blendPose(int from, float fromTime, int anim, float time, float w, std::vector<glm::mat4>& globals) const {
    std::vector<Vec3> t, s;
    std::vector<glm::quat> r;
    localPose(*this, anim, time, t, r, s);
    if (w < 1.0f) {
        std::vector<Vec3> t0, s0;
        std::vector<glm::quat> r0;
        localPose(*this, from, fromTime, t0, r0, s0);
        const float k = std::clamp(w, 0.0f, 1.0f);
        for (size_t i = 0; i < nodes.size(); ++i) {
            t[i] = glm::mix(t0[i], t[i], k);
            s[i] = glm::mix(s0[i], s[i], k);
            r[i] = glm::slerp(r0[i], r[i], k); // o caminho curto (sinal do quaternion)
        }
    }
    globals.resize(nodes.size());
    for (int i : order) {
        const size_t k = static_cast<size_t>(i);
        const glm::mat4 local = glm::translate(glm::mat4(1.0f), t[k]) * glm::mat4_cast(r[k]) * glm::scale(glm::mat4(1.0f), s[k]);
        globals[k] = nodes[k].parent >= 0 ? globals[static_cast<size_t>(nodes[k].parent)] * local : local;
    }
}

bool SkinRig::morphWeights(int anim, float time, int node, float* out, uint32_t count) const {
    if (anim < 0 || anim >= static_cast<int>(animations.size()) || node < 0) return false;
    for (const Channel& c : animations[static_cast<size_t>(anim)].channels) {
        if (c.path != 3 || c.node != node || c.weightCount == 0 || c.times.empty()) continue;
        const size_t keys = std::min(c.times.size(), c.weights.size() / c.weightCount);
        if (keys == 0) return false;
        size_t lo = 0, hi = 0;
        float f = 0.0f;
        if (time >= c.times[keys - 1]) {
            lo = hi = keys - 1;
        } else if (time > c.times.front()) {
            hi = static_cast<size_t>(std::upper_bound(c.times.begin(), c.times.begin() + std::ptrdiff_t(keys), time) -
                                     c.times.begin());
            lo = hi - 1;
            const float span = c.times[hi] - c.times[lo];
            f = c.step || span <= 0.0f ? 0.0f : (time - c.times[lo]) / span;
        }
        for (uint32_t t = 0; t < count; ++t) {
            const float a = t < c.weightCount ? c.weights[lo * c.weightCount + t] : 0.0f;
            const float b = t < c.weightCount ? c.weights[hi * c.weightCount + t] : 0.0f;
            out[t] = a + (b - a) * f;
        }
        return true;
    }
    return false;
}

void SkinRig::jointMatrices(int skin, const std::vector<glm::mat4>& globals, glm::mat4* out) const {
    if (skin < 0 || skin >= static_cast<int>(skins.size())) return;
    const Skin& sk = skins[static_cast<size_t>(skin)];
    for (size_t j = 0; j < sk.joints.size(); ++j) {
        const int n = sk.joints[j];
        const glm::mat4 g = n >= 0 && n < static_cast<int>(globals.size()) ? globals[static_cast<size_t>(n)] : glm::mat4(1.0f);
        out[j] = g * (j < sk.inverseBind.size() ? sk.inverseBind[j] : glm::mat4(1.0f));
    }
}

} // namespace eruption
