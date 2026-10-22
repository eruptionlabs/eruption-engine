// Relevo empacotado (formats/GroundPack): .glb + .ter do chao -> .relevo -> de volta.
//
// O que se verifica, e por que cada um importa:
//   1. a volta reproduz o .ter byte a byte e o .glb igual  — o motor le' os
//                                                           mesmos arquivos de
//                                                           antes
//   2. a colisao que difere da malha sobrevive              — a fenda que se ve'
//                                                           e por onde se anda
//   3. .glb que nao e' a grade do chao e' recusado          — o mapa continua
//                                                           indo como esta', sem
//                                                           .relevo errado
//   4. .relevo truncado da' erro, nao lixo                  — arquivo cortado no
//                                                           download
//
//   g++ -std=c++17 -I src -I <nlohmann>/include tests/test_ground_pack.cpp src/formats/GroundPack.cpp
#include "formats/GroundPack.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace eruption;
namespace fs = std::filesystem;

static int g_falhas = 0;

static void checa(bool ok, const char* titulo) {
    std::printf("  [%s] %s\n", ok ? "ok" : "FALHA", titulo);
    if (!ok) ++g_falhas;
}

static std::vector<uint8_t> le(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

static void grava(const fs::path& p, const std::vector<uint8_t>& v) {
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(v.data()), static_cast<std::streamsize>(v.size()));
}

template <typename T>
static void poe(std::vector<uint8_t>& o, T v) {
    const auto* p = reinterpret_cast<const uint8_t*>(&v);
    o.insert(o.end(), p, p + sizeof(T));
}

// O par que o gerador de mapas grava, montado a' mao: malha da grade visual
// e cubos da grade de colisao (altura negada, cantos presos na borda).
static void escreveChao(const fs::path& glb, const fs::path& ter, const std::vector<float>& vis,
                        const std::vector<float>& col, int w, int d, float cell) {
    std::vector<float> pos, nrm, uv;
    auto h = [&](int x, int z) { return vis[size_t(z) * w + x]; };
    for (int z = 0; z < d; ++z)
        for (int x = 0; x < w; ++x) {
            pos.insert(pos.end(), {float(x) * cell, h(x, z), float(z) * cell});
            const int xl = x > 0 ? x - 1 : 0, xr = x + 1 < w ? x + 1 : w - 1;
            const int zd = z > 0 ? z - 1 : 0, zu = z + 1 < d ? z + 1 : d - 1;
            const float dx = (h(xr, z) - h(xl, z)) / (float(xr - xl) * cell);
            const float dz = (h(x, zu) - h(x, zd)) / (float(zu - zd) * cell);
            const float inv = 1.0f / std::sqrt(dx * dx + 1.0f + dz * dz);
            nrm.insert(nrm.end(), {-dx * inv, inv, -dz * inv});
            uv.insert(uv.end(), {float(x) * cell / 32.0f, float(z) * cell / 32.0f});
        }
    std::vector<uint32_t> idx;
    for (int z = 0; z + 1 < d; ++z)
        for (int x = 0; x + 1 < w; ++x) {
            const uint32_t a = uint32_t(z * w + x), b = a + 1, c = a + uint32_t(w), e = c + 1;
            idx.insert(idx.end(), {a, c, b, b, c, e});
        }
    std::vector<uint8_t> bin;
    auto junta = [&](const auto& v) {
        const size_t at = bin.size();
        const auto* p = reinterpret_cast<const uint8_t*>(v.data());
        bin.insert(bin.end(), p, p + v.size() * sizeof(v[0]));
        return at;
    };
    const size_t oP = junta(pos), oN = junta(nrm), oU = junta(uv), oI = junta(idx);
    using nlohmann::json;
    const size_t nv = size_t(w) * d;
    json j;
    j["asset"] = {{"version", "2.0"}};
    j["meshes"] = json::array({{{"name", "Ground"}, {"primitives", json::array({{{"attributes", {{"POSITION", 0}, {"NORMAL", 1}, {"TEXCOORD_0", 2}}}, {"indices", 3}}})}}});
    j["buffers"] = json::array({{{"byteLength", bin.size()}}});
    j["bufferViews"] = json::array({{{"buffer", 0}, {"byteOffset", oP}, {"byteLength", pos.size() * 4}},
                                    {{"buffer", 0}, {"byteOffset", oN}, {"byteLength", nrm.size() * 4}},
                                    {{"buffer", 0}, {"byteOffset", oU}, {"byteLength", uv.size() * 4}},
                                    {{"buffer", 0}, {"byteOffset", oI}, {"byteLength", idx.size() * 4}}});
    j["accessors"] = json::array({{{"bufferView", 0}, {"componentType", 5126}, {"count", nv}, {"type", "VEC3"}},
                                  {{"bufferView", 1}, {"componentType", 5126}, {"count", nv}, {"type", "VEC3"}},
                                  {{"bufferView", 2}, {"componentType", 5126}, {"count", nv}, {"type", "VEC2"}},
                                  {{"bufferView", 3}, {"componentType", 5125}, {"count", idx.size()}, {"type", "SCALAR"}}});
    std::string texto = j.dump();
    while (texto.size() % 4) texto.push_back(' ');
    std::vector<uint8_t> g;
    poe(g, 0x46546C67u);
    poe(g, 2u);
    poe(g, uint32_t(12 + 8 + texto.size() + 8 + bin.size()));
    poe(g, uint32_t(texto.size()));
    poe(g, 0x4E4F534Au);
    g.insert(g.end(), texto.begin(), texto.end());
    poe(g, uint32_t(bin.size()));
    poe(g, 0x004E4942u);
    g.insert(g.end(), bin.begin(), bin.end());
    grava(glb, g);

    std::vector<uint8_t> t;
    t.insert(t.end(), {'E', 'R', 'U', 'P', 'T', 'G', 'N', 'D', 'B', 'I', 'N'});
    poe(t, 2u);
    poe(t, uint32_t(w));
    poe(t, uint32_t(d));
    poe(t, cell);
    poe(t, 0.0f);
    poe(t, 0.0f);
    poe(t, 0u); // texturas
    poe(t, 0u); // superficies
    poe(t, uint32_t(nv));
    auto c = [&](int x, int z) { return -col[size_t(z < d ? z : d - 1) * w + (x < w ? x : w - 1)]; };
    for (int z = 0; z < d; ++z)
        for (int x = 0; x < w; ++x) {
            for (float v : {c(x, z), c(x + 1, z), c(x, z + 1), c(x + 1, z + 1)}) poe(t, v);
            for (int k = 0; k < 3; ++k) poe(t, int32_t(-1));
        }
    poe(t, 0u); // lightmaps
    poe(t, 0u); // agua
    grava(ter, t);
}

