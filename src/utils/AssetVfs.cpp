#include "utils/AssetVfs.hpp"

#include "core/JobSystem.hpp"
#include "core/Logger.hpp"
#include "core/Project.hpp"
#include "utils/ReadWholeFile.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#ifdef ERUPTION_HAVE_ZSTD
#include <zstd.h>
#endif
#ifdef ERUPTION_HAVE_ZSTD_DECODER
#include "formats/EcpReader.hpp"
#include "tufo/Archive.hpp"
#endif

#ifndef _WIN32
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace eruption {
namespace vfs {

namespace fs = std::filesystem;

namespace {

constexpr char kMagic[8] = {'E', 'R', 'U', 'P', 'E', 'M', 'A', 'P'};
constexpr uint32_t kVersion = 1;
constexpr size_t kHeaderSize = 64;
constexpr size_t kEntryFixed = 3 + 6 * 8; // raiz, tipo, codec + seis campos de 64 bits

enum Kind : uint8_t { kData = 0, kMeta = 1, kAbsent = 2, kDir = 3 };
enum Codec : uint8_t { kRaw = 0, kZstd = 1 };

// Hash por palavra (o FNV byte a byte custava ~1 s por GB no build do pacote).
uint64_t hash64(const uint8_t* p, size_t n) {
    uint64_t h = 0x9E3779B97F4A7C15ull ^ n;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        std::memcpy(&w, p + i, 8);
        h = (h ^ w) * 0xff51afd7ed558ccdull;
        h ^= h >> 32;
    }
    for (; i < n; ++i) h = (h ^ p[i]) * 0x100000001b3ull;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ull;
    h ^= h >> 33;
    return h;
}

// Tamanho e data do arquivo no disco. A data e' uma marca opaca (ns desde a
// epoca no POSIX): so' e' comparada com outra marca daqui.
bool diskStat(const std::string& path, uint64_t& size, int64_t& mtime, bool& isDir) {
#ifndef _WIN32
    struct ::stat st {};
    if (::stat(path.c_str(), &st) != 0) return false;
    size = static_cast<uint64_t>(st.st_size);
#if defined(__APPLE__)
    mtime = int64_t(st.st_mtimespec.tv_sec) * 1000000000ll + st.st_mtimespec.tv_nsec;
#else
    mtime = int64_t(st.st_mtim.tv_sec) * 1000000000ll + st.st_mtim.tv_nsec;
#endif
    isDir = S_ISDIR(st.st_mode);
    return true;
#else
    std::error_code ec;
    const fs::file_status s = fs::status(path, ec);
    if (ec || !fs::exists(s)) return false;
    isDir = fs::is_directory(s);
    size = isDir ? 0 : static_cast<uint64_t>(fs::file_size(path, ec));
    mtime = static_cast<int64_t>(fs::last_write_time(path, ec).time_since_epoch().count());
    return true;
#endif
}

std::string keyOf(const fs::path& p) {
    if (p.is_absolute()) return p.lexically_normal().generic_string();
    std::error_code ec;
    return (fs::current_path(ec) / p).lexically_normal().generic_string();
}

bool derivedCache(const std::string& key) {
    auto ends = [&key](const char* s) {
        const size_t n = std::strlen(s);
        return key.size() >= n && key.compare(key.size() - n, n, s) == 0;
    };
    return ends(".etexpack") || ends(".probes") || ends(".etex") || key.find("/.cache/") != std::string::npos;
}

struct Entry {
    uint64_t offset = 0, stored = 0, raw = 0, hash = 0, srcSize = 0;
    int64_t srcMtime = 0;
    uint8_t kind = kData, codec = kRaw;
    bool stale = false;
};

// ERUPTION_TEST_MAP_BUNDLE_VERIFY=1 (teste): confere o hash de cada entrada lida.
bool verifyReads() {
    static const bool on = std::getenv("ERUPTION_TEST_MAP_BUNDLE_VERIFY") != nullptr;
    return on;
}

class Bundle {
public:
    std::string file;
    std::string stem;
    std::unordered_map<std::string, Entry> entries;
    uint64_t dataBegin = 0, dataEnd = 0;
    int64_t fileMtime = 0;

    ~Bundle() {
        m_stop = true;
        if (m_prefetch.joinable()) m_prefetch.join();
#ifndef _WIN32
        if (m_fd >= 0) ::close(m_fd);
#else
        if (m_file) std::fclose(m_file);
#endif
    }

    bool open(const fs::path& emap, std::string& err) {
        file = emap.string();
#ifndef _WIN32
        m_fd = ::open(file.c_str(), O_RDONLY | O_CLOEXEC);
        if (m_fd < 0) return fail(err, "nao abriu");
#else
        m_file = std::fopen(file.c_str(), "rb");
        if (!m_file) return fail(err, "nao abriu");
#endif
        uint8_t head[kHeaderSize];
        if (!readAt(0, head, kHeaderSize) || std::memcmp(head, kMagic, 8) != 0) return fail(err, "cabecalho invalido");
        uint32_t version = 0, count = 0;
        uint64_t tocOff = 0, tocSize = 0, tocHash = 0;
        std::memcpy(&version, head + 8, 4);
        std::memcpy(&count, head + 12, 4);
        std::memcpy(&tocOff, head + 16, 8);
        std::memcpy(&tocSize, head + 24, 8);
        std::memcpy(&dataBegin, head + 32, 8);
        std::memcpy(&dataEnd, head + 40, 8);
        std::memcpy(&tocHash, head + 48, 8);
        dataEnd += dataBegin;
        if (version != kVersion) return fail(err, "versao diferente");
        if (tocSize > (256ull << 20)) return fail(err, "indice grande demais");
        std::vector<uint8_t> toc(static_cast<size_t>(tocSize));
        if (!readAt(tocOff, toc.data(), toc.size()) || hash64(toc.data(), toc.size()) != tocHash)
            return fail(err, "indice corrompido");
        size_t c = 0;
        auto need = [&](size_t n) { return c + n <= toc.size(); };
        uint32_t nroots = 0;
        if (!need(4)) return fail(err, "indice curto");
        std::memcpy(&nroots, toc.data() + c, 4);
        c += 4;
        const fs::path emapDir = emap.parent_path();
        std::vector<fs::path> roots;
        for (uint32_t i = 0; i < nroots; ++i) {
            uint16_t n = 0;
            if (!need(2)) return fail(err, "indice curto");
            std::memcpy(&n, toc.data() + c, 2);
            c += 2;
            if (!need(n)) return fail(err, "indice curto");
            const std::string r(reinterpret_cast<const char*>(toc.data() + c), n);
            c += n;
            roots.push_back(resolveRoot(r, emapDir));
        }
        entries.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            uint16_t n = 0;
            if (!need(2)) return fail(err, "indice curto");
            std::memcpy(&n, toc.data() + c, 2);
            c += 2;
            if (!need(n + kEntryFixed)) return fail(err, "indice curto");
            const std::string rel(reinterpret_cast<const char*>(toc.data() + c), n);
            c += n;
            Entry e;
            const uint8_t root = toc[c];
            e.kind = toc[c + 1];
            e.codec = toc[c + 2];
            c += 3;
            std::memcpy(&e.offset, toc.data() + c, 8);
            std::memcpy(&e.stored, toc.data() + c + 8, 8);
            std::memcpy(&e.raw, toc.data() + c + 16, 8);
            std::memcpy(&e.hash, toc.data() + c + 24, 8);
            std::memcpy(&e.srcSize, toc.data() + c + 32, 8);
            std::memcpy(&e.srcMtime, toc.data() + c + 40, 8);
            c += 48;
            if (root >= roots.size()) return fail(err, "raiz invalida");
            if (e.kind == kData && (e.offset < dataBegin || e.offset + e.stored > dataEnd)) return fail(err, "entrada fora dos dados");
            entries.emplace(keyOf(roots[root] / rel), e);
        }
        return true;
    }

    // Fracao das paginas dos dados no cache do sistema (mincore), acima de 90%.
    bool mostlyCached() const {
#ifndef _WIN32
        const long page = ::sysconf(_SC_PAGESIZE);
        if (page <= 0 || dataEnd <= dataBegin) return false;
        const uint64_t first = dataBegin / static_cast<uint64_t>(page) * static_cast<uint64_t>(page);
        const size_t len = static_cast<size_t>(dataEnd - first);
        void* map = ::mmap(nullptr, len, PROT_READ, MAP_SHARED, m_fd, static_cast<off_t>(first));
        if (map == MAP_FAILED) return false;
        std::vector<unsigned char> vec((len + static_cast<size_t>(page) - 1) / static_cast<size_t>(page));
        size_t resident = 0;
        if (::mincore(map, len, vec.data()) == 0)
            for (unsigned char v : vec) resident += v & 1u;
        ::munmap(map, len);
        return resident * 10 >= vec.size() * 9;
#else
        return false;
#endif
    }

    // Leitura em sequencia de todo o pacote, na ordem da carga: quem pede uma
    // entrada espera a leitura passar por ela e pega do cache do sistema. Com
    // o disco frio isto troca centenas de buscas da cabeca por uma leitura so'.
    void startPrefetch() {
        if (mostlyCached()) {
            // Pacote ja' na memoria (troca de volta para um mapa visitado):
            // a leitura em sequencia so' copiaria memoria (2 GB no maior mapa de teste) e
            // faria as leituras logo a' frente dela esperarem.
            m_prefetchDone = true;
            return;
        }
        m_prefetch = std::thread([this] {
#if !defined(_WIN32) && !defined(__APPLE__)
            ::posix_fadvise(m_fd, static_cast<off_t>(dataBegin), static_cast<off_t>(dataEnd - dataBegin),
                            POSIX_FADV_SEQUENTIAL);
#endif
            std::vector<uint8_t> chunk(8u << 20);
            uint64_t off = dataBegin;
            while (off < dataEnd && !m_stop) {
                const size_t n = static_cast<size_t>(std::min<uint64_t>(chunk.size(), dataEnd - off));
                if (!readAt(off, chunk.data(), n)) break;
                off += n;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_cursor = off;
                }
                m_cv.notify_all();
            }
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_prefetchDone = true;
            }
            m_cv.notify_all();
        });
    }

    bool readEntry(const std::string& key, const Entry& e, std::vector<uint8_t>& out) {
        const auto t0 = std::chrono::steady_clock::now();
        uint64_t cursorAtAsk = 0;
        bool ahead = false; // longe a' frente da leitura em sequencia: vai ao disco
        {
            // Logo a' frente da leitura em sequencia: espera ela chegar (uma
            // leitura so' no disco). Longe dela (a carga pediu fora da ordem do
            // build): le' direto - esperar o pacote inteiro passar custou 43 s
            // num warp para um mapa de teste (a cena e' lida bem depois no build).
            constexpr uint64_t kWaitWindow = 96ull << 20;
            const uint64_t end = e.offset + e.stored;
            std::unique_lock<std::mutex> lock(m_mutex);
            cursorAtAsk = m_prefetchDone ? dataEnd : std::max(m_cursor, dataBegin);
            ahead = !m_prefetchDone && m_cursor < end && end - m_cursor > kWaitWindow;
            if (!m_prefetchDone && m_cursor < end && !ahead)
                m_cv.wait(lock, [&] { return m_prefetchDone || m_cursor >= end; });
        }
        // Com o disco frio, as leituras diretas vao uma por vez (como a trava
        // do disco fazia antes do pacote): em paralelo elas e a leitura em
        // sequencia disputavam a cabeca do HDD. A descompressao fica fora.
        std::unique_lock<std::mutex> direct(m_directMutex, std::defer_lock);
        if (ahead) direct.lock();
        const auto t1 = std::chrono::steady_clock::now();
        bool ok = false;
        if (e.codec == kRaw) {
            out.resize(static_cast<size_t>(e.raw));
            ok = e.stored == e.raw && readAt(e.offset, out.data(), out.size());
        } else if (e.codec == kZstd) {
#ifdef ERUPTION_HAVE_ZSTD
            std::vector<uint8_t> packed(static_cast<size_t>(e.stored));
            const bool got = readAt(e.offset, packed.data(), packed.size());
            if (direct.owns_lock()) direct.unlock();
            if (got) {
                out.resize(static_cast<size_t>(e.raw));
                thread_local std::unique_ptr<ZSTD_DCtx, size_t (*)(ZSTD_DCtx*)> dctx(ZSTD_createDCtx(), ZSTD_freeDCtx);
                const size_t n = ZSTD_decompressDCtx(dctx.get(), out.data(), out.size(), packed.data(), packed.size());
                ok = !ZSTD_isError(n) && n == e.raw;
            }
#endif
        }
        slowReadLog(key, e, cursorAtAsk, t0, t1);
        if (ok && verifyReads() && hash64(out.data(), out.size()) != e.hash) {
            ERUPTION_LOG_ERROR("[MAPBUNDLE] %s: entrada corrompida (hash)", file.c_str());
            ok = false;
        }
        if (!ok) out.clear();
        return ok;
    }

