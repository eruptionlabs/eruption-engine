// Leitor de pacotes de assets (.ecp): so' leitura, um header, C++17.
//
// Formato (anexo-so', zstd por arquivo, indice e rodape, sem cifra):
//   cabecalho 32 B "ECPK" | dados | indice zstd | rodape 32 B "ECPF" (ultimos 32 bytes).
// Rodape: indice (offset, tamanho) + XXH64 do indice com seed = XXH64 dos 24 bytes
// iniciais do rodape. Indice: entradas ordenadas por caminho (bytes), cada uma com
// codec (0 cru, 1 zstd, 2 zstd com dicionario), tombstone (bit0 de flags), offset,
// tamanho gravado, tamanho cru e XXH64 dos bytes crus. Caminhos: '\\' vira '/',
// componentes vazios e "." somem, ".." invalida, A-Z vira a-z (so' ASCII).
// Varios pacotes formam uma sobreposicao: vale o ULTIMO que cita o caminho, e um
// tombstone esconde o arquivo tambem dos anteriores.
//
// Exige zstd (zstd.h) so' para DESCOMPRIMIR. Trata o pacote como nao confiavel:
// todo offset/tamanho e' conferido, versao e codec desconhecidos falham no Open
// (nunca derrubam o processo).
//
//   eruption::ecp::Reader r;
//   std::string err;
//   if (!r.Open({"base.ecp", "patch_001.ecp"}, &err)) ...   // o ultimo vence
//   std::vector<uint8_t> bytes = r.Read("Dir\\Arquivo.ext");
//
// Threads: depois de um Open bem-sucedido o Reader e' imutavel; Exists, Read,
// ReadView, Stat e List podem rodar em varias threads sem trava. Open e Close
// nao podem rodar junto com mais nada.
//
// Plataformas: POSIX (Linux, Android) por mmap, Windows por CreateFileMapping.
// O pacote tem que ser um arquivo de verdade (nao da' para mapear de dentro
// do APK comprimido). Em alvo de 32 bits, pacote maior que o espaco de
// enderecos falha no Open: divida em pacotes menores (a sobreposicao cobre).
// No navegador (Emscripten) o mmap do MEMFS copia o arquivo para a memoria.
#pragma once

#include <zstd.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <sys/mman.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

