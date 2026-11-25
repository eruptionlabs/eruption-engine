#include "game/MapBundleBaker.hpp"

#include "core/Engine.hpp"
#include "core/Logger.hpp"
#include "formats/GltfParser.hpp"
#include "renderer/ModelRenderer.hpp"
#include "utils/AssetVfs.hpp"
#include "utils/EmbeddedTextureBake.hpp"
#include "utils/ProbeCachePath.hpp"

#include <atomic>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>

namespace eruption {

namespace {
std::atomic<int> g_exitCode{0};

// Carga assentada: nenhum caminho novo pedido por este tempo, sem relevo na fila.
constexpr double kQuietSeconds = 4.0;
constexpr int kMinFrames = 240;
// Teto: um mapa sem pacote nenhum leva 50-150 s; ninguem espera meia hora.
constexpr double kMaxSeconds = 30.0 * 60.0;
} // namespace

int MapBundleBaker::exitCode() { return g_exitCode.load(); }

MapBundleBaker::MapBundleBaker(Engine& engine, Options options) : m_engine(engine), m_opt(std::move(options)) {
    vfs::setBundlesEnabled(false); // o build le' do disco e regenera o que faltar
    vfs::beginRecord();
    m_start = m_lastChange = m_lastLog = Clock::now();
    ERUPTION_LOG_WARN("[BAKEMAP] %s: abrindo o mapa e anotando o que a carga le'", m_opt.map.c_str());
}

void MapBundleBaker::update() {
    if (m_done) return;
    ++m_frames;
    const auto now = Clock::now();
    const size_t count = vfs::recordedCount();
    if (count != m_lastCount) {
        m_lastCount = count;
        m_lastChange = now;
    }
    const size_t relief = m_engine.modelRenderer().pendingReliefJobs();
    const double total = std::chrono::duration<double>(now - m_start).count();
    if (std::chrono::duration<double>(now - m_lastLog).count() >= 10.0) {
        m_lastLog = now;
        ERUPTION_LOG_WARN("[BAKEMAP] %.0f s: %zu arquivos anotados, %zu relevo(s) na fila", total, count, relief);
    }
    const double quiet = std::chrono::duration<double>(now - m_lastChange).count();
    if (m_frames >= kMinFrames && relief == 0 && quiet >= kQuietSeconds) finish(false);
    else if (total >= kMaxSeconds) finish(true);
}

void MapBundleBaker::finish(bool timedOut) {
    m_done = true;
    if (timedOut) ERUPTION_LOG_WARN("[BAKEMAP] %s: a carga nao assentou em 30 min; gravando o que foi anotado", m_opt.map.c_str());
    // Sondas: no PC a carga as coze (e grava no cache do usuario, ou ao lado do .glb se o
    // projeto pede) em vez de ler; o celular so' le' - entram no pacote como
    // <mapa>.glb.probes, saindo de onde a carga as gravou.
    std::vector<vfs::Recorded> files = vfs::endRecord();
    const std::string glbTail = "/" + m_opt.map + ".glb";
    std::vector<vfs::Recorded> extra;
    for (const vfs::Recorded& r : files)
        if (r.path.size() > glbTail.size() && r.path.compare(r.path.size() - glbTail.size(), glbTail.size(), glbTail) == 0) {
            vfs::Recorded probes{r.path + ".probes", 3, std::string()};
            std::error_code ec;
            // O mais novo dos dois (uma grade velha ao lado do .glb nao cobre a que a carga acabou de gravar).
            const std::string user = probecache::userFile(r.path);
            if (!user.empty() && std::filesystem::exists(user, ec) &&
                (!std::filesystem::exists(probes.path, ec) ||
                 std::filesystem::last_write_time(user, ec) > std::filesystem::last_write_time(probes.path, ec)))
                probes.source = user;
            extra.push_back(std::move(probes));
        }
    files.insert(files.end(), extra.begin(), extra.end());

    // Sem imagem duplicada: a imagem embutida que o pacote de texturas do
    // .glb (tambem no .emap) ja' traz comprimida sai do .glb do pacote. So'
    // com o pacote de texturas inteiro no .emap; o .glb do disco fica como esta'.
    std::unordered_set<std::string> packed;
    for (const vfs::Recorded& r : files)
        if (r.access >= 3) packed.insert(r.path);
    std::unordered_map<std::string, std::unordered_set<std::string>> covered;
    for (auto& [glb, c] : recordedPackCoverage())
        if (packed.count(glb) && packed.count(c.pack)) covered[glb].insert(c.images.begin(), c.images.end());
    std::atomic<uint64_t> strippedBytes{0};
    std::atomic<size_t> strippedFiles{0};
    auto stripImages = [&covered, &strippedBytes, &strippedFiles](const std::string& key, std::vector<uint8_t>& bytes) {
        const auto it = covered.find(key);
        if (it == covered.end()) return;
        const std::unordered_set<std::string>& names = it->second;
        const size_t saved = GltfParser::stripEmbeddedImages(
            bytes, std::filesystem::path(key).filename().string(),
            [&names](const std::string& name) { return names.count(name) != 0; });
        if (saved == 0) return;
        strippedBytes += saved;
        ++strippedFiles;
    };

    const std::filesystem::path out = vfs::bundlePathFor(m_opt.map);
    const vfs::WriteReport r = vfs::writeBundle(out, files, m_opt.compress, m_opt.level, stripImages);
    if (!r.error.empty()) {
        ERUPTION_LOG_ERROR("[BAKEMAP] %s: %s", m_opt.map.c_str(), r.error.c_str());
        g_exitCode = 1;
    } else {
        ERUPTION_LOG_WARN("[BAKEMAP] %s gravado: %zu entradas (%zu com dados, %zu so' tamanho/data, %zu ausencias), "
                          "%.1f MB das fontes -> %.1f MB no pacote, em %.1f s (carga + build %.0f s)",
                          out.string().c_str(), r.entries, r.data, r.meta, r.absent, double(r.sourceBytes) / 1048576.0,
                          double(r.storedBytes) / 1048576.0, r.ms / 1000.0,
                          std::chrono::duration<double>(Clock::now() - m_start).count());
        ERUPTION_LOG_WARN("[BAKEMAP] imagens ja' no pacote de texturas tiradas de %zu .glb (de %zu cobertos): %.1f MB",
                          strippedFiles.load(), covered.size(), double(strippedBytes.load()) / 1048576.0);
    }
    m_engine.requestQuit();
}

} // namespace eruption
