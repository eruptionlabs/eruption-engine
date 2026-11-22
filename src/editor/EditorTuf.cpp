// Janela > Pacotes (.tuf): o painel gráfico do arquivo .tuf (Tufo, tools/tufo),
// com as mesmas operações do `tufo` (tufo/Ops.hpp): árvore de pastas
// virtual com filtro, prévia da entrada, extrair, acrescentar, remover,
// verificar, compactar (repack) e estatísticas.
//
// Regras:
//  - Tarefas longas (verificar, extrair, acrescentar, remover, repack) rodam no
//    JobSystem com barra de progresso e botão Cancelar; a interface não trava.
//  - O painel abre o seu PRÓPRIO leitor do arquivo (o formato é imutável depois
//    de aberto), então navegar num pacote que o jogo montou é seguro.
//  - Ações que escrevem pedem confirmação e recusam um .tuf que o projeto em
//    execução tem montado (os leitores do jogo mapeiam o arquivo): a saída é
//    abrir uma cópia (Repack para um arquivo novo) e mexer nela.
//
// ERUPTION_TEST_TUF_PANEL=<arquivo.tuf>[|<entrada>] (teste/capturas): abre a
// janela no frame 40 com o pacote, expande as pastas e seleciona a entrada
// (a primeira imagem, sem o segundo campo).
#include "editor/Editor.hpp"
#include "editor/EditorTheme.hpp"
#include "editor/Translate.hpp"

#include "core/Engine.hpp"
#include "core/JobSystem.hpp"
#include "core/Project.hpp"
#include "utils/AssetVfs.hpp"
#include "utils/TufGpuReady.hpp"
#include "tufo/Ops.hpp"

#include <imgui.h>
#include <stb_image.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <future>
#include <set>
#include <thread>

