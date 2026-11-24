#include "utils/EmbeddedTextureBake.hpp"
#include "utils/AssetVfs.hpp"
#include "utils/ReadWholeFile.hpp"
#include "utils/TextureFormatPolicy.hpp"
#include "formats/GroundPack.hpp"

#include "core/JobSystem.hpp"
#include "core/Logger.hpp"
#include "formats/ModelFile.hpp"
#include "utils/ImageUtils.hpp"
#include "utils/PbrMaterialProfile.hpp"
#include "utils/PbrDeferred.hpp"
#include "utils/PbrTextureLoader.hpp"
#include "ml/NeuralPbr.hpp"

#include <atomic>
#include <cctype>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <mutex>
#include <unordered_map>

#if defined(__linux__) && !defined(__ANDROID__)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace eruption {

namespace fs = std::filesystem;

std::mutex& diskReadMutex() {
    static std::mutex m;
    return m;
}

namespace {

constexpr uint32_t kPackMagic = 0x504b5445; // 'ETKP'
// Bump when the encoder or the synthesis changes so old caches are ignored.
// 4 (2026-09-29): altura/normal/AO sintetizados por HeightFromAlbedo
// (gradiente + multi-escala) - pack antigo carrega luminancia crua.
// 5: normal hibrida multi-escala (HeightFromAlbedo, passo 6).
// 6: altura estrutural (rede de rejunte por forma + distancia).
// 7: peso estrutural por pixel, material organico nunca vira rejunte.
// 8: polaridade global com troca regional so' com folga de 2x.
// 9: escala comum de polaridade + evidencia por densidade de linha.
// 10: histerese na mascara de rejunte.
// 11: raio do chanfro medido so' na regiao estrutural.
// 14: normal cru (RGBA8) no pacote quando a GPU nao tem BC7.
constexpr uint32_t kPackVersion = 14;

void put32(std::vector<uint8_t>& v, uint32_t x) {
    const size_t o = v.size();
    v.resize(o + 4);
    std::memcpy(v.data() + o, &x, 4);
}
void put64(std::vector<uint8_t>& v, uint64_t x) {
    const size_t o = v.size();
    v.resize(o + 8);
    std::memcpy(v.data() + o, &x, 8);
}

struct Reader {
    const uint8_t* d;
    size_t n;
    size_t c = 0;
    bool u32(uint32_t& x) {
        if (c + 4 > n) return false;
        std::memcpy(&x, d + c, 4); c += 4; return true;
    }
    bool u64(uint64_t& x) {
        if (c + 8 > n) return false;
        std::memcpy(&x, d + c, 8); c += 8; return true;
    }
    bool f32(float& x) {
        if (c + 4 > n) return false;
        std::memcpy(&x, d + c, 4); c += 4; return true;
    }
    bool str(std::string& s) {
        uint32_t len = 0;
        if (!u32(len) || len > (1u << 20) || c + len > n) return false;
        s.assign(reinterpret_cast<const char*>(d + c), len);
        c += len;
        return true;
    }
};

// Máscara de diagnóstico: ERUPTION_BC_MASK=<bits> — 1 albedo, 2 normal,
// 4 mrahw. Default 7 (tudo). Serve para atribuir uma regressão visual ao mapa
// certo em vez de chutar; entra na chave do cache para não misturar bakes.
uint32_t bcMask() {
    static const uint32_t m = []() -> uint32_t {
        if (const char* e = std::getenv("ERUPTION_BC_MASK")) {
            return static_cast<uint32_t>(std::strtoul(e, nullptr, 10)) & 7u;
        }
        return 7u;
    }();
    return m;
}

uint32_t formatKey() {
    // Pacote BC classico: a mesma chave de antes da spec 51 (os packs que ja'
    // existem continuam valendo). Fora dele (ASTC/ETC2, BC7 no albedo): a chave
    // da politica, com o bit alto para nunca coincidir com a classica.
    if (!texfmt::classicActive())
        return (texfmt::policyKey(texfmt::deviceCaps(), texfmt::options()) ^ (bcMask() * 0x9E3779B9u)) | 0x80000000u;
    const BcFormatSupport& s = bcFormatSupport();
    return (s.bc1 ? 1u : 0u) | (s.bc3 ? 2u : 0u) | (s.bc5 ? 4u : 0u) | (s.bc7 ? 8u : 0u)
         | (bcMask() << 4);
}

// O pack e' chaveado por maxSize e por sintese de PBR, mas era gravado num
// caminho UNICO. Resultado: trocar de preset (low usa 512, high usa 1024)
// invalidava o cache e recomprimia TUDO - medido em parana_field: 23,8 s de
// re-bake por troca de preset. Agora cada combinacao tem o seu arquivo, entao
// alternar entre presets so' paga na primeira vez de cada um.
// Fora do pacote BC classico o arquivo leva a etiqueta da politica (spec 51):
// A/B entre formatos (bancada, ERUPTION_TEXTURE_COMPRESSION) nao reassa a cada troca.
std::string packPathFor(const std::string& glbPath, uint32_t maxSize, bool synthPbr) {
    const std::string tag = texfmt::activeTag();
    return glbPath + "." + std::to_string(maxSize) + (synthPbr ? "s" : "n") + (tag.empty() ? "" : "." + tag) +
           ".etexpack";
}
// Caminho historico, sem sufixo. Continua sendo LIDO para nao invalidar packs
// que ja' existem no disco de quem atualiza a engine - o cache e' artefato
// derivado, mas re-gerar custa 24 s num mapa grande.
std::string legacyPackPathFor(const std::string& glbPath) { return glbPath + ".etexpack"; }


// ---- Fonte alternativa para a sintese de PBR (ferramenta de empacotamento) --------------------
// ERUPTION_TUF_PBR_SOURCE=<pasta>: para cada imagem embutida, se existir <pasta>/<sha1 dos bytes da
// imagem>.png com a mesma largura e altura, a rede de PBR roda nessa imagem; o albedo continua a
// original. Serve para o albedo ser o original e o PBR sair de uma versao limpa da mesma imagem.
// So' vale onde o assador roda (o motor em jogo nao le' esta variavel).
std::string sha1Hex(const uint8_t* d, size_t n) {
    uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    std::vector<uint8_t> m(d, d + n);
    m.push_back(0x80);
    while (m.size() % 64 != 56) m.push_back(0);
    const uint64_t bits = static_cast<uint64_t>(n) * 8;
    for (int i = 7; i >= 0; --i) m.push_back(static_cast<uint8_t>(bits >> (i * 8)));
    const auto rol = [](uint32_t x, int c) { return (x << c) | (x >> (32 - c)); };
    for (size_t o = 0; o < m.size(); o += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t(m[o + i * 4]) << 24) | (uint32_t(m[o + i * 4 + 1]) << 16) | (uint32_t(m[o + i * 4 + 2]) << 8) | m[o + i * 4 + 3];
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], dd = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t fx, k;
            if (i < 20) { fx = (b & c) | (~b & dd); k = 0x5A827999u; }
            else if (i < 40) { fx = b ^ c ^ dd; k = 0x6ED9EBA1u; }
            else if (i < 60) { fx = (b & c) | (b & dd) | (c & dd); k = 0x8F1BBCDCu; }
            else { fx = b ^ c ^ dd; k = 0xCA62C1D6u; }
            const uint32_t t = rol(a, 5) + fx + e + k + w[i];
            e = dd; dd = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += dd; h[4] += e;
    }
    char out[41];
    for (int i = 0; i < 5; ++i) std::snprintf(out + i * 8, 9, "%08x", h[i]);
    return std::string(out, 40);
}

const std::string& pbrSourceDir() {
    static const std::string dir = [] {
        const char* e = std::getenv("ERUPTION_TUF_PBR_SOURCE");
        return std::string(e ? e : "");
    }();
    return dir;
}

// Pixels RGBA da imagem alternativa (mesmo tamanho da original) ou vazio.
std::vector<uint8_t> pbrSourcePixels(const EmbeddedTexture& et, int w, int h) {
    std::vector<uint8_t> out;
    if (pbrSourceDir().empty() || !et.hasEncodedData()) return out;
    const std::string path = pbrSourceDir() + "/" + sha1Hex(et.encodedData.data(), et.encodedData.size()) + ".png";
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return out;
    std::ifstream f(path, std::ios::binary);
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    ImageData img = ImageUtils::loadFromMemoryRaw(bytes.data(), bytes.size());
    if (!img.isValid() || img.width != w || img.height != h || img.channels != 4) return out;
    return std::move(img.pixels);
}