private:
    // ERUPTION_TEST_VFS_LOG=1: entrada que levou mais de 250 ms (espera pela
    // leitura em sequencia + leitura/descompressao) - mostra quando a ordem
    // da carga se afasta da ordem do build.
    void slowReadLog(const std::string& key, const Entry& e, uint64_t cursor, std::chrono::steady_clock::time_point t0,
                     std::chrono::steady_clock::time_point t1) const {
        static const bool on = std::getenv("ERUPTION_TEST_VFS_LOG") != nullptr;
        if (!on) return;
        const auto t2 = std::chrono::steady_clock::now();
        const double wait = std::chrono::duration<double, std::milli>(t1 - t0).count();
        const double read = std::chrono::duration<double, std::milli>(t2 - t1).count();
        if (wait + read < 250.0) return;
        ERUPTION_LOG_WARN("[VFS] lenta: %s (%.1f MB em %.0f MB, leitura em sequencia em %.0f MB): espera %.0f ms, "
                          "leitura %.0f ms",
                          key.c_str(), double(e.stored) / 1048576.0, double(e.offset - dataBegin) / 1048576.0,
                          double(cursor - dataBegin) / 1048576.0, wait, read);
    }
#ifndef _WIN32
    int m_fd = -1;
#else
    std::FILE* m_file = nullptr;
    std::mutex m_fileMutex;