namespace eruption {

namespace fs = std::filesystem;

namespace {

constexpr uint64_t kPreviewMaxRaw = 128ull << 20; // maior entrada que a prévia lê
constexpr size_t kTextShown = 64 * 1024;          // texto: só o começo
constexpr size_t kHexShown = 512;                 // hexdump curto
constexpr int kPreviewSide = 1024;                // imagem reduzida para no máximo isto

enum class PreviewKind { None, Image, Text, Hex, TooBig, Error };

// Resultado da prévia, montado fora da interface.
struct PreviewData {
    std::string key;
    PreviewKind kind = PreviewKind::None;
    std::string text; // texto, hexdump ou mensagem
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;         // reduzida
    int fileW = 0, fileH = 0; // original
};

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string extensionOf(const std::string& key) {
    const size_t dot = key.rfind('.');
    const size_t slash = key.rfind('/');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return "";
    return key.substr(dot + 1);
}

bool looksLikeText(const std::vector<uint8_t>& d) {
    const size_t n = std::min<size_t>(d.size(), 4096);
    if (n == 0) return true;
    size_t odd = 0;
    for (size_t i = 0; i < n; ++i) {
        const uint8_t c = d[i];
        if (c == 0) return false;
        if (c < 9 || (c > 13 && c < 32)) ++odd;
    }
    return odd * 50 < n; // até 2% de controle: texto
}

std::string hexdump(const std::vector<uint8_t>& d) {
    std::string out;
    const size_t n = std::min(d.size(), kHexShown);
    char buf[32]; // "%06zx" de size_t cabe ate 16 digitos
    for (size_t row = 0; row < n; row += 16) {
        std::snprintf(buf, sizeof buf, "%06zx  ", row);
        out += buf;
        for (size_t i = 0; i < 16; ++i) {
            if (row + i < n) {
                std::snprintf(buf, sizeof buf, "%02x ", d[row + i]);
                out += buf;
            } else {
                out += "   ";
            }
            if (i == 7) out += ' ';
        }
        out += " |";
        for (size_t i = 0; i < 16 && row + i < n; ++i) out += (d[row + i] >= 32 && d[row + i] < 127) ? static_cast<char>(d[row + i]) : '.';
        out += "|\n";
    }
    return out;
}

// Lê e prepara a prévia (fio de trabalho). O Archive é imutável: sem trava.
PreviewData buildPreview(std::shared_ptr<tufo::Archive> ar, const std::string& key, uint64_t rawSize) {
    PreviewData p;
    p.key = key;
    if (rawSize > kPreviewMaxRaw) {
        p.kind = PreviewKind::TooBig;
        return p;
    }
    // Texturas prontas para a GPU (formato versao 2): o .glb guardado nao tem as
    // imagens; a previa decodifica o BC do pacote companheiro numa folha de contato.
    tufo::Info info;
    if (ar->stat(key, &info) && (info.flags & (tufo::kFlagGpuReady | tufo::kFlagCompanion))) {
        std::string companion = key;
        if ((info.flags & tufo::kFlagGpuReady) && !tufGpuCompanionKey(*ar, key, companion)) {
            p.kind = PreviewKind::Error;
            p.text = "GPU-ready entry without its texture package";
            return p;
        }
        std::vector<uint8_t> pack;
        std::string perr;
        std::vector<TufDecodedImage> imgs;
        if (!ar->read(companion, pack, true, &perr) || !decodeTufGpuImages(pack, imgs)) {
            p.kind = PreviewKind::Error;
            p.text = perr.empty() ? std::string("Could not decode the GPU-ready textures") : perr;
            return p;
        }
        constexpr int kCell = 128, kCols = 8, kMaxImgs = 48;
        const int n = static_cast<int>(std::min<size_t>(imgs.size(), kMaxImgs));
        const int cols = std::min(kCols, n), rows = (n + cols - 1) / cols;
        p.w = cols * kCell;
        p.h = rows * kCell;
        p.fileW = p.w;
        p.fileH = p.h;
        p.rgba.assign(static_cast<size_t>(p.w) * static_cast<size_t>(p.h) * 4, 24);
        for (int i = 0; i < n; ++i) {
            const TufDecodedImage& im = imgs[static_cast<size_t>(i)];
            const int side = std::max(im.width, im.height);
            const int cw = std::max(1, im.width * kCell / side), ch = std::max(1, im.height * kCell / side);
            for (int y = 0; y < ch; ++y)
                for (int x = 0; x < cw; ++x) {
                    const size_t src = (static_cast<size_t>(y * im.height / ch) * static_cast<size_t>(im.width) +
                                        static_cast<size_t>(x * im.width / cw)) * 4;
                    const size_t dst = (static_cast<size_t>((i / cols) * kCell + y) * static_cast<size_t>(p.w) +
                                        static_cast<size_t>((i % cols) * kCell + x)) * 4;
                    std::memcpy(&p.rgba[dst], &im.rgba[src], 3);
                    p.rgba[dst + 3] = 255;
                }
        }
        p.text = std::to_string(imgs.size());
        p.kind = PreviewKind::Image;
        return p;
    }
    std::vector<uint8_t> d;
    std::string err;
    if (!ar->read(key, d, true, &err)) {
        p.kind = PreviewKind::Error;
        p.text = err;
        return p;
    }
    const std::string ext = lower(extensionOf(key));
    const bool imageExt = ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "bmp" || ext == "tga" || ext == "gif";
    if (imageExt) {
        int w = 0, h = 0, ch = 0;
        unsigned char* px = stbi_load_from_memory(d.data(), static_cast<int>(std::min<size_t>(d.size(), 0x7fffffff)), &w, &h, &ch, 4);
        if (px) {
            p.fileW = w;
            p.fileH = h;
            const int step = std::max(1, (std::max(w, h) + kPreviewSide - 1) / kPreviewSide);
            p.w = std::max(1, w / step);
            p.h = std::max(1, h / step);
            p.rgba.resize(static_cast<size_t>(p.w) * static_cast<size_t>(p.h) * 4);
            for (int y = 0; y < p.h; ++y)
                for (int x = 0; x < p.w; ++x)
                    std::memcpy(&p.rgba[(static_cast<size_t>(y) * static_cast<size_t>(p.w) + static_cast<size_t>(x)) * 4],
                                px + (static_cast<size_t>(y * step) * static_cast<size_t>(w) + static_cast<size_t>(x * step)) * 4, 4);
            stbi_image_free(px);
            p.kind = PreviewKind::Image;
            return p;
        }
        // Não decodificou (formato que o stb não lê, p.ex. KTX2): cai no hexdump.
    }
    const bool textExt = ext == "txt" || ext == "json" || ext == "luau" || ext == "lua" || ext == "md" || ext == "csv" ||
                         ext == "xml" || ext == "ini" || ext == "cfg" || ext == "glsl" || ext == "scene" || ext == "spr" ||
                         ext == "env" || ext == "mtl" || ext == "obj";
    if (textExt || (!imageExt && looksLikeText(d))) {
        p.kind = PreviewKind::Text;
        p.text.assign(reinterpret_cast<const char*>(d.data()), std::min(d.size(), kTextShown));
        if (d.size() > kTextShown) p.text += "\n...";
        return p;
    }
    p.kind = PreviewKind::Hex;
    p.text = hexdump(d);
    return p;
}

enum class TaskKind { None, Verify, Extract, Add, Remove, Repack };

// O que a tarefa devolve: um código de mensagem (traduzido na interface, com os
// números e textos abaixo) ou o texto de erro da biblioteca, que sai como veio.
enum class Msg { Error, Verified, VerifyFailed, VerifyCancelled, Extracted, Removed, Added, Repacked };
struct TaskResult {
    TaskKind kind = TaskKind::None;
    bool ok = false;
    Msg msg = Msg::Error;
    long count = 0;
    std::string a, b; // texto da mensagem (pasta, tamanho, saida)
    std::string error;
    std::vector<std::string> report;
};

std::string messageText(const TaskResult& r) {
    char buf[1400];
    switch (r.msg) {
        case Msg::Verified: std::snprintf(buf, sizeof buf, tr("All %ld entries verified."), r.count); break;
        case Msg::VerifyFailed: std::snprintf(buf, sizeof buf, tr("%ld entries FAILED verification."), r.count); break;
        case Msg::VerifyCancelled: std::snprintf(buf, sizeof buf, "%s", tr("Verification cancelled.")); break;
        case Msg::Extracted: std::snprintf(buf, sizeof buf, tr("%ld file(s) extracted to %s"), r.count, r.a.c_str()); break;
        case Msg::Removed: std::snprintf(buf, sizeof buf, tr("%ld path(s) removed. Repack to recover the space."), r.count); break;
        case Msg::Added: std::snprintf(buf, sizeof buf, tr("%ld file(s) added."), r.count); break;
        case Msg::Repacked: std::snprintf(buf, sizeof buf, tr("Repacked: %s in %s"), r.a.c_str(), r.b.c_str()); break;
        case Msg::Error: return r.error == "cancelled" ? std::string(tr("Cancelled.")) : r.error;
    }
    return buf;
}

} // namespace

struct Editor::TufState {
    char path[1024] = {};
    char filter[128] = {};
    char extractDir[1024] = {};
    char addPath[1024] = {};
    char addPrefix[256] = {};
    char repackOut[1024] = {};
    int level = 19;