namespace eruption::ecp {

constexpr uint16_t kVersion = 1;
constexpr size_t kHeaderSize = 32;
constexpr size_t kFooterSize = 32;

enum Codec : uint8_t { kRaw = 0, kZstd = 1, kZstdDict = 2 };

struct EntryInfo {
    uint64_t rawSize = 0;
    uint64_t storedSize = 0;
    uint64_t xxh64 = 0;
    uint64_t mtime = 0;  // unix seconds
    uint8_t codec = 0;
    int pack = -1;       // index into the Open() path list
};

namespace detail {

// ---- XXH64 (public-domain algorithm by Yann Collet), used for checksums ----
inline uint64_t rotl64(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
inline uint64_t rd64(const uint8_t* p) { uint64_t v; std::memcpy(&v, p, 8); return v; }
inline uint32_t rd32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
inline uint16_t rd16(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }

inline uint64_t xxh64(const void* data, size_t len, uint64_t seed = 0) {
    constexpr uint64_t P1 = 11400714785074694791ULL, P2 = 14029467366897019727ULL,
                       P3 = 1609587929392839161ULL, P4 = 9650029242287828579ULL,
                       P5 = 2870177450012600261ULL;
    auto round = [&](uint64_t acc, uint64_t in) { return rotl64(acc + in * P2, 31) * P1; };
    auto merge = [&](uint64_t acc, uint64_t v) { return (acc ^ round(0, v)) * P1 + P4; };
    const uint8_t* p = static_cast<const uint8_t*>(data);
    const uint8_t* end = p + len;
    uint64_t h;
    if (len >= 32) {
        uint64_t v1 = seed + P1 + P2, v2 = seed + P2, v3 = seed, v4 = seed - P1;
        const uint8_t* lim = end - 32;
        do {
            v1 = round(v1, rd64(p)); v2 = round(v2, rd64(p + 8));
            v3 = round(v3, rd64(p + 16)); v4 = round(v4, rd64(p + 24));
            p += 32;
        } while (p <= lim);
        h = rotl64(v1, 1) + rotl64(v2, 7) + rotl64(v3, 12) + rotl64(v4, 18);
        h = merge(h, v1); h = merge(h, v2); h = merge(h, v3); h = merge(h, v4);
    } else {
        h = seed + P5;
    }
    h += static_cast<uint64_t>(len);
    while (p + 8 <= end) { h ^= round(0, rd64(p)); h = rotl64(h, 27) * P1 + P4; p += 8; }
    if (p + 4 <= end) { h ^= static_cast<uint64_t>(rd32(p)) * P1; h = rotl64(h, 23) * P2 + P3; p += 4; }
    while (p < end) { h ^= (*p++) * P5; h = rotl64(h, 11) * P1; }
    h ^= h >> 33; h *= P2; h ^= h >> 29; h *= P3; h ^= h >> 32;
    return h;
}

// Same rule as tools/ecp.py: '\\' -> '/', drop "", "." components, reject "..",
// ASCII-only lowercase. Returns false for an invalid path.
inline bool normalize(const std::string& in, std::string& out) {
    out.clear();
    out.reserve(in.size());
    size_t i = 0, n = in.size();
    while (i <= n) {
        size_t j = i;
        while (j < n && in[j] != '/' && in[j] != '\\') ++j;
        size_t len = j - i;
        if (len == 2 && in[i] == '.' && in[i + 1] == '.') return false;
        if (len != 0 && !(len == 1 && in[i] == '.')) {
            if (!out.empty()) out.push_back('/');
            for (size_t k = i; k < j; ++k) {
                char c = in[k];
                if (c == '\0') return false;
                out.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c);
            }
        }
        i = j + 1;
    }
    return !out.empty();
}

// Read-only whole-file memory mapping.
class Mapping {
public:
    Mapping() = default;
    Mapping(const Mapping&) = delete;
    Mapping& operator=(const Mapping&) = delete;
    Mapping(Mapping&& o) noexcept { *this = std::move(o); }
    Mapping& operator=(Mapping&& o) noexcept {
        if (this != &o) {
            Close();
            data_ = o.data_; size_ = o.size_;
#if defined(_WIN32)
            file_ = o.file_; map_ = o.map_;
            o.file_ = INVALID_HANDLE_VALUE; o.map_ = nullptr;
#endif
            o.data_ = nullptr; o.size_ = 0;
        }
        return *this;
    }
    ~Mapping() { Close(); }

    bool Open(const std::string& path, std::string* err) {
        Close();
#if defined(_WIN32)
        int wn = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
        std::wstring wp(static_cast<size_t>(wn > 0 ? wn : 1), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &wp[0], wn);
        file_ = CreateFileW(wp.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
        if (file_ == INVALID_HANDLE_VALUE) return Fail(err, "cannot open " + path);
        LARGE_INTEGER sz;
        if (!GetFileSizeEx(file_, &sz) || sz.QuadPart <= 0) return Fail(err, "empty or unreadable " + path);
        if (static_cast<uint64_t>(sz.QuadPart) > SIZE_MAX) return Fail(err, "pack too large to map: " + path);
        map_ = CreateFileMappingW(file_, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!map_) return Fail(err, "CreateFileMapping failed: " + path);
        data_ = static_cast<const uint8_t*>(MapViewOfFile(map_, FILE_MAP_READ, 0, 0, 0));
        if (!data_) return Fail(err, "MapViewOfFile failed: " + path);
        size_ = static_cast<size_t>(sz.QuadPart);
#else
        int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) return Fail(err, "cannot open " + path);
        struct stat st;
        if (::fstat(fd, &st) != 0 || st.st_size <= 0) { ::close(fd); return Fail(err, "empty or unreadable " + path); }
        if (static_cast<uint64_t>(st.st_size) > SIZE_MAX) { ::close(fd); return Fail(err, "pack too large to map: " + path); }
        void* m = ::mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
        ::close(fd);  // the mapping stays valid
        if (m == MAP_FAILED) return Fail(err, "mmap failed: " + path);
        data_ = static_cast<const uint8_t*>(m);
        size_ = static_cast<size_t>(st.st_size);
#endif
        return true;
    }