#endif
    std::thread m_prefetch;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    uint64_t m_cursor = 0;
    bool m_prefetchDone = false;
    std::mutex m_directMutex; // leituras fora da ordem com o disco frio (readEntry)
    std::atomic<bool> m_stop{false};

    static bool fail(std::string& err, const char* why) {
        err = why;
        return false;
    }

    static fs::path resolveRoot(const std::string& r, const fs::path& emapDir) {
        const size_t bar = r.find('|');
        const std::string tag = bar == std::string::npos ? std::string() : r.substr(0, bar);
        const std::string rel = bar == std::string::npos ? r : r.substr(bar + 1);
        std::error_code ec;
        if (tag == "@project") return project::isOpen() ? project::root() : project::engineRoot();
        if (tag == "@engine") return project::engineRoot().empty() ? fs::current_path(ec) : project::engineRoot();
        if (tag == "@cwd") return fs::current_path(ec);
        if (tag == "@root") return fs::path("/");
        return emapDir / rel;
    }

    bool readAt(uint64_t off, void* dst, size_t n) {
#ifndef _WIN32
        uint8_t* p = static_cast<uint8_t*>(dst);
        while (n > 0) {
            const ssize_t got = ::pread(m_fd, p, n, static_cast<off_t>(off));
            if (got <= 0) return false;
            p += got;
            off += static_cast<uint64_t>(got);
            n -= static_cast<size_t>(got);
        }
        return true;
#else
        std::lock_guard<std::mutex> lock(m_fileMutex);
        if (_fseeki64(m_file, static_cast<long long>(off), SEEK_SET) != 0) return false;
        return std::fread(dst, 1, n, m_file) == n;
#endif
    }
};

struct State {
    std::shared_mutex mutex;
    std::vector<std::shared_ptr<Bundle>> mounts; // o mais novo primeiro
    std::atomic<int> mounted{0};
    // ERUPTION_NO_MAP_BUNDLE=1: ignora os .emap (A/B de carga, ou mapa em edicao).
    std::atomic<bool> enabled{std::getenv("ERUPTION_NO_MAP_BUNDLE") == nullptr};

    std::mutex recMutex;
    std::atomic<bool> recording{false};
    std::unordered_map<std::string, size_t> recIndex;
    std::vector<Recorded> recList;

    std::atomic<uint64_t> bundleReads{0}, bundleBytes{0}, bundleProbes{0};
    std::atomic<uint64_t> diskReads{0}, diskBytes{0}, diskProbes{0};
    std::atomic<int> diskLogged{0};
};

// Nunca destruido: o fio de leitura de um pacote pode estar no meio na saida.
State& state() {
    static State* s = new State();
    return *s;
}

// ERUPTION_TEST_VFS_LOG=1 (teste): com pacote montado, registra cada arquivo
// que ainda foi ao disco (os 300 primeiros) - e' como se acha o que falta no build.
void logDisk(const char* what, const std::string& path) {
    static const bool on = std::getenv("ERUPTION_TEST_VFS_LOG") != nullptr;
    State& s = state();
    if (!on || s.mounted.load() == 0 || s.diskLogged.fetch_add(1) >= 300) return;
    ERUPTION_LOG_WARN("[VFS] disco (%s): %s", what, path.c_str());
}

bool fastPath() {
    State& s = state();
    return s.mounted.load(std::memory_order_relaxed) == 0 && !s.recording.load(std::memory_order_relaxed);
}

void note(const std::string& key, uint8_t access) {
    State& s = state();
    if (!s.recording.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lock(s.recMutex);
    auto it = s.recIndex.find(key);
    if (it == s.recIndex.end()) {
        s.recIndex.emplace(key, s.recList.size());
        s.recList.push_back({key, access, std::string()});
    } else if (s.recList[it->second].access < access) {
        s.recList[it->second].access = access;
    }
}

struct Hit {
    std::shared_ptr<Bundle> bundle;
    const Entry* entry = nullptr;
};

bool lookup(const std::string& key, Hit& hit) {
    State& s = state();
    if (s.mounted.load(std::memory_order_relaxed) == 0) return false;
    std::shared_lock<std::shared_mutex> lock(s.mutex);
    for (const auto& b : s.mounts) {
        const auto it = b->entries.find(key);
        if (it == b->entries.end() || it->second.stale) continue;
        hit.bundle = b;
        hit.entry = &it->second;
        return true;
    }
    return false;
}

bool diskRead(const std::string& path, std::vector<uint8_t>& out) {
    State& s = state();
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        s.diskProbes.fetch_add(1, std::memory_order_relaxed);
        out.clear();
        return false;
    }
    const bool ok = readWholeFile(f, out);
    s.diskReads.fetch_add(1, std::memory_order_relaxed);
    s.diskBytes.fetch_add(out.size(), std::memory_order_relaxed);
    logDisk("leitura", path);
    return ok;
}