    std::shared_ptr<tufo::Archive> ar;
    std::string openedPath;
    bool newPackage = false; // caminho que ainda não existe: o primeiro Adicionar cria
    std::vector<tufo::EntryInfo> entries;
    tufo::TreeNode tree;
    tufo::Stats stats;
    bool hasStats = false;
    bool showStats = false;

    struct Row {
        const tufo::TreeNode* node;
        int depth;
    };
    std::set<std::string> expanded;
    std::vector<Row> rows;      // linhas visíveis da árvore
    std::vector<int> matches;   // com filtro: índices em entries
    bool rowsDirty = true;
    std::string lastFilter;

    std::string selected; // chave ou pasta
    bool selectedDir = false;
    int selectedEntry = -1;

    // Prévia
    std::future<PreviewData> previewJob;
    std::string previewWanted;
    PreviewData preview;
    Editor::GuiTexture previewTex;

    // Tarefa longa
    std::future<TaskResult> task;
    tufo::Progress progress;
    TaskKind running = TaskKind::None;
    std::chrono::steady_clock::time_point taskStart;
    std::string status;
    bool statusError = false;
    std::vector<std::string> report; // linhas "RUIM ..." da última verificação

    // Confirmação
    TaskKind confirm = TaskKind::None;
    bool openConfirm = false;
    std::string confirmTarget;

    bool testDone = false;
    bool testLayout = false;

    void setStatus(const std::string& s, bool error) {
        status = s;
        statusError = error;
    }
};

namespace {

bool isTufFile(const std::string& p) {
    const std::string e = lower(fs::path(p).extension().string());
    return e == ".tuf";
}

// Os .tuf que o projeto aberto monta (e que o jogo em execução lê).
std::vector<std::string> mountedTufs() {
    std::vector<std::string> out;
    if (!vfs::packsMounted()) return out;
    for (const std::string& p : Project::instance().packFiles())
        if (isTufFile(p)) out.push_back(p);
    return out;
}

bool samePath(const std::string& a, const std::string& b) {
    std::error_code ec;
    const fs::path ca = fs::weakly_canonical(a, ec);
    const fs::path cb = fs::weakly_canonical(b, ec);
    return !ca.empty() && ca == cb;
}

bool isMounted(const std::string& path) {
    for (const std::string& m : mountedTufs())
        if (samePath(m, path)) return true;
    return false;
}

} // namespace

Editor::TufState& Editor::tufState() {
    if (!m_tuf) m_tuf = std::make_shared<TufState>();
    return *m_tuf;
}

void Editor::shutdownTuf() {
    if (!m_tuf) return;
    TufState& s = *m_tuf;
    s.progress.cancel = true;
    if (s.task.valid()) s.task.wait();
    if (s.previewJob.valid()) s.previewJob.wait();
    releaseTexture(s.previewTex);
}

namespace {

template <class S>
void refreshView(S& s, const std::string& path) {
    s.entries.clear();
    s.tree = tufo::TreeNode();
    s.hasStats = false;
    s.selected.clear();
    s.selectedEntry = -1;
    s.selectedDir = false;
    s.openedPath.clear();
    if (!s.ar || !s.ar->isOpen()) return;
    s.openedPath = path;
    s.entries = s.ar->listInfo();
    s.tree = tufo::buildTree(s.entries);
    s.stats = s.ar->stats();
    s.hasStats = true;
    s.rowsDirty = true;
}

} // namespace

// Abre (ou reabre) o arquivo do campo de caminho.
template <class S>
static bool tufOpen(S& s, const std::string& path, bool keepExpanded) {
    s.ar.reset();
    s.report.clear();
    s.newPackage = false;
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        refreshView(s, path);
        s.setStatus(tr("The file does not exist. Use \"New package\" to create it with the first files you add."), true);
        return false;
    }
    auto ar = std::make_shared<tufo::Archive>();
    std::string err;
    if (!ar->open(path, &err)) {
        refreshView(s, path);
        s.setStatus(err, true);
        return false;
    }
    s.ar = ar;
    if (!keepExpanded) s.expanded.clear();
    refreshView(s, path);
    s.setStatus("", false);
    return true;
}

