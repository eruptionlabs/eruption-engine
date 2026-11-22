#include "core/CustomCursor.hpp"
#include "core/Logger.hpp"
#include "core/Project.hpp"
#include "utils/ImageUtils.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <stb_image.h>
#include <memory>
#include <unordered_map>
#include <vector>

#if !defined(__ANDROID__) && !defined(__EMSCRIPTEN__)
#define ERUPTION_HW_CURSOR 1
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_internal.h>
#endif

namespace eruption::customcursor {

#ifdef ERUPTION_HW_CURSOR
namespace {

constexpr int kMaxSide = 128;     // cursores maiores falham em varios sistemas
constexpr size_t kMaxEntries = 64; // acima disso, descarta os que nao estao em uso

struct Entry {
    std::vector<GLFWcursor*> cursors;           // vazio com frames: desenho por software
    std::vector<std::vector<uint8_t>> frames;   // RGBA fw x fh por quadro, sem escala (software)
    int fw = 0, fh = 0, hotX = 0, hotY = 0;     // hotspot em pixels da imagem original
    float fps = 8.0f;
    float userScale = 1.0f;                     // opcao scale do jogo (o software ja desenha em pixels logicos)
    bool software = false;                      // sem GLFWcursor: o motor desenha o cursor
};

std::unordered_map<std::string, Entry> s_cache;
std::string s_activeKey;           // vazio = cursor do sistema
const Entry* s_active = nullptr;
int s_activeFrame = -1;
int s_created = 0;
bool s_ownsImguiFlag = false;
bool s_hiddenBySoftware = false; // GLFW_CURSOR_HIDDEN posto por este arquivo

// O backend GLFW do ImGui chama glfwSetCursor(seta) em todo NewFrame, o que
// apagava o cursor do jogo (no X11, alternando com ele: cintilacao). Enquanto
// ha cursor do jogo, o ImGui nao mexe no cursor do sistema.
void guardImguiCursor(bool on) {
    if (!ImGui::GetCurrentContext()) return;
    ImGuiIO& io = ImGui::GetIO();
    if (on) {
        io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
        s_ownsImguiFlag = true;
    } else if (s_ownsImguiFlag) {
        io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
        s_ownsImguiFlag = false;
    }
}

// Copia o quadro (sx,sy,fw,fh) da folha para um RGBA de dw x dh, vizinho mais
// proximo (arte em pixels nao deve borrar).
std::vector<uint8_t> cropScaled(const uint8_t* src, int sw, int sx, int sy, int fw, int fh, int dw, int dh) {
    std::vector<uint8_t> out(size_t(dw) * dh * 4);
    for (int y = 0; y < dh; ++y) {
        const int syy = sy + std::min(fh - 1, int((y + 0.5f) * fh / dh));
        for (int x = 0; x < dw; ++x) {
            const int sxx = sx + std::min(fw - 1, int((x + 0.5f) * fw / dw));
            std::copy_n(src + (size_t(syy) * sw + sxx) * 4, 4, &out[(size_t(y) * dw + x) * 4]);
        }
    }
    return out;
}

void destroyEntry(Entry& e) {
    for (GLFWcursor* c : e.cursors) glfwDestroyCursor(c);
    e.cursors.clear();
}

// ERUPTION_SOFTWARE_CURSOR=1 (ou set_cursor com software=true): pula o GLFWcursor e
// desenha o cursor como sprite de UI por cima de tudo. Tambem e' o que acontece
// sozinho quando o sistema recusa o cursor de hardware (alguns compositores
// Wayland, imagem grande demais) e o GLFW nao entrega o cursor do jogo.
bool forceSoftware(const CursorOptions& opt) {
    const char* e = std::getenv("ERUPTION_SOFTWARE_CURSOR");
    return opt.software || (e && e[0] && e[0] != '0');
}

Entry build(const std::string& path, int hotX, int hotY, const CursorOptions& opt, float scale) {
    Entry e;
    e.fps = opt.fps > 0.0f ? opt.fps : 8.0f;
    int w = 0, h = 0, ch = 0;
    uint8_t* px = ImageUtils::loadRawVfs(project::gameRef(path).string(), w, h, ch, 4);
    if (!px) {
        ERUPTION_LOG_WARN("Cursor: nao consegui ler %s (cursor do sistema)", path.c_str());
        return e;
    }
    const int fw = opt.frameW > 0 ? std::min(opt.frameW, w) : w;
    const int fh = opt.frameH > 0 ? std::min(opt.frameH, h) : h;
    const int cols = std::max(1, w / fw);
    const int rows = std::max(1, h / fh);
    const int n = std::clamp(opt.frames, 1, cols * rows);
    e.fw = fw; e.fh = fh;
    e.hotX = std::clamp(hotX, 0, fw - 1);
    e.hotY = std::clamp(hotY, 0, fh - 1);
    e.userScale = opt.scale > 0.0f ? opt.scale : 1.0f;
    for (int i = 0; i < n; ++i) e.frames.push_back(cropScaled(px, w, (i % cols) * fw, (i / cols) * fh, fw, fh, fw, fh));
    stbi_image_free(px);
    if (forceSoftware(opt)) {
        e.software = true;
        ERUPTION_LOG_WARN("Cursor: %s por software (%d quadro(s))", path.c_str(), n);
        return e;
    }
    int dw = std::max(1, int(std::lround(fw * scale)));
    int dh = std::max(1, int(std::lround(fh * scale)));
    float k = scale;
    if (dw > kMaxSide || dh > kMaxSide) {
        const float f = float(kMaxSide) / float(std::max(dw, dh));
        dw = std::max(1, int(dw * f));
        dh = std::max(1, int(dh * f));
        k *= f;
    }
    const int hx = std::clamp(int(std::lround(hotX * k)), 0, dw - 1);
    const int hy = std::clamp(int(std::lround(hotY * k)), 0, dh - 1);
    for (int i = 0; i < n; ++i) {
        std::vector<uint8_t> frame = cropScaled(e.frames[size_t(i)].data(), fw, 0, 0, fw, fh, dw, dh);
        GLFWimage img{dw, dh, frame.data()};
        GLFWcursor* c = glfwCreateCursor(&img, hx, hy);
        if (!c) {
            ERUPTION_LOG_WARN("Cursor: o sistema recusou %s (cursor por software)", path.c_str());
            destroyEntry(e);
            e.software = true;
            return e;
        }
        ++s_created;
        e.cursors.push_back(c);
    }
    ERUPTION_LOG_INFO("Cursor: %s criado (%d quadro(s), %dx%d, hotspot %d,%d)", path.c_str(), int(e.cursors.size()),
                      dw, dh, hx, hy);
    return e;
}

void setSoftwareHook(bool on);

// Volta o cursor do sistema a ser visivel se foi este arquivo que o escondeu.
void releaseSoftware(GLFWwindow* window) {
    setSoftwareHook(false);
    if (!s_hiddenBySoftware) return;
    s_hiddenBySoftware = false;
    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
}

void showFrame(GLFWwindow* window, const Entry& e, int frame) {
    if (frame == s_activeFrame || e.cursors.empty()) return;
    s_activeFrame = frame;
    glfwSetCursor(window, e.cursors[size_t(frame)]);
}

void drawSoftware() {
    if (!s_active || !s_active->software || s_active->frames.empty() || !ImGui::GetCurrentContext()) return;
    if (!ImGui::IsMousePosValid()) return; // ponteiro fora da janela
    const Entry& e = *s_active;
    const size_t n = e.frames.size();
    const size_t f = n < 2 ? 0 : size_t(glfwGetTime() * e.fps) % n;
    const std::vector<uint8_t>& px = e.frames[f];
    const ImVec2 m = ImGui::GetIO().MousePos;
    const float k = e.userScale;
    const float ox = std::round(m.x - e.hotX * k), oy = std::round(m.y - e.hotY * k);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    // Uma retangulo por trecho horizontal de mesma cor: ~50 por quadro de 32x32.
    for (int y = 0; y < e.fh; ++y) {
        int x = 0;
        while (x < e.fw) {
            const uint8_t* p = &px[(size_t(y) * e.fw + x) * 4];
            if (p[3] == 0) { ++x; continue; }
            int x2 = x + 1;
            while (x2 < e.fw && std::equal(p, p + 4, &px[(size_t(y) * e.fw + x2) * 4])) ++x2;
            dl->AddRectFilled(ImVec2(ox + x * k, oy + y * k), ImVec2(ox + x2 * k, oy + (y + 1) * k),
                              IM_COL32(p[0], p[1], p[2], p[3]));
            x = x2;
        }
    }
}


ImGuiContext* s_hookCtx = nullptr;
ImGuiID s_hookId = 0;

void hookDraw(ImGuiContext*, ImGuiContextHook*) { drawSoftware(); }

// O cursor por software e' desenhado no EndFrame do ImGui (lista de primeiro
// plano), sem o Render do motor saber dele: serve a qualquer tela com ImGui.
void setSoftwareHook(bool on) {
    ImGuiContext* ctx = ImGui::GetCurrentContext();
    if (s_hookId && (!on || ctx != s_hookCtx)) {
        if (s_hookCtx == ctx) ImGui::RemoveContextHook(ctx, s_hookId);
        s_hookId = 0;
        s_hookCtx = nullptr;
    }
    if (on && ctx && !s_hookId) {
        ImGuiContextHook h;
        h.Type = ImGuiContextHookType_EndFramePre;
        h.Callback = hookDraw;
        s_hookId = ImGui::AddContextHook(ctx, &h);
        s_hookCtx = ctx;
    }
}

} // namespace

void set(GLFWwindow* window, const std::string& path, int hotX, int hotY, const CursorOptions& opt) {
    if (!window) return;
    if (path.empty()) {
        if (s_activeKey.empty()) return;
        s_activeKey.clear();
        s_active = nullptr;
        s_activeFrame = -1;
        guardImguiCursor(false);
        releaseSoftware(window);
        glfwSetCursor(window, nullptr);
        return;
    }
    float sx = 1.0f, sy = 1.0f;
    glfwGetWindowContentScale(window, &sx, &sy);
    const float scale = std::clamp(std::max(sx, 1.0f) * (opt.scale > 0.0f ? opt.scale : 1.0f), 0.25f, 16.0f);
    // A escala entra na chave: o cursor de hardware tem tamanho fixo.
    const std::string key = path + "|" + std::to_string(hotX) + "," + std::to_string(hotY) + "|" +
                            std::to_string(opt.frameW) + "x" + std::to_string(opt.frameH) + "|" +
                            std::to_string(opt.frames) + "|" + std::to_string(int(opt.fps * 100)) + "|" +
                            std::to_string(int(scale * 1000)) + (forceSoftware(opt) ? "|sw" : "");
    if (key == s_activeKey) return; // mesma chamada de sempre: nada a fazer
    auto it = s_cache.find(key);
    if (it == s_cache.end()) {
        if (s_cache.size() >= kMaxEntries) {
            for (auto d = s_cache.begin(); d != s_cache.end();) {
                if (d->first == s_activeKey) { ++d; continue; }
                destroyEntry(d->second);
                d = s_cache.erase(d);
            }
        }
        it = s_cache.emplace(key, build(path, hotX, hotY, opt, scale)).first;
    }
    s_activeKey = key;
    s_active = &it->second;
    s_activeFrame = -1;
    if (s_active->frames.empty()) {
        guardImguiCursor(false);
        releaseSoftware(window);
        glfwSetCursor(window, nullptr); // falhou: sistema
    } else if (s_active->software) {
        guardImguiCursor(true);
        glfwSetCursor(window, nullptr);
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_HIDDEN);
        s_hiddenBySoftware = true;
        setSoftwareHook(true);
    } else {
        releaseSoftware(window);
        guardImguiCursor(true);
        showFrame(window, *s_active, 0);
    }
}