    void Close() {
#if defined(_WIN32)
        if (data_) UnmapViewOfFile(data_);
        if (map_) CloseHandle(map_);
        if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
        file_ = INVALID_HANDLE_VALUE; map_ = nullptr;
#else
        if (data_) ::munmap(const_cast<uint8_t*>(data_), size_);
#endif
        data_ = nullptr; size_ = 0;
    }

    const uint8_t* data() const { return data_; }
    size_t size() const { return size_; }

private:
    static bool Fail(std::string* err, const std::string& m) { if (err) *err = m; return false; }
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
#if defined(_WIN32)
    HANDLE file_ = INVALID_HANDLE_VALUE;
    HANDLE map_ = nullptr;
#endif
};

struct DCtxHolder {
    ZSTD_DCtx* ctx = ZSTD_createDCtx();
    ~DCtxHolder() { ZSTD_freeDCtx(ctx); }
};
inline ZSTD_DCtx* threadDCtx() { thread_local DCtxHolder h; return h.ctx; }

struct DDictDeleter { void operator()(ZSTD_DDict* d) const { ZSTD_freeDDict(d); } };
using DDictPtr = std::unique_ptr<ZSTD_DDict, DDictDeleter>;

struct PackEntry {
    uint64_t offset, csize, rsize, xxh, mtime;
    uint16_t dictId;
    uint8_t codec, flags;
};

struct PackData {
    Mapping map;
    std::unordered_map<uint16_t, DDictPtr> dicts;
    std::vector<std::pair<std::string, PackEntry>> entries;  // as stored in the index
};

}  // namespace detail

class Reader {
public:
    struct Options {
        bool verifyOnRead = false;  // recompute xxh64 of every Read() result
    };

    Reader() = default;
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;

    // Open packs in priority order: later paths override earlier ones.
    bool Open(const std::vector<std::string>& paths, std::string* error = nullptr) {
        return Open(paths, Options{}, error);
    }
    bool Open(const std::vector<std::string>& paths, Options opt, std::string* error) {
        Close();
        opt_ = opt;
        for (const std::string& p : paths) {
            auto pk = std::make_unique<detail::PackData>();
            std::string e;
            if (!LoadPack(p, *pk, &e)) { Close(); if (error) *error = p + ": " + e; return false; }
            packs_.push_back(std::move(pk));
        }
        for (size_t i = 0; i < packs_.size(); ++i) {
            for (auto& kv : packs_[i]->entries) {
                if (kv.second.flags & 1) merged_.erase(kv.first);            // tombstone
                else merged_[kv.first] = Ref{static_cast<int>(i), &kv.second};
            }
        }
        sorted_.reserve(merged_.size());
        for (auto& kv : merged_) sorted_.push_back(&kv.first);
        std::sort(sorted_.begin(), sorted_.end(),
                  [](const std::string* a, const std::string* b) { return *a < *b; });
        return true;
    }

    void Close() {
        merged_.clear(); sorted_.clear(); packs_.clear();
    }

    bool IsOpen() const { return !packs_.empty(); }
    size_t FileCount() const { return merged_.size(); }
    size_t PackCount() const { return packs_.size(); }
    // Arquivos vivos (sem tombstones) citados pelo pacote i, antes da sobreposicao.
    size_t PackEntryCount(size_t i) const {
        if (i >= packs_.size()) return 0;
        size_t n = 0;
        for (const auto& kv : packs_[i]->entries) n += (kv.second.flags & 1) ? 0 : 1;
        return n;
    }

    bool Exists(const std::string& path) const { return Find(path) != nullptr; }

