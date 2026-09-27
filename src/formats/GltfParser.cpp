#include "formats/GltfParser.hpp"
#include "core/Logger.hpp"
#include "utils/ImageUtils.hpp"
#include <glm/gtx/matrix_decompose.hpp>

#include <tiny_gltf.h>

namespace tinygltf {
// tinygltf requires an image loader callback when the GLB contains embedded
// images. We keep the raw encoded bytes in image->image and decode them
// ourselves afterwards so we have full control over the pixel layout.
static bool EruptionLoadImageData(Image* image, const int image_idx, std::string* err,
                                  std::string* warn, int req_width, int req_height,
                                  const unsigned char* bytes, int size, void* user_data) {
    (void)image_idx; (void)err; (void)warn;
    (void)req_width; (void)req_height; (void)user_data;
    if (!image || !bytes || size <= 0) return false;
    image->image.assign(bytes, bytes + size);
    image->width = 0;
    image->height = 0;
    image->component = 4;
    image->bits = 8;
    image->pixel_type = TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;
    return true;
}
} // namespace tinygltf

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iconv.h>
#include <unordered_map>
#include <vector>

namespace eruption {

namespace {

// Sanity limits
static constexpr uint32_t MAX_GLTF_NODES = 200000;
static constexpr uint32_t MAX_GLTF_VERTICES = 10000000;
static constexpr uint32_t MAX_GLTF_INDICES = 10000000;

// Some converters write 2-byte (EUC-KR) texture filenames into the GLB material
// name but mark the JSON as UTF-8. Re-decode the bytes so those legacy folder
// names resolve correctly.
static std::string tryEucKrToUtf8(const std::string& input) {
    if (input.empty()) return input;
    iconv_t cd = iconv_open("UTF-8//IGNORE", "EUC-KR//IGNORE");
    if (cd == (iconv_t)-1) return input;

    std::vector<char> inBuf(input.begin(), input.end());
    char* inPtr = inBuf.data();
    size_t inBytes = inBuf.size();

    std::vector<char> outBuf(inBytes * 4 + 4, '\0');
    char* outPtr = outBuf.data();
    size_t outBytes = outBuf.size();

    if (iconv(cd, &inPtr, &inBytes, &outPtr, &outBytes) == (size_t)-1) {
        iconv_close(cd);
        return input;
    }
    iconv_close(cd);
    return std::string(outBuf.data(), outBuf.size() - outBytes);
}

// Some legacy-to-GLB converters preserve the original texture filename in the
// material name but drop the baseColorTexture reference. Try to recover the
// albedo image from disk so the model does not render as a flat solid color.
static const std::unordered_map<std::string, std::string>& albedoTexturePathCache() {
    namespace fs = std::filesystem;
    static std::unordered_map<std::string, std::string> cache;
    static bool initialized = false;
    if (!initialized) {
        initialized = true;
        static const std::vector<std::string> exts = {".png", ".jpg", ".jpeg", ".bmp", ".tga"};
        static const std::vector<std::string> roots = {"assets/data/texture", "assets/pbr/base"};
        for (const auto& root : roots) {
            if (!fs::exists(root) || !fs::is_directory(root)) continue;
            for (const auto& entry : fs::recursive_directory_iterator(root)) {
                if (!entry.is_regular_file()) continue;
                std::string ext = entry.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (std::find(exts.begin(), exts.end(), ext) == exts.end()) continue;
                std::string stem = entry.path().stem().string();
                if (cache.find(stem) == cache.end()) {
                    cache[stem] = entry.path().string();
                }
            }
        }
    }
    return cache;
}

static std::string findAlbedoTextureByStem(const std::string& stem) {
    if (stem.empty()) return {};

    auto tryStem = [&](const std::string& s) -> std::string {
        if (s.empty()) return {};
        const auto& cache = albedoTexturePathCache();
        auto it = cache.find(s);
        if (it != cache.end()) return it->second;
        // Try lower-case variant.
        std::string lower = s;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lower != s) {
            it = cache.find(lower);
            if (it != cache.end()) return it->second;
        }
        return {};
    };

    // 1. Exact stem as stored in the material name.
    std::string path = tryStem(stem);
    if (!path.empty()) return path;

    // 2. Filename-only stem (material names often include the original folder
    //    path, e.g. "town\\castle-2").
    size_t lastSep = stem.find_last_of("\\/");
    if (lastSep != std::string::npos) {
        path = tryStem(stem.substr(lastSep + 1));
        if (!path.empty()) return path;
    }

    // 3. Source converters sometimes store EUC-KR bytes inside a UTF-8 JSON
    //    string. Re-decode and try again.
    std::string eucKrDecoded = tryEucKrToUtf8(stem);
    if (eucKrDecoded != stem) {
        path = tryStem(eucKrDecoded);
        if (!path.empty()) return path;
        size_t sep2 = eucKrDecoded.find_last_of("\\/");
        if (sep2 != std::string::npos) {
            path = tryStem(eucKrDecoded.substr(sep2 + 1));
            if (!path.empty()) return path;
        }
    }