// Pacotes .ecp do projeto: abaixo do disco. Aberto na carga e so' lido depois
// (o leitor nao trava); o shared_ptr deixa trocar sem fio lendo.
#ifdef ERUPTION_HAVE_ZSTD_DECODER
std::mutex g_packMutex;
std::shared_ptr<ecp::Reader> g_packs;
// Arquivos .tuf (utils/TuffArchive.hpp) do projeto, na ordem de montagem: o
// ULTIMO que cita o caminho vence. Ficam abaixo do .ecp e do disco.
std::vector<std::shared_ptr<tufo::Archive>> g_tufs;
std::string g_packBase; // chave normalizada, sem '/' final
std::atomic<bool> g_packsOn{false};

std::shared_ptr<ecp::Reader> packReader() {
    if (!g_packsOn.load(std::memory_order_acquire)) return nullptr;
    std::lock_guard<std::mutex> lk(g_packMutex);
    return g_packs;
}

std::vector<std::shared_ptr<tufo::Archive>> tufMounts() {
    if (!g_packsOn.load(std::memory_order_acquire)) return {};
    std::lock_guard<std::mutex> lk(g_packMutex);
    return g_tufs;
}

// Chave do pacote para um caminho do disco; vazio se fica fora da pasta base.
std::string packKey(const std::string& path) {
    std::string base;
    {
        std::lock_guard<std::mutex> lk(g_packMutex);
        base = g_packBase;
    }
    const std::string key = keyOf(path);
    if (key.size() <= base.size() + 1 || key.compare(0, base.size(), base) != 0 || key[base.size()] != '/') return {};
    return key.substr(base.size() + 1);
}

bool packRead(const std::string& path, std::vector<uint8_t>& out) {
    const auto r = packReader();
    const auto tufs = tufMounts();
    if (!r && tufs.empty()) return false;
    const std::string k = packKey(path);
    if (k.empty()) return false;
    State& s = state();
    if (r && r->ReadInto(k, out)) {
        s.bundleReads.fetch_add(1, std::memory_order_relaxed);
        s.bundleBytes.fetch_add(out.size(), std::memory_order_relaxed);
        return true;
    }
    for (auto it = tufs.rbegin(); it != tufs.rend(); ++it) {
        if (!(*it)->exists(k)) continue;
        std::string err;
        if (!(*it)->read(k, out, true, &err)) { // hash nao confere: nunca entrega dado ruim
            ERUPTION_LOG_ERROR("[VFS] %s: %s: %s", (*it)->path().c_str(), k.c_str(), err.c_str());
            out.clear();
            return false;
        }
        s.bundleReads.fetch_add(1, std::memory_order_relaxed);
        s.bundleBytes.fetch_add(out.size(), std::memory_order_relaxed);
        return true;
    }
    return false;
}

bool packExists(const std::string& path) {
    const auto r = packReader();
    const auto tufs = tufMounts();
    if (!r && tufs.empty()) return false;
    const std::string k = packKey(path);
    if (k.empty()) return false;
    if (r && r->Exists(k)) return true;
    for (const auto& t : tufs)
        if (t->exists(k)) return true;
    return false;
}

bool packStat(const std::string& path, uint64_t& size, int64_t& mtime) {
    const auto r = packReader();
    const auto tufs = tufMounts();
    if (!r && tufs.empty()) return false;
    const std::string k = packKey(path);
    if (k.empty()) return false;
    ecp::EntryInfo info;
    if (r && r->Stat(k, &info)) {
        size = info.rawSize;
        mtime = static_cast<int64_t>(info.mtime) * 1000000000ll;
        return true;
    }
    for (auto it = tufs.rbegin(); it != tufs.rend(); ++it) {
        tufo::Info ti;
        if (!(*it)->stat(k, &ti)) continue;
        size = ti.rawSize;
        mtime = ti.mtime * 1000000000ll;
        return true;
    }
    return false;
}
bool packGpuReady(const std::string& path) {
    const auto tufs = tufMounts();
    if (tufs.empty()) return false;
    const std::string k = packKey(path);
    if (k.empty()) return false;
    for (auto it = tufs.rbegin(); it != tufs.rend(); ++it) {
        tufo::Info ti;
        if (!(*it)->stat(k, &ti)) continue;
        return (ti.flags & tufo::kFlagGpuReady) != 0;
    }
    return false;
}
#else
bool packGpuReady(const std::string&) { return false; }
bool packRead(const std::string&, std::vector<uint8_t>&) { return false; }
bool packExists(const std::string&) { return false; }
bool packStat(const std::string&, uint64_t&, int64_t&) { return false; }
#endif
} // namespace

// ---- leitura ---------------------------------------------------------------

bool read(const std::string& path, std::vector<uint8_t>& out) {
    if (fastPath()) return diskRead(path, out) || packRead(path, out);
    const std::string key = keyOf(path);
    note(key, 3);
    Hit hit;
    if (lookup(key, hit)) {
        State& s = state();
        if (hit.entry->kind == kAbsent) {
            s.bundleProbes.fetch_add(1, std::memory_order_relaxed);
            out.clear();
            return false;
        }
        if (hit.entry->kind == kData) {
            if (hit.bundle->readEntry(key, *hit.entry, out)) {
                s.bundleReads.fetch_add(1, std::memory_order_relaxed);
                s.bundleBytes.fetch_add(out.size(), std::memory_order_relaxed);
                return true;
            }
            ERUPTION_LOG_WARN("[MAPBUNDLE] %s: entrada ilegivel, lendo do disco: %s", hit.bundle->file.c_str(), path.c_str());
        }
    }
    return diskRead(path, out) || packRead(path, out);
}

bool read(const fs::path& path, std::vector<uint8_t>& out) { return read(path.string(), out); }

namespace {
struct PinnedText {
    bool found = false;
    std::string text;
};
std::mutex g_pinnedMutex;
std::unordered_map<std::string, PinnedText> g_pinned;
std::atomic<bool> g_anyPinned{false};

std::string pinKey(const std::string& path) { return fs::path(path).lexically_normal().generic_string(); }

bool findPinned(const std::string& path, PinnedText& out) {
    if (!g_anyPinned.load(std::memory_order_relaxed)) return false;
    std::lock_guard<std::mutex> lk(g_pinnedMutex);
    const auto it = g_pinned.find(pinKey(path));
    if (it == g_pinned.end()) return false;
    out = it->second;
    return true;
}
} // namespace