    bool Stat(const std::string& path, EntryInfo* out) const {
        const Ref* r = Find(path);
        if (!r) return false;
        const detail::PackEntry& e = *r->e;
        out->rawSize = e.rsize; out->storedSize = e.csize; out->xxh64 = e.xxh; out->mtime = e.mtime;
        out->codec = e.codec; out->pack = r->pack;
        return true;
    }

    // Whole file, decompressed. Empty vector if missing or corrupt (use Stat() to
    // tell a missing file from a zero-length one, or ReadInto() for an error flag).
    std::vector<uint8_t> Read(const std::string& path) const {
        std::vector<uint8_t> out;
        ReadInto(path, out);
        return out;
    }

    bool ReadInto(const std::string& path, std::vector<uint8_t>& out) const {
        out.clear();
        const Ref* r = Find(path);
        if (!r) return false;
        const detail::PackData& pk = *packs_[static_cast<size_t>(r->pack)];
        const detail::PackEntry& e = *r->e;
        const uint8_t* src = pk.map.data() + e.offset;
        if (e.rsize > (uint64_t)SIZE_MAX / 2) return false;
        out.resize(static_cast<size_t>(e.rsize));
        bool ok = true;
        if (e.codec == kRaw) {
            if (e.csize != e.rsize) ok = false;
            else if (e.rsize) std::memcpy(out.data(), src, out.size());
        } else if (e.codec == kZstd || e.codec == kZstdDict) {
            size_t n;
            if (e.codec == kZstd) {
                n = ZSTD_decompressDCtx(detail::threadDCtx(), out.data(), out.size(), src, static_cast<size_t>(e.csize));
            } else {
                auto it = pk.dicts.find(e.dictId);
                if (it == pk.dicts.end()) return Fail(out);
                n = ZSTD_decompress_usingDDict(detail::threadDCtx(), out.data(), out.size(), src,
                                               static_cast<size_t>(e.csize), it->second.get());
            }
            ok = !ZSTD_isError(n) && n == out.size();
        } else {
            ok = false;
        }
        if (ok && opt_.verifyOnRead && detail::xxh64(out.data(), out.size()) != e.xxh) ok = false;
        if (!ok) out.clear();
        return ok;
    }

    // Zero-copy view of a stored-raw entry (codec 0). Returns false if the entry is
    // missing or compressed. The pointer stays valid until Close()/destruction.
    bool ReadView(const std::string& path, const uint8_t** data, size_t* size) const {
        const Ref* r = Find(path);
        if (!r || r->e->codec != kRaw) return false;
        *data = packs_[static_cast<size_t>(r->pack)]->map.data() + r->e->offset;
        *size = static_cast<size_t>(r->e->rsize);
        return true;
    }

    // All live paths (normalized) starting with the normalized prefix, sorted.
    // An empty prefix lists everything.
    std::vector<std::string> List(const std::string& prefix = std::string()) const {
        std::string pre;
        if (!prefix.empty()) {
            detail::normalize(prefix, pre);
            if (!prefix.empty() && (prefix.back() == '/' || prefix.back() == '\\') && !pre.empty()) pre.push_back('/');
        }
        std::vector<std::string> out;
        auto it = std::lower_bound(sorted_.begin(), sorted_.end(), pre,
                                   [](const std::string* a, const std::string& b) { return *a < b; });
        for (; it != sorted_.end() && (*it)->compare(0, pre.size(), pre) == 0; ++it) out.push_back(**it);
        return out;
    }

private:
    struct Ref { int pack; const detail::PackEntry* e; };

    static bool Fail(std::vector<uint8_t>& out) { out.clear(); return false; }

    const Ref* Find(const std::string& path) const {
        std::string n;
        if (!detail::normalize(path, n)) return nullptr;
        auto it = merged_.find(n);
        return it == merged_.end() ? nullptr : &it->second;
    }

    static bool Err(std::string* e, const char* m) { if (e) *e = m; return false; }

