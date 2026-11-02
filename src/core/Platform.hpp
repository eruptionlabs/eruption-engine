#pragma once

// O pouco que muda de sistema para sistema (Linux, Windows, macOS, Android),
// num lugar só.
#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <string>
#include <type_traits>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX // std::min/std::max continuam funcionando
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <climits>
#elif defined(__linux__)
#include <climits>
#include <unistd.h>
#endif
#if defined(_WIN32)
#include <process.h>
#endif

namespace eruption::platform {

// Caminho completo do programa em execução (vazio se o sistema não diz).
inline std::filesystem::path executablePath() {
#if defined(_WIN32)
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return n > 0 && n < MAX_PATH ? std::filesystem::path(std::wstring(buf, n)) : std::filesystem::path();
#elif defined(__APPLE__)
    char buf[PATH_MAX];
    uint32_t size = sizeof(buf);
    return _NSGetExecutablePath(buf, &size) == 0 ? std::filesystem::path(buf) : std::filesystem::path();
#elif defined(__linux__) && !defined(__ANDROID__)
    char buf[PATH_MAX];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    return n > 0 ? std::filesystem::path(std::string(buf, static_cast<size_t>(n))) : std::filesystem::path();
#else
    return {};
#endif
}

inline void setEnv(const char* name, const char* value) {
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

// Reabre o próprio programa com outros argumentos e NÃO volta (o processo é
// substituído). Usado para trocar de projeto de jogo: o motor resolve as
// pastas de asset uma vez, na abertura, então um projeto novo precisa de um
// começo limpo. Só retorna (false) se não deu para trocar.
inline bool relaunch(const std::vector<std::string>& args) {
#if defined(__EMSCRIPTEN__)
    (void)args; // o navegador nao reabre processo: o projeto vem da pagina
    return false;
#else
    const std::filesystem::path exe = executablePath();
    if (exe.empty()) return false;
    std::vector<std::string> store;
    store.push_back(exe.string());
    for (const std::string& a : args) store.push_back(a);
    std::vector<char*> argv;
    for (std::string& a : store) argv.push_back(a.data());
    argv.push_back(nullptr);
    std::fflush(nullptr);
#if defined(_WIN32)
    _execv(store[0].c_str(), argv.data());
#else
    execv(store[0].c_str(), argv.data());
#endif
    return false; // execv só volta em caso de erro
#endif
}

// Data da última mudança do arquivo (0 se não existe), para recarregar quando muda.
inline long long fileTime(const std::string& path) {
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(path, ec);
    return ec ? 0 : static_cast<long long>(t.time_since_epoch().count());
}

// Pixels de interface por dp (a unidade de toque do Android: 160 dp por
// polegada). Area de toque minima e' 48 dp. No desktop, 1;
// ERUPTION_TEST_DP_SCALE=<n> (teste) imita a tela de um celular.
#if defined(__ANDROID__)
float pixelsPerDp(); // src/platform/android/AndroidMain.cpp
#else
inline float pixelsPerDp() {
    static const float s = [] {
        const char* e = std::getenv("ERUPTION_TEST_DP_SCALE");
        return e ? std::max(0.5f, static_cast<float>(std::atof(e))) : 1.0f;
    }();
    return s;
}
#endif

// Fracao da tela usada pelo swapchain (preset "display_scale"). So' o Android
// tem compositor que estica de graca; no desktop nao faz nada.
#if defined(__ANDROID__)
void setDisplayScale(float scale); // src/platform/android/AndroidMain.cpp
#else
inline void setDisplayScale(float) {}
#endif

// Pede pro sistema compartilhar um arquivo (share sheet): o celular nao
// deixa expor caminho de arquivo direto a outro aplicativo, so' um content://
// (FileProvider, do lado Java). No desktop e no navegador nao ha' share
// sheet: devolve false sem fazer nada. 'path' tem que estar dentro da pasta
// de export do app (src/platform/android/java/.../EruptionFileProvider).
#if defined(__ANDROID__)
bool shareFile(const std::string& path, const char* mimeType); // src/platform/android/AndroidMain.cpp
#else
inline bool shareFile(const std::string&, const char*) { return false; }
#endif

// Roda `fn` em outra thread e devolve o futuro (std::async). No navegador a v1
// nao tem threads (pediria COOP/COEP do servidor): roda aqui e o futuro sai
// pronto, entao quem consulta com wait_for(0) recebe `ready` na hora.
template <class F>
std::future<std::invoke_result_t<std::decay_t<F>>> runAsync(F&& fn) {
#if defined(__EMSCRIPTEN__)
    using R = std::invoke_result_t<std::decay_t<F>>;
    std::promise<R> done;
    try {
        if constexpr (std::is_void_v<R>) {
            fn();
            done.set_value();
        } else {
            done.set_value(fn());
        }
    } catch (...) {
        done.set_exception(std::current_exception());
    }
    return done.get_future();
#else
    return std::async(std::launch::async, std::forward<F>(fn));
#endif
}

} // namespace eruption::platform
