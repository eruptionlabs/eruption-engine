#include "formats/PackManager.hpp"
#include "core/Logger.hpp"
#ifdef ERUPTION_HAVE_ZSTD_DECODER
#include "formats/EcpReader.hpp"
#endif
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <chrono>
#include <fstream>
#include <utility>

namespace fs = std::filesystem;

namespace eruption {

static std::string toLower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string PackManager::normalizePath(const std::string& path) {
    std::string out = path;
    std::replace(out.begin(), out.end(), '/', '\\');
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool PackManager::pathContains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

void PackManager::scanDirectory(const std::string& dir, DirCache& cache) {
    cache.rootPath = dir;
    cache.files.clear();
    if (!fs::exists(dir) || !fs::is_directory(dir)) {
        ERUPTION_LOG_WARN("PackManager: directory not found: %s", dir.c_str());
        return;
    }

    try {
        for (const auto& entry : fs::recursive_directory_iterator(dir)) {
            if (!entry.is_regular_file()) continue;
            std::string rel = fs::relative(entry.path(), dir).string();
            cache.files.insert(normalizePath(rel));
        }
    } catch (const std::exception& e) {
        ERUPTION_LOG_WARN("PackManager: error scanning %s: %s", dir.c_str(), e.what());
    }
}

PackManager::PackManager() = default;
PackManager::~PackManager() = default; // ecp::Reader so' e' completo neste arquivo

static bool endsWithEcp(const std::string& p) {
    return p.size() > 4 && toLower(p.substr(p.size() - 4)) == ".ecp";
}

#ifdef ERUPTION_HAVE_ZSTD_DECODER
// Reabre TODOS os pacotes juntos (a sobreposicao precisa da lista inteira).
// Se o novo (`changed`) nao abre, a lista volta a ser a de antes.
bool PackManager::reopenPacksLocked(const std::string& changed) {
    auto reader = std::make_shared<ecp::Reader>();
    std::string err;
    const auto t0 = std::chrono::steady_clock::now();
    if (!reader->Open(m_packPaths, &err)) {
        ERUPTION_LOG_WARN("PackManager: pacote .ecp invalido (%s): %s", changed.c_str(), err.c_str());
        return false;
    }
    m_packBasenames.clear();
    for (const std::string& f : reader->List("")) {
        const size_t slash = f.find_last_of('/');
        m_packBasenames.emplace(slash == std::string::npos ? f : f.substr(slash + 1), f);
    }
    m_packs = std::move(reader);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    ERUPTION_LOG_INFO("PackManager: %zu pacote(s) .ecp, %zu arquivos, aberto em %.1f ms", m_packPaths.size(),
                      m_packs->FileCount(), ms);
    (void)ms; // o log pode estar compilado fora
    return true;
}
#else
bool PackManager::reopenPacksLocked(const std::string& changed) {
    ERUPTION_LOG_WARN("PackManager: %s e' pacote .ecp, mas esta build nao tem o decodificador zstd", changed.c_str());
    return false;
}
#endif

bool PackManager::addPack(const std::string& ecpPath) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_packPaths.push_back(ecpPath);
    if (!reopenPacksLocked(ecpPath)) {
        m_packPaths.pop_back();
        return false;
    }
    ResourceSourceInfo info;
    info.path = ecpPath;
    info.isDirectory = false;
#ifdef ERUPTION_HAVE_ZSTD_DECODER
    info.fileCount = static_cast<uint32_t>(m_packs->PackEntryCount(m_packPaths.size() - 1));
#endif
    m_sources.push_back(info);
    return true;
}

void PackManager::addDirectory(const std::string& dir) {
    if (endsWithEcp(dir)) {
        addPack(dir);
        return;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    DirCache cache;
    scanDirectory(dir, cache);
    m_dirs.push_back(std::move(cache));

    ResourceSourceInfo info;
    info.path = dir;
    info.isDirectory = true;
    info.fileCount = static_cast<uint32_t>(m_dirs.back().files.size());
    m_sources.push_back(info);
}

void PackManager::clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_dirs.clear();
    m_sources.clear();
    m_packPaths.clear();
    m_packBasenames.clear();
    m_packs.reset();
}

// Caminho exato dentro de uma pasta. normalizePath devolve '\\' (convencao do
// motor), que no Linux nao separa pastas: aqui vira '/' e, se o sistema de
// arquivos diferencia maiusculas, tenta tambem o nome como foi pedido.
static fs::path findExactInDir(const std::string& root, const std::string& norm, const std::string& original) {
    auto slashes = [](std::string v) { std::replace(v.begin(), v.end(), '\\', '/'); return v; };
    std::error_code ec;
    fs::path p = fs::path(root) / fs::path(slashes(norm));
    if (fs::is_regular_file(p, ec)) return p;
    p = fs::path(root) / fs::path(slashes(original));
    if (fs::is_regular_file(p, ec)) return p;
    return {};
}

// Arquivo inteiro de uma pasta, ou nullopt-like (found=false) se nao der.
static bool readFileBytes(const fs::path& p, std::vector<uint8_t>& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

std::vector<uint8_t> PackManager::extract(const std::string& filename) const {
    std::shared_ptr<ecp::Reader> packs;
    std::vector<uint8_t> bytes;
    std::string norm = normalizePath(filename);
    std::string baseName;
    {
        size_t lastSlash = norm.find_last_of('\\');
        baseName = toLower(lastSlash != std::string::npos ? norm.substr(lastSlash + 1) : norm);
    }
    std::string packBasenameHit;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        packs = m_packs;
        const bool hasPacks = packs != nullptr;

        // Passo 1: caminho exato em cada pasta (a primeira vence). Sem pacotes,
        // o comportamento antigo: a busca por nome tambem entra aqui, pasta a pasta.
        for (const auto& dir : m_dirs) {
            const fs::path candidate = findExactInDir(dir.rootPath, norm, filename);
            if (!candidate.empty()) {
                if (readFileBytes(candidate, bytes)) return bytes;
                continue;
            }
            if (hasPacks) continue;
            if (scanBasename(dir.rootPath, baseName, filename, bytes)) return bytes;
        }
        if (hasPacks) {
            auto it = m_packBasenames.find(baseName);
            if (it != m_packBasenames.end()) packBasenameHit = it->second;
        }
    }
#ifdef ERUPTION_HAVE_ZSTD_DECODER
    if (packs) {
        // Passo 2: caminho exato nos pacotes (sem trava: o leitor e' imutavel).
        if (packs->ReadInto(filename, bytes)) return bytes;
        // Passo 3: so' o nome do arquivo, nas pastas e depois nos pacotes.
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (const auto& dir : m_dirs)
                if (scanBasename(dir.rootPath, baseName, filename, bytes)) return bytes;
        }
        if (!packBasenameHit.empty() && packs->ReadInto(packBasenameHit, bytes)) return bytes;
    }
#endif
    return {};
}