// Quantas texturas cada nivel da cadeia (GPU > CPU > analitica) sintetizou.
void logSynthesisLevels(const NeuralPbrStats& before, size_t synthesized, uint64_t micros) {
    const NeuralPbrStats after = neuralPbrStats();
    const size_t gpu = static_cast<size_t>(after.gpu - before.gpu), cpu = static_cast<size_t>(after.cpu - before.cpu);
    const size_t analytic = synthesized > gpu + cpu ? synthesized - gpu - cpu : 0;
    ERUPTION_LOG_WARN("EmbeddedTextureBake: sintese PBR de %zu texturas - rede na GPU %zu, rede na CPU %zu, "
                      "analitica %zu (%.0f ms somados nas threads)",
                      synthesized, gpu, cpu, analytic, micros / 1000.0);
}

// Quem sintetizou o PBR: 0 = ninguem, 1 = classica, senao a chave da rede
// (modelo + mapeamento). Trocar de modelo invalida o pack.
// O que o motor faz com a saida da rede tambem muda o pack: a versao do
// pos-processo entra na chave (1 = altura refinada pelas linhas do albedo,
// refineHeightWithAlbedoLines; 2 = rede na resolucao da textura ate' 1024,
// synthesizePbrFromPixels; 3 = mapa PBR do disco antes da rede; 4 = margem
// circular na entrada da rede, saida emenda na borda). Pack de rede antigo e'
// re-assado uma vez.
constexpr uint32_t kNeuralPostVersion = 4u;

// ERUPTION_TEST_PBR_POST_VERSION=<n> (teste): finge que o pos-processo atual
// e' o n, para os packs de hoje virarem "anteriores" sem mexer no codigo.
// Nesse modo NADA e' gravado (os packs reais ficam intactos).
uint32_t postVersion() {
    static const uint32_t v = [] {
        const char* e = std::getenv("ERUPTION_TEST_PBR_POST_VERSION");
        return e ? static_cast<uint32_t>(std::max(1, std::atoi(e))) : kNeuralPostVersion;
    }();
    return v;
}
bool testNoWrite() { return std::getenv("ERUPTION_TEST_PBR_POST_VERSION") != nullptr; }

uint32_t synthKeyFor(uint32_t net, uint32_t postVersion) {
    const uint32_t k = (net ^ (postVersion * 0x9E3779B9u)) * 16777619u;
    return k > 1u ? k : 2u; // 0 e 1 sao "sem sintese" e "sintese classica"
}

uint32_t synthKey(bool synthPbr) {
    if (!synthPbr) return 0u;
    const uint32_t net = neuralPbrKey();
    if (net <= 1u) return 1u;
    return synthKeyFor(net, postVersion());
}

// Pack da MESMA rede com um pos-processo anterior: e' usado como esta' (o PBR
// fica como era). Antes ele era recusado e a primeira visita a cada mapa
// depois de mudar o pos-processo recomprimia tudo no meio da troca: um mapa
// de teste 84 s, o maior 4 min (168 texturas de modelos de cena). PBR nao e' gerado com
// o jogo rodando; o pack novo sai de um bake offline (apagar o .etexpack).
bool synthKeyStale(uint32_t cached, bool synthPbr) {
    if (!synthPbr) return false;
    const uint32_t net = neuralPbrKey();
    if (net <= 1u) return false;
    for (uint32_t v = 1; v < postVersion(); ++v)
        if (cached == synthKeyFor(net, v)) return true;
    return false;
}

// Pack assado com a rede numa maquina vale numa maquina SEM rede (ex.: GPU
// fraca com o PBR vindo pronto) e com a sintese desligada (carga rapida): o
// trabalho ja' esta' feito. Com rede aqui, so' o pack da mesma rede.
bool synthKeyAccepted(uint32_t cached, bool synthPbr) {
    const uint32_t local = synthKey(synthPbr);
    if (cached == local) return true;
    return local <= 1u && cached > 1u;
}

// Impressao digital do .glb: hash de 64 trechos de 64 KB espalhados pelo
// arquivo (4 MB lidos). Substitui a data de modificacao, que muda ao copiar
// o arquivo pra outra maquina e invalidava um pack identico.
uint64_t sourceFingerprint(const std::string& path, uint64_t size) {
    constexpr uint64_t kChunks = 64, kChunkBytes = 64 * 1024;
    std::ifstream disk(path, std::ios::binary);
    std::istringstream packed;
    if (!disk.is_open()) { // .glb so' num pacote .ecp do projeto: mesmos trechos, da memoria
        std::vector<uint8_t> bytes;
        if (!vfs::packsMounted() || !vfs::read(path, bytes)) return 0;
        packed.str(std::string(bytes.begin(), bytes.end()));
    }
    std::istream& f = disk.is_open() ? static_cast<std::istream&>(disk) : static_cast<std::istream&>(packed);
    uint64_t h = 1469598103934665603ull; // FNV-1a 64
    std::vector<char> chunk(kChunkBytes);
    const uint64_t step = size > kChunkBytes ? (size - kChunkBytes) / (kChunks - 1) : 0;
    for (uint64_t i = 0; i < kChunks; ++i) {
        f.seekg(static_cast<std::streamoff>(i * step));
        f.read(chunk.data(), static_cast<std::streamsize>(std::min<uint64_t>(kChunkBytes, size)));
        const std::streamsize got = f.gcount();
        for (std::streamsize b = 0; b < got; ++b) {
            h ^= static_cast<uint8_t>(chunk[static_cast<size_t>(b)]);
            h *= 1099511628211ull;
        }
        f.clear();
    }
    return h ^ size;
}

// Arquivo de onde o conteudo do .glb vem: ele mesmo, ou o .relevo de que a
// carga o desempacotou (o .glb desempacotado muda entre maquinas; o .relevo
// nao - ver GroundPack).
std::string fingerprintPath(const std::string& glbPath) {
    // O .glb existe (no pacote do mapa ou no disco): e' ele. Sem esta volta,
    // groundSourceFor consultava o disco por modelo, mesmo com o .emap montado.
    if (vfs::exists(glbPath)) return glbPath;
    const std::string relevo = groundSourceFor(glbPath);
    return relevo.empty() ? glbPath : relevo;
}

// Pacote ja' lido (do disco ou do pacote de mapa .emap) -> texturas. Tudo o que
// valida o pacote fica aqui; `trustSource`: o .glb e o pacote vieram juntos do
// .emap, que so' os guardou depois de a carga do build valida'-los (valido
// por construcao) - pula a impressao digital, que sao 64 leituras espalhadas
// pelo .glb (64 buscas da cabeca por modelo com o disco frio).
// Cabecalho do pacote (ate' a cor de bounce do chao): valida o pacote contra o
// .glb, o preset e a GPU. Compartilhado pela leitura inteira e pela adiada.
bool parsePackHeader(Reader& r, const std::string& glbPath, uint32_t maxSize, bool synthPbr, bool trustSource,
                     EmbeddedBakeResult& out, bool* stale, uint32_t& count) {
    if (stale) *stale = false;
    if (r.n < 40) return false;

    uint32_t magic = 0, version = 0, cachedMax = 0, cachedFmt = 0, cachedSynth = 0;
    uint64_t srcSize = 0, srcFingerprint = 0;
    if (!r.u32(magic) || magic != kPackMagic) return false;
    if (!r.u32(version) || version != kPackVersion) return false;
    if (!r.u64(srcSize) || !r.u64(srcFingerprint)) return false;
    if (!r.u32(cachedMax) || !r.u32(cachedFmt) || !r.u32(cachedSynth) || !r.u32(count)) return false;

    // Invalida se: o .glb mudou (conteudo, nao data - o pacote vale em outra
    // maquina), o teto de resolucao mudou (preset), o conjunto de formatos da
    // GPU mudou, ou a sintese de PBR nao e' aceitavel aqui.
    const std::string source = fingerprintPath(glbPath);
    // .glb de um .tuf com texturas prontas (kFlagGpuReady): o guardado no pacote
    // nao tem as imagens, entao tamanho e impressao digital do original nao
    // existem mais; o pacote de texturas foi gravado junto com ele, no mesmo
    // arquivo .tuf, e vale por construcao (como no .emap).
    if (!vfs::tufGpuReady(source)) {
        std::error_code ec;
        const uint64_t realSize = vfs::fileSize(source, ec);
        if (ec || realSize != srcSize) return false;
        if (!trustSource && sourceFingerprint(source, realSize) != srcFingerprint) return false;
    }
    if (cachedMax != maxSize || cachedFmt != formatKey()) return false;
    if (!synthKeyAccepted(cachedSynth, synthPbr)) {
        if (!stale || !synthKeyStale(cachedSynth, synthPbr)) return false;
        *stale = true;
    }

    uint32_t hasGround = 0;
    if (!r.u32(hasGround)) return false;
    for (int i = 0; i < 3; ++i) {
        if (!r.f32(out.groundAlbedo[i])) return false;
    }
    out.hasGroundAlbedo = hasGround != 0;
    return true;
}