namespace {

template <class S>
void rebuildRowsRec(S& s, const tufo::TreeNode& n, int depth) {
    for (const tufo::TreeNode& c : n.children) {
        s.rows.push_back({&c, depth});
        if (c.dir && s.expanded.count(c.path)) rebuildRowsRec(s, c, depth + 1);
    }
}

template <class S>
void rebuildRows(S& s) {
    s.rows.clear();
    s.matches.clear();
    const std::string f = lower(s.filter);
    if (f.empty()) {
        rebuildRowsRec(s, s.tree, 0);
    } else {
        for (size_t i = 0; i < s.entries.size(); ++i)
            if (s.entries[i].key.find(f) != std::string::npos) s.matches.push_back(static_cast<int>(i));
    }
    s.lastFilter = s.filter;
    s.rowsDirty = false;
}

template <class S>
void expandAll(S& s, const tufo::TreeNode& n) {
    for (const tufo::TreeNode& c : n.children)
        if (c.dir) {
            s.expanded.insert(c.path);
            expandAll(s, c);
        }
}

template <class S>
void selectEntry(S& s, int index) {
    if (index < 0 || index >= static_cast<int>(s.entries.size())) return;
    s.selected = s.entries[static_cast<size_t>(index)].key;
    s.selectedEntry = index;
    s.selectedDir = false;
}

template <class S>
void startTask(S& s, TaskKind kind, std::function<TaskResult()> fn) {
    s.running = kind;
    s.progress.done = 0;
    s.progress.total = 0;
    s.progress.cancel = false;
    s.taskStart = std::chrono::steady_clock::now();
    s.setStatus("", false);
    s.task = JobSystem::instance().submit(std::move(fn));
}

} // namespace