// Busca sem diferenciar maiusculas pelo nome do arquivo em toda a arvore (o que
// os callers com nome solto esperam). Cada falha de uma pasta e' uma varredura
// completa: so' roda depois dos caminhos exatos.
bool PackManager::scanBasename(const std::string& root, const std::string& lowerBase,
                               const std::string& filename, std::vector<uint8_t>& out) {
    try {
        for (const auto& entry : fs::recursive_directory_iterator(root)) {
            if (!entry.is_regular_file()) continue;
            if (toLower(entry.path().filename().string()) != lowerBase) continue;
            if (readFileBytes(entry.path(), out)) return true;
        }
    } catch (const std::exception& e) {
        ERUPTION_LOG_WARN("PackManager: extract fallback scan failed for %s: %s", filename.c_str(), e.what());
    }
    return false;
}

std::vector<uint8_t> PackManager::extractSafe(const std::string& filename) const {
    return extract(filename);
}

bool PackManager::exists(const std::string& filename) const {
    // Caminho exato sem ler o arquivo (o pacote nem descomprime); so' quando
    // nao acha cai na busca por nome, que le.
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const std::string norm = normalizePath(filename);
        for (const auto& dir : m_dirs) {
            if (!findExactInDir(dir.rootPath, norm, filename).empty()) return true;
        }
#ifdef ERUPTION_HAVE_ZSTD_DECODER
        if (m_packs && m_packs->Exists(filename)) return true;
#endif
    }
    return !extract(filename).empty();
}

#ifdef ERUPTION_HAVE_ZSTD_DECODER
// Os nomes do pacote vem com '/': o resto do motor usa '\\' (como as pastas).
static std::vector<std::string> appendPackNames(std::vector<std::string> into, std::vector<std::string> names) {
    for (std::string& n : names) {
        std::replace(n.begin(), n.end(), '/', '\\');
        into.push_back(std::move(n));
    }
    return into;
}
#endif

std::vector<std::string> PackManager::getAllFilenames() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<std::string> result;
    for (const auto& dir : m_dirs) {
        for (const auto& f : dir.files) {
            result.push_back(f);
        }
    }
#ifdef ERUPTION_HAVE_ZSTD_DECODER
    if (m_packs) result = appendPackNames(result, m_packs->List(""));
#endif
    return result;
}

std::vector<std::string> PackManager::getFilenamesByPrefix(const std::string& prefix) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<std::string> result;
    std::string normPrefix = normalizePath(prefix);

    for (const auto& dir : m_dirs) {
        for (const auto& f : dir.files) {
            if (f.size() >= normPrefix.size() &&
                std::mismatch(normPrefix.begin(), normPrefix.end(), f.begin()).first == normPrefix.end()) {
                result.push_back(f);
            }
        }
    }
#ifdef ERUPTION_HAVE_ZSTD_DECODER
    if (m_packs) result = appendPackNames(result, m_packs->List(prefix));
#endif
    return result;
}

} // namespace eruption