    return {};
}

struct GltfNodeMapping {
    int gltfNodeIndex = -1;
    int modelNodeIndex = -1;
    int parentModelNodeIndex = 0xFFFFFFFF;
};


// ---------------------------------------------------------------------------
// Silhouette bevel: legacy map geometry meets the sky in razor-sharp 90-degree edges
// (wall tops, ledges), which reads as paper cutouts. At load, find boundary
// edges (used by exactly one triangle) on near-vertical faces whose edge runs
// horizontally along the TOP of that face, and add a small 45-degree chamfer
// strip with an averaged normal. Pure load-time geometry: zero per-frame cost,
// silhouette gets a soft cut instead of a razor corner.
// Disable with ERUPTION_NO_EDGE_BEVEL=1.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Smoothing por ângulo: a geometria de mapa legado chega com normal ACHATADA por
// face, então cada quad do chão tem iluminação constante - o jogador enxerga
// os quads, e o normal-offset bias da sombra muda em degraus quadrados, o que
// vira MOIRÉ na sombra. Aqui as normais de vértices coincidentes são somadas
// quando as faces formam ângulo menor que o limiar (quinas duras de parede /
// penhasco continuam duras). Custo é de LOAD; runtime não paga nada.
// Desliga com ERUPTION_NO_NORMAL_SMOOTH=1.
// ---------------------------------------------------------------------------
static void smoothNormalsByAngle(ModelFileNode& node, float thresholdDeg = 55.0f) {
    static const bool kDisabled = std::getenv("ERUPTION_NO_NORMAL_SMOOTH") != nullptr;
    if (kDisabled) return;
    const size_t n = node.vertices.size();
    if (n < 3 || node.normals.size() != n) return;

    auto key = [](const Vec3& p) -> uint64_t {
        // 1cm: vértices coincidentes do mesmo quad/tile caem no mesmo balde.
        int64_t x = llround(p.x * 100.0) & 0x1FFFFF;
        int64_t y = llround(p.y * 100.0) & 0x1FFFFF;
        int64_t z = llround(p.z * 100.0) & 0x1FFFFF;
        return (uint64_t(x) << 42) | (uint64_t(y) << 21) | uint64_t(z);
    };

    std::unordered_map<uint64_t, std::vector<uint32_t>> buckets;
    buckets.reserve(n);
    for (uint32_t i = 0; i < n; ++i) buckets[key(node.vertices[i])].push_back(i);

    const float cosThreshold = std::cos(glm::radians(thresholdDeg));
    std::vector<Vec3> smoothed(node.normals);
    for (const auto& [k, ids] : buckets) {
        if (ids.size() < 2) continue;
        for (uint32_t i : ids) {
            Vec3 acc = node.normals[i];
            for (uint32_t j : ids) {
                if (i == j) continue;
                // Só acumula faces que pertencem à mesma superfície suave.
                if (glm::dot(node.normals[i], node.normals[j]) >= cosThreshold) {
                    acc += node.normals[j];
                }
            }
            float len = glm::length(acc);
            if (len > 1e-6f) smoothed[i] = acc / len;
        }
    }
    node.normals.swap(smoothed);
}

static void addSilhouetteBevels(ModelFileNode& node) {
    static const bool kDisabled = std::getenv("ERUPTION_NO_EDGE_BEVEL") != nullptr;
    if (kDisabled) return;

    const size_t vcount = node.vertices.size();
    if (vcount < 3 || vcount % 3 != 0) return;      // expanded soup expected
    const size_t triCount = vcount / 3;
    if (triCount > 60000) return;                    // skip mega meshes (terrain ground)
    if (node.texCoords.size() != vcount || node.normals.size() != vcount ||
        node.colors.size() != vcount) return;

    const bool hasTexIds = node.perVertexTexIds.size() == vcount;
    const bool hasBlend  = node.perVertexBlendWeights.size() == vcount;

    auto qkey = [](const Vec3& p) -> uint64_t {
        // 1cm quantisation so coincident corners of neighbouring triangles merge.
        int64_t x = llround(p.x * 100.0f) & 0x1FFFFF;
        int64_t y = llround(p.y * 100.0f) & 0x1FFFFF;
        int64_t z = llround(p.z * 100.0f) & 0x1FFFFF;
        return (uint64_t(x) << 42) | (uint64_t(y) << 21) | uint64_t(z);
    };

    struct EdgeInfo { uint32_t v0, v1, tri; uint8_t count; };
    std::unordered_map<uint64_t, EdgeInfo> edges;
    edges.reserve(triCount * 3);
    auto ekey = [](uint64_t a, uint64_t b) {
        if (a > b) std::swap(a, b);
        // Mix the two position keys into one map key.
        return a * 1000003ULL ^ b;
    };

    for (size_t t = 0; t < triCount; ++t) {
        const uint32_t i0 = uint32_t(t * 3), i1 = i0 + 1, i2 = i0 + 2;
        const uint64_t k0 = qkey(node.vertices[i0]);
        const uint64_t k1 = qkey(node.vertices[i1]);
        const uint64_t k2 = qkey(node.vertices[i2]);
        const uint32_t vs[3][2] = {{i0, i1}, {i1, i2}, {i2, i0}};
        const uint64_t ks[3] = {ekey(k0, k1), ekey(k1, k2), ekey(k2, k0)};
        for (int e = 0; e < 3; ++e) {
            auto it = edges.find(ks[e]);
            if (it == edges.end()) {
                edges.emplace(ks[e], EdgeInfo{vs[e][0], vs[e][1], uint32_t(t), 1});
            } else if (it->second.count < 255) {
                it->second.count++;
            }
        }
    }

    size_t boundaryCount = 0;
    for (const auto& [key, e] : edges) {
        if (e.count == 1) ++boundaryCount;
    }
    if (edges.empty() || boundaryCount * 5 > edges.size() * 2) return; // >40% boundary: foliage/cutout, not a wall

    const Vec3 up(0.0f, 1.0f, 0.0f);
    const float W = 0.10f;      // chamfer size in world units
    int emitted = 0;
    const int kMaxStrips = 3000;

    for (const auto& [key, e] : edges) {
        if (e.count != 1) continue;              // boundary edge only
        if (emitted >= kMaxStrips) break;

        const Vec3 A = node.vertices[e.v0];
        const Vec3 B = node.vertices[e.v1];
        Vec3 edgeDir = B - A;
        const float elen = glm::length(edgeDir);
        if (elen < 0.2f) continue;               // skip micro edges
        if (std::abs(edgeDir.y) > 0.3f * elen) continue; // horizontal-ish only

        // Face normal (geometric) of the owning triangle.
        const uint32_t t0 = e.tri * 3;
        const Vec3 fA = node.vertices[t0], fB = node.vertices[t0 + 1], fC = node.vertices[t0 + 2];
        Vec3 fn = glm::cross(fB - fA, fC - fA);
        const float fl = glm::length(fn);
        if (fl < 1e-6f) continue;
        fn /= fl;
        if (std::abs(fn.y) > 0.35f) continue;    // near-vertical faces only (walls)

        // Top edge of that face: the edge sits at/above the remaining vertex.
        float otherY = -1e9f;
        for (int c = 0; c < 3; ++c) {
            const uint32_t vi = t0 + c;
            if (vi != e.v0 && vi != e.v1) otherY = node.vertices[vi].y;
        }
        if (glm::min(A.y, B.y) < otherY - 0.05f) continue;

        // Chamfer strip: edge -> edge shifted up-and-inward. Normal averaged
        // between the wall face and up, so lighting rolls over the corner.
        const Vec3 off = up * (W * 0.6f) - fn * (W * 0.6f);
        const Vec3 A2 = A + off, B2 = B + off;
        const Vec3 nrm = glm::normalize(fn + up);

        auto emitVert = [&](const Vec3& pos, uint32_t srcIdx) {
            node.vertices.push_back(pos);
            node.texCoords.push_back(node.texCoords[srcIdx]);
            node.normals.push_back(nrm);
            node.colors.push_back(node.colors[srcIdx]);
            node.indices.push_back(static_cast<uint32_t>(node.indices.size()));
            if (hasTexIds) node.perVertexTexIds.push_back(node.perVertexTexIds[srcIdx]);
            if (hasBlend) {
                node.perVertexBlendTexIds.push_back(node.perVertexBlendTexIds[srcIdx]);
                node.perVertexBlendWeights.push_back(node.perVertexBlendWeights[srcIdx]);
            }
        };
        // Two triangles, both windings irrelevant (model pipeline culls none).
        emitVert(A, e.v0); emitVert(B, e.v1); emitVert(B2, e.v1);
        emitVert(A, e.v0); emitVert(B2, e.v1); emitVert(A2, e.v0);
        ++emitted;
    }
}

// Normalise a texture/material identifier so it matches the renderer's lookup.
// Engine::resolveModelTexture replaces '/' with '\' and strips a leading '\'.
static std::string sanitizeTextureName(const std::string& name) {
    std::string s = name;
    std::replace(s.begin(), s.end(), '/', '\\');
    if (!s.empty() && s[0] == '\\') s = s.substr(1);
    return s;
}

static std::string materialName(const tinygltf::Model& model, int materialIndex) {
    if (materialIndex < 0 || materialIndex >= static_cast<int>(model.materials.size())) {
        return "";
    }
    return sanitizeTextureName(model.materials[materialIndex].name);
}

template<typename T>
static std::vector<T> readAccessor(const tinygltf::Model& model, int accessorIndex) {
    std::vector<T> out;
    if (accessorIndex < 0 || accessorIndex >= static_cast<int>(model.accessors.size())) {
        return out;
    }
    const tinygltf::Accessor& accessor = model.accessors[accessorIndex];
    const tinygltf::BufferView& view = model.bufferViews[accessor.bufferView];
    const tinygltf::Buffer& buffer = model.buffers[view.buffer];

    const uint8_t* base = buffer.data.data() + view.byteOffset + accessor.byteOffset;
    size_t count = accessor.count;
    size_t stride = view.byteStride;

    out.resize(count);
    for (size_t i = 0; i < count; ++i) {
        const uint8_t* src = base + i * (stride ? stride : sizeof(T));
        std::memcpy(&out[i], src, sizeof(T));
    }
    return out;
}

static std::vector<float> readFloatAccessor(const tinygltf::Model& model, int accessorIndex) {
    std::vector<float> out;
    if (accessorIndex < 0) return out;
    const tinygltf::Accessor& accessor = model.accessors[accessorIndex];
    const tinygltf::BufferView& view = model.bufferViews[accessor.bufferView];
    const tinygltf::Buffer& buffer = model.buffers[view.buffer];

    const uint8_t* base = buffer.data.data() + view.byteOffset + accessor.byteOffset;
    size_t count = accessor.count;
    size_t stride = view.byteStride ? view.byteStride : sizeof(float);

    out.resize(count);
    if (stride == sizeof(float)) {
        std::memcpy(out.data(), base, count * sizeof(float));
    } else {
        for (size_t i = 0; i < count; ++i) {
            const float* src = reinterpret_cast<const float*>(base + i * stride);
            out[i] = src[0];
        }
    }
    return out;
}

static std::vector<Vec3> readVec3Accessor(const tinygltf::Model& model, int accessorIndex) {
    std::vector<Vec3> out;
    if (accessorIndex < 0) return out;
    const tinygltf::Accessor& accessor = model.accessors[accessorIndex];
    const tinygltf::BufferView& view = model.bufferViews[accessor.bufferView];
    const tinygltf::Buffer& buffer = model.buffers[view.buffer];

    const uint8_t* base = buffer.data.data() + view.byteOffset + accessor.byteOffset;
    size_t count = accessor.count;
    size_t stride = view.byteStride ? view.byteStride : (3 * sizeof(float));

    out.resize(count);
    if (stride == sizeof(Vec3)) {
        std::memcpy(out.data(), base, count * sizeof(Vec3));
    } else {
        for (size_t i = 0; i < count; ++i) {
            const float* src = reinterpret_cast<const float*>(base + i * stride);
            out[i] = Vec3(src[0], src[1], src[2]);
        }
    }
    return out;
}

static std::vector<Vec2> readVec2Accessor(const tinygltf::Model& model, int accessorIndex) {
    std::vector<Vec2> out;
    if (accessorIndex < 0) return out;
    const tinygltf::Accessor& accessor = model.accessors[accessorIndex];
    const tinygltf::BufferView& view = model.bufferViews[accessor.bufferView];
    const tinygltf::Buffer& buffer = model.buffers[view.buffer];

    const uint8_t* base = buffer.data.data() + view.byteOffset + accessor.byteOffset;
    size_t count = accessor.count;
    size_t stride = view.byteStride ? view.byteStride : (2 * sizeof(float));

    out.resize(count);
    if (stride == sizeof(Vec2)) {
        std::memcpy(out.data(), base, count * sizeof(Vec2));
    } else {
        for (size_t i = 0; i < count; ++i) {
            const float* src = reinterpret_cast<const float*>(base + i * stride);
            out[i] = Vec2(src[0], src[1]);
        }
    }
    return out;
}

static std::vector<uint32_t> readIndexAccessor(const tinygltf::Model& model, int accessorIndex) {
    std::vector<uint32_t> out;
    if (accessorIndex < 0) return out;
    const tinygltf::Accessor& accessor = model.accessors[accessorIndex];
    const tinygltf::BufferView& view = model.bufferViews[accessor.bufferView];
    const tinygltf::Buffer& buffer = model.buffers[view.buffer];

    const uint8_t* base = buffer.data.data() + view.byteOffset + accessor.byteOffset;
    size_t count = accessor.count;

    if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT) {
        size_t stride = view.byteStride ? view.byteStride : sizeof(uint32_t);
        out.resize(count);
        if (stride == sizeof(uint32_t)) {
            std::memcpy(out.data(), base, count * sizeof(uint32_t));
        } else {
            for (size_t i = 0; i < count; ++i) {
                std::memcpy(&out[i], base + i * stride, sizeof(uint32_t));
            }
        }
    } else if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
        size_t stride = view.byteStride ? view.byteStride : sizeof(uint16_t);
        out.resize(count);
        for (size_t i = 0; i < count; ++i) {
            uint16_t v;
            std::memcpy(&v, base + i * stride, sizeof(uint16_t));
            out[i] = static_cast<uint32_t>(v);
        }
    } else if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) {
        size_t stride = view.byteStride ? view.byteStride : sizeof(uint8_t);
        out.resize(count);
        for (size_t i = 0; i < count; ++i) {
            out[i] = static_cast<uint32_t>(*(base + i * stride));
        }
    }
    return out;
}