bool parseCacheBytes(const std::vector<uint8_t>& buf, const std::string& glbPath, uint32_t maxSize, bool synthPbr,
                     bool trustSource, EmbeddedBakeResult& out, bool* stale) {
    if (stale) *stale = false;
    if (buf.size() < 40) return false;
    Reader r{buf.data(), buf.size()};
    uint32_t count = 0;
    if (!parsePackHeader(r, glbPath, maxSize, synthPbr, trustSource, out, stale, count)) return false;

    for (uint32_t i = 0; i < count; ++i) {
        std::string name;
        if (!r.str(name)) return false;
        uint32_t mask = 0;
        if (!r.u32(mask)) return false;
        BakedEmbeddedTexture t;
        if (mask & 1u) { if (!readEtexBytes(r.d, r.n, r.c, t.albedo)) return false; }
        if (mask & 2u) { if (!readEtexBytes(r.d, r.n, r.c, t.normal)) return false; }
        if (mask & 4u) { if (!readEtexBytes(r.d, r.n, r.c, t.mrahw)) return false; }
        t.albedoOpaque = (mask & 8u) != 0;
        out.bytes += t.albedo.byteSize() + t.normal.byteSize() + t.mrahw.byteSize();
        out.textures.emplace(std::move(name), std::move(t));
    }
    out.fromCache = true;
    return true;
}

// Build do pacote do mapa: que pacote cada .glb usou e quais imagens ele traz.
std::mutex g_coverageMutex;
std::unordered_map<std::string, EmbeddedPackCoverage> g_coverage;

void noteCoverage(const std::string& glbPath, const std::string& packPath, const EmbeddedBakeResult& res) {
    if (!vfs::recording()) return;
    EmbeddedPackCoverage c;
    c.pack = vfs::key(packPath);
    c.images.reserve(res.textures.size());
    for (const auto& t : res.textures) c.images.push_back(t.first);
    std::lock_guard<std::mutex> lock(g_coverageMutex);
    g_coverage[vfs::key(glbPath)] = std::move(c);
}

bool readCache(const std::string& glbPath, uint32_t maxSize, bool synthPbr,
               EmbeddedBakeResult& out, bool* stale = nullptr) {
    if (stale) *stale = false;
    std::string path = packPathFor(glbPath, maxSize, synthPbr);
    // Sintese desligada: o pack assado com ela tambem serve (synthKeyAccepted).
    if (!synthPbr && !vfs::exists(path) && vfs::exists(packPathFor(glbPath, maxSize, true)))
        path = packPathFor(glbPath, maxSize, true);
    if (!vfs::exists(path)) {
        // Compatibilidade: pack antigo sem sufixo. O cabecalho carrega
        // maxSize/synthPbr, entao a validacao abaixo rejeita se nao bater.
        if (!texfmt::classicActive()) return false; // o legado e' sempre BC classico
        path = legacyPackPathFor(glbPath);
        if (!vfs::exists(path)) return false;
    }

    std::vector<uint8_t> buf;
    const bool fromBundle = vfs::inBundle(path);
    {
        // Do pacote do mapa sem a trava do disco: ela serializava a
        // descompressao zstd entre os fios do preload.
        std::unique_lock<std::mutex> disco(diskReadMutex(), std::defer_lock);
        if (!fromBundle) disco.lock();
        if (!vfs::read(path, buf)) return false;
        if (!fromBundle) testDiskThrottle(buf.size(), true);
    }
    const bool trust = fromBundle && vfs::inBundle(fingerprintPath(glbPath));
    if (parseCacheBytes(buf, glbPath, maxSize, synthPbr, trust, out, stale)) {
        noteCoverage(glbPath, path, out);
        if (vfs::tufGpuReady(glbPath))
            ERUPTION_LOG_WARN("[TUFGPU] '%s': %zu texturas prontas do pacote .tuf (%.1f MB), sem compressao",
                              glbPath.c_str(), out.textures.size(), static_cast<double>(out.bytes) / 1048576.0);
        return true;
    }
    if (!fromBundle) return false;
    // A copia do pacote de mapa nao serve aqui (outro preset, outra GPU): a
    // do disco, se houver, ainda pode servir.
    out = EmbeddedBakeResult{};
    {
        std::lock_guard<std::mutex> disco(diskReadMutex());
        if (!vfs::readDisk(path, buf)) return false;
    }
    if (!parseCacheBytes(buf, glbPath, maxSize, synthPbr, false, out, stale)) return false;
    noteCoverage(glbPath, path, out);
    return true;
}

// Fracao do arquivo que ja' esta' no cache do sistema (mincore). 1 onde nao da'
// para saber: sem a medida o adiamento nao liga.
double residentFraction(const std::string& path) {
#if defined(__linux__) && !defined(__ANDROID__)
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return 1.0;
    struct stat st{};
    if (::fstat(fd, &st) != 0 || st.st_size <= 0) {
        ::close(fd);
        return 1.0;
    }
    const size_t len = static_cast<size_t>(st.st_size);
    void* map = ::mmap(nullptr, len, PROT_READ, MAP_SHARED, fd, 0);
    ::close(fd);
    if (map == MAP_FAILED) return 1.0;
    const size_t page = static_cast<size_t>(::sysconf(_SC_PAGESIZE));
    std::vector<unsigned char> vec((len + page - 1) / page);
    double frac = 1.0;
    if (::mincore(map, len, vec.data()) == 0) {
        size_t resident = 0;
        for (unsigned char v : vec) resident += v & 1u;
        frac = static_cast<double>(resident) / static_cast<double>(vec.size());
    }
    ::munmap(map, len);
    return frac;
#else
    (void)path;
    return 1.0;
#endif
}

// LEITURA EM DUAS FASES (PBR adiado, ver PbrDeferred.hpp). Medido a frio num
// mapa de 390 MB de pacote (HD): 12,7 s de 17,8 s da carga eram a leitura, e
// 75% dos bytes sao normal e MRAH-W. Aqui le' so' o albedo de cada textura e,
// dos blocos PBR, o cabecalho de 20 bytes (os tamanhos dos mips saem da
// geometria do bloco), sem tocar nos dados. O formato do pacote nao muda: os
// pacotes que ja' existem valem como estao.
//
// So' compensa em pacote grande e fora do cache: cada bloco pulado custa uma
// busca (medido: 580 leituras em 1,6 s num pacote de 24 MB, que le' inteiro em
// 0,7 s), entao pacote pequeno ou ja' no cache do sistema segue o caminho de
// sempre. Qualquer surpresa no formato (formato sem geometria conhecida, bloco
// que nao fecha, arquivo que nao termina onde o ultimo bloco termina) devolve
// false e a leitura inteira roda.
constexpr uint64_t kDeferMinPackBytes = 96ull << 20;