void pinText(const std::string& path, bool found, std::string text) {
    std::lock_guard<std::mutex> lk(g_pinnedMutex);
    g_pinned[pinKey(path)] = PinnedText{found, std::move(text)};
    g_anyPinned.store(true, std::memory_order_relaxed);
}

void clearPinnedText() {
    std::lock_guard<std::mutex> lk(g_pinnedMutex);
    g_pinned.clear();
    g_anyPinned.store(false, std::memory_order_relaxed);
}

bool readText(const std::string& path, std::string& out) {
    PinnedText pinned;
    if (findPinned(path, pinned)) {
        if (pinned.found) out = pinned.text;
        return pinned.found;
    }
    std::vector<uint8_t> bytes;
    if (!read(path, bytes)) return false;
    out.assign(bytes.begin(), bytes.end());
    return true;
}

bool exists(const std::string& path) {
    PinnedText pinned;
    if (findPinned(path, pinned)) return pinned.found;
    State& s = state();
    if (fastPath()) {
        std::error_code ec;
        return fs::exists(path, ec) || packExists(path);
    }
    const std::string key = keyOf(path);
    note(key, 1);
    Hit hit;
    if (lookup(key, hit)) {
        s.bundleProbes.fetch_add(1, std::memory_order_relaxed);
        return hit.entry->kind != kAbsent;
    }
    s.diskProbes.fetch_add(1, std::memory_order_relaxed);
    logDisk("existe?", path);
    std::error_code ec;
    return fs::exists(path, ec) || packExists(path);
}

bool exists(const fs::path& path) { return exists(path.string()); }

bool stat(const std::string& path, uint64_t& size, int64_t& mtime) {
    State& s = state();
    bool isDir = false;
    if (fastPath()) return diskStat(path, size, mtime, isDir) || packStat(path, size, mtime);
    const std::string key = keyOf(path);
    note(key, 2);
    Hit hit;
    if (lookup(key, hit)) {
        s.bundleProbes.fetch_add(1, std::memory_order_relaxed);
        if (hit.entry->kind == kAbsent) return false;
        size = hit.entry->srcSize;
        mtime = hit.entry->srcMtime;
        return true;
    }
    s.diskProbes.fetch_add(1, std::memory_order_relaxed);
    logDisk("tamanho/data", path);
    return diskStat(path, size, mtime, isDir) || packStat(path, size, mtime);
}

uint64_t fileSize(const std::string& path, std::error_code& ec) {
    uint64_t size = 0;
    int64_t mtime = 0;
    if (!stat(path, size, mtime)) {
        ec = std::make_error_code(std::errc::no_such_file_or_directory);
        return 0;
    }
    ec.clear();
    return size;
}

bool inBundle(const std::string& path) {
    if (fastPath()) return false;
    Hit hit;
    return lookup(keyOf(path), hit) && hit.entry->kind == kData;
}

bool readDisk(const std::string& path, std::vector<uint8_t>& out) { return diskRead(path, out); }

bool tufGpuReady(const std::string& path) {
    std::error_code ec;
    if (fs::exists(path, ec)) return false; // o arquivo solto vence o pacote
    return packGpuReady(path);
}

// ---- pacotes .ecp do projeto ---------------------------------------------------

#ifdef ERUPTION_HAVE_ZSTD_DECODER
bool mountPacks(const std::vector<std::string>& ecpPaths, const std::string& baseDir) {
    unmountPacks();
    if (ecpPaths.empty()) return false;
    // .ecp e .tuf podem vir juntos na lista: cada um vai para o seu leitor.
    std::vector<std::string> ecps;
    std::vector<std::shared_ptr<tufo::Archive>> tufs;
    std::string err;
    const auto t0 = std::chrono::steady_clock::now();
    for (const std::string& p : ecpPaths) {
        if (p.size() > 4 && p.compare(p.size() - 4, 4, ".tuf") == 0) {
            auto t = std::make_shared<tufo::Archive>();
            if (!t->open(p, &err)) {
                ERUPTION_LOG_WARN("[VFS] arquivo .tuf nao abriu: %s", err.c_str());
                continue;
            }
            tufs.push_back(std::move(t));
        } else {
            ecps.push_back(p);
        }
    }
    std::shared_ptr<ecp::Reader> reader;
    if (!ecps.empty()) {
        reader = std::make_shared<ecp::Reader>();
        if (!reader->Open(ecps, &err)) {
            ERUPTION_LOG_WARN("[VFS] pacotes .ecp do projeto nao abriram: %s", err.c_str());
            reader.reset();
        }
    }
    if (!reader && tufs.empty()) return false;
    std::string base = keyOf(baseDir);
    while (base.size() > 1 && base.back() == '/') base.pop_back();
    {
        std::lock_guard<std::mutex> lk(g_packMutex);
        g_packs = std::move(reader);
        g_tufs = std::move(tufs);
        g_packBase = base;
    }
    g_packsOn.store(true, std::memory_order_release);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    size_t files = packReader() ? packReader()->FileCount() : 0;
    for (const auto& t : tufMounts()) files += t->fileCount();
    ERUPTION_LOG_WARN("[VFS] %zu pacote(s) .ecp/.tuf, %zu arquivos, abertos em %.1f ms (base %s)", ecpPaths.size(), files,
                      ms, base.c_str());
    return true;
}

void unmountPacks() {
    g_packsOn.store(false, std::memory_order_release);
    std::lock_guard<std::mutex> lk(g_packMutex);
    g_packs.reset();
    g_tufs.clear();
    g_packBase.clear();
}

bool packsMounted() { return g_packsOn.load(std::memory_order_acquire); }

std::vector<std::string> listPacked(const std::string& dir) {
    std::vector<std::string> out;
    const auto r = packReader();
    const auto tufs = tufMounts();
    if (!r && tufs.empty()) return out;
    std::string base;
    {
        std::lock_guard<std::mutex> lk(g_packMutex);
        base = g_packBase;
    }
    std::string key = packKey((fs::path(dir) / "x").string()); // chave de uma filha: tira a pasta
    if (key.empty()) return out;
    key.resize(key.size() - 1); // "pasta/" (ou "" para a propria base nao existe: precisa de filha)
    if (r)
        for (const std::string& f : r->List(key)) out.push_back(base + "/" + f);
    for (const auto& t : tufs)
        for (const std::string& f : t->list(key)) out.push_back(base + "/" + f);
    if (!tufs.empty()) {
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
    }
    return out;
}
#else
bool mountPacks(const std::vector<std::string>&, const std::string&) {
    ERUPTION_LOG_WARN("[VFS] pacotes .ecp pedidos, mas esta build nao tem o decodificador zstd");
    return false;
}
void unmountPacks() {}
bool packsMounted() { return false; }
std::vector<std::string> listPacked(const std::string&) { return {}; }
#endif