static std::vector<Vec4> readColorAccessor(const tinygltf::Model& model, int accessorIndex) {
    std::vector<Vec4> out;
    if (accessorIndex < 0) return out;
    const tinygltf::Accessor& accessor = model.accessors[accessorIndex];
    const tinygltf::BufferView& view = model.bufferViews[accessor.bufferView];
    const tinygltf::Buffer& buffer = model.buffers[view.buffer];

    const uint8_t* base = buffer.data.data() + view.byteOffset + accessor.byteOffset;
    size_t count = accessor.count;

    out.resize(count, Vec4(1.0f));
    if (accessor.type == TINYGLTF_TYPE_VEC4) {
        if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT) {
            size_t stride = view.byteStride ? view.byteStride : (4 * sizeof(float));
            for (size_t i = 0; i < count; ++i) {
                const float* src = reinterpret_cast<const float*>(base + i * stride);
                out[i] = Vec4(src[0], src[1], src[2], src[3]);
            }
        } else if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) {
            size_t stride = view.byteStride ? view.byteStride : 4;
            for (size_t i = 0; i < count; ++i) {
                const uint8_t* src = base + i * stride;
                out[i] = Vec4(src[0] / 255.0f, src[1] / 255.0f, src[2] / 255.0f, src[3] / 255.0f);
            }
        } else if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
            size_t stride = view.byteStride ? view.byteStride : 8;
            for (size_t i = 0; i < count; ++i) {
                const uint16_t* src = reinterpret_cast<const uint16_t*>(base + i * stride);
                out[i] = Vec4(src[0] / 65535.0f, src[1] / 65535.0f, src[2] / 65535.0f, src[3] / 65535.0f);
            }
        }
    } else if (accessor.type == TINYGLTF_TYPE_VEC3) {
        if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT) {
            size_t stride = view.byteStride ? view.byteStride : (3 * sizeof(float));
            for (size_t i = 0; i < count; ++i) {
                const float* src = reinterpret_cast<const float*>(base + i * stride);
                out[i] = Vec4(src[0], src[1], src[2], 1.0f);
            }
        } else if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) {
            size_t stride = view.byteStride ? view.byteStride : 3;
            for (size_t i = 0; i < count; ++i) {
                const uint8_t* src = base + i * stride;
                out[i] = Vec4(src[0] / 255.0f, src[1] / 255.0f, src[2] / 255.0f, 1.0f);
            }
        } else if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
            size_t stride = view.byteStride ? view.byteStride : 6;
            for (size_t i = 0; i < count; ++i) {
                const uint16_t* src = reinterpret_cast<const uint16_t*>(base + i * stride);
                out[i] = Vec4(src[0] / 65535.0f, src[1] / 65535.0f, src[2] / 65535.0f, 1.0f);
            }
        }
    }
    return out;
}

static uint32_t vec4ToBgra(const Vec4& c) {
    uint8_t r = static_cast<uint8_t>(glm::clamp(c.x, 0.0f, 1.0f) * 255.0f);
    uint8_t g = static_cast<uint8_t>(glm::clamp(c.y, 0.0f, 1.0f) * 255.0f);
    uint8_t b = static_cast<uint8_t>(glm::clamp(c.z, 0.0f, 1.0f) * 255.0f);
    uint8_t a = static_cast<uint8_t>(glm::clamp(c.w, 0.0f, 1.0f) * 255.0f);
    return (r << 0) | (g << 8) | (b << 16) | (a << 24);
}

static Mat4 nodeLocalMatrix(const tinygltf::Node& node) {
    Mat4 m(1.0f);
    if (!node.matrix.empty()) {
        for (int i = 0; i < 16; ++i) {
            m[i / 4][i % 4] = static_cast<float>(node.matrix[i]);
        }
    } else {
        if (!node.translation.empty()) {
            m = glm::translate(m, Vec3(static_cast<float>(node.translation[0]),
                                        static_cast<float>(node.translation[1]),
                                        static_cast<float>(node.translation[2])));
        }
        if (!node.rotation.empty()) {
            glm::quat q(static_cast<float>(node.rotation[3]),
                        static_cast<float>(node.rotation[0]),
                        static_cast<float>(node.rotation[1]),
                        static_cast<float>(node.rotation[2]));
            m = m * glm::mat4_cast(q);
        }
        if (!node.scale.empty()) {
            m = glm::scale(m, Vec3(static_cast<float>(node.scale[0]),
                                    static_cast<float>(node.scale[1]),
                                    static_cast<float>(node.scale[2])));
        }
    }
    return m;
}

static int modelNodeIndexForGltfNode(const std::vector<GltfNodeMapping>& mappings, int gltfIndex) {
    for (const auto& m : mappings) {
        if (m.gltfNodeIndex == gltfIndex) return m.modelNodeIndex;
    }
    return -1;
}