// ignoreResident: na troca de mapa o pacote no cache do sistema tambem adia o PBR. Sem
// disco a esperar, o custo que sobra e' o do fio principal (copia e envio de centenas
// de MB de normal/MRAH-W no quadro da troca: 0,3 a 0,5 s num mapa de 500 MB); adiado,
// o envio anda por quadro (PbrDeferred::pump) e a troca so' leva o albedo.
bool readCacheDeferred(const std::string& glbPath, uint32_t maxSize, bool synthPbr, EmbeddedBakeResult& out,
                       bool* stale, bool ignoreResident = false) {
    if (stale) *stale = false;
    std::string path = packPathFor(glbPath, maxSize, synthPbr);
    if (!synthPbr && !vfs::exists(path) && vfs::exists(packPathFor(glbPath, maxSize, true)))
        path = packPathFor(glbPath, maxSize, true);
    if (!vfs::exists(path)) {
        if (!texfmt::classicActive()) return false;
        path = legacyPackPathFor(glbPath);
        if (!vfs::exists(path)) return false;
    }
    if (vfs::inBundle(path)) return false; // entrada comprimida do .emap: sem leitura parcial
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return false;
    const uint64_t fileSize = fs::file_size(path, ec);
    if (ec || fileSize < 64) return false;
    // Pacote pequeno: cada bloco pulado custa uma busca, e le' inteiro e' mais rapido (mapa interno pequeno, 14 MB,
    // disco lento: 0,7 s inteiro contra 2 s adiado). Na troca de mapa (ignoreResident) o limite vale
    // ate' com o override de teste, que so' forca o adiamento da carga a frio.
    if (ignoreResident && fileSize < kDeferMinPackBytes) return false;
    if (PbrDeferred::testOverride() < 1) {
        if (fileSize < kDeferMinPackBytes) return false;
        if (!ignoreResident && residentFraction(path) > 0.9) return false;
    }

    const auto t0 = std::chrono::steady_clock::now();
    auto bail = [&](const char* why) {
        ERUPTION_LOG_WARN("[PBRDEFER] '%s': %s; leitura inteira", path.c_str(), why);
        out = EmbeddedBakeResult{};
        return false;
    };
    std::lock_guard<std::mutex> disco(diskReadMutex());
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    uint64_t lastOff = 0, lastEnd = 0;
    auto readAt = [&](uint64_t off, void* dst, size_t n) -> size_t {
        f.clear();
        f.seekg(static_cast<std::streamoff>(off));
        f.read(static_cast<char*>(dst), static_cast<std::streamsize>(n));
        // Teste de disco lento: o que cai dentro da leitura anterior nao toca o disco; so'
        // conta a busca quando o arquivo nao segue em sequencia.
        if (!(off >= lastOff && off + n <= lastEnd)) {
            const bool seek = off > lastEnd || off < lastOff;
            testDiskThrottle(off + n - (seek ? off : lastEnd), seek);
            if (seek) lastOff = off;
            lastEnd = off + n;
        }
        return static_cast<size_t>(f.gcount());
    };

    uint8_t head[64];
    const size_t headGot = readAt(0, head, sizeof head);
    Reader r{head, headGot};
    uint32_t count = 0;
    if (!parsePackHeader(r, glbPath, maxSize, synthPbr, false, out, stale, count)) {
        out = EmbeddedBakeResult{};
        return false;
    }

    uint64_t pos = r.c;
    uint64_t albedoBytes = 0, deferredBytes = 0, retries = 0;
    std::vector<uint8_t> scratch;
    // Formato do normal e do MRAH-W, aprendido nos cabecalhos lidos: o tamanho do
    // bloco PBR e' previsto pelo formato e pelas dimensoes do albedo, e assim a
    // cabeca nao busca o cabecalho de cada bloco (435 buscas num mapa de 145
    // texturas; com a previsao, 145). A previsao erra quando o PBR tem outra
    // dimensao que o albedo (medido: 0 a 7% das texturas); o erro aparece no
    // nome da textura seguinte (tamanho absurdo, byte de controle, mascara
    // desconhecida) e a textura anterior e' refeita lendo os cabecalhos de verdade.
    uint32_t learnedFmt[2] = {0, 0};

    struct Parsed {
        std::string name;
        BakedEmbeddedTexture tex;
        uint64_t start = 0, end = 0;
        bool predicted = false; // algum bloco PBR teve o tamanho previsto
        uint64_t albedoBytes = 0, deferredBytes = 0;
    };
    enum Rc { kOk, kBadName, kFatal, kOverflow };
    // Le' uma textura em `at`; `verify` = le' os cabecalhos PBR em vez de prever o tamanho.
    auto parseOne = [&](uint64_t at, bool verify, Parsed& o) -> Rc {
        uint8_t nb[512];
        const size_t g = readAt(at, nb, sizeof nb);
        uint32_t nameLen = 0;
        if (g < 8) return kBadName;
        std::memcpy(&nameLen, nb, 4);
        if (nameLen > 400 || 4u + nameLen + 4u > g) return kBadName;
        o = Parsed{};
        o.name.assign(reinterpret_cast<const char*>(nb + 4), nameLen);
        // Nome com bytes de controle ou mascara com bits desconhecidos = lemos o meio de um bloco.
        for (const char c : o.name)
            if (static_cast<unsigned char>(c) < 0x20) return kBadName;
        uint32_t mask = 0;
        std::memcpy(&mask, nb + 4 + nameLen, 4);
        if ((mask & ~0xFu) != 0 || nameLen == 0) return kBadName;
        o.start = at;
        uint64_t p = at + 8 + nameLen;
        if (mask & 1u) {
            uint8_t h20[20];
            EtexData hdr;
            uint64_t bytes = 0;
            // O cabecalho do albedo costuma estar na leitura do nome (evita uma chamada).
            const bool inName = 8u + nameLen + 20u <= g;
            if (inName) std::memcpy(h20, nb + 8 + nameLen, 20);
            if ((!inName && readAt(p, h20, 20) != 20) || !readEtexHeaderBytes(h20, 20, hdr, bytes) ||
                p + bytes > fileSize)
                return kFatal;
            scratch.resize(static_cast<size_t>(bytes));
            std::memcpy(scratch.data(), h20, 20); // o resto vem em sequencia ao cabecalho
            if (readAt(p + 20, scratch.data() + 20, scratch.size() - 20) != scratch.size() - 20) return kFatal;
            size_t cur = 0;
            if (!readEtexBytes(scratch.data(), scratch.size(), cur, o.tex.albedo) || cur != scratch.size() ||
                o.tex.albedo.mipSizes != hdr.mipSizes)
                return kFatal;
            o.albedoBytes = bytes;
            p += bytes;
        }
        for (const uint32_t bit : {2u, 4u}) {
            if (!(mask & bit)) continue;
            const int k = bit == 2u ? 0 : 1;
            auto src = std::make_shared<PbrDeferredSource>();
            const bool predict = (mask & 1u) && learnedFmt[k] != 0 && !verify;
            if (predict) {
                if (!etexBlockGeometry(learnedFmt[k], o.tex.albedo.width, o.tex.albedo.height,
                                       o.tex.albedo.mipCount, src->header, src->bytes))
                    return kFatal;
                if (p + src->bytes > fileSize) return kOverflow;
                o.predicted = true;
            } else {
                uint8_t h20[20];
                if (readAt(p, h20, 20) != 20 || !readEtexHeaderBytes(h20, 20, src->header, src->bytes) ||
                    p + src->bytes > fileSize)
                    return kFatal;
                learnedFmt[k] = src->header.vkFormat;
            }
            src->path = path;
            src->offset = p;
            (bit == 2u ? o.tex.normal : o.tex.mrahw) = src->header;
            o.deferredBytes += src->bytes;
            p += src->bytes;
            (bit == 2u ? o.tex.normalSrc : o.tex.mrahwSrc) = std::move(src);
        }
        o.tex.albedoOpaque = (mask & 8u) != 0;
        o.end = p;
        return kOk;
    };
    auto commit = [&](Parsed& o) {
        albedoBytes += o.albedoBytes;
        deferredBytes += o.deferredBytes;
        out.bytes += o.tex.albedo.byteSize();
        out.textures.emplace(std::move(o.name), std::move(o.tex));
    };

    Parsed last;
    bool haveLast = false;
    for (uint32_t i = 0; i < count; ++i) {
        Parsed cur;
        Rc rc = parseOne(pos, false, cur);
        if ((rc == kBadName || rc == kFatal) && haveLast && last.predicted) {
            // A previsao do bloco anterior errou: ele e' refeito lendo os cabecalhos.
            ++retries;
            Parsed redo;
            if (parseOne(last.start, true, redo) != kOk) return bail("textura anterior nao leu");
            last = std::move(redo);
            pos = last.end;
            rc = parseOne(pos, false, cur);
        }
        if (rc == kOverflow) {
            ++retries;
            rc = parseOne(pos, true, cur);
        }
        if (rc == kBadName) return bail("nome de textura fora do esperado");
        if (rc != kOk) return bail("bloco do pacote desconhecido");
        if (haveLast) commit(last);
        last = std::move(cur);
        haveLast = true;
        pos = last.end;
    }
    if (haveLast && last.end != fileSize && last.predicted) { // a ultima tambem pode ter sido mal prevista
        ++retries;
        Parsed redo;
        if (parseOne(last.start, true, redo) != kOk) return bail("ultima textura nao leu");
        last = std::move(redo);
        pos = last.end;
    }
    if (haveLast) commit(last);
    if (pos != fileSize) return bail("o ultimo bloco nao termina no fim do arquivo");
    out.bytes += deferredBytes;
    out.fromCache = true;
    noteCoverage(glbPath, path, out);
    ERUPTION_LOG_WARN("[PBRDEFER] '%s': albedo lido (%.1f MB), PBR adiado (%.1f MB em %zu texturas, %llu refeitas) em %.0f ms",
                      path.c_str(), static_cast<double>(albedoBytes) / 1048576.0,
                      static_cast<double>(deferredBytes) / 1048576.0, out.textures.size(),
                      static_cast<unsigned long long>(retries),
                      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    return true;
}

// O pacote (.etexpack) em memoria. false se a origem nao tem tamanho.
bool serializePack(const std::string& glbPath, uint32_t maxSize, bool synthPbr, const EmbeddedBakeResult& res,
                   std::vector<uint8_t>& buf) {
    std::error_code ec;
    const std::string source = fingerprintPath(glbPath);
    const uint64_t srcSize = vfs::fileSize(source, ec); // disco ou pacote .ecp
    if (ec) return false;
    buf.clear();
    buf.reserve(res.bytes + 4096);
    put32(buf, kPackMagic);
    put32(buf, kPackVersion);
    put64(buf, srcSize);
    put64(buf, sourceFingerprint(source, srcSize));
    put32(buf, maxSize);
    put32(buf, formatKey());
    put32(buf, synthKey(synthPbr));
    put32(buf, static_cast<uint32_t>(res.textures.size()));
    put32(buf, res.hasGroundAlbedo ? 1u : 0u);
    for (int i = 0; i < 3; ++i) {
        const size_t o = buf.size();
        buf.resize(o + 4);
        std::memcpy(buf.data() + o, &res.groundAlbedo[i], 4);
    }
    for (const auto& [name, t] : res.textures) {
        put32(buf, static_cast<uint32_t>(name.size()));
        buf.insert(buf.end(), name.begin(), name.end());
        uint32_t mask = 0;
        if (t.albedo.valid()) mask |= 1u;
        if (t.normal.valid()) mask |= 2u;
        if (t.mrahw.valid()) mask |= 4u;
        // Bit 8: albedo sem alfa. Leitor antigo ignora; o formato sozinho so'
        // diz isso no BC1 (ASTC 4x4 e' o mesmo com e sem alfa).
        if (t.albedo.valid() && t.albedoOpaque) mask |= 8u;
        put32(buf, mask);
        if (mask & 1u) appendEtexBytes(t.albedo, buf);
        if (mask & 2u) appendEtexBytes(t.normal, buf);
        if (mask & 4u) appendEtexBytes(t.mrahw, buf);
    }
    return true;
}

void writeCache(const std::string& glbPath, uint32_t maxSize, bool synthPbr,
                const EmbeddedBakeResult& res) {
    std::error_code ec;
    std::vector<uint8_t> buf;
    if (!serializePack(glbPath, maxSize, synthPbr, res, buf)) return;
    // Write to a temp file and rename: a crash mid-bake must not leave a
    // truncated cache that the next run would happily read.
    const std::string finalPath = packPathFor(glbPath, maxSize, synthPbr);
    const std::string tmpPath = finalPath + ".tmp";
    // Projeto so' de pacotes: a pasta do .glb nao existe no disco; o cache (que
    // e' escrito pelo motor) nasce la', sem conteudo do jogo.
    if (vfs::packsMounted()) fs::create_directories(fs::path(finalPath).parent_path(), ec);
    {
        std::ofstream f(tmpPath, std::ios::binary | std::ios::trunc);
        if (!f.is_open()) {
            ERUPTION_LOG_WARN("EmbeddedTextureBake: cannot write cache '%s'", tmpPath.c_str());
            return;
        }
        f.write(reinterpret_cast<const char*>(buf.data()),
                static_cast<std::streamsize>(buf.size()));
    }
    fs::rename(tmpPath, finalPath, ec);
    if (ec) {
        fs::remove(tmpPath, ec);
        return;
    }
    // Pacote assado agora no build do mapa: vai inteiro para o .emap (a carga
    // so' perguntou se ele existia).
    if (vfs::recording()) {
        vfs::recordExtra(finalPath);
        noteCoverage(glbPath, finalPath, res);
    }
}

} // namespace