// ---- pacotes ---------------------------------------------------------------

fs::path bundlePathFor(const std::string& mapStem) {
    const fs::path ext = project::game("assets/external") / mapStem;
    std::error_code ec;
    if (fs::is_directory(ext, ec)) return ext / (mapStem + ".emap");
    return project::game("assets/data") / (mapStem + ".emap");
}

void setBundlesEnabled(bool enabled) {
    state().enabled = enabled;
    if (!enabled) unmountAll();
}

bool anyMounted() { return state().mounted.load() > 0; }

void unmountAll() {
    State& s = state();
    std::vector<std::shared_ptr<Bundle>> gone;
    {
        std::unique_lock<std::shared_mutex> lock(s.mutex);
        gone.swap(s.mounts);
        s.mounted = 0;
    }
}

bool mountMap(const std::string& mapStem) {
    State& s = state();
    if (!s.enabled || mapStem.empty()) return false;
    const auto t0 = std::chrono::steady_clock::now();
    fs::path emap;
    uint64_t size = 0;
    int64_t mtime = 0;
    bool isDir = false;
    for (const fs::path& c : {project::game("assets/external") / mapStem / (mapStem + ".emap"),
                              project::game("assets/data") / (mapStem + ".emap"),
                              project::game("assets/models") / (mapStem + ".emap")}) {
        if (diskStat(c.string(), size, mtime, isDir) && !isDir) {
            emap = c;
            break;
        }
    }
    if (emap.empty()) return false;

    auto b = std::make_shared<Bundle>();
    std::string err;
    if (!b->open(emap, err)) {
        ERUPTION_LOG_WARN("[MAPBUNDLE] %s: %s; o mapa abre pelos arquivos soltos", emap.string().c_str(), err.c_str());
        return false;
    }
    b->stem = mapStem;
    b->fileMtime = mtime;
    const auto t1 = std::chrono::steady_clock::now();

    // Origem que mudou depois do build. Conferir arquivo por arquivo custava
    // 2,8 s num mapa de teste (6153 stat a ~0,45 ms cada no NTFS via FUSE), entao:
    //  - conteudo lido de uma fonte (.glb, cena, .ter, .nav, .env, retoques):
    //    tamanho+data do arquivo, se ele existe no disco (no jogo exportado
    //    pode existir so' o pacote);
    //  - cache derivado (.etexpack, .etex, sondas, .cache/): valida-se pela
    //    propria chave na leitura, e o pacote de texturas que nao bate cai
    //    para o do disco;
    //  - so' tamanho/data (fonte de um .etex) e ausencia: pela data da pasta,
    //    que muda quando um arquivo entra, sai ou e' trocado por renomeacao.
    //    Fonte editada no lugar sem mexer na pasta so' entra no proximo build.
    size_t staleSources = 0, checked = 0, staleDirs = 0;
    std::string firstStale;
    std::unordered_set<std::string> changedDirs;
    for (auto& [key, e] : b->entries) {
        if (e.kind == kAbsent || e.kind == kMeta || (e.kind == kData && derivedCache(key))) continue;
        uint64_t fsz = 0;
        int64_t fmt = 0;
        bool fdir = false;
        ++checked;
        if (!diskStat(key, fsz, fmt, fdir)) continue; // so' no pacote (jogo exportado)
        if (e.kind == kDir) {
            if (fdir && fmt != e.srcMtime) changedDirs.insert(key);
            continue;
        }
        if (fsz == e.srcSize && fmt == e.srcMtime) continue;
        e.stale = true;
        if (firstStale.empty()) firstStale = key;
        ++staleSources;
    }
    if (!changedDirs.empty())
        for (auto& [key, e] : b->entries) {
            if (e.kind != kAbsent && e.kind != kMeta) continue;
            if (!changedDirs.count(key.substr(0, key.find_last_of('/')))) continue;
            e.stale = true;
            ++staleDirs;
        }
    const auto t2 = std::chrono::steady_clock::now();
    b->startPrefetch();
    {
        std::unique_lock<std::shared_mutex> lock(s.mutex);
        s.mounts.erase(std::remove_if(s.mounts.begin(), s.mounts.end(),
                                      [&](const std::shared_ptr<Bundle>& m) { return m->file == b->file; }),
                       s.mounts.end());
        s.mounts.insert(s.mounts.begin(), b);
        if (s.mounts.size() > 2) s.mounts.resize(2);
        s.mounted = static_cast<int>(s.mounts.size());
    }
    s.bundleReads = 0;
    s.bundleBytes = 0;
    s.bundleProbes = 0;
    s.diskReads = 0;
    s.diskBytes = 0;
    s.diskProbes = 0;
    s.diskLogged = 0;
    auto ms = [](auto a, auto z) { return std::chrono::duration<double, std::milli>(z - a).count(); };
    ERUPTION_LOG_WARN("[MAPBUNDLE] %s montado: %zu entradas, %.0f MB; indice %.1f ms, %zu origens conferidas em %.1f ms",
                      emap.string().c_str(), b->entries.size(), double(b->dataEnd - b->dataBegin) / (1024.0 * 1024.0),
                      ms(t0, t1), checked, ms(t1, t2));
    if (staleSources)
        ERUPTION_LOG_WARN("[MAPBUNDLE] %s: %zu arquivo(s) mudaram depois do build (ex.: %s); esses vem do disco. "
                          "Reconstrua o mapa (--bake-map %s ou File > Build Map)",
                          emap.filename().string().c_str(), staleSources, firstStale.c_str(), mapStem.c_str());
    if (staleDirs) {
        ERUPTION_LOG_INFO("[MAPBUNDLE] %s: %zu consulta(s) de pasta que mudou depois do build vao ao disco",
                          emap.filename().string().c_str(), staleDirs);
    }
    return true;
}