int main() {
    std::printf("=== relevo empacotado ===\n");
    const fs::path dir = fs::temp_directory_path() / "eruption_test_ground_pack";
    fs::create_directories(dir);
    const int w = 9, d = 7;
    const float cell = 4.0f;
    std::vector<float> vis(size_t(w) * d), col;
    for (int z = 0; z < d; ++z)
        for (int x = 0; x < w; ++x) vis[size_t(z) * w + x] = std::sin(0.7f * x) * 20.0f + float(z * z) * 1.5f - 12.0f;
    // Uma fenda de duas casas: funda na malha, fechada na colisao.
    vis[size_t(3) * w + 4] -= 90.0f;
    vis[size_t(3) * w + 5] -= 90.0f;
    col = vis;
    col[size_t(3) * w + 4] += 90.0f;
    col[size_t(3) * w + 5] += 90.0f;
    escreveChao(dir / "a.glb", dir / "a.ter", vis, col, w, d, cell);

    std::string erro;
    checa(packGround(dir / "a.glb", dir / "a.ter", dir / "a.relevo", erro), "empacota o par do gerador");
    if (!erro.empty()) std::printf("    %s\n", erro.c_str());
    checa(unpackGround(dir / "a.relevo", dir / "b.glb", dir / "b.ter", erro), "desempacota");
    checa(le(dir / "a.ter") == le(dir / "b.ter"), "o .ter volta byte a byte (com a fenda fechada)");
    checa(le(dir / "a.glb") == le(dir / "b.glb"), "o .glb volta byte a byte (com a fenda aberta)");
    checa(le(dir / "a.relevo").size() < le(dir / "a.glb").size(), "o .relevo e' menor que o .glb sozinho");

    // UV de outra escala: nao e' a grade do chao, nao empacota.
    std::vector<uint8_t> g = le(dir / "a.glb");
    const uint32_t jsonLen = *reinterpret_cast<const uint32_t*>(g.data() + 12);
    const size_t uvAt = 20 + jsonLen + 8 + size_t(w) * d * 24 + 8; // u do vertice 1
    float u;
    std::memcpy(&u, g.data() + uvAt, 4);
    u += 0.5f;
    std::memcpy(g.data() + uvAt, &u, 4);
    grava(dir / "c.glb", g);
    fs::remove(dir / "c.relevo");
    checa(!packGround(dir / "c.glb", dir / "a.ter", dir / "c.relevo", erro) && !fs::exists(dir / "c.relevo"),
          "malha que nao e' a grade e' recusada, sem gravar nada");

    std::vector<uint8_t> r = le(dir / "a.relevo");
    r.resize(r.size() / 2);
    grava(dir / "d.relevo", r);
    checa(!unpackGround(dir / "d.relevo", dir / "d.glb", dir / "d.ter", erro), ".relevo cortado da' erro");

    fs::remove_all(dir);
    std::printf("%s\n", g_falhas ? "FALHOU" : "tudo certo");
    return g_falhas ? 1 : 0;
}