    static bool LoadPack(const std::string& path, detail::PackData& pk, std::string* err) {
        using namespace detail;
        if (!pk.map.Open(path, err)) return false;
        const uint8_t* base = pk.map.data();
        const uint64_t size = pk.map.size();
        if (size < kHeaderSize + kFooterSize) return Err(err, "file too small");
        if (std::memcmp(base, "ECPK", 4) != 0) return Err(err, "bad header magic");
        if (rd16(base + 4) != kVersion) return Err(err, "unsupported version");
        const uint8_t* f = base + size - kFooterSize;
        if (std::memcmp(f, "ECPF", 4) != 0 || rd16(f + 4) != kVersion) return Err(err, "bad footer");
        const uint64_t ioff = rd64(f + 8), isz = rd64(f + 16), csum = rd64(f + 24);
        if (ioff < kHeaderSize || isz > size || ioff > size - kFooterSize - isz) return Err(err, "index out of range");
        const uint64_t dataEnd = ioff;
        if (xxh64(base + ioff, static_cast<size_t>(isz), xxh64(f, 24)) != csum) return Err(err, "footer checksum mismatch");

        unsigned long long rawIdx = ZSTD_getFrameContentSize(base + ioff, static_cast<size_t>(isz));
        if (rawIdx == ZSTD_CONTENTSIZE_UNKNOWN || rawIdx == ZSTD_CONTENTSIZE_ERROR || rawIdx > (1ULL << 31))
            return Err(err, "bad index frame");
        std::vector<uint8_t> ix(static_cast<size_t>(rawIdx));
        size_t got = ZSTD_decompress(ix.data(), ix.size(), base + ioff, static_cast<size_t>(isz));
        if (ZSTD_isError(got) || got != ix.size()) return Err(err, "index decompress failed");

        const uint8_t* p = ix.data();
        const uint8_t* end = p + ix.size();
        auto need = [&](size_t n) { return static_cast<size_t>(end - p) >= n; };
        if (!need(16) || std::memcmp(p, "ECIX", 4) != 0) return Err(err, "bad index magic");
        uint32_t count = rd32(p + 8);
        uint16_t nd = rd16(p + 12);
        p += 16;
        for (uint16_t i = 0; i < nd; ++i) {
            if (!need(28 + 1)) return Err(err, "truncated dict table");
            uint16_t id = rd16(p);
            uint64_t off = rd64(p + 4), sz = rd64(p + 12), xx = rd64(p + 20);
            p += 28;
            size_t nl = *p++;
            if (!need(nl)) return Err(err, "truncated dict name");
            p += nl;
            if (off < kHeaderSize || sz > dataEnd || off > dataEnd - sz) return Err(err, "dict out of range");
            if (xxh64(base + off, static_cast<size_t>(sz)) != xx) return Err(err, "dict checksum mismatch");
            DDictPtr dd(ZSTD_createDDict(base + off, static_cast<size_t>(sz)));
            if (!dd) return Err(err, "bad dictionary");
            pk.dicts[id] = std::move(dd);
        }
        pk.entries.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            if (!need(2)) return Err(err, "truncated entry");
            size_t pl = rd16(p); p += 2;
            if (!need(pl + 44)) return Err(err, "truncated entry");
            std::string name(reinterpret_cast<const char*>(p), pl);
            p += pl;
            PackEntry e;
            e.codec = p[0]; e.flags = p[1]; e.dictId = rd16(p + 2);
            e.offset = rd64(p + 4); e.csize = rd64(p + 12); e.rsize = rd64(p + 20);
            e.xxh = rd64(p + 28); e.mtime = rd64(p + 36);
            p += 44;
            if (!(e.flags & 1) && (e.offset < kHeaderSize || e.csize > dataEnd || e.offset > dataEnd - e.csize))
                return Err(err, "entry data out of range");
            pk.entries.emplace_back(std::move(name), e);
        }
        if (p != end) return Err(err, "trailing bytes in index");
        return true;
    }

    Options opt_;
    std::vector<std::unique_ptr<detail::PackData>> packs_;
    std::unordered_map<std::string, Ref> merged_;
    std::vector<const std::string*> sorted_;
};

}  // namespace eruption::ecp
