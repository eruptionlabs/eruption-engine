// Ponto de entrada do Android (NativeActivity). Tres papeis:
//  1. Extrai os arquivos do APK para a pasta do app na primeira execucao (o
//     motor abre tudo por caminho relativo) e entra nela.
//  2. Implementa o subconjunto do GLFW e do backend de ImGui que o motor usa
//     (include/GLFW/glfw3.h, include/imgui_impl_glfw.h) sobre a ANativeWindow
//     e a fila de eventos, com o toque fazendo papel de mouse.
//  3. Manda stdout/stderr para o logcat e chama o eruptionMain() (o mesmo main() do PC).
//
// DUAS THREADS. A do android_main so' atende o sistema (ciclo de vida e
// toque): se ela ficar ocupada, o Android mostra "nao esta respondendo" - e
// o carregamento do mapa leva dezenas de segundos no celular. O motor roda
// numa thread propria; o toque vira uma fila que o motor esvazia em
// glfwPollEvents. Quando o sistema tira a janela, a thread do Android espera
// o motor terminar antes de devolve-la (destruir a swapchain de uma janela
// ja' devolvida derrubava o driver).
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include "core/Input.hpp"
#include "core/Platform.hpp"
#include "core/TouchRouter.hpp"

#include <android/asset_manager.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/window.h>
#include <android_native_app_glue.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <array>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <unistd.h>
#include <utility>
#include <vector>

int eruptionMain(int argc, char** argv);

struct GLFWwindow {
    void* user = nullptr;
    bool shouldClose = false;
    double cursorX = 0.0, cursorY = 0.0;
    int buttons[3] = {GLFW_RELEASE, GLFW_RELEASE, GLFW_RELEASE};
    GLFWkeyfun keyCb = nullptr;
    GLFWcharfun charCb = nullptr;
    GLFWmousebuttonfun mouseCb = nullptr;
    GLFWcursorposfun cursorCb = nullptr;
    GLFWscrollfun scrollCb = nullptr;
    GLFWframebuffersizefun sizeCb = nullptr;
    int lastW = 0, lastH = 0;
};