Counters counters() {
    State& s = state();
    Counters c;
    c.bundleReads = s.bundleReads;
    c.bundleBytes = s.bundleBytes;
    c.bundleProbes = s.bundleProbes;
    c.diskReads = s.diskReads;
    c.diskBytes = s.diskBytes;
    c.diskProbes = s.diskProbes;
    return c;
}

void logCounters(const char* label) {
    if (!anyMounted()) return;
    const Counters c = counters();
    ERUPTION_LOG_WARN("[VFS] %s: pacote %llu leituras (%.0f MB) + %llu consultas; disco %llu leituras (%.1f MB) + %llu "
                      "consultas",
                      label ? label : "", static_cast<unsigned long long>(c.bundleReads), double(c.bundleBytes) / 1048576.0,
                      static_cast<unsigned long long>(c.bundleProbes), static_cast<unsigned long long>(c.diskReads),
                      double(c.diskBytes) / 1048576.0, static_cast<unsigned long long>(c.diskProbes));
}

// ---- build -----------------------------------------------------------------

void beginRecord() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.recMutex);
    s.recIndex.clear();
    s.recList.clear();
    s.recording = true;
}

bool recording() { return state().recording.load(); }

size_t recordedCount() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.recMutex);
    return s.recList.size();
}

std::vector<Recorded> endRecord() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.recMutex);
    s.recording = false;
    s.recIndex.clear();
    return std::move(s.recList);
}

void recordExtra(const std::string& path) { note(keyOf(path), 3); }

std::string key(const std::string& path) { return keyOf(path); }

namespace {

struct Root {
    std::string tag; // gravado no indice
    std::string base; // chave absoluta da raiz, com '/' no fim
};

struct PackItem {
    std::string rel;
    uint8_t root = 0;
    Entry e;
    std::string source; // caminho absoluto
};

bool writeAll(std::ofstream& f, const void* p, size_t n) {
    f.write(static_cast<const char*>(p), static_cast<std::streamsize>(n));
    return static_cast<bool>(f);
}

} // namespace