namespace {
std::atomic<uint32_t> g_maxSize{1024};
}
void setEmbeddedTextureMaxSize(uint32_t maxSize) { g_maxSize.store(maxSize); }
uint32_t embeddedTextureMaxSize() { return g_maxSize.load(); }

size_t removeEmbeddedBakeCaches(const std::string& glbPath) {
    // Todos os nomes que packPathFor/legacyPackPathFor geram: "<glb>.etexpack"
    // e "<glb>.<lado><s|n>[.<formato>].etexpack".
    const fs::path glb(glbPath);
    const std::string prefix = glb.filename().string() + ".";
    const std::string suffix = ".etexpack";
    size_t removed = 0;
    std::error_code ec;
    for (fs::directory_iterator it(glb.parent_path().empty() ? fs::path(".") : glb.parent_path(), ec), end;
         !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (name.size() < prefix.size() + suffix.size() - 1 || name.compare(0, prefix.size(), prefix) != 0 ||
            name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0)
            continue;
        std::error_code rec;
        if (fs::remove(it->path(), rec)) ++removed;
    }
    return removed;
}

namespace {

// DUMP DE DIAGNOSTICO (ERUPTION_TEST_DUMP_TEX_DIR): grava BMP 24 bpp cru, sem
// compressao nenhuma - existe so' pra o autor OLHAR o dado bruto que o bake
// produz, sem passar pela interpretacao de nenhum shader. BGR, bottom-up,
// linha alinhada em 4 bytes - o formato mais chato de BMP que existe, de
// proposito: qualquer visualizador abre sem duvida de codec.
bool writeBmp24(const std::string& path, int w, int h, const std::vector<uint8_t>& rgb) {
    if (w <= 0 || h <= 0 || rgb.size() != static_cast<size_t>(w) * h * 3) return false;
    const int rowSize = ((w * 3 + 3) / 4) * 4;
    const uint32_t pixelDataSize = static_cast<uint32_t>(rowSize) * static_cast<uint32_t>(h);
    const uint32_t fileSize = 14 + 40 + pixelDataSize;
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f.is_open()) return false;
    auto w16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    auto w32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    f.write("BM", 2);
    w32(fileSize); w32(0); w32(14 + 40);
    w32(40);                         // BITMAPINFOHEADER
    w32(static_cast<uint32_t>(w));
    w32(static_cast<uint32_t>(h));   // positivo = bottom-up
    w16(1); w16(24); w32(0); w32(pixelDataSize);
    w32(2835); w32(2835); w32(0); w32(0);
    std::vector<uint8_t> row(static_cast<size_t>(rowSize), 0);
    for (int y = h - 1; y >= 0; --y) {
        for (int x = 0; x < w; ++x) {
            const uint8_t* p = &rgb[(static_cast<size_t>(y) * w + x) * 3];
            row[static_cast<size_t>(x) * 3 + 0] = p[2]; // B
            row[static_cast<size_t>(x) * 3 + 1] = p[1]; // G
            row[static_cast<size_t>(x) * 3 + 2] = p[0]; // R
        }
        f.write(reinterpret_cast<const char*>(row.data()), rowSize);
    }
    return f.good();
}

std::string sanitizeForFilename(std::string s) {
    for (auto& c : s) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '.') c = '_';
    }
    return s;
}

// Quanto cada textura contribui para a cor da luz que o chao devolve.
//
// Esta media alimenta DOIS consumidores reais: o hemisferio inferior do
// ambiente e o termo de "GI de um bounce" do passe direcional. E' literalmente
// a cor da luz que sobe do chao e ilumina a parte de baixo de tudo.
//
// Era a media de TODAS as texturas embutidas com peso IGUAL - ceu, agua, metal,
// parede, interface. Num mapa vulcanico, vermelho e marrom no chao, o resultado
// saia cinza-arroxeado, com o azul puxando a media para longe do que o chao
// realmente e'.
//
// O criterio agora e' o fisicamente correto e nao depende de autoria nenhuma:
// AREA VOLTADA PARA CIMA. Luz refletida difusa sobe de superficie horizontal;
// parede vertical devolve para o lado e teto/domo de ceu nao devolve nada para
// cima. Uma textura usada so' em parede ou no domo do ceu recebe peso ~0
// sozinha, sem ninguem precisar marca-la.
//
// A normal e' tomada em espaco de modelo. Para terreno de mapa isso coincide
// com o mundo; um asset girado no proprio no' seria mal classificado, mas
// terreno nao e' girado e e' ele que domina a area.
std::unordered_map<std::string, double> computeUpFacingArea(const ModelFile& model) {
    std::unordered_map<std::string, double> area;

    for (const auto& node : model.nodes) {
        if (node.vertices.empty() || node.indices.size() < 3) continue;
        if (node.textureNames.empty()) continue;

        // Subamostra: a cor media nao precisa de todo triangulo, e mapas
        // grandes tem milhoes deles. 1 em cada 7 ja' estabiliza a media.
        const size_t triCount = node.indices.size() / 3;
        const size_t stride = (triCount > 20000) ? 7 : 1;

        for (size_t t = 0; t < triCount; t += stride) {
            const uint32_t i0 = node.indices[t * 3 + 0];
            const uint32_t i1 = node.indices[t * 3 + 1];
            const uint32_t i2 = node.indices[t * 3 + 2];
            if (i0 >= node.vertices.size() || i1 >= node.vertices.size() ||
                i2 >= node.vertices.size()) continue;

            const Vec3& a = node.vertices[i0];
            const Vec3& b = node.vertices[i1];
            const Vec3& c = node.vertices[i2];
            const Vec3 cross = glm::cross(b - a, c - a);
            const float len = glm::length(cross);
            if (len <= 1e-8f) continue;

            // O Y da normal E' o cosseno com a vertical, entao o produto
            // (area x cosseno) e' exatamente a projecao horizontal do
            // triangulo - a grandeza que governa quanta luz ele manda para
            // cima. Sai de graca: len/2 * (cross.y/len) = cross.y/2.
            const float upward = cross.y * 0.5f;
            if (upward <= 0.0f) continue; // virado para baixo ou vertical

            uint16_t texLocal = 0;
            if (!node.perVertexTexIds.empty() && i0 < node.perVertexTexIds.size())
                texLocal = node.perVertexTexIds[i0];
            if (texLocal >= node.textureNames.size()) continue;

            area[node.textureNames[texLocal]] += double(upward) * double(stride);
        }
    }
    return area;
}

} // namespace