namespace {

const char* kTag = "Eruption";
android_app* g_app = nullptr;
GLFWwindow g_window;
std::atomic<bool> g_closeRequested{false}; // sistema pediu para fechar (thread do Android)
std::atomic<bool> g_windowReady{false};    // janela em paisagem, o motor pode criar a superficie
std::atomic<bool> g_engineDone{false};
// Pausa: janela tirada pelo sistema (apps recentes/home). O motor solta a
// superficie e espera a janela nova em vez de fechar.
std::atomic<bool> g_surfaceLost{false};     // thread do Android pediu para soltar
std::atomic<bool> g_surfaceReleased{false}; // motor soltou
std::atomic<bool> g_windowBack{false};      // janela nova chegou
void (*g_releaseHook)(void*) = nullptr;
int (*g_restoreHook)(void*) = nullptr;
void* g_hookUser = nullptr;
void (*g_pauseHook)(void*, int) = nullptr;
void* g_pauseUser = nullptr;
std::mutex g_doneMutex;
std::condition_variable g_doneCv;

// Fila de entrada: preenchida pela thread do Android, esvaziada pelo motor.
struct InputEvent {
    enum Kind { Cursor, Button, Scroll, Touch } kind;
    double x = 0.0, y = 0.0;
    int button = 0, action = 0;
    int64_t t = 0; // toque: hora do evento no Android (ns), nao a do quadro
};
std::mutex g_inputMutex;
std::vector<InputEvent> g_inputQueue;
int g_buttonState[3] = {GLFW_RELEASE, GLFW_RELEASE, GLFW_RELEASE}; // visao da thread do Android
void pushInput(const InputEvent& e) {
    std::lock_guard<std::mutex> lock(g_inputMutex);
    g_inputQueue.push_back(e);
}
GLFWvidmode g_mode{};

// ---- log -------------------------------------------------------------------
void startLogPump() {
    // O processo sobrevive a' activity: na recriada o stdout ja' vai para o pipe.
    static bool started = false;
    if (started) return;
    started = true;
    static int fds[2];
    if (pipe(fds) != 0) return;
    setvbuf(stdout, nullptr, _IOLBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    dup2(fds[1], STDOUT_FILENO);
    dup2(fds[1], STDERR_FILENO);
    std::thread([] {
        char buf[1024];
        std::string line;
        ssize_t n;
        while ((n = read(fds[0], buf, sizeof(buf))) > 0) {
            line.append(buf, static_cast<size_t>(n));
            size_t p;
            while ((p = line.find('\n')) != std::string::npos) {
                __android_log_write(ANDROID_LOG_INFO, kTag, line.substr(0, p).c_str());
                line.erase(0, p + 1);
            }
        }
    }).detach();
}

// ---- arquivos do APK --------------------------------------------------------
std::string readAsset(AAssetManager* am, const char* path) {
    AAsset* a = AAssetManager_open(am, path, AASSET_MODE_BUFFER);
    if (!a) return {};
    std::string s(static_cast<const char*>(AAsset_getBuffer(a)), static_cast<size_t>(AAsset_getLength(a)));
    AAsset_close(a);
    return s;
}

// Arquivo que o APK anterior extraiu e este nao traz mais: apaga, e as pastas
// que ficarem vazias. So' o que esta' na lista velha (.manifest), entao o que
// o jogo grava (saves, configuracao, preset_override.txt) nunca e' tocado.
// Sem isso, um mapa ou script tirado do jogo seguia no aparelho depois do
// install -r e continuava sendo lido.
size_t removeStale(const std::string& root, const std::string& oldList, const std::string& newList) {
    namespace fs = std::filesystem;
    std::unordered_set<std::string> keep;
    {
        std::istringstream in(newList);
        std::string rel;
        while (std::getline(in, rel))
            if (!rel.empty()) keep.insert(rel);
    }
    std::istringstream in(oldList);
    std::string rel;
    size_t count = 0;
    while (std::getline(in, rel)) {
        if (rel.empty() || keep.count(rel) || rel.find("..") != std::string::npos || rel[0] == '/') continue;
        std::error_code ec;
        fs::path f = fs::path(root) / rel;
        if (!fs::remove(f, ec)) continue;
        ++count;
        for (f = f.parent_path(); f != fs::path(root) && fs::is_empty(f, ec) && !ec; f = f.parent_path())
            fs::remove(f, ec);
    }
    return count;
}

// android/manifest.txt lista os arquivos; android/stamp.txt muda a cada APK.
// So' copia de novo quando o carimbo muda; a lista copiada fica em .manifest
// para a proxima versao saber o que apagar.
bool extractAssets(AAssetManager* am, const std::string& root) {
    namespace fs = std::filesystem;
    const std::string stamp = readAsset(am, "android/stamp.txt");
    std::string have;
    {
        std::ifstream in(root + "/.stamp");
        std::getline(in, have, '\0');
    }
    if (!stamp.empty() && have == stamp) return true;
    const std::string manifest = readAsset(am, "android/manifest.txt");
    {
        std::ifstream old(root + "/.manifest");
        const std::string oldList((std::istreambuf_iterator<char>(old)), std::istreambuf_iterator<char>());
        if (const size_t gone = removeStale(root, oldList, manifest))
            __android_log_print(ANDROID_LOG_INFO, kTag, "%zu arquivos da versao anterior apagados", gone);
    }
    std::istringstream list(manifest);
    std::string rel;
    size_t count = 0;
    while (std::getline(list, rel)) {
        if (rel.empty()) continue;
        AAsset* a = AAssetManager_open(am, rel.c_str(), AASSET_MODE_STREAMING);
        if (!a) {
            __android_log_print(ANDROID_LOG_WARN, kTag, "asset ausente: %s", rel.c_str());
            continue;
        }
        const fs::path out = fs::path(root) / rel;
        std::error_code ec;
        fs::create_directories(out.parent_path(), ec);
        FILE* f = std::fopen(out.c_str(), "wb");
        if (f) {
            char buf[1 << 16];
            int n;
            while ((n = AAsset_read(a, buf, sizeof(buf))) > 0) std::fwrite(buf, 1, static_cast<size_t>(n), f);
            std::fclose(f);
        }
        AAsset_close(a);
        ++count;
    }
    std::ofstream(root + "/.manifest") << manifest;
    std::ofstream(root + "/.stamp") << stamp;
    __android_log_print(ANDROID_LOG_INFO, kTag, "%zu arquivos extraidos para %s", count, root.c_str());
    return true;
}

// ---- toque ------------------------------------------------------------------
// A thread do Android so' enfileira cada dedo pelo id (pousou, moveu, saiu); quem
// decide o papel de cada um e' a thread do motor (rotearToque), que conhece as
// areas da interface do quadro.
enum TouchKind { kTouchDown, kTouchMove, kTouchUp, kTouchCancel };

int32_t onInput(android_app*, AInputEvent* ev) {
    if (AInputEvent_getType(ev) != AINPUT_EVENT_TYPE_MOTION) return 0;
    const int32_t raw = AMotionEvent_getAction(ev);
    const int32_t action = raw & AMOTION_EVENT_ACTION_MASK;
    const size_t idx = static_cast<size_t>((raw & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >>
                                           AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
    const size_t n = AMotionEvent_getPointerCount(ev);
    const auto touch = [ev](size_t i, int kind) {
        pushInput({InputEvent::Touch, AMotionEvent_getX(ev, i), AMotionEvent_getY(ev, i),
                   AMotionEvent_getPointerId(ev, i), kind, AMotionEvent_getEventTime(ev)});
    };
    switch (action) {
        case AMOTION_EVENT_ACTION_DOWN:
        case AMOTION_EVENT_ACTION_POINTER_DOWN:
            if (idx < n) touch(idx, kTouchDown);
            break;
        case AMOTION_EVENT_ACTION_MOVE:
            for (size_t i = 0; i < n; ++i) touch(i, kTouchMove);
            break;
        case AMOTION_EVENT_ACTION_UP:
        case AMOTION_EVENT_ACTION_POINTER_UP:
            if (idx < n) touch(idx, kTouchUp);
            break;
        case AMOTION_EVENT_ACTION_CANCEL:
            for (size_t i = 0; i < n; ++i) touch(i, kTouchCancel);
            break;
        default:
            break;
    }
    return 1;
}

void onCmd(android_app* app, int32_t cmd) {
    if (cmd == APP_CMD_INIT_WINDOW && g_surfaceReleased) {
        g_windowBack = true;
        return;
    }
    if (cmd != APP_CMD_TERM_WINDOW && cmd != APP_CMD_DESTROY) return;
    __android_log_print(ANDROID_LOG_WARN, "Eruption", "janela fechada pelo sistema (cmd %d)", cmd);
    // Antes do motor comecar (janela em retrato trocada pela de paisagem) nao
    // ha' nada a fechar.
    if (!g_windowReady) return;
    if (cmd == APP_CMD_TERM_WINDOW && g_releaseHook && !g_engineDone) {
        // Pausa: o motor solta a superficie antes de a janela voltar ao sistema.
        g_windowBack = false;
        g_surfaceLost = true;
        std::unique_lock<std::mutex> lock(g_doneMutex);
        g_doneCv.wait_for(lock, std::chrono::seconds(4),
                          [] { return g_surfaceReleased.load() || g_engineDone.load(); });
        return;
    }
    g_closeRequested = true;
    // A janela so' pode voltar ao sistema depois que o motor soltar a
    // superficie. Teto de 4 s: o sistema da' ~5 s antes do "nao responde".
    std::unique_lock<std::mutex> lock(g_doneMutex);
    g_doneCv.wait_for(lock, std::chrono::seconds(4), [] { return g_engineDone.load(); });
    (void)app;
}

void pumpEvents(int timeoutMs) {
    int events;
    android_poll_source* source;
    while (ALooper_pollOnce(timeoutMs, nullptr, &events, reinterpret_cast<void**>(&source)) >= 0) {
        if (source) source->process(g_app, source);
        if (g_app->destroyRequested) {
            g_closeRequested = true;
            break;
        }
        timeoutMs = 0;
    }
}

int windowW() { return g_app && g_app->window ? ANativeWindow_getWidth(g_app->window) : 0; }
int windowH() { return g_app && g_app->window ? ANativeWindow_getHeight(g_app->window) : 0; }

// Escala de saida 0.5..1: o swapchain sai menor que a tela e o compositor do
// Android estica de graca no hardware de video. Tudo que roda na resolucao de
// saida (FSR, pos, UI) fica proporcionalmente mais barato. Toque e ImGui
// seguem em coordenadas da janela; so' o framebuffer encolhe. Vem do preset
// ("display_scale", platform::setDisplayScale); ERUPTION_DISPLAY_SCALE manda.
float g_displayScale = 1.0f;
bool g_displayScaleFromEnv = false;

float displayScale() {
    static const bool s_init = [] {
        if (const char* e = std::getenv("ERUPTION_DISPLAY_SCALE")) {
            // O env vai ate' 0,25 (bancada: separa custo por pixel de custo fixo
            // num aparelho sem timestamp); o preset continua com piso de 0,5.
            g_displayScale = std::clamp(static_cast<float>(std::atof(e)), 0.25f, 1.0f);
            g_displayScaleFromEnv = true;
        }
        return true;
    }();
    (void)s_init;
    return g_displayScale;
}

} // namespace

namespace eruption::platform {
void setDisplayScale(float scale) {
    displayScale(); // le' o env antes de decidir
    const float s = std::clamp(scale, 0.5f, 1.0f);
    if (g_displayScaleFromEnv || s == g_displayScale) return;
    g_displayScale = s;
    g_window.lastW = g_window.lastH = 0; // o proximo glfwPollEvents recria o swapchain
}
} // namespace eruption::platform

namespace eruption::platform {
// Densidade da tela: AConfiguration da' dpi (160 = 1 px por dp). O toque e o
// ImGui seguem a janela, nao o framebuffer, entao display_scale nao entra.
float pixelsPerDp() {
    const int32_t dpi = g_app && g_app->config ? AConfiguration_getDensity(g_app->config) : 0;
    return dpi > 0 && dpi != ACONFIGURATION_DENSITY_NONE ? static_cast<float>(dpi) / 160.0f : 1.0f;
}

// Chama EruptionActivity.shareFile(String,String) por JNI: o Java abre o
// share sheet do sistema com um content:// (EruptionFileProvider), ja' que o
// Android nao deixa outro app abrir um caminho de arquivo direto. 'path' tem
// que estar dentro de internalDataPath/export (unica pasta que o provider
// serve). Chamado da thread do motor (nao a do Android): anexa e desanexa da
// JVM a cada chamada, compartilhar e' raro e nao vale manter anexado.
bool shareFile(const std::string& path, const char* mimeType) {
    if (!g_app || !g_app->activity || !g_app->activity->vm) return false;
    JavaVM* vm = g_app->activity->vm;
    JNIEnv* env = nullptr;
    const jint getRc = vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    const bool attached = getRc == JNI_OK;
    if (!attached && vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return false;
    bool ok = false;
    jobject activity = g_app->activity->clazz;
    jclass cls = env->GetObjectClass(activity);
    jmethodID mid = env->GetMethodID(cls, "shareFile", "(Ljava/lang/String;Ljava/lang/String;)Z");
    if (mid) {
        jstring jpath = env->NewStringUTF(path.c_str());
        jstring jmime = env->NewStringUTF(mimeType && *mimeType ? mimeType : "*/*");
        ok = env->CallBooleanMethod(activity, mid, jpath, jmime) == JNI_TRUE;
        env->DeleteLocalRef(jpath);
        env->DeleteLocalRef(jmime);
    } else {
        env->ExceptionClear();
        __android_log_print(ANDROID_LOG_ERROR, kTag, "shareFile: EruptionActivity.shareFile nao encontrado");
    }
    env->DeleteLocalRef(cls);
    if (!attached) vm->DetachCurrentThread();
    return ok;
}
} // namespace eruption::platform

// ---- GLFW -------------------------------------------------------------------
int glfwInit() { return GLFW_TRUE; }
void glfwTerminate() {}
void glfwWindowHint(int, int) {}
GLFWmonitor* glfwGetPrimaryMonitor() { return reinterpret_cast<GLFWmonitor*>(&g_mode); }
const GLFWvidmode* glfwGetVideoMode(GLFWmonitor*) {
    g_mode = {windowW(), windowH(), 8, 8, 8, 60};
    return &g_mode;
}
GLFWwindow* glfwCreateWindow(int, int, const char*, GLFWmonitor*, GLFWwindow*) {
    g_window.lastW = windowW();
    g_window.lastH = windowH();
    return g_app && g_app->window ? &g_window : nullptr;
}
void glfwDestroyWindow(GLFWwindow*) {}
void glfwSetWindowPos(GLFWwindow*, int, int) {}
void glfwSetWindowSize(GLFWwindow*, int, int) {}
void glfwSetWindowTitle(GLFWwindow*, const char*) {}
void glfwSetWindowIcon(GLFWwindow*, int, const GLFWimage*) {}
void glfwMaximizeWindow(GLFWwindow*) {}
int glfwWindowShouldClose(GLFWwindow* w) { return (w && w->shouldClose) || g_closeRequested ? GLFW_TRUE : GLFW_FALSE; }
void glfwSetWindowShouldClose(GLFWwindow* w, int v) { if (w) w->shouldClose = v != 0; }
void eruptionAndroidSetSurfaceHooks(void (*release)(void*), int (*restore)(void*), void* user) {
    g_releaseHook = release;
    g_restoreHook = restore;
    g_hookUser = user;
}

void eruptionAndroidSetPauseHook(void (*pause)(void*, int), void* user) {
    g_pauseHook = pause;
    g_pauseUser = user;
}

namespace {
// ---- roteador de toque (thread do motor): core/TouchRouter, o mesmo do navegador.
void cursorMotor(double x, double y) {
    g_window.cursorX = x;
    g_window.cursorY = y;
    if (g_window.cursorCb) g_window.cursorCb(&g_window, x, y);
}
void botaoMotor(int b, int action) {
    if (g_window.buttons[b] == action) return;
    g_window.buttons[b] = action;
    if (g_window.mouseCb) g_window.mouseCb(&g_window, b, action, 0);
}
eruption::TouchRouter& roteador() {
    static eruption::TouchRouter r({
        cursorMotor,
        botaoMotor,
        [](double dy) { if (g_window.scrollCb) g_window.scrollCb(&g_window, 0.0, dy); },
        [](float x, float y) { ImGui_ImplGlfw_CursorPosCallback(&g_window, x, y); },
        [](int action) { ImGui_ImplGlfw_MouseButtonCallback(&g_window, GLFW_MOUSE_BUTTON_LEFT, action, 0); },
        windowH,
    });
    return r;
}
void soltarDedos() { roteador().releaseAll(); }
void rotearToque(const InputEvent& e) {
    const float x = static_cast<float>(e.x), y = static_cast<float>(e.y);
    // ERUPTION_TEST_TOUCH_LOG=1 (teste): cada dedo no logcat, com a hora do
    // evento, para conferir gestos injetados (sendevent) contra os limites.
    static const bool kLog = std::getenv("ERUPTION_TEST_TOUCH_LOG") != nullptr;
    if (kLog)
        __android_log_print(ANDROID_LOG_INFO, "Eruption", "[TOQUE] id=%d acao=%d x=%.0f y=%.0f t=%.1f ms", e.button,
                            e.action, static_cast<double>(x), static_cast<double>(y), static_cast<double>(e.t) * 1e-6);
    switch (e.action) {
        case kTouchDown: roteador().down(e.button, x, y, e.t); break;
        case kTouchMove: roteador().move(e.button, x, y); break;
        case kTouchUp: roteador().up(e.button, x, y, e.t, false); break;
        default: roteador().up(e.button, x, y, e.t, true); break;
    }
}

} // namespace

void glfwPollEvents() {
    // Pausa (thread do motor): solta a superficie, avisa a thread do Android e
    // espera a janela nova; ao voltar recria e segue no mesmo ponto do jogo.
    if (g_surfaceLost && !g_surfaceReleased) {
        g_releaseHook(g_hookUser);
        {
            std::lock_guard<std::mutex> lock(g_doneMutex);
            g_surfaceReleased = true;
        }
        g_doneCv.notify_all();
        __android_log_print(ANDROID_LOG_WARN, "Eruption", "pausado: superficie solta");
        if (g_pauseHook) g_pauseHook(g_pauseUser, 1);
        while (!g_windowBack && !g_closeRequested) std::this_thread::sleep_for(std::chrono::milliseconds(30));
        if (g_pauseHook) g_pauseHook(g_pauseUser, 0);
        if (!g_closeRequested && g_app->window) {
            const int ok = g_restoreHook(g_hookUser);
            __android_log_print(ANDROID_LOG_WARN, "Eruption", "retomado (%d)", ok);
            if (!ok) g_closeRequested = true;
        }
        g_surfaceLost = false;
        g_surfaceReleased = false;
        g_windowBack = false;
        g_window.lastW = g_window.lastH = 0; // forca o callback de tamanho abaixo
        {
            std::lock_guard<std::mutex> lock(g_inputMutex);
            g_inputQueue.clear();
        }
        soltarDedos();
        botaoMotor(GLFW_MOUSE_BUTTON_RIGHT, GLFW_RELEASE);
    }
    // Roda na thread do motor: aplica o toque que a thread do Android enfileirou.
    // Aperto que vem depois de o cursor pular para o dedo fica para o quadro
    // seguinte: no mesmo quadro o motor via o salto com o botao apertado e
    // girava a camera para o outro lado.
    std::vector<InputEvent> events;
    {
        std::lock_guard<std::mutex> lock(g_inputMutex);
        size_t n = 0;
        bool moved = false;
        for (; n < g_inputQueue.size(); ++n) {
            const InputEvent& e = g_inputQueue[n];
            if (e.kind == InputEvent::Button && e.action == GLFW_PRESS && moved) break;
            if (e.kind == InputEvent::Touch && e.action == kTouchDown && moved) break;
            if (e.kind == InputEvent::Cursor || (e.kind == InputEvent::Touch && e.action != kTouchDown)) moved = true;
        }
        events.assign(g_inputQueue.begin(), g_inputQueue.begin() + static_cast<std::ptrdiff_t>(n));
        g_inputQueue.erase(g_inputQueue.begin(), g_inputQueue.begin() + static_cast<std::ptrdiff_t>(n));
    }
    for (const InputEvent& e : events) {
        switch (e.kind) {
            case InputEvent::Cursor:
                g_window.cursorX = e.x;
                g_window.cursorY = e.y;
                if (g_window.cursorCb) g_window.cursorCb(&g_window, e.x, e.y);
                break;
            case InputEvent::Button: {
                // Toque que comeca sobre a interface (painel, analogico) e' so'
                // dela: vai para o ImGui e nao chega ao motor, que giraria a
                // camera. O clique de toque curto que vem depois tambem e' dela.
                static bool uiTouch = false, uiTap = false;
                if (e.button == GLFW_MOUSE_BUTTON_RIGHT && e.action == GLFW_PRESS)
                    uiTouch = eruption::Input::inUiTouchRect(static_cast<float>(g_window.cursorX),
                                                             static_cast<float>(g_window.cursorY));
                if (uiTouch && e.button == GLFW_MOUSE_BUTTON_RIGHT) {
                    ImGui_ImplGlfw_MouseButtonCallback(&g_window, e.button, e.action, 0);
                    if (e.action == GLFW_RELEASE) {
                        uiTouch = false;
                        uiTap = true;
                    }
                    break;
                }
                if (e.button == GLFW_MOUSE_BUTTON_LEFT && uiTap) {
                    if (e.action == GLFW_RELEASE) uiTap = false;
                    break;
                }
                if (e.button == GLFW_MOUSE_BUTTON_RIGHT && e.action == GLFW_PRESS) uiTap = false;
                g_window.buttons[e.button] = e.action;
                if (g_window.mouseCb) g_window.mouseCb(&g_window, e.button, e.action, 0);
                break;
            }
            case InputEvent::Scroll:
                if (g_window.scrollCb) g_window.scrollCb(&g_window, e.x, e.y);
                break;
            case InputEvent::Touch:
                rotearToque(e);
                break;
        }
    }
    const int w = windowW(), h = windowH();
    if ((w != g_window.lastW || h != g_window.lastH) && w > 0 && h > 0) {
        g_window.lastW = w;
        g_window.lastH = h;
        if (g_window.sizeCb) {
            int fw = 0, fh = 0;
            glfwGetFramebufferSize(&g_window, &fw, &fh);
            g_window.sizeCb(&g_window, fw, fh);
        }
    }
}
void glfwGetFramebufferSize(GLFWwindow*, int* w, int* h) {
    if (w) *w = static_cast<int>(std::lround(windowW() * displayScale()));
    if (h) *h = static_cast<int>(std::lround(windowH() * displayScale()));
}
void glfwGetWindowSize(GLFWwindow*, int* w, int* h) {
    if (w) *w = windowW();
    if (h) *h = windowH();
}
void glfwSetWindowUserPointer(GLFWwindow* w, void* p) { if (w) w->user = p; }
void* glfwGetWindowUserPointer(GLFWwindow* w) { return w ? w->user : nullptr; }
void glfwSetInputMode(GLFWwindow*, int, int) {}
int glfwGetMouseButton(GLFWwindow* w, int b) { return w && b >= 0 && b < 3 ? w->buttons[b] : GLFW_RELEASE; }
void glfwGetCursorPos(GLFWwindow* w, double* x, double* y) {
    if (x) *x = w ? w->cursorX : 0.0;
    if (y) *y = w ? w->cursorY : 0.0;
}
GLFWkeyfun glfwSetKeyCallback(GLFWwindow* w, GLFWkeyfun cb) { std::swap(w->keyCb, cb); return cb; }
GLFWcharfun glfwSetCharCallback(GLFWwindow* w, GLFWcharfun cb) { std::swap(w->charCb, cb); return cb; }
GLFWmousebuttonfun glfwSetMouseButtonCallback(GLFWwindow* w, GLFWmousebuttonfun cb) { std::swap(w->mouseCb, cb); return cb; }
GLFWcursorposfun glfwSetCursorPosCallback(GLFWwindow* w, GLFWcursorposfun cb) { std::swap(w->cursorCb, cb); return cb; }
GLFWscrollfun glfwSetScrollCallback(GLFWwindow* w, GLFWscrollfun cb) { std::swap(w->scrollCb, cb); return cb; }
GLFWframebuffersizefun glfwSetFramebufferSizeCallback(GLFWwindow* w, GLFWframebuffersizefun cb) {
    std::swap(w->sizeCb, cb);
    return cb;
}
int glfwJoystickPresent(int) { return GLFW_FALSE; }
int glfwJoystickIsGamepad(int) { return GLFW_FALSE; }
const char* glfwGetJoystickName(int) { return nullptr; }
int glfwGetGamepadState(int, GLFWgamepadstate*) { return GLFW_FALSE; }
const float* glfwGetJoystickAxes(int, int* count) { if (count) *count = 0; return nullptr; }
const unsigned char* glfwGetJoystickButtons(int, int* count) { if (count) *count = 0; return nullptr; }
const char** glfwGetRequiredInstanceExtensions(uint32_t* count) {
    static const char* ext[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
    if (count) *count = 2;
    return ext;
}
VkResult glfwCreateWindowSurface(VkInstance instance, GLFWwindow*, const VkAllocationCallbacks* allocator,
                                 VkSurfaceKHR* surface) {
    VkAndroidSurfaceCreateInfoKHR ci{};
    ci.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
    ci.window = g_app->window;
    return vkCreateAndroidSurfaceKHR(instance, &ci, allocator, surface);
}

// ---- ImGui ------------------------------------------------------------------
namespace {
std::chrono::steady_clock::time_point g_lastFrame;
}
bool ImGui_ImplGlfw_InitForVulkan(GLFWwindow*, bool) {
    ImGuiIO& io = ImGui::GetIO();
    io.BackendPlatformName = "eruption_android";
    g_lastFrame = std::chrono::steady_clock::now();
    return true;
}
void ImGui_ImplGlfw_Shutdown() {}
void ImGui_ImplGlfw_NewFrame() {
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(windowW()), static_cast<float>(windowH()));
    io.DisplayFramebufferScale = ImVec2(displayScale(), displayScale());
    const auto now = std::chrono::steady_clock::now();
    io.DeltaTime = std::max(std::chrono::duration<float>(now - g_lastFrame).count(), 1e-4f);
    g_lastFrame = now;
}
void ImGui_ImplGlfw_KeyCallback(GLFWwindow*, int, int, int, int) {}
void ImGui_ImplGlfw_MouseButtonCallback(GLFWwindow*, int button, int action, int) {
    // Para o motor, um dedo e' o botao direito (gira a camera); para o ImGui,
    // qualquer toque de um dedo e' o esquerdo, senao nenhum controle da tela
    // responde. Dois dedos (botao do meio) ficam so' com a camera.
    if (button == GLFW_MOUSE_BUTTON_RIGHT || button == GLFW_MOUSE_BUTTON_LEFT)
        ImGui::GetIO().AddMouseButtonEvent(0, action == GLFW_PRESS);
}
void ImGui_ImplGlfw_CursorPosCallback(GLFWwindow*, double x, double y) {
    ImGui::GetIO().AddMousePosEvent(static_cast<float>(x), static_cast<float>(y));
}
void ImGui_ImplGlfw_ScrollCallback(GLFWwindow*, double xo, double yo) {
    ImGui::GetIO().AddMouseWheelEvent(static_cast<float>(xo), static_cast<float>(yo));
}
void ImGui_ImplGlfw_CharCallback(GLFWwindow*, unsigned int c) { ImGui::GetIO().AddInputCharacter(c); }

// ---- entrada ----------------------------------------------------------------
void android_main(android_app* app) {
    // Globais sobrevivem entre activities no mesmo processo: a recriada (o
    // sistema destroi a primeira, p.ex. ao girar) nao pode herdar nada.
    g_window = GLFWwindow{};
    soltarDedos();
    g_closeRequested = false;
    g_windowReady = false;
    g_engineDone = false;
    g_surfaceLost = false;
    g_surfaceReleased = false;
    g_windowBack = false;
    std::fill(std::begin(g_buttonState), std::end(g_buttonState), GLFW_RELEASE);
    {
        std::lock_guard<std::mutex> lock(g_inputMutex);
        g_inputQueue.clear();
    }
    g_app = app;
    app->onAppCmd = onCmd;
    app->onInputEvent = onInput;
    startLogPump();
    // Jogo nao deixa a tela apagar: o carregamento leva dezenas de segundos sem
    // toque, a tela dormia e o sistema tirava a janela no meio da carga.
    ANativeActivity_setWindowFlags(app->activity, AWINDOW_FLAG_KEEP_SCREEN_ON, 0);

    // Thread do motor: extracao dos arquivos, variaveis, espera da janela e o
    // jogo. Tudo que demora fica fora da thread que responde ao sistema.
    std::thread engine([app] {
        const std::string root = app->activity->internalDataPath;
        extractAssets(app->activity->assetManager, root);
        if (chdir(root.c_str()) != 0) __android_log_print(ANDROID_LOG_ERROR, kTag, "chdir falhou: %s", root.c_str());

        // android/env.txt: VAR=valor por linha (ex.: preset grafico). Lidos direto
        // do APK, como o args.txt (argumentos do main(), um por linha).
        AAssetManager* am = app->activity->assetManager;
        auto applyEnv = [](const std::string& text) {
            std::istringstream env(text);
            std::string line;
            while (std::getline(env, line)) {
                const size_t eq = line.find('=');
                if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
                setenv(line.substr(0, eq).c_str(), line.substr(eq + 1).c_str(), 1);
            }
        };
        applyEnv(readAsset(am, "android/env.txt"));
        // Ajuste sem reinstalar: env.txt na pasta externa do app (gravavel pelo
        // adb em /sdcard/Android/data/<pacote>/files) sobrepoe o do APK.
        if (app->activity->externalDataPath) {
            std::ifstream extra(std::string(app->activity->externalDataPath) + "/env.txt");
            if (extra) {
                std::stringstream ss;
                ss << extra.rdbuf();
                applyEnv(ss.str());
                __android_log_print(ANDROID_LOG_INFO, kTag, "env.txt externo aplicado");
            }
        }
        std::vector<std::string> args{"eruption-engine"};
        {
            std::istringstream in(readAsset(am, "android/args.txt"));
            std::string line;
            while (std::getline(in, line))
                if (!line.empty()) args.push_back(line);
        }
        // ERUPTION_TEST_MAP=<mapa> (teste, pelo env.txt): abre outro mapa do APK
        // no lugar do primeiro, sem remontar o pacote por mapa.
        if (const char* e = std::getenv("ERUPTION_TEST_MAP"); e && *e) {
            for (size_t i = 1; i + 1 < args.size(); ++i)
                if (args[i] == "--map") args[i + 1] = e;
        }

        while (!g_windowReady && !g_closeRequested) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        int rc = 0;
        if (!g_closeRequested) {
            std::vector<char*> argv;
            for (std::string& a : args) argv.push_back(a.data());
            argv.push_back(nullptr);
            rc = eruptionMain(static_cast<int>(args.size()), argv.data());
        }
        __android_log_print(ANDROID_LOG_INFO, kTag, "main() terminou com %d", rc);
        {
            std::lock_guard<std::mutex> lock(g_doneMutex);
            g_engineDone = true;
        }
        g_doneCv.notify_all();
    });

    // A janela nativa chega depois do primeiro ciclo de eventos. O manifesto
    // pede paisagem, mas o app pode abrir em retrato e o sistema gira logo
    // depois, destruindo a primeira janela: iniciar o Vulkan nela travava o
    // motor (S24). Libera o motor com a janela em paisagem, ou 3 s depois de
    // ela existir para aparelho que nao gira.
    std::chrono::steady_clock::time_point windowSince{};
    while (!g_engineDone) {
        pumpEvents(16);
        if (!g_windowReady && app->window) {
            if (windowSince == std::chrono::steady_clock::time_point{}) windowSince = std::chrono::steady_clock::now();
            if (windowW() >= windowH() || std::chrono::steady_clock::now() - windowSince > std::chrono::seconds(3))
                g_windowReady = true;
        }
    }
    engine.join();
    ANativeActivity_finish(app->activity);
    while (!app->destroyRequested) pumpEvents(100);
}
