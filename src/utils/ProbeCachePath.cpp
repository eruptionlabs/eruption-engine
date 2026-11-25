#include "utils/ProbeCachePath.hpp"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <system_error>

namespace eruption::probecache {

namespace {
const char* envDir() {
    const char* e = std::getenv("ERUPTION_PROBE_CACHE_DIR");
    return e && *e ? e : nullptr;
}

// FNV-1a do caminho canonico: o mesmo mapa em duas pastas tem dois arquivos.
uint64_t pathHash(const std::string& glbPath) {
    std::error_code ec;
    std::filesystem::path p = std::filesystem::weakly_canonical(std::filesystem::absolute(glbPath, ec), ec);
    const std::string s = ec ? glbPath : p.generic_string();
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}
} // namespace

std::string userDir() {
    if (const char* e = envDir()) return e;
#if defined(__ANDROID__)
    return std::string();
#elif defined(_WIN32)
    if (const char* l = std::getenv("LOCALAPPDATA"); l && *l) return std::string(l) + "/eruption/probes";
    return std::string();
#else
    if (const char* x = std::getenv("XDG_CACHE_HOME"); x && *x) return std::string(x) + "/eruption/probes";
    if (const char* h = std::getenv("HOME"); h && *h) return std::string(h) + "/.cache/eruption/probes";
    return std::string();
#endif
}

std::string userFile(const std::string& glbPath) {
    const std::string dir = userDir();
    if (dir.empty() || glbPath.empty()) return std::string();
    static const char* kHex = "0123456789abcdef";
    uint64_t h = pathHash(glbPath);
    std::string hex(16, '0');
    for (int i = 15; i >= 0; --i, h >>= 4) hex[size_t(i)] = kHex[h & 15u];
    return dir + "/" + std::filesystem::path(glbPath).filename().string() + "-" + hex + ".probes";
}

WritePolicy writePolicy(const std::string& projectChoice) {
    if (envDir()) return WritePolicy::User;
    if (projectChoice == "beside") return WritePolicy::Beside;
    return userDir().empty() ? WritePolicy::Beside : WritePolicy::User;
}

} // namespace eruption::probecache