std::unordered_map<std::string, EmbeddedPackCoverage> recordedPackCoverage() {
    std::lock_guard<std::mutex> lock(g_coverageMutex);
    return g_coverage;
}

bool readEmbeddedBakeCache(const ModelFile& model, uint32_t maxTextureSize, bool synthesizePbr,
                           EmbeddedBakeResult& out, bool allowDeferredPbr) {
    if (model.embeddedTextures.empty() || model.filePath.empty() || !texfmt::anyActive()) return false;
    if (std::getenv("ERUPTION_TEST_DUMP_TEX_DIR")) return false; // o dump quer bake fresco
    if (allowDeferredPbr && readCacheDeferred(model.filePath, maxTextureSize, synthesizePbr, out, nullptr, true)) return true;
    out = EmbeddedBakeResult{};
    return readCache(model.filePath, maxTextureSize, synthesizePbr, out);
}

namespace {
// _mrahw/_normal do disco em RGBA cru (o resto do bake comprime os pixels).
bool loadDiskPbrRgba(const std::string& albedoName, PbrTextureData& mrahw, PbrTextureData& normal) {
    const PbrTexturePathPair paths = resolvePbrTexturePaths(albedoName);
    if (!paths.valid || paths.mrahw.empty() || paths.normal.empty()) return false;
    auto load = [](const std::string& path, PbrTextureData& out) {
        ImageData img = ImageUtils::loadPNG(path);
        if (!img.isValid() || (img.channels != 4 && img.channels != 3)) return false;
        if (img.channels == 3) {
            std::vector<uint8_t> rgba(static_cast<size_t>(img.width) * img.height * 4, 255);
            for (size_t i = 0; i < static_cast<size_t>(img.width) * img.height; ++i)
                for (int c = 0; c < 3; ++c) rgba[i * 4 + c] = img.pixels[i * 3 + c];
            img.pixels = std::move(rgba);
        }
        out.pixels = std::move(img.pixels);
        out.width = img.width;
        out.height = img.height;
        out.channels = 4;
        return true;
    };
    return load(paths.mrahw, mrahw) && load(paths.normal, normal);
}
} // namespace

namespace {
EmbeddedBakeResult bakeFresh(const ModelFile& model, uint32_t maxTextureSize, bool synthesizePbr, bool writeCacheFile);
} // namespace

bool buildEmbeddedBakePack(const ModelFile& model, uint32_t maxTextureSize, bool synthesizePbr,
                           std::vector<uint8_t>& packBytes, std::string& packPath,
                           std::vector<std::string>* imageNames, bool* fromDiskCache) {
    if (fromDiskCache) *fromDiskCache = false;
    if (model.embeddedTextures.empty() || model.filePath.empty() || !texfmt::anyActive()) return false;
    EmbeddedBakeResult res;
    // Um pacote em disco valido para este .glb, preset e GPU e' reaproveitado:
    // os bytes saem exatamente os que o motor ja' usa.
    bool stale = false;
    if (readCache(model.filePath, maxTextureSize, synthesizePbr, res, &stale) && !stale) {
        if (fromDiskCache) *fromDiskCache = true;
    } else {
        res = bakeFresh(model, maxTextureSize, synthesizePbr, false);
    }
    if (res.textures.empty()) return false;
    if (!serializePack(model.filePath, maxTextureSize, synthesizePbr, res, packBytes)) return false;
    packPath = packPathFor(model.filePath, maxTextureSize, synthesizePbr);
    if (imageNames) {
        imageNames->clear();
        for (const auto& t : res.textures) imageNames->push_back(t.first);
    }
    return true;
}

bool parseEmbeddedBakePackBytes(const std::vector<uint8_t>& buf, EmbeddedBakeResult& out) {
    Reader r{buf.data(), buf.size()};
    uint32_t magic = 0, version = 0, count = 0, skip = 0, hasGround = 0;
    uint64_t skip64 = 0;
    if (!r.u32(magic) || magic != kPackMagic || !r.u32(version) || version != kPackVersion) return false;
    if (!r.u64(skip64) || !r.u64(skip64) || !r.u32(skip) || !r.u32(skip) || !r.u32(skip) || !r.u32(count)) return false;
    if (!r.u32(hasGround)) return false;
    for (int i = 0; i < 3; ++i)
        if (!r.f32(out.groundAlbedo[i])) return false;
    out.hasGroundAlbedo = hasGround != 0;
    for (uint32_t i = 0; i < count; ++i) {
        std::string name;
        uint32_t mask = 0;
        if (!r.str(name) || !r.u32(mask)) return false;
        BakedEmbeddedTexture t;
        if ((mask & 1u) && !readEtexBytes(r.d, r.n, r.c, t.albedo)) return false;
        if ((mask & 2u) && !readEtexBytes(r.d, r.n, r.c, t.normal)) return false;
        if ((mask & 4u) && !readEtexBytes(r.d, r.n, r.c, t.mrahw)) return false;
        t.albedoOpaque = (mask & 8u) != 0;
        out.bytes += t.albedo.byteSize() + t.normal.byteSize() + t.mrahw.byteSize();
        out.textures.emplace(std::move(name), std::move(t));
    }
    return true;
}

namespace {
// .glb de um .tuf com texturas prontas: as imagens tiradas dele so' existem no pacote de texturas. Um
// pacote de outro conteudo do .glb (companheira velha que o .tuf manteve ao reempacotar o mapa) nao as tem,
// e sem este aviso a falha aparece so' como "failed to decode" por imagem.
void warnStrippedNotInPack(const ModelFile& model, const EmbeddedBakeResult& res) {
    if (!vfs::tufGpuReady(model.filePath)) return;
    size_t missing = 0;
    std::string first;
    for (const EmbeddedTexture& et : model.embeddedTextures) {
        if (!et.encodedData.isDeferred() || res.textures.count(et.name)) continue;
        if (missing++ == 0) first = et.name;
    }
    if (missing)
        ERUPTION_LOG_ERROR("[TUFGPU] '%s': %zu imagens tiradas do .glb nao estao no pacote de texturas (ex.: '%s'): "
                           "o pacote e' de outro conteudo do .glb. Reempacote o mapa com --gpu-ready",
                           model.filePath.c_str(), missing, first.c_str());
}
} // namespace

EmbeddedBakeResult bakeEmbeddedTextures(const ModelFile& model, uint32_t maxTextureSize,
                                        bool synthesizePbr, bool allowDeferredPbr) {
    EmbeddedBakeResult res;
    if (model.embeddedTextures.empty()) return res;
    if (!texfmt::anyActive()) {
        // Nenhum formato de bloco nesta GPU (ou modo off): caminho RGBA8, nada a assar.
        return res;
    }

    // ERUPTION_TEST_DUMP_TEX_DIR=<pasta>: pula o cache pra forcar bake FRESCO
    // (o cache so' guarda os bytes ja' comprimidos em BC7, nao da' pra tirar
    // BMP dali sem descomprimir) - ver o dump logo abaixo, ainda dentro do
    // parallelFor, onde mrahw.pixels/normal.pixels ainda sao RGBA cru.
    const bool dumpRequested = std::getenv("ERUPTION_TEST_DUMP_TEX_DIR") != nullptr;
    bool stale = false;
    if (allowDeferredPbr && !model.filePath.empty() && !dumpRequested &&
        readCacheDeferred(model.filePath, maxTextureSize, synthesizePbr, res, &stale)) {
        warnStrippedNotInPack(model, res);
        return res;
    }
    res = EmbeddedBakeResult{};
    if (!model.filePath.empty() && !dumpRequested &&
        readCache(model.filePath, maxTextureSize, synthesizePbr, res, &stale)) {
        ERUPTION_LOG_WARN("EmbeddedTextureBake: cache HIT%s '%s' (%zu texturas, %.1f MB comprimidos)",
                          stale ? " (pos-processo de PBR anterior, usado como esta')" : "",
                          model.filePath.c_str(), res.textures.size(),
                          static_cast<double>(res.bytes) / (1024.0 * 1024.0));
        warnStrippedNotInPack(model, res);
        return res;
    }
    res = EmbeddedBakeResult{};
    // Com o pacote de mapa montado isto nao devia acontecer: o build guardou o
    // pacote de cada modelo que a carga abriu.
    if (vfs::anyMounted())
        ERUPTION_LOG_WARN("[MAPBUNDLE] '%s' sem pacote de texturas valido no .emap: comprimindo agora. "
                          "Reconstrua o mapa (--bake-map)", model.filePath.c_str());
    // .glb de um .tuf com texturas prontas cujo pacote nao serve aqui (outro
    // preset ou GPU): as imagens nao estao no .glb, entao nao ha o que comprimir.
    // Nada de assar nem de gravar cache em disco: o erro diz o que refazer.
    if (vfs::tufGpuReady(model.filePath)) {
        ERUPTION_LOG_ERROR("[TUFGPU] '%s': o pacote .tuf traz as texturas prontas, mas nao as deste preset/GPU "
                           "(teto %u, formatos). Reempacote com --gpu-ready para este preset", model.filePath.c_str(),
                           maxTextureSize);
        return EmbeddedBakeResult{};
    }
    return bakeFresh(model, maxTextureSize, synthesizePbr, true);
}