static std::vector<int> buildGltfParentMap(const tinygltf::Model& model) {
    std::vector<int> parents(model.nodes.size(), -1);
    for (int i = 0; i < static_cast<int>(model.nodes.size()); ++i) {
        for (int child : model.nodes[i].children) {
            if (child >= 0 && child < static_cast<int>(model.nodes.size())) {
                parents[child] = i;
            }
        }
    }
    return parents;
}

static bool isBlockingMaterial(const tinygltf::Material& mat) {
    std::string name = mat.name;
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    static const std::vector<std::string> blockingNames = {
        "blocking_ao_basic", "blocking_glass_fake",
        "paralax_interiors", "parallax_interiors"
    };
    for (const auto& s : blockingNames) {
        if (name.find(s) != std::string::npos) {
            return true;
        }
    }
    return false;
}

static bool shouldSkipGltfNode(const tinygltf::Model& model, const tinygltf::Node& node) {
    std::string name = node.name;
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    static const std::vector<std::string> skipNames = {
        "sky", "fog", "fog_fg", "lightblocker", "light_blocker",
        "lightblock", "blocker", "environment", "background"
    };
    for (const auto& s : skipNames) {
        if (name.find(s) != std::string::npos) {
            return true;
        }
    }
    // Skip absurdly scaled objects that are clearly sky/background cards.
    if (!node.scale.empty()) {
        float maxScale = 0.0f;
        for (double v : node.scale) maxScale = std::max(maxScale, static_cast<float>(std::abs(v)));
        if (maxScale > 1000.0f) return true;
    }
    // Skip meshes whose materials are Blender-only blocking/parallax/matte cards
    // that do not survive glTF export and show up as untextured white/dark planes.
    if (node.mesh >= 0 && node.mesh < static_cast<int>(model.meshes.size())) {
        const tinygltf::Mesh& mesh = model.meshes[node.mesh];
        for (const auto& prim : mesh.primitives) {
            if (prim.material >= 0 && prim.material < static_cast<int>(model.materials.size())) {
                if (isBlockingMaterial(model.materials[prim.material])) {
                    return true;
                }
            }
        }
    }
    return false;
}

static std::vector<GltfNodeMapping> buildNodeMapping(const tinygltf::Model& model) {
    std::vector<int> gltfParents = buildGltfParentMap(model);
    std::vector<GltfNodeMapping> mappings;
    mappings.reserve(model.nodes.size());

    for (int i = 0; i < static_cast<int>(model.nodes.size()); ++i) {
        GltfNodeMapping m;
        m.gltfNodeIndex = i;
        mappings.push_back(m);
    }

    // First pass: assign modelNodeIndex to glTF nodes that carry a mesh.
    // Skip sky/fog/light-blocker cards that only occlude the playable scene.
    int nextModelIndex = 0;
    for (auto& m : mappings) {
        const tinygltf::Node& node = model.nodes[m.gltfNodeIndex];
        if (node.mesh >= 0 && !shouldSkipGltfNode(model, node)) {
            m.modelNodeIndex = nextModelIndex++;
        }
    }

    // Second pass: resolve parent model node indices by walking glTF parents.
    for (auto& m : mappings) {
        if (m.modelNodeIndex < 0) continue;
        int current = gltfParents[m.gltfNodeIndex];
        while (current >= 0) {
            int parentModelIdx = modelNodeIndexForGltfNode(mappings, current);
            if (parentModelIdx >= 0) {
                m.parentModelNodeIndex = parentModelIdx;
                break;
            }
            current = gltfParents[current];
        }
    }

    return mappings;
}

static Mat4 computeGltfNodeWorldMatrix(const tinygltf::Model& model,
                                       const std::vector<int>& parents,
                                       int nodeIndex,
                                       std::vector<Mat4>& cache) {
    if (nodeIndex < 0 || nodeIndex >= static_cast<int>(model.nodes.size())) {
        return Mat4(1.0f);
    }
    if (cache[nodeIndex] != Mat4(0.0f)) {
        return cache[nodeIndex];
    }
    Mat4 local = nodeLocalMatrix(model.nodes[nodeIndex]);
    int parent = parents[nodeIndex];
    if (parent >= 0) {
        cache[nodeIndex] = computeGltfNodeWorldMatrix(model, parents, parent, cache) * local;
    } else {
        cache[nodeIndex] = local;
    }
    return cache[nodeIndex];
}

} // anonymous namespace

