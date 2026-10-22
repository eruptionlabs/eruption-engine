#include "formats/GroundPack.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

namespace eruption {

namespace {

namespace fs = std::filesystem;
using nlohmann::json;

constexpr char kMagic[] = "ERUPTRELEVO"; // 11 bytes, como o ERUPTGNDBIN do .ter
constexpr uint32_t kVersion = 1;
constexpr size_t kCubeBytes = 28;        // 4 alturas + 3 superficies
constexpr float kNormalTolerance = 1e-5f;

bool readFile(const fs::path& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

bool writeFile(const fs::path& path, const std::vector<uint8_t>& data) {
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(f);
}

template <typename T>
T load(const uint8_t* p) {
    T v;
    std::memcpy(&v, p, sizeof(T));
    return v;
}

template <typename T>
void put(std::vector<uint8_t>& out, const T& v) {
    const auto* p = reinterpret_cast<const uint8_t*>(&v);
    out.insert(out.end(), p, p + sizeof(T));
}

void putBytes(std::vector<uint8_t>& out, const uint8_t* p, size_t n) {
    put(out, static_cast<uint32_t>(n));
    out.insert(out.end(), p, p + n);
}

// Onde cada atributo da malha "Ground" mora no BIN do .glb (bufferView do
// acessor; o empacotamento exige um so' buffer, sem stride).
struct GroundLayout {
    size_t pos = 0, nrm = 0, uv = 0, idx = 0, binSize = 0;
    size_t vertices = 0, indices = 0;
};

bool groundLayout(const json& doc, GroundLayout& out, std::string& error) {
    try {
        if (doc.at("meshes").size() != 1 || doc.at("meshes")[0].at("primitives").size() != 1 ||
            doc.at("buffers").size() != 1) {
            error = "the glb is not a single ground mesh";
            return false;
        }
        const json& prim = doc["meshes"][0]["primitives"][0];
        const json& attrs = prim.at("attributes");
        if (attrs.size() != 3) {
            error = "the ground mesh has attributes other than POSITION, NORMAL, TEXCOORD_0";
            return false;
        }
        auto offsetOf = [&](int accessor, const char* type, int component, size_t& count) {
            const json& a = doc.at("accessors").at(accessor);
            const json& v = doc.at("bufferViews").at(a.at("bufferView").get<size_t>());
            if (a.at("type") != type || a.at("componentType") != component || v.contains("byteStride") ||
                v.value("buffer", 0) != 0)
                throw std::runtime_error(std::string("unexpected accessor layout for ") + type);
            count = a.at("count").get<size_t>();
            return v.value("byteOffset", size_t(0)) + a.value("byteOffset", size_t(0));
        };
        size_t n = 0, nn = 0, nu = 0;
        out.pos = offsetOf(attrs.at("POSITION").get<int>(), "VEC3", 5126, n);
        out.nrm = offsetOf(attrs.at("NORMAL").get<int>(), "VEC3", 5126, nn);
        out.uv = offsetOf(attrs.at("TEXCOORD_0").get<int>(), "VEC2", 5126, nu);
        out.idx = offsetOf(prim.at("indices").get<int>(), "SCALAR", 5125, out.indices);
        if (nn != n || nu != n) throw std::runtime_error("attribute counts differ");
        out.vertices = n;
        out.binSize = doc["buffers"][0].at("byteLength").get<size_t>();
    } catch (const std::exception& e) {
        error = std::string("glb: ") + e.what();
        return false;
    }
    return true;
}

// A malha que o gerador de mapas grava (plugins/mapgen, writeGroundGlb):
// vertice (x, z) em (x * cell, h, z * cell), normal pela diferenca central,
// UV de 32 unidades e dois triangulos por casa.
void buildGround(const std::vector<float>& h, uint32_t w, uint32_t d, float cell, const GroundLayout& L,
                 std::vector<uint8_t>& bin) {
    bin.assign(L.binSize, 0);
    auto* pos = reinterpret_cast<float*>(bin.data() + L.pos);
    auto* nrm = reinterpret_cast<float*>(bin.data() + L.nrm);
    auto* uv = reinterpret_cast<float*>(bin.data() + L.uv);
    auto* idx = reinterpret_cast<uint32_t*>(bin.data() + L.idx);
    auto at = [&](uint32_t x, uint32_t z) { return h[size_t(z) * w + x]; };
    for (uint32_t z = 0; z < d; ++z)
        for (uint32_t x = 0; x < w; ++x) {
            const size_t v = size_t(z) * w + x;
            const float fx = static_cast<float>(x), fz = static_cast<float>(z);
            pos[v * 3 + 0] = fx * cell;
            pos[v * 3 + 1] = at(x, z);
            pos[v * 3 + 2] = fz * cell;
            const uint32_t xl = x > 0 ? x - 1 : 0, xr = std::min(w - 1, x + 1);
            const uint32_t zd = z > 0 ? z - 1 : 0, zu = std::min(d - 1, z + 1);
            const float dx = (at(xr, z) - at(xl, z)) / (static_cast<float>(xr - xl) * cell);
            const float dz = (at(x, zu) - at(x, zd)) / (static_cast<float>(zu - zd) * cell);
            const float inv = 1.0f / std::sqrt(dx * dx + 1.0f + dz * dz);
            nrm[v * 3 + 0] = -dx * inv;
            nrm[v * 3 + 1] = inv;
            nrm[v * 3 + 2] = -dz * inv;
            uv[v * 2 + 0] = fx * cell / 32.0f;
            uv[v * 2 + 1] = fz * cell / 32.0f;
        }
    size_t k = 0;
    for (uint32_t z = 0; z + 1 < d; ++z)
        for (uint32_t x = 0; x + 1 < w; ++x) {
            const uint32_t a = z * w + x, b = a + 1, c = a + w, e = c + 1;
            for (uint32_t i : {a, c, b, b, c, e}) idx[k++] = i;
        }
}

// Cubo (x, z) do .ter: os quatro cantos da grade de colisao na ordem
// (x, z), (x+1, z), (x, z+1), (x+1, z+1), presos na borda; superficies -1.
void buildCubes(const std::vector<float>& col, uint32_t w, uint32_t d, uint8_t* cubes) {
    auto at = [&](uint32_t x, uint32_t z) { return col[size_t(std::min(z, d - 1)) * w + std::min(x, w - 1)]; };
    for (uint32_t z = 0; z < d; ++z)
        for (uint32_t x = 0; x < w; ++x) {
            uint8_t* c = cubes + (size_t(z) * w + x) * kCubeBytes;
            const float hs[4] = {at(x, z), at(x + 1, z), at(x, z + 1), at(x + 1, z + 1)};
            const int32_t none[3] = {-1, -1, -1};
            std::memcpy(c, hs, sizeof(hs));
            std::memcpy(c + sizeof(hs), none, sizeof(none));
        }
}

struct GlbParts {
    std::vector<uint8_t> json; // chunk JSON, com o preenchimento
    const uint8_t* bin = nullptr;
    size_t binSize = 0;
};

bool splitGlb(const std::vector<uint8_t>& glb, GlbParts& out, std::string& error) {
    if (glb.size() < 28 || load<uint32_t>(glb.data()) != 0x46546C67u) {
        error = "not a glb";
        return false;
    }
    const uint32_t jsonLen = load<uint32_t>(glb.data() + 12);
    if (20 + size_t(jsonLen) + 8 > glb.size() || load<uint32_t>(glb.data() + 16) != 0x4E4F534Au) {
        error = "glb without a JSON chunk";
        return false;
    }
    out.json.assign(glb.begin() + 20, glb.begin() + 20 + jsonLen);
    const size_t binHead = 20 + size_t(jsonLen);
    out.binSize = load<uint32_t>(glb.data() + binHead);
    if (load<uint32_t>(glb.data() + binHead + 4) != 0x004E4942u || binHead + 8 + out.binSize != glb.size()) {
        error = "glb without a single BIN chunk at the end";
        return false;
    }
    out.bin = glb.data() + binHead + 8;
    return true;
}

std::vector<uint8_t> joinGlb(const std::vector<uint8_t>& jsonChunk, const std::vector<uint8_t>& bin) {
    std::vector<uint8_t> glb;
    put(glb, 0x46546C67u);
    put(glb, 2u);
    put(glb, static_cast<uint32_t>(12 + 8 + jsonChunk.size() + 8 + bin.size()));
    put(glb, static_cast<uint32_t>(jsonChunk.size()));
    put(glb, 0x4E4F534Au);
    glb.insert(glb.end(), jsonChunk.begin(), jsonChunk.end());
    put(glb, static_cast<uint32_t>(bin.size()));
    put(glb, 0x004E4942u);
    glb.insert(glb.end(), bin.begin(), bin.end());
    return glb;
}

// Cabecalho do .ter ate' a contagem de cubos: magic, versao, w, d, escala,
// origem e as listas de texturas e superficies (vazias num mapa gerado).
bool terHeader(const std::vector<uint8_t>& ter, uint32_t& w, uint32_t& d, size_t& cubesAt, std::string& error) {
    if (ter.size() < 43 || std::memcmp(ter.data(), "ERUPTGNDBIN", 11) != 0 || load<uint32_t>(ter.data() + 11) != 2) {
        error = "ter: not an ERUPTGNDBIN version 2 file";
        return false;
    }
    w = load<uint32_t>(ter.data() + 15);
    d = load<uint32_t>(ter.data() + 19);
    size_t o = 35;
    const uint32_t textures = load<uint32_t>(ter.data() + o);
    o += 4;
    for (uint32_t i = 0; i < textures && o + 2 <= ter.size(); ++i) o += 2 + load<uint16_t>(ter.data() + o);
    if (o + 4 > ter.size()) {
        error = "ter: truncated header";
        return false;
    }
    o += 4 + size_t(load<uint32_t>(ter.data() + o)) * 40;
    if (o + 4 > ter.size() || load<uint32_t>(ter.data() + o) != size_t(w) * d ||
        o + 4 + size_t(w) * d * kCubeBytes > ter.size()) {
        error = "ter: cube count does not match the grid";
        return false;
    }
    cubesAt = o + 4;
    return true;
}

} // namespace

bool packGround(const fs::path& glbPath, const fs::path& terPath, const fs::path& out, std::string& error) {
    std::vector<uint8_t> glb, ter;
    if (!readFile(glbPath, glb) || !readFile(terPath, ter)) {
        error = "could not read " + glbPath.string() + " / " + terPath.string();
        return false;
    }
    GlbParts parts;
    if (!splitGlb(glb, parts, error)) return false;
    const json doc = json::parse(parts.json.begin(), parts.json.end(), nullptr, false);
    GroundLayout L;
    if (doc.is_discarded()) {
        error = "glb: invalid JSON";
        return false;
    }
    if (!groundLayout(doc, L, error)) return false;
    uint32_t w = 0, d = 0;
    size_t cubesAt = 0;
    if (!terHeader(ter, w, d, cubesAt, error)) return false;
    if (L.vertices != size_t(w) * d || L.binSize != parts.binSize) {
        error = "glb and ter grids differ";
        return false;
    }
    const auto* pos = reinterpret_cast<const float*>(parts.bin + L.pos);
    const float cell = w > 1 ? load<float>(parts.bin + L.pos + 12) : 1.0f;
    std::vector<float> visual(L.vertices), collision(L.vertices);
    for (size_t v = 0; v < L.vertices; ++v) visual[v] = pos[v * 3 + 1];
    // A colisao do .ter e' a altura NEGADA (o formato veio de um motor com Y
    // para baixo); o canto 0 de cada cubo e' o ponto da grade.
    for (size_t v = 0; v < L.vertices; ++v) collision[v] = -load<float>(ter.data() + cubesAt + v * kCubeBytes);

    // Prova de volta antes de gravar: o que nao reproduzir fica como esta'.
    std::vector<uint8_t> bin;
    buildGround(visual, w, d, cell, L, bin);
    auto sameRange = [&](size_t at, size_t bytes) { return std::memcmp(bin.data() + at, parts.bin + at, bytes) == 0; };
    if (!sameRange(L.pos, L.vertices * 12) || !sameRange(L.uv, L.vertices * 8) || !sameRange(L.idx, L.indices * 4)) {
        error = "the glb is not the ground grid (position, UV or indices differ)";
        return false;
    }
    const auto* nrmA = reinterpret_cast<const float*>(bin.data() + L.nrm);
    const auto* nrmB = reinterpret_cast<const float*>(parts.bin + L.nrm);
    float worst = 0.0f;
    for (size_t i = 0; i < L.vertices * 3; ++i) worst = std::max(worst, std::abs(nrmA[i] - nrmB[i]));
    if (!(worst <= kNormalTolerance)) {
        error = "glb normals differ from the ground formula by " + std::to_string(worst);
        return false;
    }
    std::vector<float> stored(collision.size());
    std::transform(collision.begin(), collision.end(), stored.begin(), [](float h) { return -h; });
    std::vector<uint8_t> cubes(size_t(w) * d * kCubeBytes);
    buildCubes(stored, w, d, cubes.data());
    if (std::memcmp(cubes.data(), ter.data() + cubesAt, cubes.size()) != 0) {
        error = "the ter is not a single collision grid (corners or surfaces differ)";
        return false;
    }

    // Casas em que a colisao nao e' a altura visual, bit a bit.
    std::vector<uint32_t> diffIndex;
    std::vector<float> diffHeight;
    for (size_t v = 0; v < L.vertices; ++v)
        if (std::memcmp(&visual[v], &collision[v], sizeof(float)) != 0) {
            diffIndex.push_back(static_cast<uint32_t>(v));
            diffHeight.push_back(collision[v]);
        }

    std::vector<uint8_t> pack(kMagic, kMagic + 11);
    put(pack, kVersion);
    put(pack, w);
    put(pack, d);
    put(pack, cell);
    putBytes(pack, parts.json.data(), parts.json.size());
    put(pack, static_cast<uint32_t>(L.binSize));
    putBytes(pack, ter.data(), cubesAt - 4); // cabecalho do .ter (a contagem de cubos sai da grade)
    putBytes(pack, ter.data() + cubesAt + cubes.size(), ter.size() - cubesAt - cubes.size()); // cauda
    pack.insert(pack.end(), reinterpret_cast<const uint8_t*>(visual.data()),
                reinterpret_cast<const uint8_t*>(visual.data() + visual.size()));
    put(pack, static_cast<uint32_t>(diffIndex.size()));
    for (size_t i = 0; i < diffIndex.size(); ++i) {
        put(pack, diffIndex[i]);
        put(pack, diffHeight[i]);
    }
    if (!writeFile(out, pack)) {
        error = "could not write " + out.string();
        return false;
    }
    return true;
}

bool unpackGround(const fs::path& relevo, const fs::path& glbOut, const fs::path& terOut, std::string& error) {
    std::vector<uint8_t> pack;
    if (!readFile(relevo, pack)) {
        error = "could not read " + relevo.string();
        return false;
    }
    size_t o = 0;
    auto need = [&](size_t n) {
        if (o + n > pack.size()) throw std::runtime_error("truncated");
        const uint8_t* p = pack.data() + o;
        o += n;
        return p;
    };
    try {
        if (std::memcmp(need(11), kMagic, 11) != 0 || load<uint32_t>(need(4)) != kVersion)
            throw std::runtime_error("not a version 1 relief pack");
        const uint32_t w = load<uint32_t>(need(4)), d = load<uint32_t>(need(4));
        const float cell = load<float>(need(4));
        if (w < 2 || d < 2 || w > 4096 || d > 4096) throw std::runtime_error("bad grid size");
        const uint32_t jsonLen = load<uint32_t>(need(4));
        const uint8_t* jsonPtr = need(jsonLen);
        const std::vector<uint8_t> jsonChunk(jsonPtr, jsonPtr + jsonLen);
        const uint32_t binSize = load<uint32_t>(need(4));
        const uint32_t headLen = load<uint32_t>(need(4));
        const uint8_t* head = need(headLen);
        const uint32_t tailLen = load<uint32_t>(need(4));
        const uint8_t* tail = need(tailLen);
        const size_t n = size_t(w) * d;
        std::vector<float> visual(n);
        std::memcpy(visual.data(), need(n * sizeof(float)), n * sizeof(float));
        std::vector<float> collision = visual;
        const uint32_t diffs = load<uint32_t>(need(4));
        for (uint32_t i = 0; i < diffs; ++i) {
            const uint32_t v = load<uint32_t>(need(4));
            const float h = load<float>(need(4));
            if (v >= n) throw std::runtime_error("collision cell out of the grid");
            collision[v] = h;
        }

        const json doc = json::parse(jsonChunk.begin(), jsonChunk.end(), nullptr, false);
        GroundLayout L;
        if (doc.is_discarded() || !groundLayout(doc, L, error)) throw std::runtime_error(error.empty() ? "glb JSON" : error);
        if (L.vertices != n || L.binSize != binSize) throw std::runtime_error("glb JSON does not match the grid");
        std::vector<uint8_t> bin;
        buildGround(visual, w, d, cell, L, bin);
        if (!writeFile(glbOut, joinGlb(jsonChunk, bin))) throw std::runtime_error("could not write " + glbOut.string());

        std::vector<uint8_t> ter(head, head + headLen);
        put(ter, static_cast<uint32_t>(n));
        const size_t cubesAt = ter.size();
        ter.resize(cubesAt + n * kCubeBytes);
        for (float& h : collision) h = -h;
        buildCubes(collision, w, d, ter.data() + cubesAt);
        ter.insert(ter.end(), tail, tail + tailLen);
        if (!writeFile(terOut, ter)) throw std::runtime_error("could not write " + terOut.string());
    } catch (const std::exception& e) {
        error = relevo.string() + ": " + e.what();
        return false;
    }
    return true;
}

uint64_t fileFnv64(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return 0;
    uint64_t h = 1469598103934665603ull;
    std::vector<char> chunk(1 << 20);
    while (f) {
        f.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        for (std::streamsize i = 0; i < f.gcount(); ++i) {
            h ^= static_cast<uint8_t>(chunk[static_cast<size_t>(i)]);
            h *= 1099511628211ull;
        }
    }
    return h;
}

std::string groundSourceFor(const std::string& glbPath) {
    std::error_code ec;
    if (glbPath.empty() || fs::exists(glbPath, ec)) return {};
    const fs::path relevo = fs::path(glbPath).replace_extension(".relevo");
    return fs::exists(relevo, ec) ? relevo.string() : std::string();
}

// Versoes antigas do mesmo mapa no cache (<mapa>_<outra chave>) saem: cada
// atualizacao do .relevo deixaria mais 30-60 MB no aparelho para sempre.
static void removeOtherVersions(const fs::path& keep, const std::string& stem) {
    const std::string prefix = stem + "_";
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(keep.parent_path(), ec)) {
        const std::string name = e.path().filename().string();
        if (e.path() == keep || name.size() != prefix.size() + 16 || name.compare(0, prefix.size(), prefix) != 0) continue;
        if (name.find_first_not_of("0123456789abcdef", prefix.size()) != std::string::npos) continue;
        std::error_code rmEc;
        fs::remove_all(e.path(), rmEc);
    }
}

bool unpackGroundCached(const fs::path& relevo, fs::path& glbOut, fs::path& terOut, std::string& error) {
    const uint64_t key = fileFnv64(relevo);
    if (key == 0) {
        error = "could not read " + relevo.string();
        return false;
    }
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(key));
    const std::string stem = relevo.stem().string();
    const fs::path dir = fs::path(".cache/eruption/relevo") / (stem + "_" + hex);
    glbOut = dir / (stem + ".glb");
    terOut = dir / (stem + ".ter");
    std::error_code ec;
    if (fs::exists(glbOut, ec) && fs::exists(terOut, ec)) {
        removeOtherVersions(dir, stem);
        return true;
    }
    fs::create_directories(dir, ec);
    // Grava com outro nome e renomeia: uma carga interrompida no meio nao
    // deixa um .glb pela metade que a proxima aceitaria.
    const fs::path glbTmp = dir / (stem + ".glb.tmp"), terTmp = dir / (stem + ".ter.tmp");
    if (!unpackGround(relevo, glbTmp, terTmp, error)) return false;
    fs::rename(terTmp, terOut, ec);
    if (!ec) fs::rename(glbTmp, glbOut, ec);
    if (ec) {
        error = "could not write " + dir.string() + ": " + ec.message();
        return false;
    }
    removeOtherVersions(dir, stem);
    return true;
}

} // namespace eruption
