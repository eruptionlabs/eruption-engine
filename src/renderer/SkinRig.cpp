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

} // namespace

int SkinRig::findAnimation(const std::string& name) const {
    for (size_t i = 0; i < animations.size(); ++i)
        if (animations[i].name == name) return static_cast<int>(i);
    return -1;
}

void SkinRig::pose(int anim, float time, std::vector<glm::mat4>& globals) const {
    std::vector<Vec3> t(nodes.size()), s(nodes.size());
    std::vector<glm::quat> r(nodes.size());
    for (size_t i = 0; i < nodes.size(); ++i) {
        t[i] = nodes[i].t;
        r[i] = nodes[i].r;
        s[i] = nodes[i].s;
    }
    if (anim >= 0 && anim < static_cast<int>(animations.size())) {
        for (const Channel& c : animations[static_cast<size_t>(anim)].channels) {
            if (c.node < 0 || c.node >= static_cast<int>(nodes.size())) continue;
            const Vec4 v = sample(c, time);
            const size_t k = static_cast<size_t>(c.node);
            if (c.path == 0) t[k] = Vec3(v);
            else if (c.path == 1) r[k] = glm::normalize(glm::quat(v.w, v.x, v.y, v.z));
            else s[k] = Vec3(v);
        }
    }
    globals.resize(nodes.size());
    for (int i : order) {
        const size_t k = static_cast<size_t>(i);
        const glm::mat4 local = glm::translate(glm::mat4(1.0f), t[k]) * glm::mat4_cast(r[k]) * glm::scale(glm::mat4(1.0f), s[k]);
        globals[k] = nodes[k].parent >= 0 ? globals[static_cast<size_t>(nodes[k].parent)] * local : local;
    }
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
