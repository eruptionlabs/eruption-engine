// Prova de render de um plugin nativo Unity pela camada compat, sem janela:
// cria um contexto OpenGL 3.2 core fora de tela (EGL pbuffer), anuncia
// OpenGLCore ao plugin, chama as exportacoes como o script C# faria
// (SetTimeFromUnity, SetTextureFromUnity) e emite o evento de render.
// Depois le o framebuffer e a textura de volta e confere que o plugin
// desenhou de fato.
//
//   eruption-compat-unity-gl libRenderingPlugin.so
//
// Ferramenta de teste: o motor em si e' Vulkan; a ponte de producao sera'
// IUnityGraphicsVulkan (ver a pesquisa da camada compat).

#include "host/Host.hpp"
#include "unity/UnityHost.hpp"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>

#include <cstdio>
#include <string>
#include <vector>

using namespace eruption::compat;

namespace {

constexpr int kLado = 256;

bool criarContexto() {
    EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (d == EGL_NO_DISPLAY || !eglInitialize(d, nullptr, nullptr)) {
        // Sem servidor grafico: tenta o primeiro dispositivo EGL (headless).
        auto consultar = reinterpret_cast<PFNEGLQUERYDEVICESEXTPROC>(eglGetProcAddress("eglQueryDevicesEXT"));
        auto plataforma = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
        EGLDeviceEXT dev[4];
        EGLint n = 0;
        if (!consultar || !plataforma || !consultar(4, dev, &n) || n < 1) return false;
        d = plataforma(EGL_PLATFORM_DEVICE_EXT, dev[0], nullptr);
        if (d == EGL_NO_DISPLAY || !eglInitialize(d, nullptr, nullptr)) return false;
    }
    const EGLint atributos[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_RED_SIZE, 8,
                                EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 24, EGL_NONE};
    EGLConfig cfg;
    EGLint n = 0;
    if (!eglChooseConfig(d, atributos, &cfg, 1, &n) || n < 1) return false;
    const EGLint pb[] = {EGL_WIDTH, kLado, EGL_HEIGHT, kLado, EGL_NONE};
    EGLSurface s = eglCreatePbufferSurface(d, cfg, pb);
    if (!eglBindAPI(EGL_OPENGL_API)) return false;
    const EGLint ctxAttr[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 2, EGL_CONTEXT_OPENGL_PROFILE_MASK,
                              EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
    EGLContext c = eglCreateContext(d, cfg, EGL_NO_CONTEXT, ctxAttr);
    return c != EGL_NO_CONTEXT && eglMakeCurrent(d, s, s, c);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "uso: %s libPlugin.so\n", argv[0]);
        return 2;
    }
    if (!criarContexto()) {
        std::fprintf(stderr, "sem contexto OpenGL fora de tela (EGL)\n");
        return 2;
    }
    std::printf("contexto GL: %s / %s\n", reinterpret_cast<const char*>(glGetString(GL_VERSION)),
                reinterpret_cast<const char*>(glGetString(GL_RENDERER)));

    unity::definirRenderizador(17); // OpenGLCore
    if (!unity::carregarPlugin(argv[1])) return 1;

    // Textura que o "script" entrega ao plugin (como SetTextureFromUnity no C#).
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    std::vector<unsigned char> zeros(kLado * kLado * 4, 0);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kLado, kLado, 0, GL_RGBA, GL_UNSIGNED_BYTE, zeros.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);

    std::string r;
    unity::chamarFuncao("SetTimeFromUnity", "v(f)", {"0.5"}, &r);
    unity::chamarFuncao("SetTextureFromUnity", "v(p,i,i)", {std::to_string(tex), std::to_string(kLado), std::to_string(kLado)}, &r);

    glViewport(0, 0, kLado, kLado);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    const int n = unity::emitirEventoRender(1); // GL.IssuePluginEvent(GetRenderEventFunc(), 1)
    glFinish();

    // Triangulo: pixels coloridos no framebuffer, ao redor do centro.
    std::vector<unsigned char> fb(kLado * kLado * 4);
    glReadPixels(0, 0, kLado, kLado, GL_RGBA, GL_UNSIGNED_BYTE, fb.data());
    int coloridos = 0;
    for (int i = 0; i < kLado * kLado; ++i)
        if (fb[i * 4] + fb[i * 4 + 1] + fb[i * 4 + 2] > 30) ++coloridos;
    const unsigned char* centro = &fb[((kLado / 2) * kLado + kLado / 2) * 4];

    // Textura: o plugin escreve um "plasma" de senos; conta valores distintos.
    std::vector<unsigned char> px(kLado * kLado * 4);
    glBindTexture(GL_TEXTURE_2D, tex);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    bool visto[256] = {};
    int distintos = 0;
    for (int i = 0; i < kLado * kLado; ++i)
        if (!visto[px[i * 4]]) {
            visto[px[i * 4]] = true;
            ++distintos;
        }

    std::printf("evento 1 entregue a %d plugin(s)\n", n);
    std::printf("framebuffer: %d de %d pixels coloridos; centro = (%d, %d, %d)\n", coloridos, kLado * kLado, centro[0], centro[1],
                centro[2]);
    std::printf("textura: %d tons distintos escritos pelo plugin\n", distintos);
    unity::encerrar();
    const bool ok = n == 1 && coloridos > 1000 && distintos > 16;
    std::printf("%s\n", ok ? "OK: o plugin desenhou o triangulo e atualizou a textura" : "FALHA");
    return ok ? 0 : 1;
}
