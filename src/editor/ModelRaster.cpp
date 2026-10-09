#include "editor/ModelRaster.hpp"

#include "formats/GltfParser.hpp"
#include "utils/ImageUtils.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <unordered_map>

namespace eruption {

namespace {

uint32_t averageColor(const EmbeddedTexture& et) {
    ImageData img;
    const uint8_t* px = nullptr;
    int w = 0, h = 0, ch = 0;
    if (et.isDecoded()) {
        px = et.pixels.data();
        w = et.width;
        h = et.height;
        ch = et.channels > 0 ? et.channels : 4;
    } else if (et.hasEncodedData()) {
        img = ImageUtils::loadFromMemoryRaw(et.encodedData.data(), et.encodedData.size());
        if (img.isValid()) {
            px = img.pixels.data();
            w = img.width;
            h = img.height;
            ch = img.channels;
        }
    }
    if (!px || w <= 0 || h <= 0 || ch < 3) return 0xFFB4B4B4u;
    double r = 0, g = 0, b = 0;
    size_t n = 0;
    const int step = std::max(1, std::max(w, h) / 64);
    for (int y = 0; y < h; y += step)
        for (int x = 0; x < w; x += step) {
            const uint8_t* p = px + (size_t(y) * w + x) * ch;
            if (ch == 4 && p[3] < 128) continue; // recorte por alfa (folhas)
            r += p[0];
            g += p[1];
            b += p[2];
            ++n;
        }
    if (!n) return 0xFFB4B4B4u;
    return 0xFF000000u | (uint32_t(b / n) << 16) | (uint32_t(g / n) << 8) | uint32_t(r / n);
}

} // namespace

bool loadRasterMesh(const std::string& path, RasterMesh& out, std::string& error, size_t maxTriangles) {
    out = RasterMesh{};
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (data.size() < 12) {
        error = "can't read the file";
        return false;
    }
    ModelFile model = GltfParser::parse(data.data(), data.size(), std::filesystem::path(path).parent_path().string());
    if (model.nodes.empty()) {
        error = "no meshes in the file";
        return false;
    }
    std::unordered_map<std::string, uint32_t> texColor;
    auto colorOfTexture = [&](const std::string& name) {
        auto it = texColor.find(name);
        if (it != texColor.end()) return it->second;
        uint32_t c = 0xFFB4B4B4u;
        for (const auto& et : model.embeddedTextures)
            if (et.name == name) {
                c = averageColor(et);
                break;
            }
        texColor.emplace(name, c);
        return c;
    };
    size_t total = 0;
    for (const auto& node : model.nodes) {
        const ModelFileNode& geo =
            node.geometryShareIndex >= 0 && size_t(node.geometryShareIndex) < model.nodes.size() ? model.nodes[size_t(node.geometryShareIndex)] : node;
        total += (geo.indices.empty() ? geo.vertices.size() : geo.indices.size()) / 3;
    }
    out.sourceTriangles = total;
    // Modelo enorme: um em cada `keep` triângulos (miniatura não precisa de tudo).
    const size_t keep = std::max<size_t>(1, (total + maxTriangles - 1) / std::max<size_t>(1, maxTriangles));
    Vec3 lo(FLT_MAX), hi(-FLT_MAX);
    size_t triCounter = 0;
    for (const auto& node : model.nodes) {
        const ModelFileNode& geo =
            node.geometryShareIndex >= 0 && size_t(node.geometryShareIndex) < model.nodes.size() ? model.nodes[size_t(node.geometryShareIndex)] : node;
        if (geo.vertices.empty()) continue;
        const Mat4& m = node.renderMatrix;
        const uint32_t base = static_cast<uint32_t>(out.pos.size());
        for (size_t v = 0; v < geo.vertices.size(); ++v) {
            const Vec3 p = Vec3(m * Vec4(geo.vertices[v], 1.0f));
            out.pos.push_back(p);
            lo = glm::min(lo, p);
            hi = glm::max(hi, p);
            uint32_t c = 0xFFB4B4B4u;
            if (v < geo.perVertexTexIds.size() && geo.perVertexTexIds[v] < geo.textureNames.size())
                c = colorOfTexture(geo.textureNames[geo.perVertexTexIds[v]]);
            else if (v < geo.colors.size()) {
                const uint32_t bgra = geo.colors[v];
                c = 0xFF000000u | ((bgra & 0xFFu) << 16) | (bgra & 0xFF00u) | ((bgra >> 16) & 0xFFu);
            }
            out.color.push_back(c);
        }
        const size_t n = geo.indices.empty() ? geo.vertices.size() : geo.indices.size();
        for (size_t i = 0; i + 2 < n; i += 3) {
            if (triCounter++ % keep) continue;
            for (int k = 0; k < 3; ++k)
                out.indices.push_back(base + (geo.indices.empty() ? uint32_t(i + k) : geo.indices[i + size_t(k)]));
        }
    }
    if (out.indices.empty()) {
        error = "no triangles in the file";
        return false;
    }
    out.lo = lo;
    out.hi = hi;
    return true;
}

void rasterizeMesh(const RasterMesh& mesh, float yaw, float pitch, int w, int h, std::vector<uint8_t>& rgba) {
    rgba.assign(size_t(w) * h * 4, 0);
    if (mesh.indices.empty() || w <= 0 || h <= 0) return;
    std::vector<float> depth(size_t(w) * h, FLT_MAX);
    const Vec3 center = (mesh.lo + mesh.hi) * 0.5f;
    const float radius = std::max(1e-4f, glm::length(mesh.hi - mesh.lo) * 0.5f);
    const float fov = glm::radians(30.0f);
    const float dist = radius / std::sin(fov * 0.5f) * 1.05f;
    const Vec3 dir(std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw));
    const Vec3 eye = center + dir * dist;
    const Mat4 view = glm::lookAtRH(eye, center, Vec3(0.0f, 1.0f, 0.0f));
    const Mat4 proj = glm::perspectiveRH_NO(fov, float(w) / float(h), dist * 0.05f, dist * 3.0f);
    const Mat4 vp = proj * view;
    const Vec3 light = glm::normalize(Vec3(0.45f, 0.8f, 0.35f));
    std::vector<Vec3> sp(mesh.pos.size());
    std::vector<bool> ok(mesh.pos.size());
    for (size_t i = 0; i < mesh.pos.size(); ++i) {
        const Vec4 c = vp * Vec4(mesh.pos[i], 1.0f);
        ok[i] = c.w > 1e-5f;
        if (ok[i]) sp[i] = Vec3((c.x / c.w * 0.5f + 0.5f) * w, (1.0f - (c.y / c.w * 0.5f + 0.5f)) * h, c.z / c.w);
    }
    for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
        const uint32_t ia = mesh.indices[t], ib = mesh.indices[t + 1], ic = mesh.indices[t + 2];
        if (ia >= sp.size() || ib >= sp.size() || ic >= sp.size() || !ok[ia] || !ok[ib] || !ok[ic]) continue;
        const Vec3 a = sp[ia], b = sp[ib], c = sp[ic];
        const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        if (std::abs(area) < 1e-8f) continue;
        // Luz pela normal da face (os dois lados: folhas e planos).
        Vec3 n = glm::cross(mesh.pos[ib] - mesh.pos[ia], mesh.pos[ic] - mesh.pos[ia]);
        const float nl = glm::length(n);
        n = nl > 0.0f ? n / nl : Vec3(0, 1, 0);
        if (glm::dot(n, eye - mesh.pos[ia]) < 0.0f) n = -n;
        const float shade = 0.35f + 0.65f * std::max(0.0f, glm::dot(n, light));
        const uint32_t col = mesh.color[ia];
        const uint8_t cr = uint8_t(std::min(255.0f, float(col & 0xFF) * shade));
        const uint8_t cg = uint8_t(std::min(255.0f, float((col >> 8) & 0xFF) * shade));
        const uint8_t cb = uint8_t(std::min(255.0f, float((col >> 16) & 0xFF) * shade));
        const int x0 = std::max(0, int(std::floor(std::min({a.x, b.x, c.x}))));
        const int x1 = std::min(w - 1, int(std::ceil(std::max({a.x, b.x, c.x}))));
        const int y0 = std::max(0, int(std::floor(std::min({a.y, b.y, c.y}))));
        const int y1 = std::min(h - 1, int(std::ceil(std::max({a.y, b.y, c.y}))));
        const float inv = 1.0f / area;
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                const float px = x + 0.5f, py = y + 0.5f;
                const float w0 = ((b.x - px) * (c.y - py) - (b.y - py) * (c.x - px)) * inv;
                const float w1 = ((c.x - px) * (a.y - py) - (c.y - py) * (a.x - px)) * inv;
                const float w2 = 1.0f - w0 - w1;
                if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;
                const float z = w0 * a.z + w1 * b.z + w2 * c.z;
                float& d = depth[size_t(y) * w + x];
                if (z >= d) continue;
                d = z;
                uint8_t* p = &rgba[(size_t(y) * w + x) * 4];
                p[0] = cr;
                p[1] = cg;
                p[2] = cb;
                p[3] = 255;
            }
    }
}

} // namespace eruption