WriteReport writeBundle(const fs::path& emapPath, const std::vector<Recorded>& files, bool compress, int level,
                        const Transform& transform) {
    WriteReport rep;
    const auto t0 = std::chrono::steady_clock::now();
#ifndef ERUPTION_HAVE_ZSTD
    compress = false;
    (void)compress; // sem zstd o valor so' e' escrito, nunca lido
    (void)level;
#endif
    // Raizes: a pasta do .emap (o mapa), o projeto, o motor e a pasta de
    // trabalho (os caches em .cache/). A chave vai relativa a' mais funda.
    const std::string emapDir = keyOf(emapPath.parent_path());
    std::vector<Root> roots;
    auto addRoot = [&](const std::string& tag, const fs::path& base) {
        if (base.empty()) return;
        std::string k = keyOf(base);
        if (k.empty() || k.back() != '/') k += '/';
        const std::string rel = fs::path(keyOf(base)).lexically_relative(emapDir).generic_string();
        roots.push_back({tag + "|" + (rel.empty() ? "." : rel), k});
    };
    addRoot("", emapPath.parent_path());
    addRoot("@project", project::isOpen() ? project::root() : project::engineRoot());
    addRoot("@engine", project::engineRoot());
    {
        std::error_code ec;
        addRoot("@cwd", fs::current_path(ec));
    }
#ifndef _WIN32
    // Consultas por nome sem pasta ("/<textura>_rough.jpg", busca de PBR do
    // disco para textura embutida): so' ausencias, mas eram ~50 stat por mapa.
    addRoot("@root", fs::path("/"));
#endif
    const std::string self = keyOf(emapPath);
    std::vector<PackItem> items;
    items.reserve(files.size());
    for (const Recorded& r : files) {
        const std::string& key = r.path;
        if (key == self || (key.size() > 4 && key.compare(key.size() - 4, 4, ".tmp") == 0)) continue;
        if (key.size() > 5 && key.compare(key.size() - 5, 5, ".emap") == 0) continue;
        int best = -1;
        for (size_t i = 0; i < roots.size(); ++i)
            if (key.compare(0, roots[i].base.size(), roots[i].base) == 0 &&
                (best < 0 || roots[i].base.size() > roots[size_t(best)].base.size()))
                best = int(i);
        if (best < 0) continue; // fora do jogo e do motor
        PackItem it;
        it.rel = key.substr(roots[size_t(best)].base.size());
        it.root = static_cast<uint8_t>(best);
        it.source = r.source.empty() ? key : r.source;
        uint64_t size = 0;
        int64_t mtime = 0;
        bool isDir = false;
        if (!diskStat(it.source, size, mtime, isDir)) {
            it.e.kind = kAbsent;
            uint64_t ds = 0;
            bool dd = false;
            diskStat(key.substr(0, key.find_last_of('/')), ds, it.e.srcMtime, dd);
            ++rep.absent;
        } else if (isDir) {
            it.e.kind = kDir;
            it.e.srcMtime = mtime;
            ++rep.meta;
        } else {
            it.e.kind = r.access >= 3 ? kData : kMeta;
            it.e.srcSize = size;
            it.e.srcMtime = mtime;
            if (it.e.kind == kData) ++rep.data;
            else ++rep.meta;
        }
        if (it.rel.size() > 0xFFFF) continue;
        items.push_back(std::move(it));
    }

    // Pasta de cada consulta sem conteudo (so' tamanho/data, ou ausencia):
    // a data dela no build. Na montagem, pasta que mudou devolve essas
    // consultas ao disco (arquivo novo, apagado ou trocado por renomeacao).
    {
        std::unordered_set<std::string> have;
        for (const PackItem& it : items)
            if (it.e.kind == kDir) have.insert(it.source);
        std::vector<PackItem> dirsToAdd;
        for (const PackItem& it : items) {
            if (it.e.kind != kMeta && it.e.kind != kAbsent) continue;
            const std::string dir = it.source.substr(0, it.source.find_last_of('/'));
            if (!have.insert(dir).second) continue;
            uint64_t ds = 0;
            int64_t dm = 0;
            bool dd = false;
            const std::string& base = roots[it.root].base;
            if (!diskStat(dir, ds, dm, dd) || !dd || dir.size() + 1 <= base.size()) continue;
            PackItem d;
            d.root = it.root;
            d.rel = dir.substr(base.size());
            d.source = dir;
            d.e.kind = kDir;
            d.e.srcMtime = dm;
            dirsToAdd.push_back(std::move(d));
        }
        for (PackItem& d : dirsToAdd) items.push_back(std::move(d));
    }

    size_t tocSize = 4;
    for (const Root& r : roots) tocSize += 2 + r.tag.size();
    for (const PackItem& it : items) tocSize += 2 + it.rel.size() + kEntryFixed;
    const uint64_t dataBegin = kHeaderSize + tocSize;

    // Pacote em outro disco: <mapa>.emap pode ser um link para o arquivo de
    // verdade. Grava no destino do link (o rename por cima do link o trocaria
    // por um arquivo neste disco). Num disco cheio o pacote nasce picotado e
    // a leitura "em sequencia" sai a 10-12 MB/s num HD que faz 112 MB/s; o
    // link deixa o pacote num disco com espaco, onde ele sai continuo. As
    // raizes do indice continuam relativas a' pasta do link.
    fs::path dest = emapPath;
    {
        std::error_code lec;
        if (fs::is_symlink(emapPath, lec)) {
            const fs::path target = fs::read_symlink(emapPath, lec);
            if (!lec) dest = target.is_absolute() ? target : emapPath.parent_path() / target;
        }
    }
    const fs::path tmp = dest.string() + ".tmp";
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) {
        rep.error = "nao consegui criar " + tmp.string();
        return rep;
    }
    {
        const std::vector<uint8_t> zeros(static_cast<size_t>(dataBegin), 0);
        writeAll(f, zeros.data(), zeros.size());
    }
    // Em lotes de ~512 MB crus: le' em sequencia, comprime em paralelo, grava
    // na ordem anotada.
    uint64_t cursor = dataBegin;
    std::vector<size_t> dataIdx;
    for (size_t i = 0; i < items.size(); ++i)
        if (items[i].e.kind == kData) dataIdx.push_back(i);
    for (size_t first = 0; first < dataIdx.size();) {
        std::vector<size_t> batch;
        uint64_t batchBytes = 0;
        while (first < dataIdx.size() && (batch.empty() || batchBytes < (512ull << 20))) {
            batch.push_back(dataIdx[first]);
            batchBytes += items[dataIdx[first]].e.srcSize;
            ++first;
        }
        std::vector<std::vector<uint8_t>> raw(batch.size()), packed(batch.size());
        for (size_t k = 0; k < batch.size(); ++k) {
            std::ifstream in(items[batch[k]].source, std::ios::binary);
            if (!readWholeFile(in, raw[k])) raw[k].clear();
        }
        JobSystem::instance().parallelFor(static_cast<uint32_t>(batch.size()), [&](uint32_t k) {
            Entry& e = items[batch[k]].e;
            if (transform && !raw[k].empty()) transform(items[batch[k]].source, raw[k]);
            e.raw = raw[k].size();
            e.hash = hash64(raw[k].data(), raw[k].size());
            e.codec = kRaw;
#ifdef ERUPTION_HAVE_ZSTD
            if (compress && raw[k].size() > 512) {
                std::vector<uint8_t> out(ZSTD_compressBound(raw[k].size()));
                const size_t n = ZSTD_compress(out.data(), out.size(), raw[k].data(), raw[k].size(), level);
                // So' compensa se ganhar: descomprimir BC7 que nao encolhe e' tempo jogado fora.
                if (!ZSTD_isError(n) && n < raw[k].size() - raw[k].size() / 32) {
                    out.resize(n);
                    packed[k] = std::move(out);
                    e.codec = kZstd;
                }
            }
#endif
        });
        for (size_t k = 0; k < batch.size(); ++k) {
            Entry& e = items[batch[k]].e;
            const std::vector<uint8_t>& bytes = e.codec == kZstd ? packed[k] : raw[k];
            e.offset = cursor;
            e.stored = bytes.size();
            if (!writeAll(f, bytes.data(), bytes.size())) {
                rep.error = "falha gravando " + tmp.string();
                return rep;
            }
            cursor += bytes.size();
            rep.rawBytes += e.raw;
            rep.storedBytes += e.stored;
            rep.sourceBytes += e.srcSize;
        }
    }

    std::vector<uint8_t> toc;
    toc.reserve(tocSize);
    auto put = [&toc](const void* p, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        toc.insert(toc.end(), b, b + n);
    };
    const uint32_t nroots = static_cast<uint32_t>(roots.size());
    put(&nroots, 4);
    for (const Root& r : roots) {
        const uint16_t n = static_cast<uint16_t>(r.tag.size());
        put(&n, 2);
        put(r.tag.data(), r.tag.size());
    }
    for (const PackItem& it : items) {
        const uint16_t n = static_cast<uint16_t>(it.rel.size());
        put(&n, 2);
        put(it.rel.data(), it.rel.size());
        const uint8_t fixed[3] = {it.root, it.e.kind, it.e.codec};
        put(fixed, 3);
        put(&it.e.offset, 8);
        put(&it.e.stored, 8);
        put(&it.e.raw, 8);
        put(&it.e.hash, 8);
        put(&it.e.srcSize, 8);
        put(&it.e.srcMtime, 8);
    }
    uint8_t head[kHeaderSize] = {};
    std::memcpy(head, kMagic, 8);
    const uint32_t count = static_cast<uint32_t>(items.size());
    const uint64_t tocOff = kHeaderSize, tocLen = toc.size(), dataLen = cursor - dataBegin, tocHash = hash64(toc.data(), toc.size());
    std::memcpy(head + 8, &kVersion, 4);
    std::memcpy(head + 12, &count, 4);
    std::memcpy(head + 16, &tocOff, 8);
    std::memcpy(head + 24, &tocLen, 8);
    std::memcpy(head + 32, &dataBegin, 8);
    std::memcpy(head + 40, &dataLen, 8);
    std::memcpy(head + 48, &tocHash, 8);
    f.seekp(0);
    if (!writeAll(f, head, kHeaderSize) || !writeAll(f, toc.data(), toc.size())) {
        rep.error = "falha gravando o indice";
        return rep;
    }
    f.close();
    if (!f) {
        rep.error = "falha fechando " + tmp.string();
        return rep;
    }
    std::error_code ec;
    fs::rename(tmp, dest, ec);
    if (ec) {
        fs::remove(dest, ec);
        fs::rename(tmp, dest, ec);
    }
    if (ec) {
        rep.error = "nao consegui trocar " + emapPath.string() + ": " + ec.message();
        return rep;
    }
    rep.entries = items.size();
    rep.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return rep;
}

} // namespace vfs
} // namespace eruption