ModelFile GltfParser::parse(const uint8_t* data, size_t size,
                            const std::string& baseDir) {
    ModelFile model;
    model.versionMajor = 2;
    model.versionMinor = 2;
    model.version = 0x0202;

    if (size < 12 || std::memcmp(data, "glTF", 4) != 0) {
        ERUPTION_LOG_ERROR("GltfParser: invalid GLB magic");
        return model;
    }

    tinygltf::TinyGLTF loader;
    loader.SetImageLoader(tinygltf::EruptionLoadImageData, nullptr);

    tinygltf::Model gltf;
    std::string err;
    std::string warn;

    bool ok = loader.LoadBinaryFromMemory(&gltf, &err, &warn,
                                          data, static_cast<unsigned int>(size),
                                          baseDir);
    if (!warn.empty()) {
        ERUPTION_LOG_WARN("GltfParser: %s", warn.c_str());
    }
    if (!ok) {
        ERUPTION_LOG_ERROR("GltfParser: failed to parse GLB: %s", err.c_str());
        return model;
    }

    if (gltf.nodes.size() > MAX_GLTF_NODES) {
        ERUPTION_LOG_ERROR("GltfParser: node count %zu exceeds limit", gltf.nodes.size());
        return model;
    }

    // Full map bakes produced by eruption-asset-extractor-bake pre-apply the
    // source Y-down -> Y-up flip at the instance level, so their world-space
    // transforms must NOT receive the global Y-flip again. Mark them as native
    // v2.2 so ModelRenderer loads them exactly like other authored GLB assets.
    // Per-model conversions from eruption-asset-extractor still emit raw Y-down
    // data and rely on the engine's source flip.
    if (gltf.asset.generator == "eruption-asset-extractor-bake") {
        model.versionMajor = 2;
        model.versionMinor = 2;
        model.version = 0x0202;
    } else if (gltf.asset.generator == "eruption-asset-extractor" ||
               gltf.asset.generator == "eruption-map-to-glb") {
        model.versionMajor = 1;
        model.versionMinor = 4;
        model.version = 0x0104;
    }

    model.name = gltf.asset.generator;

    // Collect embedded image metadata. We keep the raw encoded bytes (PNG/JPEG)
    // and decode them lazily when the texture is actually resolved. This keeps
    // the GLB parse fast for huge assets such as the Poly Haven Hidden Alley.
    std::vector<std::string> imageNames;
    imageNames.reserve(gltf.images.size());
    for (size_t i = 0; i < gltf.images.size(); ++i) {
        tinygltf::Image& img = gltf.images[i];
        if (img.image.empty()) {
            imageNames.push_back("");
            continue;
        }

        // Imagem com URI = arquivo de verdade ao lado do GLB (fluxo padrão
        // glTF). Nomear pelo STEM do arquivo, sem o prefixo '#', faz o
        // material resolver PBR autoral (<stem>_mrahw/_normal) e bake .etex
        // exatamente como qualquer textura de disco. Só imagem realmente
        // embutida (sem URI) usa o nome sintético com '#'.
        std::string name = "#image" + std::to_string(i);
        if (!img.uri.empty()) {
            std::string stem = img.uri;
            size_t slash = stem.find_last_of("/\\");
            if (slash != std::string::npos) stem = stem.substr(slash + 1);
            size_t dot = stem.find_last_of('.');
            if (dot != std::string::npos) stem = stem.substr(0, dot);
            if (!stem.empty()) name = stem;
        } else if (!img.name.empty()) {
            name = "#" + img.name;
        }

        EmbeddedTexture et;
        et.name = name;
        // MOVE, nao copia. O carregador de imagem ja' copiou os bytes crus do
        // GLB para img.image (EruptionLoadImageData faz assign); copiar de novo
        // aqui duplicava ~290 MB de PNG no parana_field - dois memcpy enormes
        // por carga. `gltf` e' local a parse() e img.image nao e' lido depois
        // deste ponto, entao mover e' seguro.
        et.encodedData = std::move(img.image); // lazy decode
        model.embeddedTextures.push_back(std::move(et));

        // Global texture list used by the source loader paths.
        model.textures.push_back(name);
        imageNames.push_back(name);
    }

    // Map each glTF material to its base-color embedded texture name.
    // Materials without a base-color texture get a 1x1 solid-color texture so
    // the renderer never has to fall back to an invalid/missing texture slot.
    std::vector<std::string> materialTextureName(gltf.materials.size());
    for (size_t mi = 0; mi < gltf.materials.size(); ++mi) {
        const tinygltf::Material& mat = gltf.materials[mi];
        int texIdx = mat.pbrMetallicRoughness.baseColorTexture.index;
        if (texIdx >= 0 && texIdx < static_cast<int>(gltf.textures.size())) {
            int imgIdx = gltf.textures[texIdx].source;
            if (imgIdx >= 0 && imgIdx < static_cast<int>(imageNames.size()) && !imageNames[imgIdx].empty()) {
                materialTextureName[mi] = imageNames[imgIdx];
                continue;
            }
        }

        // No texture: try to recover an albedo image from the material name
        // (source converters often store the original filename there) before
        // falling back to a 1x1 solid color.
        std::string matNameSanitized = sanitizeTextureName(mat.name);
        std::string solidName = matNameSanitized.empty() ? ("#solid_material_" + std::to_string(mi)) : ("#solid_" + matNameSanitized);
        std::string stem = matNameSanitized.empty() ? std::string() : std::filesystem::path(matNameSanitized).stem().string();
        std::string albedoPath = findAlbedoTextureByStem(stem);
        if (!albedoPath.empty()) {
            ImageData img = ImageUtils::loadPNG(albedoPath);
            if (img.isValid() && !img.pixels.empty() && img.width > 0 && img.height > 0) {
                EmbeddedTexture et;
                et.name = solidName;
                et.width = img.width;
                et.height = img.height;
                et.channels = img.channels;
                et.pixels = std::move(img.pixels);
                model.embeddedTextures.push_back(std::move(et));
                model.textures.push_back(solidName);
                imageNames.push_back(solidName);
                materialTextureName[mi] = solidName;
                continue;
            }
        }

        // Fallback: synthesize a 1x1 RGBA texture from baseColorFactor.
        const auto& c = mat.pbrMetallicRoughness.baseColorFactor;
        EmbeddedTexture et;
        et.name = solidName;
        et.width = 1;
        et.height = 1;
        et.channels = 4;
        et.pixels.resize(4);
        et.pixels[0] = static_cast<uint8_t>(glm::clamp(c.size() > 0 ? c[0] : 1.0, 0.0, 1.0) * 255.0);
        et.pixels[1] = static_cast<uint8_t>(glm::clamp(c.size() > 1 ? c[1] : 1.0, 0.0, 1.0) * 255.0);
        et.pixels[2] = static_cast<uint8_t>(glm::clamp(c.size() > 2 ? c[2] : 1.0, 0.0, 1.0) * 255.0);
        et.pixels[3] = static_cast<uint8_t>(glm::clamp(c.size() > 3 ? c[3] : 1.0, 0.0, 1.0) * 255.0);
        model.embeddedTextures.push_back(std::move(et));
        model.textures.push_back(solidName);
        imageNames.push_back(solidName);
        materialTextureName[mi] = solidName;
    }

    auto mappings = buildNodeMapping(gltf);

    // Pre-compute glTF world transforms so static rendering respects parenting.
    std::vector<int> gltfParents = buildGltfParentMap(gltf);
    std::vector<Mat4> gltfWorldMatrices(gltf.nodes.size(), Mat4(0.0f));
    for (size_t i = 0; i < gltf.nodes.size(); ++i) {
        gltfWorldMatrices[i] = computeGltfNodeWorldMatrix(gltf, gltfParents, static_cast<int>(i), gltfWorldMatrices);
    }

    // Reserve nodes in the order they will be emitted.
    int meshNodeCount = 0;
    for (const auto& m : mappings) {
        if (m.modelNodeIndex >= 0) ++meshNodeCount;
    }
    model.nodes.resize(meshNodeCount);

    // Instancing de geometria. Vários nós costumam referenciar a MESMA mesh do
    // glTF (uma árvore instanciada 883 vezes, por exemplo). Materializar os
    // vértices por instância multiplicava a malha por dezenas de vezes: no
    // parana_field, 583k vértices únicos viravam 91M, com 191 s de parse e
    // 18,7 GB de RSS. Agora só o primeiro nó de cada mesh ("dono") expande a
    // geometria; os demais guardam geometryShareIndex e o renderer reusa o
    // mesmo ModelMeshGPU com outra matriz.
    //
    // Isso substitui o antigo cap de expansões (kLargeMeshVerts/20000), que
    // era um paliativo: ele descartava geometria em silêncio a partir da
    // terceira instância de uma mesh grande - e não pegava as duas árvores do
    // parana_field, de 17038 e 16981 vértices, logo abaixo do limite.
    std::unordered_map<int, int> meshGeometryOwner; // gltf mesh -> modelNodeIndex do dono
    // hash de conteudo -> modelNodeIndex do dono (dedup de geometria duplicada
    // byte-a-byte, ver o bloco no fim do laco). multimap: colisao de hash e'
    // resolvida por comparacao exata, nao descartando candidato.
    std::unordered_multimap<uint64_t, int> contentOwners;
    size_t dedupHits = 0;
    // ERUPTION_NO_GEOM_SHARE=1 (debug): volta a expandir a geometria por
    // instância, como antes. Serve para A/B visual e para bisect quando um
    // asset se comportar de forma estranha.
    static const bool kGeomShareOff = std::getenv("ERUPTION_NO_GEOM_SHARE") != nullptr;
    for (const auto& m : mappings) {
        if (m.modelNodeIndex < 0) continue;

        const tinygltf::Node& gltfNode = gltf.nodes[m.gltfNodeIndex];
        ModelFileNode& node = model.nodes[m.modelNodeIndex];

        node.name = gltfNode.name.empty() ? ("Node_" + std::to_string(m.modelNodeIndex)) : gltfNode.name;
        node.parentIndex = (m.parentModelNodeIndex < 0)
                               ? 0xFFFFFFFF
                               : static_cast<uint32_t>(m.parentModelNodeIndex);

        Mat4 local = nodeLocalMatrix(gltfNode);
        node.nodeMatrix = local;
        node.renderMatrix = gltfWorldMatrices[m.gltfNodeIndex];

        // Decompose for animation fallback / legacy model compatibility.
        Vec3 decScale(1.0f);
        glm::quat decRot(1.0f, 0.0f, 0.0f, 0.0f);
        Vec3 decPos(0.0f);
        Vec3 skew(0.0f);
        Vec4 perspective(0.0f);
        glm::decompose(local, decScale, decRot, decPos, skew, perspective);
        // glm::decompose might return conjugate rotation depending on GLM version
        decRot = glm::conjugate(decRot);

        if (!gltfNode.scale.empty()) {
            node.scale = Vec3(static_cast<float>(gltfNode.scale[0]),
                              static_cast<float>(gltfNode.scale[1]),
                              static_cast<float>(gltfNode.scale[2]));
        } else {
            node.scale = decScale;
        }
        if (!gltfNode.translation.empty()) {
            node.position = Vec3(static_cast<float>(gltfNode.translation[0]),
                                  static_cast<float>(gltfNode.translation[1]),
                                  static_cast<float>(gltfNode.translation[2]));
        } else {
            node.position = decPos;
        }
        if (!gltfNode.rotation.empty()) {
            glm::quat q(static_cast<float>(gltfNode.rotation[3]),
                         static_cast<float>(gltfNode.rotation[0]),
                         static_cast<float>(gltfNode.rotation[1]),
                         static_cast<float>(gltfNode.rotation[2]));
            node.rotationAxis = glm::axis(q);
            node.rotationAngle = glm::angle(q);
        } else {
            if (glm::length(decRot) > 0.001f) {
                node.rotationAxis = glm::axis(decRot);
                node.rotationAngle = glm::angle(decRot);
            }
        }

        if (gltfNode.mesh < 0 || gltfNode.mesh >= static_cast<int>(gltf.meshes.size())) {
            continue;
        }

        const tinygltf::Mesh& mesh = gltf.meshes[gltfNode.mesh];

        // Esta mesh já foi expandida por outro nó? Então este nó não materializa
        // nada: aponta para o dono e fica só com a matriz. Os nomes de textura
        // são copiados porque saem do material da primitiva - idênticos para a
        // mesma mesh - e o resto do engine lê essa lista pelo nó.
        {
            auto ownerIt = kGeomShareOff ? meshGeometryOwner.end()
                                         : meshGeometryOwner.find(gltfNode.mesh);
            if (ownerIt != meshGeometryOwner.end()) {
                const ModelFileNode& owner = model.nodes[ownerIt->second];
                node.geometryShareIndex = ownerIt->second;
                node.textureNames = owner.textureNames;
                node.pbrTextureNames = owner.pbrTextureNames;
                node.box = owner.box; // AABB local; a matriz do nó é aplicada depois
                continue;
            }
            if (!kGeomShareOff) meshGeometryOwner[gltfNode.mesh] = m.modelNodeIndex;
        }

        // Collect texture/material names used by this node and map primitive material to local index.
        std::unordered_map<std::string, uint16_t> texNameToLocal;
        std::vector<uint16_t> primitiveTexLocal(mesh.primitives.size(), 0);

        for (size_t p = 0; p < mesh.primitives.size(); ++p) {
            const tinygltf::Primitive& prim = mesh.primitives[p];
            std::string texName;
            if (prim.material >= 0 && prim.material < static_cast<int>(materialTextureName.size())) {
                texName = materialTextureName[prim.material];
            }
            if (texName.empty()) texName = materialName(gltf, prim.material);
            if (texName.empty()) texName = "default";

            // The embedded image name is often a synthetic "#___name_0000.png" that
            // does not match cooked PBR maps. Keep the original material name so
            // the renderer can resolve _mrahw/_normal pairs from assets/data/texture.
            std::string pbrName = materialName(gltf, prim.material);
            if (pbrName.empty()) pbrName = texName;

            auto it = texNameToLocal.find(texName);
            if (it == texNameToLocal.end()) {
                uint16_t idx = static_cast<uint16_t>(node.textureNames.size());
                texNameToLocal[texName] = idx;
                node.textureNames.push_back(texName);
                node.pbrTextureNames.push_back(pbrName);
            } else {
                // If the runtime albedo name already exists, keep the first PBR
                // name we recorded; it should match the same material.
            }
            primitiveTexLocal[p] = texNameToLocal[texName];
        }

        // Resolve a glTF material index (as referenced by _BLEND_TEX_INDEX) to a
        // local slot in this node's texture list, appending it if new. Mirrors
        // the primitive-material handling above so the crossfade target resolves
        // exactly like the primary texture downstream.
        auto blendMaterialLocalSlot = [&](uint32_t matIdx) -> uint16_t {
            std::string texName;
            if (matIdx < materialTextureName.size()) texName = materialTextureName[matIdx];
            if (texName.empty()) texName = materialName(gltf, static_cast<int>(matIdx));
            if (texName.empty()) return 0;
            auto it = texNameToLocal.find(texName);
            if (it != texNameToLocal.end()) return it->second;
            uint16_t idx = static_cast<uint16_t>(node.textureNames.size());
            texNameToLocal[texName] = idx;
            node.textureNames.push_back(texName);
            std::string pbrName = materialName(gltf, static_cast<int>(matIdx));
            node.pbrTextureNames.push_back(pbrName.empty() ? texName : pbrName);
            return idx;
        };

        // Merge all primitives into one vertex/index stream, expanding per-corner data.
        for (size_t p = 0; p < mesh.primitives.size(); ++p) {
            const tinygltf::Primitive& prim = mesh.primitives[p];

            auto itPos = prim.attributes.find("POSITION");
            auto itNrm = prim.attributes.find("NORMAL");
            auto itUv = prim.attributes.find("TEXCOORD_0");
            auto itCol = prim.attributes.find("COLOR_0");
            auto itBlendTex = prim.attributes.find("_BLEND_TEX_INDEX");
            auto itBlendPbr = prim.attributes.find("_BLEND_PBR_INDEX");
            auto itBlendNormal = prim.attributes.find("_BLEND_NORMAL_INDEX");
            auto itBlendWeight = prim.attributes.find("_BLEND_WEIGHT");
            // World-space splat mask (new, optional): a single material index
            // (resolved just like _BLEND_TEX_INDEX) plus a per-vertex UV to
            // sample it at. Absent on every GLB baked before this feature.
            auto itBlendMaskTex = prim.attributes.find("_BLEND_MASK_TEX_INDEX");
            auto itBlendMaskUV = prim.attributes.find("_BLEND_MASK_UV");
            // Segunda mascara (dobra o splat de 4 pra 8 camadas + base = 9).
            // Reusa _BLEND_MASK_UV - e' so' posicao de mundo, serve pras duas.
            auto itBlendMaskTex2 = prim.attributes.find("_BLEND_MASK_TEX_INDEX_2");
            // Splat de ate' 8 camadas. Cada _SPLAT_TEX_n aponta um material do
            // GLB (resolvido igual ao _BLEND_TEX_INDEX); a mascara de peso e'
            // _BLEND_MASK_TEX_INDEX (camadas 0..3) ou _BLEND_MASK_TEX_INDEX_2
            // (camadas 4..7), lidas como RGBA em vez de R.
            decltype(prim.attributes.find("")) itSplat[8];
            bool hasSplat = false;
            for (int sIdx = 0; sIdx < 8; ++sIdx) {
                itSplat[sIdx] = prim.attributes.find("_SPLAT_TEX_" + std::to_string(sIdx));
                if (itSplat[sIdx] != prim.attributes.end()) hasSplat = true;
            }
            // Self-lit glow (new, optional): a flat 0..1 strength, no texture
            // lookup. Absent on every GLB baked before this feature.
            auto itEmissive = prim.attributes.find("_EMISSIVE_STRENGTH");

            if (itPos == prim.attributes.end()) continue;

            std::vector<Vec3> positions = readVec3Accessor(gltf, itPos->second);
            std::vector<Vec3> normals = (itNrm != prim.attributes.end())
                                            ? readVec3Accessor(gltf, itNrm->second)
                                            : std::vector<Vec3>(positions.size(), Vec3(0.0f, 1.0f, 0.0f));
            std::vector<Vec2> uvs = (itUv != prim.attributes.end())
                                        ? readVec2Accessor(gltf, itUv->second)
                                        : std::vector<Vec2>(positions.size(), Vec2(0.0f));
            std::vector<Vec4> colors = (itCol != prim.attributes.end())
                                           ? readColorAccessor(gltf, itCol->second)
                                           : std::vector<Vec4>(positions.size(), Vec4(1.0f));

            std::vector<uint32_t> blendTex = (itBlendTex != prim.attributes.end())
                                           ? readIndexAccessor(gltf, itBlendTex->second)
                                           : std::vector<uint32_t>(positions.size(), 0);
                                           
            std::vector<uint32_t> blendPbr = (itBlendPbr != prim.attributes.end())
                                           ? readIndexAccessor(gltf, itBlendPbr->second)
                                           : std::vector<uint32_t>(positions.size(), 0);
                                           
            std::vector<uint32_t> blendNorm = (itBlendNormal != prim.attributes.end())
                                           ? readIndexAccessor(gltf, itBlendNormal->second)
                                           : std::vector<uint32_t>(positions.size(), 0);
                                           
            std::vector<float> blendWeight = (itBlendWeight != prim.attributes.end())
                                           ? readFloatAccessor(gltf, itBlendWeight->second)
                                           : std::vector<float>(positions.size(), 0.0f);

            std::vector<uint32_t> blendMaskTex = (itBlendMaskTex != prim.attributes.end())
                                           ? readIndexAccessor(gltf, itBlendMaskTex->second)
                                           : std::vector<uint32_t>(positions.size(), 0);

            std::vector<uint32_t> blendMaskTex2 = (itBlendMaskTex2 != prim.attributes.end())
                                           ? readIndexAccessor(gltf, itBlendMaskTex2->second)
                                           : std::vector<uint32_t>(positions.size(), 0);

            std::vector<Vec2> blendMaskUV = (itBlendMaskUV != prim.attributes.end())
                                           ? readVec2Accessor(gltf, itBlendMaskUV->second)
                                           : std::vector<Vec2>(positions.size(), Vec2(0.0f));

            std::vector<float> emissive = (itEmissive != prim.attributes.end())
                                           ? readFloatAccessor(gltf, itEmissive->second)
                                           : std::vector<float>(positions.size(), 0.0f);

            std::vector<uint32_t> splatTex[8];
            for (int sIdx = 0; sIdx < 8; ++sIdx) {
                splatTex[sIdx] = (itSplat[sIdx] != prim.attributes.end())
                               ? readIndexAccessor(gltf, itSplat[sIdx]->second)
                               : std::vector<uint32_t>(positions.size(), 0);
                if (splatTex[sIdx].size() < positions.size())
                    splatTex[sIdx].resize(positions.size(), 0);
            }

            if (normals.size() < positions.size()) normals.resize(positions.size(), Vec3(0.0f, 1.0f, 0.0f));
            if (uvs.size() < positions.size()) uvs.resize(positions.size(), Vec2(0.0f));
            if (colors.size() < positions.size()) colors.resize(positions.size(), Vec4(1.0f));
            if (blendTex.size() < positions.size()) blendTex.resize(positions.size(), 0);
            if (blendPbr.size() < positions.size()) blendPbr.resize(positions.size(), 0);
            if (blendNorm.size() < positions.size()) blendNorm.resize(positions.size(), 0);
            if (blendWeight.size() < positions.size()) blendWeight.resize(positions.size(), 0.0f);
            if (blendMaskTex.size() < positions.size()) blendMaskTex.resize(positions.size(), 0);
            if (blendMaskTex2.size() < positions.size()) blendMaskTex2.resize(positions.size(), 0);
            if (blendMaskUV.size() < positions.size()) blendMaskUV.resize(positions.size(), Vec2(0.0f));
            if (emissive.size() < positions.size()) emissive.resize(positions.size(), 0.0f);

            // Recompute flat normals if missing.
            if (itNrm == prim.attributes.end() && !positions.empty()) {
                std::vector<Vec3> faceNormals(positions.size(), Vec3(0.0f));
                std::vector<int> counts(positions.size(), 0);
                for (size_t i = 0; i + 2 < positions.size(); i += 3) {
                    Vec3 v0 = positions[i];
                    Vec3 v1 = positions[i + 1];
                    Vec3 v2 = positions[i + 2];
                    Vec3 n = glm::cross(v1 - v0, v2 - v0);
                    float len = glm::length(n);
                    if (len > 1e-5f) n /= len;
                    else n = Vec3(0.0f, 1.0f, 0.0f);
                    for (int j = 0; j < 3; ++j) {
                        faceNormals[i + j] += n;
                        counts[i + j]++;
                    }
                }
                for (size_t i = 0; i < positions.size(); ++i) {
                    if (counts[i] > 0) {
                        normals[i] = glm::normalize(faceNormals[i] / static_cast<float>(counts[i]));
                    }
                }
            }

            std::vector<uint32_t> indices = readIndexAccessor(gltf, prim.indices);
            bool usePrimitiveIndices = prim.indices >= 0 && !indices.empty();
            uint32_t indexCount = usePrimitiveIndices ? static_cast<uint32_t>(indices.size())
                                                       : static_cast<uint32_t>(positions.size());

            uint16_t texLocal = primitiveTexLocal[p];

            node.vertices.reserve(node.vertices.size() + indexCount);
            node.texCoords.reserve(node.texCoords.size() + indexCount);
            node.normals.reserve(node.normals.size() + indexCount);
            node.colors.reserve(node.colors.size() + indexCount);
            node.indices.reserve(node.indices.size() + indexCount);
            node.perVertexTexIds.reserve(node.perVertexTexIds.size() + indexCount);

            for (uint32_t i = 0; i < indexCount; ++i) {
                uint32_t srcIdx = usePrimitiveIndices ? indices[i] : i;
                if (srcIdx >= positions.size()) continue;

                node.vertices.push_back(positions[srcIdx]);
                Vec2 uv = uvs[srcIdx];
                uv.y = 1.0f - uv.y; // glTF V=0 bottom -> engine V=0 top
                node.texCoords.push_back(uv);
                node.normals.push_back(normals[srcIdx]);
                node.colors.push_back(vec4ToBgra(colors[srcIdx]));
                node.indices.push_back(static_cast<uint32_t>(node.indices.size()));
                node.perVertexTexIds.push_back(texLocal);

                // Per-vertex crossfade authored in the GLB.
                if (itBlendWeight != prim.attributes.end()) {
                    float w = (srcIdx < blendWeight.size()) ? blendWeight[srcIdx] : 0.0f;
                    uint16_t bLocal = 0;
                    if (w > 0.0f) {
                        uint32_t bMat = (srcIdx < blendTex.size()) ? blendTex[srcIdx] : 0u;
                        bLocal = blendMaterialLocalSlot(bMat);
                        if (bLocal == 0) w = 0.0f; // unresolved target -> no blend
                    }
                    node.perVertexBlendTexIds.push_back(bLocal);
                    node.perVertexBlendWeights.push_back(w);
                }

                // World-space splat mask authored in the GLB.
                if (itBlendMaskUV != prim.attributes.end()) {
                    uint16_t mLocal = 0;
                    if (itBlendMaskTex != prim.attributes.end()) {
                        uint32_t mMat = (srcIdx < blendMaskTex.size()) ? blendMaskTex[srcIdx] : 0u;
                        mLocal = blendMaterialLocalSlot(mMat);
                    }
                    node.perVertexBlendMaskTexIds.push_back(mLocal);
                    node.perVertexBlendMaskUV.push_back(
                        mLocal != 0 && srcIdx < blendMaskUV.size() ? blendMaskUV[srcIdx] : Vec2(0.0f));

                    // Segunda mascara: mesma UV, indice de material proprio.
                    uint16_t mLocal2 = 0;
                    if (itBlendMaskTex2 != prim.attributes.end()) {
                        uint32_t mMat2 = (srcIdx < blendMaskTex2.size()) ? blendMaskTex2[srcIdx] : 0u;
                        mLocal2 = blendMaterialLocalSlot(mMat2);
                    }
                    node.perVertexBlendMaskTexIds2.push_back(mLocal2);
                }

                if (hasSplat) {
                    for (int sIdx = 0; sIdx < 8; ++sIdx) {
                        uint32_t sMat = splatTex[sIdx][srcIdx];
                        node.perVertexSplatTexIds[sIdx].push_back(
                            sMat != 0 ? blendMaterialLocalSlot(sMat) : 0);
                    }
                }

                if (itEmissive != prim.attributes.end()) {
                    node.perVertexEmissive.push_back(
                        srcIdx < emissive.size() ? emissive[srcIdx] : 0.0f);
                }
            }
        }

        // Keep the crossfade arrays parallel to the vertex list even when only
        // some primitives carried _BLEND_WEIGHT.
        if (!node.perVertexBlendWeights.empty() &&
            node.perVertexBlendWeights.size() != node.vertices.size()) {
            node.perVertexBlendTexIds.resize(node.vertices.size(), 0);
            node.perVertexBlendWeights.resize(node.vertices.size(), 0.0f);
        }
        // Same for the splat-mask arrays, only some primitives may carry _BLEND_MASK_UV.
        if (!node.perVertexBlendMaskUV.empty() &&
            node.perVertexBlendMaskUV.size() != node.vertices.size()) {
            node.perVertexBlendMaskTexIds.resize(node.vertices.size(), 0);
            node.perVertexBlendMaskTexIds2.resize(node.vertices.size(), 0);
            node.perVertexBlendMaskUV.resize(node.vertices.size(), Vec2(0.0f));
        }
        // Idem pro splat: so' as primitivas de terreno carregam _SPLAT_TEX_n.
        for (int sIdx = 0; sIdx < 8; ++sIdx) {
            if (!node.perVertexSplatTexIds[sIdx].empty() &&
                node.perVertexSplatTexIds[sIdx].size() != node.vertices.size()) {
                node.perVertexSplatTexIds[sIdx].resize(node.vertices.size(), 0);
            }
        }
        // Same for emissive, only some primitives may carry _EMISSIVE_STRENGTH.
        if (!node.perVertexEmissive.empty() &&
            node.perVertexEmissive.size() != node.vertices.size()) {
            node.perVertexEmissive.resize(node.vertices.size(), 0.0f);
        }

        // Normais suaves por ângulo antes do chanfro: mata o facetado dos
        // quads e o moiré que ele causava na sombra.
        smoothNormalsByAngle(node);

        // Soften razor silhouettes with a load-time chamfer on boundary edges.
        addSilhouetteBevels(node);

        // Compute local bounding box.
        node.box = BoundingBox();
        for (const auto& v : node.vertices) {
            node.box.min = glm::min(node.box.min, v);
            node.box.max = glm::max(node.box.max, v);
        }

        // DEDUP POR CONTEUDO. O compartilhamento acima e' por INDICE de mesh
        // do glTF: so' dispara quando dois nos referenciam o MESMO objeto
        // mesh. Mapa convertido de acervo legado nao e' assim - o conversor emite uma
        // mesh (e accessors, e bytes) POR POSICIONAMENTO. Medido em
        // um mapa de cidade: 2536 meshes, accessors 100% distintos, mas apenas
        // 118 GEOMETRIAS unicas por hash de conteudo - 2418 duplicatas
        // byte-a-byte (95,3%). Sem isto, a mesma prop de 48 vertices sobe
        // 778 vezes para a VRAM e paga 778 draw calls.
        //
        // Hash rapido (FNV-1a sobre posicoes+indices+UV) so' para achar
        // CANDIDATOS; a igualdade e' confirmada por comparacao exata, entao
        // colisao nao funde geometria diferente.
        if (!kGeomShareOff && !node.vertices.empty() && node.geometryShareIndex < 0) {
            uint64_t h = 1469598103934665603ull;
            auto mix = [&h](const void* data, size_t bytes) {
                const uint8_t* p = static_cast<const uint8_t*>(data);
                for (size_t i = 0; i < bytes; ++i) { h ^= p[i]; h *= 1099511628211ull; }
            };
            mix(node.vertices.data(), node.vertices.size() * sizeof(Vec3));
            if (!node.indices.empty())
                mix(node.indices.data(), node.indices.size() * sizeof(uint32_t));
            if (!node.texCoords.empty())
                mix(node.texCoords.data(), node.texCoords.size() * sizeof(Vec2));
            if (!node.perVertexTexIds.empty())
                mix(node.perVertexTexIds.data(), node.perVertexTexIds.size() * sizeof(uint16_t));

            auto range = contentOwners.equal_range(h);
            for (auto it = range.first; it != range.second; ++it) {
                const ModelFileNode& cand = model.nodes[it->second];
                if (cand.vertices.size() != node.vertices.size()) continue;
                if (cand.indices.size() != node.indices.size()) continue;
                if (cand.texCoords.size() != node.texCoords.size()) continue;
                if (cand.perVertexTexIds.size() != node.perVertexTexIds.size()) continue;
                if (cand.normals.size() != node.normals.size()) continue;
                if (cand.textureNames != node.textureNames) continue; // material conta
                if (std::memcmp(cand.vertices.data(), node.vertices.data(),
                                node.vertices.size() * sizeof(Vec3)) != 0) continue;
                if (!node.indices.empty() &&
                    std::memcmp(cand.indices.data(), node.indices.data(),
                                node.indices.size() * sizeof(uint32_t)) != 0) continue;
                if (!node.texCoords.empty() &&
                    std::memcmp(cand.texCoords.data(), node.texCoords.data(),
                                node.texCoords.size() * sizeof(Vec2)) != 0) continue;
                if (!node.perVertexTexIds.empty() &&
                    std::memcmp(cand.perVertexTexIds.data(), node.perVertexTexIds.data(),
                                node.perVertexTexIds.size() * sizeof(uint16_t)) != 0) continue;
                if (!node.normals.empty() &&
                    std::memcmp(cand.normals.data(), node.normals.data(),
                                node.normals.size() * sizeof(Vec3)) != 0) continue;
                // Atributos opcionais: so' funde quando AMBOS nao os tem, para
                // nao perder blend/splat por vertice numa fusao silenciosa.
                if (!node.perVertexBlendTexIds.empty() || !cand.perVertexBlendTexIds.empty()) continue;
                if (!node.faces.empty() || !cand.faces.empty()) continue;
                // Identico: vira instancia do dono e devolve a memoria.
                node.geometryShareIndex = it->second;
                node.box = cand.box;
                node.vertices.clear();  node.vertices.shrink_to_fit();
                node.indices.clear();   node.indices.shrink_to_fit();
                node.texCoords.clear(); node.texCoords.shrink_to_fit();
                node.normals.clear();   node.normals.shrink_to_fit();
                node.colors.clear();    node.colors.shrink_to_fit();
                node.perVertexTexIds.clear(); node.perVertexTexIds.shrink_to_fit();
                ++dedupHits;
                break;
            }
            if (node.geometryShareIndex < 0)
                contentOwners.emplace(h, m.modelNodeIndex);
        }
    }
    if (dedupHits > 0) {
        ERUPTION_LOG_WARN("GltfParser: dedup por conteudo fundiu %zu nos (de %zu) "
                          "em geometria compartilhada", dedupHits, model.nodes.size());
    }

    // Animation (bonus): convert glTF animation channels to per-node keyframes.
    if (!gltf.animations.empty()) {
        model.animation = ModelAnimation{};
        model.animation->frameRate = 30.0f;
        float maxTime = 0.0f;

        for (const tinygltf::Animation& anim : gltf.animations) {
            for (const tinygltf::AnimationChannel& channel : anim.channels) {
                int targetNode = channel.target_node;
                if (targetNode < 0 || targetNode >= static_cast<int>(gltf.nodes.size())) continue;
                int modelIdx = modelNodeIndexForGltfNode(mappings, targetNode);
                if (modelIdx < 0) continue;
                ModelFileNode& node = model.nodes[modelIdx];

                const tinygltf::AnimationSampler& sampler = anim.samplers[channel.sampler];
                std::vector<float> times = readAccessor<float>(gltf, sampler.input);
                if (times.empty()) continue;

                if (channel.target_path == "scale") {
                    std::vector<Vec3> values = readAccessor<Vec3>(gltf, sampler.output);
                    for (size_t i = 0; i < times.size() && i < values.size(); ++i) {
                        node.scaleKeyframes.push_back({static_cast<int32_t>(times[i] * 30.0f), values[i], 0.0f});
                        maxTime = glm::max(maxTime, times[i]);
                    }
                } else if (channel.target_path == "rotation") {
                    std::vector<glm::quat> values = readAccessor<glm::quat>(gltf, sampler.output);
                    for (size_t i = 0; i < times.size() && i < values.size(); ++i) {
                        node.rotKeyframes.push_back({static_cast<int32_t>(times[i] * 30.0f), values[i]});
                        maxTime = glm::max(maxTime, times[i]);
                    }
                } else if (channel.target_path == "translation") {
                    std::vector<Vec3> values = readAccessor<Vec3>(gltf, sampler.output);
                    for (size_t i = 0; i < times.size() && i < values.size(); ++i) {
                        node.posKeyframes.push_back({static_cast<int32_t>(times[i] * 30.0f), values[i], 0});
                        maxTime = glm::max(maxTime, times[i]);
                    }
                }
            }
        }

        model.animation->duration = maxTime * 30.0f;
    }

    model.calculateBoundingBox();

    return model;
}

} // namespace eruption
