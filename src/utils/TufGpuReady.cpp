#include "utils/TufGpuReady.hpp"

#include "core/JobSystem.hpp"
#include "core/Logger.hpp"
#include "formats/GltfParser.hpp"
#include "utils/BcCompressor.hpp"
#include "utils/EmbeddedTextureBake.hpp"
#include "utils/ImageUtils.hpp"
#include "utils/TextureFormatPolicy.hpp"

#include "tufo/Ops.hpp"

#include <algorithm>
#include <cctype>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <unordered_map>

namespace eruption {

namespace fs = std::filesystem;

namespace {

std::vector<TextureVariant> g_variants;

bool endsWith(const std::string& s, const char* suffix) {
    const size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// Caminho virtual de uma uri relativa ao .glb (`dir` com a barra final), com %xx decodificado.
std::string uriKey(const std::string& dir, const std::string& uri) {
    std::string d;
    for (size_t i = 0; i < uri.size(); ++i) {
        if (uri[i] == '%' && i + 2 < uri.size() && std::isxdigit(static_cast<unsigned char>(uri[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(uri[i + 2]))) {
            d.push_back(static_cast<char>(std::stoi(uri.substr(i + 1, 2), nullptr, 16)));
            i += 2;
        } else {
            d.push_back(uri[i]);
        }
    }
    return (fs::path(dir + d)).lexically_normal().generic_string();
}

// Empacotar: so' .glb com imagens embutidas que o assador cobre.
bool packGlb(const std::string& key, const std::string& diskPath, const std::vector<uint8_t>& bytes,
             std::vector<uint8_t>& stored, tufo::GpuReadyCodec::Companions& companions, std::vector<std::string>& skip,
             std::string* err) {
    if (!endsWith(key, ".glb") || bytes.size() < 28 || std::memcmp(bytes.data(), "glTF", 4) != 0) return false;
    const fs::path disk = fs::absolute(diskPath);
    ModelFile model = GltfParser::parse(bytes, disk.parent_path().string());
    if (model.embeddedTextures.empty()) return false;
    model.filePath = disk.string(); // o assador procura o pacote em disco ao lado dele

    const auto t0 = std::chrono::steady_clock::now();
    // Uma companheira por combinacao (teto, sintese, formatos) de preset: o .glb
    // guardado nao tem mais as imagens, entao todo preset tem de achar a sua.
    std::unordered_map<std::string, int> coverage;
    size_t built = 0, reused = 0;
    for (const TextureVariant& v : g_variants) {
        texfmt::setOptions(v.options);
        std::vector<uint8_t> pack;
        std::string packPath;
        std::vector<std::string> names;
        bool fromDisk = false;
        if (!buildEmbeddedBakePack(model, v.maxSize, v.synthPbr, pack, packPath, &names, &fromDisk)) {
            if (err) *err = "nao foi possivel assar as texturas embutidas para o preset " + v.preset;
            return false;
        }
        for (const std::string& n : names) ++coverage[n];
        // O sufixo que o motor procura: o caminho do pacote menos o caminho do .glb.
        companions.emplace_back(key + packPath.substr(disk.string().size()), std::move(pack));
        (fromDisk ? reused : built)++;
    }
    // So' sai do .glb a imagem que TODA combinacao cobre.
    stored = bytes;
    const auto everyVariant = [&](const std::string& n) {
        const auto it = coverage.find(n);
        return it != coverage.end() && it->second == static_cast<int>(g_variants.size());
    };
    const size_t saved = GltfParser::stripEmbeddedImages(stored, disk.filename().string(), everyVariant);
    // Imagens em arquivo (uri): o pacote as cobre em formato de bloco, entao o arquivo nao entra no .tuf
    // (o .glb guardado as marca e o motor aceita a falta). O dono quer so' BC no pacote.
    std::vector<std::string> uris;
    const size_t marked = GltfParser::markBcOnlyUriImages(stored, everyVariant, &uris);
    const size_t slash = key.find_last_of('/');
    const std::string dir = slash == std::string::npos ? std::string() : key.substr(0, slash + 1);
    for (const std::string& u : uris) skip.push_back(uriKey(dir, u));
    if (saved == 0 && marked == 0) stored = bytes;
    ERUPTION_LOG_WARN("[TUFGPU] %s: %zu pacotes de texturas (%zu assados, %zu do disco), %.1f MB de imagens saem do .glb, %zu imagens uri nao entram, em %.0f ms",
                      key.c_str(), companions.size(), built, reused, double(saved) / 1048576.0, marked,
                      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    return true;
}

// Entrada companheira do .glb: <chave>.<teto><s|n>[.<fmt>].etexpack.
bool findCompanion(const tufo::Archive& ar, const std::string& key, std::string& found) {
    const size_t slash = key.find_last_of('/');
    const std::string dir = slash == std::string::npos ? std::string() : key.substr(0, slash);
    const std::string stem = key + ".";
    for (const std::string& k : ar.list(dir)) {
        if (k.size() <= stem.size() + 9 || k.compare(0, stem.size(), stem) != 0 || !endsWith(k, ".etexpack")) continue;
        tufo::Info ti;
        if (ar.stat(k, &ti) && (ti.flags & tufo::kFlagCompanion)) {
            found = k;
            return true;
        }
    }
    return false;
}

std::vector<uint8_t> albedoPng(const BakedEmbeddedTexture& t) {
    std::vector<uint8_t> rgba, png;
    if (!decodeEtexMip0(t.albedo, rgba) || t.albedo.width == 0 || t.albedo.height == 0) return png;
    ImageUtils::encodePNG(static_cast<int>(t.albedo.width), static_cast<int>(t.albedo.height), 4, rgba.data(), png);
    return png;
}

bool unpackGlb(const std::string& key, const std::vector<uint8_t>& stored, const tufo::Archive& ar,
               std::vector<uint8_t>& out, std::string* err) {
    out = stored;
    std::string companion;
    if (!findCompanion(ar, key, companion)) {
        if (err) *err = key + ": falta o pacote de texturas (<arquivo>.*.etexpack) no .tuf";
        return false;
    }
    std::vector<uint8_t> packBytes;
    if (!ar.read(companion, packBytes, true, err)) return false;
    EmbeddedBakeResult res;
    if (!parseEmbeddedBakePackBytes(packBytes, res)) {
        if (err) *err = companion + ": pacote de texturas ilegivel (versao do formato?)";
        return false;
    }
    const size_t n = GltfParser::restoreEmbeddedImages(out, [&res](const std::string& name) {
        const auto it = res.textures.find(name);
        return it == res.textures.end() ? std::vector<uint8_t>() : albedoPng(it->second);
    });
    ERUPTION_LOG_WARN("[TUFGPU] %s: %zu imagens remontadas do formato de bloco (com perda)", key.c_str(), n);
    return true;
}

// Imagens por uri que o .tuf nao guarda: remontadas do BC de uma das combinacoes (com perda).
bool unpackExtras(const std::string& key, const std::vector<uint8_t>& stored, const tufo::Archive& ar,
                  std::vector<std::pair<std::string, std::vector<uint8_t>>>& files, std::string* err) {
    const auto uris = GltfParser::bcOnlyUriImages(stored);
    if (uris.empty()) return true;
    std::string companion;
    if (!findCompanion(ar, key, companion)) return true;
    std::vector<uint8_t> packBytes;
    if (!ar.read(companion, packBytes, true, err)) return false;
    EmbeddedBakeResult res;
    if (!parseEmbeddedBakePackBytes(packBytes, res)) return true;
    const size_t slash = key.find_last_of('/');
    const std::string dir = slash == std::string::npos ? std::string() : key.substr(0, slash + 1);
    for (const auto& u : uris) {
        const auto it = res.textures.find(u.second);
        if (it == res.textures.end()) continue;
        std::vector<uint8_t> png = albedoPng(it->second);
        if (!png.empty()) files.emplace_back(uriKey(dir, u.first), std::move(png));
    }
    return true;
}

} // namespace

bool setupOfflineTextureBake() {
    bcCompressorInit();
    BcFormatSupport s;
    s.bc1 = s.bc3 = s.bc5 = s.bc7 = true;
    setBcFormatSupport(s);
    texfmt::setDeviceCaps(texfmt::parseCaps("bc"));
    g_variants = engineTextureVariants();
    // ERUPTION_TUF_VARIANTS="1024s,512n": so' estas combinacoes (teto + s/n de sintese de PBR) entram
    // no .tuf; vale para a migracao dos mapas (o resto dos presets comprime ao carregar).
    if (const char* only = std::getenv("ERUPTION_TUF_VARIANTS")) {
        const std::string list = std::string(",") + only + ",";
        std::vector<TextureVariant> kept;
        for (TextureVariant& v : g_variants)
            if (list.find("," + std::to_string(v.maxSize) + (v.synthPbr ? "s" : "n") + ",") != std::string::npos) kept.push_back(std::move(v));
        g_variants.swap(kept);
    }
    JobSystem::instance().init();
    for (const TextureVariant& v : g_variants)
        ERUPTION_LOG_WARN("[TUFGPU] combinacao: teto %u, sintese %s, presets %s", v.maxSize, v.synthPbr ? "s" : "n", v.preset.c_str());
    return !g_variants.empty() && texfmt::anyActive();
}

bool tufGpuCompanionKey(const tufo::Archive& ar, const std::string& key, std::string& companionKey) {
    return findCompanion(ar, key, companionKey);
}

bool decodeTufGpuImages(const std::vector<uint8_t>& companionBytes, std::vector<TufDecodedImage>& out) {
    EmbeddedBakeResult res;
    if (!parseEmbeddedBakePackBytes(companionBytes, res)) return false;
    for (const auto& kv : res.textures) {
        TufDecodedImage d;
        d.name = kv.first;
        if (!decodeEtexMip0(kv.second.albedo, d.rgba)) continue;
        d.width = static_cast<int>(kv.second.albedo.width);
        d.height = static_cast<int>(kv.second.albedo.height);
        out.push_back(std::move(d));
    }
    std::sort(out.begin(), out.end(), [](const TufDecodedImage& a, const TufDecodedImage& b) { return a.name < b.name; });
    return !out.empty();
}

void registerTufGpuReadyCodec() {
    tufo::GpuReadyCodec c;
    c.pack = packGlb;
    c.unpack = unpackGlb;
    c.unpackExtras = unpackExtras;
    tufo::setGpuReadyCodec(std::move(c));
}

} // namespace eruption