void Editor::drawTufPanel() {
    TufState& s = tufState();

    // Teste: abre a janela com o pacote pedido.
    static const char* testPanel = std::getenv("ERUPTION_TEST_TUF_PANEL");
    if (testPanel && !s.testDone && m_frame >= 40) {
        s.testDone = true;
        s.testLayout = true;
        m_showTuf = true;
        std::string spec = testPanel, key;
        const size_t bar = spec.find('|');
        if (bar != std::string::npos) {
            key = spec.substr(bar + 1);
            spec = spec.substr(0, bar);
        }
        std::snprintf(s.path, sizeof(s.path), "%s", spec.c_str());
        if (tufOpen(s, s.path, false)) {
            expandAll(s, s.tree);
            if (key.empty()) {
                for (size_t i = 0; i < s.entries.size() && key.empty(); ++i) {
                    const std::string e = lower(extensionOf(s.entries[i].key));
                    if (e == "png" || e == "jpg") key = s.entries[i].key;
                }
            }
            for (size_t i = 0; i < s.entries.size(); ++i)
                if (s.entries[i].key == key) selectEntry(s, static_cast<int>(i));
            s.showStats = true;
        }
    }

    // Termina a tarefa em andamento.
    if (s.task.valid() && s.task.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const TaskResult r = s.task.get();
        const TaskKind kind = s.running;
        s.running = TaskKind::None;
        if (kind == TaskKind::Verify) s.report = r.report;
        // Escreveu: o painel reabre o arquivo (o índice mudou).
        if (kind == TaskKind::Add || kind == TaskKind::Remove) tufOpen(s, s.path, true);
        s.setStatus(messageText(r), !r.ok);
    }
    // Prévia pronta: textura na thread da interface.
    if (s.previewJob.valid() && s.previewJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        PreviewData p = s.previewJob.get();
        if (p.key == s.selected) {
            releaseTexture(s.previewTex);
            s.preview = std::move(p);
            if (s.preview.kind == PreviewKind::Image) {
                s.previewTex = makeTexture(s.preview.rgba.data(), s.preview.w, s.preview.h, false);
                s.preview.rgba.clear();
                s.preview.rgba.shrink_to_fit();
            }
        }
    }
    // Pede a prévia da seleção (uma por vez).
    if (!s.selectedDir && s.selectedEntry >= 0 && s.ar && s.previewWanted != s.selected && !s.previewJob.valid()) {
        s.previewWanted = s.selected;
        s.preview = PreviewData();
        releaseTexture(s.previewTex);
        std::shared_ptr<tufo::Archive> ar = s.ar;
        const std::string key = s.selected;
        const uint64_t raw = s.entries[static_cast<size_t>(s.selectedEntry)].info.rawSize;
        s.previewJob = JobSystem::instance().submit([ar, key, raw] { return buildPreview(ar, key, raw); });
    }

    if (!m_showTuf) return;

    if (s.testLayout) {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + 24, vp->WorkPos.y + 24), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(std::min(1100.0f, vp->WorkSize.x - 48), std::min(680.0f, vp->WorkSize.y - 48)),
                                 ImGuiCond_Always);
        ImGui::SetNextWindowFocus();
    } else {
        ImGui::SetNextWindowSize(ImVec2(1000, 620), ImGuiCond_FirstUseEver);
    }
    if (!ImGui::Begin(trw("Packages (.tuf)###Packages"), &m_showTuf)) {
        ImGui::End();
        return;
    }

    const bool busy = s.running != TaskKind::None;
    const bool isOpen = s.ar && s.ar->isOpen();

    // ---- caminho e pacotes montados do projeto
    ImGui::SetNextItemWidth(std::max(160.0f, ImGui::GetContentRegionAvail().x - 330.0f));
    const bool enter = ImGui::InputTextWithHint("##tufpath", tr("package .tuf (absolute path)"), s.path, sizeof(s.path),
                                                ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    ImGui::BeginDisabled(busy || s.path[0] == '\0');
    if (ImGui::Button(tr("Open")) || (enter && !busy && s.path[0])) tufOpen(s, s.path, false);
    ImGui::SameLine();
    if (ImGui::Button(tr("New package"))) {
        s.ar.reset();
        refreshView(s, s.path);
        std::error_code ec;
        if (fs::exists(s.path, ec)) {
            s.setStatus(tr("That file already exists. Open it, or type another name."), true);
        } else {
            s.newPackage = true;
            s.openedPath = s.path;
            s.setStatus(tr("New package: add files below to create it."), false);
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    const std::vector<std::string> mounted = mountedTufs();
    ImGui::BeginDisabled(busy || mounted.empty());
    if (ImGui::Button(tr("Project packages")) ) ImGui::OpenPopup("##tufmounted");
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("%s", tr("The .tuf files the open project mounts (packs/*.tuf)."));
    if (ImGui::BeginPopup("##tufmounted")) {
        for (const std::string& m : mounted)
            if (ImGui::Selectable(m.c_str())) {
                std::snprintf(s.path, sizeof(s.path), "%s", m.c_str());
                tufOpen(s, s.path, false);
            }
        ImGui::EndPopup();
    }

    if (!s.status.empty()) {
        if (s.statusError) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", s.status.c_str());
        else ImGui::TextDisabled("%s", s.status.c_str());
    }
    const bool mountedNow = (isOpen || s.newPackage) && isMounted(s.openedPath);
    if (mountedNow)
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "%s",
                           tr("This package is mounted by the running project: it is read-only here. Repack it into a "
                              "new file to edit a copy."));
    if (isOpen) {
        const tufo::Stats& st = s.stats;
        ImGui::TextDisabled(tr("%llu files, %s original -> %s on disk (generation %llu)"),
                            static_cast<unsigned long long>(st.entries), tufo::humanSize(st.rawBytes).c_str(),
                            tufo::humanSize(st.fileBytes).c_str(), static_cast<unsigned long long>(st.generation));
    }

    // ---- progresso
    if (busy) {
        const uint64_t total = s.progress.total.load();
        const float frac = total ? static_cast<float>(s.progress.done.load()) / static_cast<float>(total) : 0.0f;
        char label[96];
        std::snprintf(label, sizeof label, "%llu / %llu", static_cast<unsigned long long>(s.progress.done.load()),
                      static_cast<unsigned long long>(total));
        ImGui::ProgressBar(total ? frac : -1.0f * static_cast<float>(ImGui::GetTime()), ImVec2(ImGui::GetContentRegionAvail().x - 90, 0), total ? label : nullptr);
        ImGui::SameLine();
        if (ImGui::Button(tr("Cancel"))) s.progress.cancel = true;
    }

    // ---- ações (acima das colunas)
    if (isOpen || s.newPackage) {
        ImGui::BeginDisabled(busy || !isOpen);
        if (ImGui::Button(tr("Verify"))) {
            std::shared_ptr<tufo::Archive> ar = s.ar;
            tufo::Progress* prog = &s.progress;
            const unsigned threads = std::max(1u, std::thread::hardware_concurrency() / 2);
            startTask(s, TaskKind::Verify, [ar, prog, threads] {
                TaskResult r;
                r.kind = TaskKind::Verify;
                std::vector<std::string> rep;
                const size_t bad = ar->verifyAll(&rep, threads, prog);
                r.report = rep;
                r.ok = bad == 0 && !prog->cancel.load();
                r.count = static_cast<long>(bad ? bad : ar->fileCount());
                r.msg = prog->cancel.load() ? Msg::VerifyCancelled : bad ? Msg::VerifyFailed : Msg::Verified;
                return r;
            });
        }
        ImGui::SetItemTooltip("%s", tr("Reads every entry and checks its hashes. Runs in the background."));
        ImGui::SameLine();
        if (ImGui::Button(tr("Statistics"))) s.showStats = !s.showStats;
        ImGui::SameLine();
        ImGui::BeginDisabled(!isOpen);
        if (ImGui::Button(tr("Repack..."))) {
            if (!s.repackOut[0]) std::snprintf(s.repackOut, sizeof(s.repackOut), "%s", (s.openedPath + ".repack.tuf").c_str());
            ImGui::OpenPopup("##tufrepack");
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("%s", tr("Copies only what is alive into a new .tuf (the original stays as it is)."));
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(busy || !isOpen || s.selected.empty());
        if (ImGui::Button(tr("Extract..."))) {
            if (!s.extractDir[0]) std::snprintf(s.extractDir, sizeof(s.extractDir), "%s", (fs::temp_directory_path() / "tuf_extract").string().c_str());
            ImGui::OpenPopup("##tufextract");
        }
        ImGui::SetItemTooltip("%s", tr("Extracts the selected file or folder to a folder on disk."));
        ImGui::SameLine();
        ImGui::BeginDisabled(mountedNow);
        if (ImGui::Button(tr("Remove"))) {
            s.confirm = TaskKind::Remove;
            s.confirmTarget = s.selected;
            s.openConfirm = true;
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("%s", tr("Takes the selected file or folder out of the package. The space returns on Repack."));
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(busy || mountedNow);
        if (ImGui::Button(tr("Add..."))) {
            if (s.selectedDir && s.addPrefix[0] == '\0') std::snprintf(s.addPrefix, sizeof(s.addPrefix), "%s", s.selected.c_str());
            ImGui::OpenPopup("##tufadd");
        }
        ImGui::SetItemTooltip("%s", tr("Adds files or a folder from disk. Safe append: stopping midway never loses the previous state."));
        ImGui::EndDisabled();
    }

    // ---- popups de entrada
    if (ImGui::BeginPopup("##tufextract")) {
        ImGui::TextUnformatted(tr("Extract to folder:"));
        ImGui::SetNextItemWidth(420);
        ImGui::InputText("##exdir", s.extractDir, sizeof(s.extractDir));
        ImGui::TextDisabled("%s", s.selected.c_str());
        if (ImGui::Button(tr("Extract")) && s.extractDir[0] && s.ar) {
            std::shared_ptr<tufo::Archive> ar = s.ar;
            tufo::Progress* prog = &s.progress;
            const std::string target = s.selected, dir = s.extractDir;
            startTask(s, TaskKind::Extract, [ar, prog, target, dir] {
                TaskResult r;
                r.kind = TaskKind::Extract;
                std::string err;
                r.count = tufo::extractTo(*ar, target, dir, prog, &err);
                r.ok = r.count >= 0;
                r.msg = r.ok ? Msg::Extracted : Msg::Error;
                r.a = dir;
                r.error = err;
                return r;
            });
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("##tufrepack")) {
        ImGui::TextUnformatted(tr("New package (must not exist yet):"));
        ImGui::SetNextItemWidth(420);
        ImGui::InputText("##rpout", s.repackOut, sizeof(s.repackOut));
        const bool bad = !s.repackOut[0] || samePath(s.repackOut, s.openedPath) || isMounted(s.repackOut);
        ImGui::BeginDisabled(bad);
        if (ImGui::Button(tr("Repack")) && s.ar) {
            const std::string in = s.openedPath, out = s.repackOut;
            tufo::Progress* prog = &s.progress;
            startTask(s, TaskKind::Repack, [in, out, prog] {
                TaskResult r;
                r.kind = TaskKind::Repack;
                prog->total = 1;
                std::string err;
                r.ok = tufo::repack(in, out, &err);
                prog->done = 1;
                r.msg = r.ok ? Msg::Repacked : Msg::Error;
                r.error = err;
                if (r.ok) {
                    tufo::Archive a;
                    a.open(out, nullptr);
                    r.a = tufo::humanSize(a.stats().fileBytes);
                    r.b = out;
                }
                return r;
            });
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("##tufadd")) {
        ImGui::TextUnformatted(tr("File or folder to add (absolute path):"));
        ImGui::SetNextItemWidth(420);
        ImGui::InputText("##addpath", s.addPath, sizeof(s.addPath));
        ImGui::TextUnformatted(tr("Folder inside the package (empty = root):"));
        ImGui::SetNextItemWidth(420);
        ImGui::InputText("##addprefix", s.addPrefix, sizeof(s.addPrefix));
        ImGui::SetNextItemWidth(120);
        ImGui::SliderInt(tr("zstd level"), &s.level, 0, 22);
        ImGui::SetItemTooltip("%s", tr("0 stores everything raw; 19 is the CLI default; lower is faster."));
        ImGui::BeginDisabled(!s.addPath[0]);
        if (ImGui::Button(tr("Add"))) {
            s.confirm = TaskKind::Add;
            s.confirmTarget = s.addPath;
            s.openConfirm = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::EndPopup();
    }

    // Confirmação de quem escreve.
    if (s.openConfirm) {
        ImGui::OpenPopup("###TufConfirm");
        s.openConfirm = false;
    }
    if (ImGui::BeginPopupModal((std::string(tr("Modify the package?")) + "###TufConfirm").c_str(), nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(s.openedPath.c_str());
        if (s.confirm == TaskKind::Remove) ImGui::Text(tr("Remove \"%s\" from the package."), s.confirmTarget.c_str());
        else ImGui::Text(tr("Add \"%s\" to the package."), s.confirmTarget.c_str());
        ImGui::TextDisabled("%s", tr("The change is written to the file now. A backup is not made: repack a copy first if unsure."));
        if (ImGui::Button(tr("Confirm"))) {
            const std::string path = s.openedPath, target = s.confirmTarget;
            const bool create = s.newPackage;
            tufo::Progress* prog = &s.progress;
            if (s.confirm == TaskKind::Remove) {
                s.ar.reset(); // solta o mapeamento antes de escrever
                startTask(s, TaskKind::Remove, [path, target] {
                    TaskResult r;
                    r.kind = TaskKind::Remove;
                    std::string err;
                    r.count = tufo::removeTargets(path, {target}, &err);
                    r.ok = r.count >= 0;
                    r.msg = r.ok ? Msg::Removed : Msg::Error;
                    r.error = err;
                    return r;
                });
            } else {
                tufo::AddOptions o;
                o.write.level = s.level;
                o.threads = std::max(1u, std::thread::hardware_concurrency() / 2);
                o.create = create;
                o.prefix = s.addPrefix;
                std::error_code ec;
                // Pasta: o que ela contém entra sob o prefixo, com o nome dela.
                if (fs::is_directory(target, ec)) o.base = fs::path(target).parent_path().string();
                s.ar.reset();
                startTask(s, TaskKind::Add, [path, target, o, prog] {
                    TaskResult r;
                    r.kind = TaskKind::Add;
                    std::string err;
                    r.count = tufo::addPaths(path, {target}, o, prog, &err);
                    r.ok = r.count >= 0;
                    r.msg = r.ok ? Msg::Added : Msg::Error;
                    r.error = err;
                    return r;
                });
            }
            s.newPackage = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Cancel"))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (s.showStats && isOpen) {
        const tufo::Stats& st = s.stats;
        ImGui::BeginChild("##tufstats", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 7.0f), ImGuiChildFlags_Border);
        ImGui::Text(tr("File: %s (generation %llu)"), tufo::humanSize(st.fileBytes).c_str(), static_cast<unsigned long long>(st.generation));
        ImGui::Text(tr("Paths: %llu, blobs (alive + dead): %llu, composites: %llu, dictionaries: %llu"),
                    static_cast<unsigned long long>(st.entries), static_cast<unsigned long long>(st.blobs),
                    static_cast<unsigned long long>(st.composites), static_cast<unsigned long long>(st.dicts));
        ImGui::Text(tr("Original size of all paths: %s; unique after dedupe: %s (%.1f%%)"), tufo::humanSize(st.rawBytes).c_str(),
                    tufo::humanSize(st.uniqueRawBytes).c_str(), st.rawBytes ? 100.0 * double(st.uniqueRawBytes) / double(st.rawBytes) : 0.0);
        ImGui::Text(tr("Stored (after zstd): %s (%.2fx over the unique size)"), tufo::humanSize(st.storedBytes).c_str(),
                    st.storedBytes ? double(st.uniqueRawBytes) / double(st.storedBytes) : 0.0);
        ImGui::Text(tr("Raw blobs (did not compress): %llu; paths sharing a blob: %llu"),
                    static_cast<unsigned long long>(st.rawCodecBlobs), static_cast<unsigned long long>(st.dedupedEntries));
        ImGui::Text(tr("Wasted space (recovered by Repack): %s"), tufo::humanSize(st.deadBytes).c_str());
        ImGui::EndChild();
    }
    if (!s.report.empty()) {
        ImGui::BeginChild("##tufreport", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 4.2f), ImGuiChildFlags_Border);
        for (const std::string& line : s.report) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", line.c_str());
        ImGui::EndChild();
    }

    if (!isOpen) {
        ImGui::Separator();
        ImGui::TextDisabled("%s", tr("Open a .tuf by its path, or pick one of the project's packages."));
        ImGui::End();
        return;
    }

    // ---- árvore (esquerda) e prévia (direita)
    ImGui::Separator();
    ImGui::SetNextItemWidth(260);
    ImGui::InputTextWithHint("##tuffilter", tr("filter by name"), s.filter, sizeof(s.filter));
    ImGui::SameLine();
    ImGui::TextDisabled(tr("%zu of %zu"), s.filter[0] ? s.matches.size() : s.entries.size(), s.entries.size());
    if (s.rowsDirty || s.lastFilter != s.filter) rebuildRows(s);

    const float rightW = std::min(420.0f, ImGui::GetContentRegionAvail().x * 0.4f);
    ImGui::BeginChild("##tuftree", ImVec2(ImGui::GetContentRegionAvail().x - rightW - 8, 0), ImGuiChildFlags_Border);
    if (ImGui::BeginTable("##tuftable", 5,
                          ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("Name"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(tr("Original"), ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn(tr("Stored"), ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn(tr("Codec"), ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn(tr("Dedup"), ImGuiTableColumnFlags_WidthFixed, 50);
        ImGui::TableHeadersRow();
        const bool flat = s.filter[0] != '\0';
        const int count = flat ? static_cast<int>(s.matches.size()) : static_cast<int>(s.rows.size());
        ImGuiListClipper clip;
        clip.Begin(count);
        while (clip.Step()) {
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
                const tufo::TreeNode* n = nullptr;
                const tufo::EntryInfo* e = nullptr;
                int depth = 0;
                std::string label;
                if (flat) {
                    e = &s.entries[static_cast<size_t>(s.matches[static_cast<size_t>(i)])];
                    label = e->key;
                } else {
                    n = s.rows[static_cast<size_t>(i)].node;
                    depth = s.rows[static_cast<size_t>(i)].depth;
                    if (n->entry >= 0) e = &s.entries[static_cast<size_t>(n->entry)];
                    label = n->dir ? std::string(s.expanded.count(n->path) ? "v " : "> ") + n->name + "/" : n->name;
                }
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::PushID(i);
                if (depth) ImGui::Indent(14.0f * static_cast<float>(depth));
                const std::string path = flat ? e->key : n->path;
                const bool isDir = !flat && n->dir;
                const bool sel = s.selected == path && s.selectedDir == isDir;
                if (ImGui::Selectable(label.c_str(), sel, ImGuiSelectableFlags_SpanAllColumns)) {
                    s.selected = path;
                    s.selectedDir = isDir;
                    s.selectedEntry = isDir ? -1 : (flat ? s.matches[static_cast<size_t>(i)] : n->entry);
                    if (isDir) {
                        if (!s.expanded.erase(path)) s.expanded.insert(path);
                        s.rowsDirty = true;
                    }
                }
                if (depth) ImGui::Unindent(14.0f * static_cast<float>(depth));
                ImGui::PopID();
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(tufo::humanSize(isDir ? n->raw : e->info.rawSize).c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(tufo::humanSize(isDir ? n->stored : e->info.storedSize).c_str());
                ImGui::TableSetColumnIndex(3);
                if (e) ImGui::TextUnformatted(tufo::codecLabel(e->codecMask).c_str());
                else ImGui::TextDisabled(tr("%u files"), n->files);
                ImGui::TableSetColumnIndex(4);
                if (e && e->deduped) ImGui::TextColored(kAccent, "%s", tr("shared"));
            }
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##tufpreview", ImVec2(0, 0), ImGuiChildFlags_Border);
    if (s.selected.empty()) {
        ImGui::TextDisabled("%s", tr("Select a file or folder."));
    } else if (s.selectedDir) {
        ImGui::TextColored(kAccent, "%s/", s.selected.c_str());
        const tufo::TreeNode* found = nullptr;
        std::function<void(const tufo::TreeNode&)> find = [&](const tufo::TreeNode& n) {
            for (const tufo::TreeNode& c : n.children) {
                if (c.dir && c.path == s.selected) found = &c;
                else if (c.dir && !found && s.selected.compare(0, c.path.size() + 1, c.path + "/") == 0) find(c);
            }
        };
        find(s.tree);
        if (found) ImGui::TextDisabled(tr("%u files, %s original, %s stored"), found->files, tufo::humanSize(found->raw).c_str(),
                                       tufo::humanSize(found->stored).c_str());
    } else if (s.selectedEntry >= 0) {
        const tufo::EntryInfo& e = s.entries[static_cast<size_t>(s.selectedEntry)];
        ImGui::TextColored(kAccent, "%s", fs::path(e.key).filename().string().c_str());
        ImGui::TextDisabled("%s", e.key.c_str());
        ImGui::TextDisabled(tr("%s original, %s stored, %s%s"), tufo::humanSize(e.info.rawSize).c_str(),
                            tufo::humanSize(e.info.storedSize).c_str(), tufo::codecLabel(e.codecMask).c_str(),
                            e.deduped ? tr(", shared blob (dedup)") : "");
        if (e.info.segments > 1) ImGui::TextDisabled(tr("composite entry: %u segments"), e.info.segments);
        ImGui::Separator();
        const PreviewData& p = s.preview;
        if (p.key != s.selected) {
            ImGui::TextDisabled("%s", tr("Loading preview..."));
        } else {
            switch (p.kind) {
                case PreviewKind::Image:
                    ImGui::TextDisabled(tr("%d x %d"), p.fileW, p.fileH);
                    if (!p.text.empty()) // folha de contato das texturas prontas para a GPU
                        ImGui::TextDisabled(tr("%s textures decoded from the GPU block format (lossy)"), p.text.c_str());
                    if (s.previewTex.valid()) {
                        const float side = std::min(ImGui::GetContentRegionAvail().x, ImGui::GetContentRegionAvail().y);
                        const float k = side / static_cast<float>(std::max(s.previewTex.width, s.previewTex.height));
                        ImGui::Image(reinterpret_cast<ImTextureID>(s.previewTex.set),
                                     ImVec2(static_cast<float>(s.previewTex.width) * k, static_cast<float>(s.previewTex.height) * k));
                    }
                    break;
                case PreviewKind::Text:
                    ImGui::BeginChild("##tuftext", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
                    ImGui::TextUnformatted(p.text.c_str());
                    ImGui::EndChild();
                    break;
                case PreviewKind::Hex:
                    ImGui::TextDisabled("%s", tr("Binary: first bytes"));
                    ImGui::BeginChild("##tufhex", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
                    ImGui::TextUnformatted(p.text.c_str());
                    ImGui::EndChild();
                    break;
                case PreviewKind::TooBig:
                    ImGui::TextDisabled("%s", tr("Too large to preview. Extract it instead."));
                    break;
                case PreviewKind::Error:
                    ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", p.text.c_str());
                    break;
                case PreviewKind::None:
                    break;
            }
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

} // namespace eruption