void tick(GLFWwindow* window) {
    if (!s_active || s_active->frames.empty()) return;
    if (!s_ownsImguiFlag) { // o contexto do ImGui pode nascer depois do set_cursor
        guardImguiCursor(true);
        if (s_ownsImguiFlag) s_activeFrame = -1; // ele ja pode ter posto a seta: reaplica
    }
    if (s_active->software) { setSoftwareHook(true); return; } // contexto do ImGui novo: religa
    if (s_active->cursors.size() < 2) return;
    const double t = glfwGetTime();
    showFrame(window, *s_active, int(t * s_active->fps) % int(s_active->cursors.size()));
}

void shutdown(GLFWwindow* window) {
    // ERUPTION_TEST_CURSOR_COUNT=1 (teste): loga quantos GLFWcursor foram criados
    // na sessao (o log de INFO some do build de release).
    if (std::getenv("ERUPTION_TEST_CURSOR_COUNT")) ERUPTION_LOG_WARN("Cursor: criados=%d", s_created);
    guardImguiCursor(false);
    if (window) { releaseSoftware(window); glfwSetCursor(window, nullptr); }
    for (auto& [k, e] : s_cache) destroyEntry(e);
    s_cache.clear();
    s_activeKey.clear();
    s_active = nullptr;
    s_activeFrame = -1;
}

int createdCount() { return s_created; }

#else // Android e navegador: o jogo chama, nada acontece.

void set(GLFWwindow*, const std::string&, int, int, const CursorOptions&) {}
void tick(GLFWwindow*) {}
void shutdown(GLFWwindow*) {}
int createdCount() { return 0; }

#endif

} // namespace eruption::customcursor