namespace {
// Bake sem cache (o pack falta, ou o .glb/preset/GPU mudou).
EmbeddedBakeResult bakeFresh(const ModelFile& model, uint32_t maxTextureSize, bool synthesizePbr, bool writeCacheFile) {
    EmbeddedBakeResult res;
    bcCompressorInit();
    const bool dumpRequested = std::getenv("ERUPTION_TEST_DUMP_TEX_DIR") != nullptr;

    // CONTEXTO DE MATERIAL pra' sintese de altura: a imagem e' ORGANICA se
    // algum material que a usa for grama/vegetacao/terra/neve na tabela de
    // perfis (pbrTextureNames guarda o nome do material, textureNames o da
    // imagem embutida, no mesmo indice).
    std::unordered_map<std::string, bool> organicImage;
    for (const auto& node : model.nodes) {
        for (size_t k = 0; k < node.textureNames.size() && k < node.pbrTextureNames.size(); ++k) {
            const std::string& cat = getPbrProfile(node.pbrTextureNames[k]).category;
            if (cat == "grass" || cat == "vegetation" || cat == "dirt" || cat == "snow")
                organicImage[node.textureNames[k]] = true;
        }
    }

    // Imagem cujo material ja' acha o PBR entregue em disco pelo NOME DO
    // MATERIAL ("Tr-wood6.png" -> Tr-wood6_mrahw.png): o desenho usa esse mapa
    // (ModelRenderer: o nome do material vence o da imagem), entao sintetizar
    // a partir da cor so' gastava tempo no primeiro carregamento. O nome da
    // imagem embutida ("#modelo_slot_crc") nunca acha arquivo. Medido no
    // vilarejo (lpkit, 39 modelos): ~150 imagens sintetizadas, ~20 s de CPU.
    std::unordered_map<std::string, bool> materialHasDiskPbr;
    for (const auto& node : model.nodes) {
        for (size_t k = 0; k < node.textureNames.size() && k < node.pbrTextureNames.size(); ++k) {
            const std::string& mat = node.pbrTextureNames[k];
            if (mat.empty() || mat == node.textureNames[k]) continue;
            bool& has = materialHasDiskPbr[node.textureNames[k]];
            if (!has) has = resolvePbrTexturePaths(mat).valid;
        }
    }

    // Peso de cada textura na cor de bounce: area voltada para cima.
    const auto upArea = computeUpFacingArea(model);
    double maxUpArea = 0.0;
    for (const auto& kv : upArea) maxUpArea = std::max(maxUpArea, kv.second);
    auto textureGroundWeight = [&](const std::string& texName) -> float {
        // Agua e metal ficam de fora mesmo sendo horizontais: sao praticamente
        // especulares e refletem a CENA, nao uma cor difusa propria. Entrar na
        // media so' tinge tudo de azul ou cinza.
        const std::string& cat = getPbrProfile(texName).category;
        if (cat == "water" || cat == "metal") return 0.0f;

        if (maxUpArea <= 0.0) return 1.0f; // sem geometria util: media antiga
        const auto it = upArea.find(texName);
        if (it == upArea.end()) return 0.0f;
        // Raiz da fracao de area: uma unica textura de chao dominante nao
        // apaga completamente as demais, mas parede e ceu continuam perto de 0.
        return static_cast<float>(std::sqrt(it->second / maxUpArea));
    };

    const auto t0 = std::chrono::steady_clock::now();
    const size_t n = model.embeddedTextures.size();
    std::vector<BakedEmbeddedTexture> baked(n);
    std::vector<std::string> names(n);
    // Ground-albedo accumulation happens here because this is the only place
    // the decoded pixels exist once the RGBA8 path is gone.
    std::vector<double> sumR(n, 0.0), sumG(n, 0.0), sumB(n, 0.0), sumN(n, 0.0);
    std::atomic<size_t> compressed{0};
    std::atomic<uint64_t> synthMicros{0};
    std::atomic<size_t> synthesized{0};
    std::atomic<size_t> fromDisk{0};
    std::atomic<size_t> pbrFromAlt{0};
    const NeuralPbrStats statsBefore = neuralPbrStats();

    JobSystem::instance().parallelFor(static_cast<uint32_t>(n), [&](uint32_t i) {
        const EmbeddedTexture& et = model.embeddedTextures[i];
        names[i] = et.name;

        std::vector<uint8_t> pixels;
        int w = 0, h = 0, ch = 4;
        if (et.isDecoded()) {
            pixels = et.pixels;
            w = et.width; h = et.height; ch = et.channels > 0 ? et.channels : 4;
        } else if (et.hasEncodedData()) {
            ImageData img = ImageUtils::loadFromMemoryRaw(et.encodedData.data(), et.encodedData.size());
            if (!img.isValid()) return;
            pixels = std::move(img.pixels);
            w = img.width; h = img.height; ch = img.channels > 0 ? img.channels : 4;
        } else {
            return;
        }
        if (ch != 4 || w <= 0 || h <= 0) return;

        const int origW = w, origH = h; // antes do teto: a fonte alternativa de PBR tem o tamanho original
        // Resolution cap (preset): the 2048^2 sheets are most of the pixels.
        ImageUtils::downscaleRGBA(w, h, pixels, static_cast<int>(maxTextureSize));

        // --- Cor da luz refletida pelo chao ---------------------------------
        // Esta media alimenta DOIS consumidores reais: o hemisferio inferior do
        // ambiente e o termo de "GI de um bounce" do passe direcional. Ou seja,
        // e' literalmente a cor da luz que sobe do chao e ilumina a parte de
        // baixo de tudo.
        //
        // Ela era a media de TODAS as texturas embutidas do GLB, com peso
        // igual: ceu, agua, metal, interface, sprite, tudo. Num mapa vulcanico
        // - vermelho e marrom no chao - o resultado saia cinza-arroxeado,
        // porque o azul da agua e o cinza do metal puxavam a media para longe
        // do que o chao realmente e'.
        //
        // Agora pesa por CATEGORIA do material, que ja' esta' autorada em
        // assets/data/pbr_materials.json. O criterio e' fisico: so' contribui
        // quem de fato devolve luz DIFUSA para cima.
        const float weight = textureGroundWeight(et.name);
        if (weight > 0.0f) {
            for (size_t p = 0; p + 1 < static_cast<size_t>(w) * h; p += 97) {
                const uint8_t* px = &pixels[p * 4];
                if (px[0] > 230 && px[1] < 25 && px[2] > 230) continue; // chroma key magenta
                if (px[0] + px[1] + px[2] < 24) continue;               // preto = vazio
                sumR[i] += px[0] * weight; sumG[i] += px[1] * weight;
                sumB[i] += px[2] * weight; sumN[i] += weight;
            }
        }

        bool hasAlpha = false;
        for (size_t p = 3; p < pixels.size(); p += 4) {
            if (pixels[p] < 250) { hasAlpha = true; break; }
        }
        if (bcMask() & 1u) {
            baked[i].albedo = compressRgbaToEtex(pixels.data(), w, h, BcKind::Albedo, hasAlpha);
            baked[i].albedoOpaque = !hasAlpha;
        }

        { // PBR: o do disco sempre; a rede so' com a sintese ligada
            PbrTextureData mrahw, normal;
            const auto itOrg = organicImage.find(et.name);
            const bool organic = itOrg != organicImage.end() && itOrg->second;
            const auto synthStart = std::chrono::steady_clock::now();
            // Mapa PBR pronto no disco (_mrahw/_normal ao lado do albedo
            // original) vence: a rede so' roda para a textura que nao tem.
            const bool doDisco = loadDiskPbrRgba(et.name, mrahw, normal);
            const auto itMat = materialHasDiskPbr.find(et.name);
            const bool porMaterial = !doDisco && itMat != materialHasDiskPbr.end() && itMat->second;
            bool synthOk = doDisco;
            if (doDisco || porMaterial) ++fromDisk;
            else if (synthesizePbr) {
                std::vector<uint8_t> alt = pbrSourcePixels(et, origW, origH);
                if (!alt.empty()) {
                    int aw = origW, ah = origH;
                    ImageUtils::downscaleRGBA(aw, ah, alt, static_cast<int>(maxTextureSize));
                    ++pbrFromAlt;
                    synthOk = synthesizePbrFromPixels(alt, w, h, 4, mrahw, normal, organic);
                } else {
                    synthOk = synthesizePbrFromPixels(pixels, w, h, 4, mrahw, normal, organic);
                }
            }
            synthMicros += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - synthStart).count());
            if (synthOk && !doDisco) ++synthesized;
            if (synthOk && mrahw.valid() && normal.valid()) {
                // DUMP CRU (ERUPTION_TEST_DUMP_TEX_DIR): pedido do autor pra
                // OLHAR o MRAH-W como ele sai do bake, sem passar por shader
                // nenhum - "talvez o problema seja esse mrahw". So' texturas
                // com area voltada pra cima acima do limiar (candidatas a
                // chao/piso; ERUPTION_TEST_DUMP_TEX_MINAREA, default 2% da
                // maior) pra nao despejar as ~292 texturas do mapa inteiro.
                if (dumpRequested) {
                    static const float kMinAreaFrac = [] {
                        const char* e = std::getenv("ERUPTION_TEST_DUMP_TEX_MINAREA");
                        return e ? std::stof(e) : 0.02f;
                    }();
                    // ERUPTION_TEST_DUMP_TEX_NAME=<substr>: pega pelo NOME em
                    // vez de area pra cima - a heuristica de area (acima)
                    // pode nao pegar o material certo quando ele perde de
                    // area total pra folhagem repetida no mapa inteiro (visto
                    // num mapa de teste: uma textura de arvore saiu com fracao 1.0). Combina com
                    // um match de posicao (ERUPTION_TEST_AABB_STATS no
                    // ModelRenderer) pra descobrir o nome primeiro.
                    static const char* kNameFilter = std::getenv("ERUPTION_TEST_DUMP_TEX_NAME");
                    const auto itArea = upArea.find(et.name);
                    const double frac = (itArea != upArea.end() && maxUpArea > 0.0)
                                       ? (itArea->second / maxUpArea) : 0.0;
                    const bool nameHit = kNameFilter && et.name.find(kNameFilter) != std::string::npos;
                    if (nameHit || frac >= kMinAreaFrac) {
                        const std::string dir = std::getenv("ERUPTION_TEST_DUMP_TEX_DIR");
                        std::error_code dec;
                        fs::create_directories(dir, dec);
                        const std::string base = dir + "/" + sanitizeForFilename(et.name);

                        std::vector<uint8_t> albRgb(static_cast<size_t>(w) * h * 3);
                        for (size_t p = 0; p < static_cast<size_t>(w) * h; ++p) {
                            albRgb[p * 3 + 0] = pixels[p * 4 + 0];
                            albRgb[p * 3 + 1] = pixels[p * 4 + 1];
                            albRgb[p * 3 + 2] = pixels[p * 4 + 2];
                        }
                        writeBmp24(base + "_albedo.bmp", w, h, albRgb);

                        const size_t mn = static_cast<size_t>(mrahw.width) * mrahw.height;
                        std::vector<uint8_t> mrahwRgb(mn * 3), heightGray(mn * 3);
                        for (size_t p = 0; p < mn; ++p) {
                            mrahwRgb[p * 3 + 0] = mrahw.pixels[p * 4 + 0]; // metallic
                            mrahwRgb[p * 3 + 1] = mrahw.pixels[p * 4 + 1]; // roughness
                            mrahwRgb[p * 3 + 2] = mrahw.pixels[p * 4 + 2]; // AO/cavidade
                            const uint8_t a = mrahw.pixels[p * 4 + 3];    // altura (255=sem dado)
                            heightGray[p * 3 + 0] = heightGray[p * 3 + 1] = heightGray[p * 3 + 2] = a;
                        }
                        writeBmp24(base + "_mrahw_rgb.bmp", mrahw.width, mrahw.height, mrahwRgb);
                        writeBmp24(base + "_height_alpha.bmp", mrahw.width, mrahw.height, heightGray);
                        std::vector<uint8_t> nrmRgb(static_cast<size_t>(normal.width) * normal.height * 3);
                        for (size_t p = 0; p < static_cast<size_t>(normal.width) * normal.height; ++p)
                            for (int c = 0; c < 3; ++c) nrmRgb[p * 3 + c] = normal.pixels[p * 4 + c];
                        writeBmp24(base + "_normal.bmp", normal.width, normal.height, nrmRgb);

                        ERUPTION_LOG_WARN("[TEXDUMP] %s upAreaFrac=%.3f -> %s_*.bmp",
                                          et.name.c_str(), frac, base.c_str());
                    }
                }
                // Normal: BC7, not BC5. This engine's normal maps are NOT unit
                // length (flat blue ~239) and their alpha carries materialProps,
                // so the two-channel BC5 + z-reconstruction convention would
                // change the look. BC7 keeps all four channels at the same
                // 1 byte/texel, and is still 4x smaller than RGBA8.
                if (bcMask() & 2u) {
                    baked[i].normal = compressRgbaToEtex(normal.pixels.data(), normal.width,
                                                         normal.height, BcKind::Normal);
                    // GPU sem BC7 (Exynos/Xclipse): sem isto o normal ficava fora
                    // do pacote e a carga sintetizava todos de novo na CPU, um a
                    // um (17 s no S24). Cru e' 4x maior, mas carrega na hora.
                    if (!baked[i].normal.valid())
                        baked[i].normal = rgbaToEtexRaw(normal.pixels.data(), normal.width, normal.height);
                }
                if (bcMask() & 4u) {
                    // Variancia da normal por mip (Toksvig) - broadening de
                    // roughness. Calculada mesmo que o normal map em si NAO
                    // seja BC-comprimido (bcMask bit 2 desligado): e' so'
                    // matematica sobre os pixels que ja' estao em memoria,
                    // nao depende do resultado comprimido.
                    auto normalVariance = computeNormalMipVariance(normal.pixels.data(),
                                                                    normal.width, normal.height);
                    // ERUPTION_DEBUG_TOKSVIG=1: confirma que a roughness
                    // sintetizada deixou de ser constante (204 = ~0.8 fixo)
                    // e passou a variar por mip - sem isso e' dificil ver o
                    // efeito olhando so' o mip0 de perto (r=1.0 sempre ali).
                    static const bool kDebugToksvig = std::getenv("ERUPTION_DEBUG_TOKSVIG") != nullptr;
                    if (kDebugToksvig) {
                        for (size_t lvl = 0; lvl < normalVariance.size(); ++lvl) {
                            double sumR = 0.0;
                            for (float v : normalVariance[lvl]) sumR += v;
                            const double avgR = sumR / std::max<size_t>(1, normalVariance[lvl].size());
                            ERUPTION_LOG_WARN("[TOKSVIG] %s mip%zu r_medio=%.4f (n=%zu)",
                                              et.name.c_str(), lvl, avgR, normalVariance[lvl].size());
                        }
                    }
                    baked[i].mrahw = compressRgbaToEtex(mrahw.pixels.data(), mrahw.width,
                                                        mrahw.height, BcKind::Data,
                                                        /*hasAlpha=*/false, &normalVariance);
                }
            }
        }
        compressed.fetch_add(1, std::memory_order_relaxed);
    });

    double r = 0, g = 0, b = 0, cnt = 0;
    for (size_t i = 0; i < n; ++i) { r += sumR[i]; g += sumG[i]; b += sumB[i]; cnt += sumN[i]; }
    if (cnt > 100) {
        res.groundAlbedo[0] = static_cast<float>(r / cnt) / 255.0f;
        res.groundAlbedo[1] = static_cast<float>(g / cnt) / 255.0f;
        res.groundAlbedo[2] = static_cast<float>(b / cnt) / 255.0f;
        res.hasGroundAlbedo = true;
    }

    for (size_t i = 0; i < n; ++i) {
        if (names[i].empty()) continue;
        if (!baked[i].albedo.valid() && !baked[i].normal.valid() && !baked[i].mrahw.valid())
            continue;
        res.bytes += baked[i].albedo.byteSize() + baked[i].normal.byteSize()
                   + baked[i].mrahw.byteSize();
        res.textures.emplace(names[i], std::move(baked[i]));
    }

    const double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
    ERUPTION_LOG_WARN("EmbeddedTextureBake: comprimiu %zu/%zu texturas embutidas em %.0f ms "
                      "(%.1f MB de payload, familia %s, teto %u)",
                      compressed.load(), n, ms,
                      static_cast<double>(res.bytes) / (1024.0 * 1024.0),
                      texfmt::modeName(texfmt::family(texfmt::deviceCaps(), texfmt::options())), maxTextureSize);
    if (synthesizePbr) logSynthesisLevels(statsBefore, synthesized.load(), synthMicros.load());
    if (pbrFromAlt.load() > 0)
        ERUPTION_LOG_WARN("EmbeddedTextureBake: PBR sintetizado de fonte alternativa (ERUPTION_TUF_PBR_SOURCE) em %zu texturas", pbrFromAlt.load());
    if (fromDisk.load() > 0)
        ERUPTION_LOG_WARN("EmbeddedTextureBake: %zu de %zu texturas com PBR do disco (sem rede)", fromDisk.load(), n);

    if (writeCacheFile && !model.filePath.empty() && !res.textures.empty() && !testNoWrite()) {
        writeCache(model.filePath, maxTextureSize, synthesizePbr, res);
    }
    return res;
}

} // namespace

} // namespace eruption
