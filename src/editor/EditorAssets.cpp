// Texturas da interface, miniaturas do painel Projeto e prévia de assets no
// Inspetor (imagem e canais, mapas PBR, material em esfera/cubo, modelos).
#include "editor/Editor.hpp"
#include "editor/EditorTheme.hpp"

#include "core/Engine.hpp"

#include <imgui.h>
#include <imgui_impl_vulkan.h>
#include <nlohmann/json.hpp>
#include <stb_image.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>

namespace eruption {

namespace {

namespace fs = std::filesystem;

constexpr int kThumbSize = 96;
constexpr int kPreviewMax = 1024;
constexpr size_t kMaxThumbs = 400;

bool isImageFile(const fs::path& p) {
    std::string e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return std::tolower(c); });
    return e == ".png" || e == ".jpg" || e == ".jpeg" || e == ".tga" || e == ".bmp";
}

bool isModelFile(const fs::path& p) {
    std::string e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return std::tolower(c); });
    return e == ".glb" || e == ".gltf";
}

bool isTextFile(const fs::path& p) {
    static const char* kExt[] = {".json", ".css", ".txt", ".md", ".env", ".ini", ".glsl", ".comp", ".frag", ".vert", ".luaurc"};
    const std::string e = p.extension().string();
    for (const char* k : kExt)
        if (e == k) return true;
    return false;
}

// Redução por média de caixa (mantém a proporção, lado maior = maxSide).
std::vector<uint8_t> downscale(const uint8_t* src, int w, int h, int maxSide, int& outW, int& outH) {
    const float s = std::min(1.0f, static_cast<float>(maxSide) / static_cast<float>(std::max(w, h)));
    outW = std::max(1, static_cast<int>(std::lround(w * s)));
    outH = std::max(1, static_cast<int>(std::lround(h * s)));
    std::vector<uint8_t> out(static_cast<size_t>(outW) * outH * 4);
    if (outW == w && outH == h) {
        std::memcpy(out.data(), src, out.size());
        return out;
    }
    for (int y = 0; y < outH; ++y) {
        const int y0 = y * h / outH, y1 = std::max(y0 + 1, (y + 1) * h / outH);
        for (int x = 0; x < outW; ++x) {
            const int x0 = x * w / outW, x1 = std::max(x0 + 1, (x + 1) * w / outW);
            unsigned sum[4] = {0, 0, 0, 0};
            for (int yy = y0; yy < y1; ++yy)
                for (int xx = x0; xx < x1; ++xx)
                    for (int c = 0; c < 4; ++c) sum[c] += src[(static_cast<size_t>(yy) * w + xx) * 4 + c];
            const unsigned n = static_cast<unsigned>((y1 - y0) * (x1 - x0));
            for (int c = 0; c < 4; ++c) out[(static_cast<size_t>(y) * outW + x) * 4 + c] = static_cast<uint8_t>(sum[c] / n);
        }
    }
    return out;
}

// Cópia sem transparência (mapas cujo alfa guarda dado, não recorte).
std::vector<uint8_t> opaque(std::vector<uint8_t> rgba) {
    for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
    return rgba;
}

// Um canal em tons de cinza (opaco).
std::vector<uint8_t> channelImage(const std::vector<uint8_t>& rgba, int channel) {
    std::vector<uint8_t> out(rgba.size());
    for (size_t i = 0; i < rgba.size(); i += 4) {
        const uint8_t v = rgba[i + static_cast<size_t>(channel)];
        out[i] = out[i + 1] = out[i + 2] = v;
        out[i + 3] = 255;
    }
    return out;
}

std::vector<uint8_t> loadImage(const fs::path& p, int& w, int& h, int& channels) {
    unsigned char* px = stbi_load(p.string().c_str(), &w, &h, &channels, 4);
    if (!px) return {};
    std::vector<uint8_t> out(px, px + static_cast<size_t>(w) * h * 4);
    stbi_image_free(px);
    return out;
}

std::string humanSize(uintmax_t bytes) {
    char buf[32];
    if (bytes >= (1u << 20)) std::snprintf(buf, sizeof(buf), "%.1f MB", bytes / 1048576.0);
    else if (bytes >= 1024) std::snprintf(buf, sizeof(buf), "%.1f KB", bytes / 1024.0);
    else std::snprintf(buf, sizeof(buf), "%ju B", bytes);
    return buf;
}

// Mapas PBR do motor ao lado do albedo: <nome>_mrahw.png e <nome>_normal.png,
// na mesma pasta ou em assets/data/texture/.
void findPbrSiblings(const fs::path& file, fs::path& albedo, fs::path& mrahw, fs::path& normal) {
    std::string stem = file.stem().string();
    for (const char* suffix : {"_mrahw", "_normal"}) {
        const size_t n = std::strlen(suffix);
        if (stem.size() > n && stem.compare(stem.size() - n, n, suffix) == 0) stem.resize(stem.size() - n);
    }
    const fs::path dirs[] = {file.parent_path(), fs::path("assets/data/texture")};
    albedo.clear();
    mrahw.clear();
    normal.clear();
    std::error_code ec;
    for (const auto& d : dirs) {
        for (const char* ext : {".png", ".jpg", ".tga"})
            if (albedo.empty() && fs::exists(d / (stem + ext), ec)) albedo = d / (stem + ext);
        if (mrahw.empty() && fs::exists(d / (stem + "_mrahw.png"), ec)) mrahw = d / (stem + "_mrahw.png");
        if (normal.empty() && fs::exists(d / (stem + "_normal.png"), ec)) normal = d / (stem + "_normal.png");
    }
}

} // namespace

// ---------------------------------------------------------------- texturas

Editor::GuiTexture Editor::makeTexture(const uint8_t* rgba, int w, int h, bool nearest) {
    GuiTexture t;
    Engine::TextureResource r;
    if (!rgba || w <= 0 || h <= 0 || !m_engine->createTextureFromPixels(rgba, w, h, r)) return t;
    t.image = r.image;
    t.alloc = r.alloc;
    t.view = r.view;
    t.width = w;
    t.height = h;
    VkSampler sampler = nearest ? m_engine->m_nearestSampler : m_engine->m_defaultSampler;
    if (m_engine->m_uiDefaultSampler != VK_NULL_HANDLE)
        sampler = nearest ? m_engine->m_uiNearestSampler : m_engine->m_uiDefaultSampler;
    t.set = ImGui_ImplVulkan_AddTexture(sampler, t.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return t;
}

uint32_t Editor::bindTexture(GuiTexture& t) {
    // Registra no array de texturas da cena (para a esfera de material).
    if (!t.valid()) return 0;
    if (t.bindlessSlot != 0xFFFFFFFFu) return t.bindlessSlot;
    BindlessDescriptor& b = m_engine->bindless();
    t.bindlessSlot = b.allocateSlotSafe();
    b.updateTextureSafe(t.bindlessSlot, t.view, m_engine->m_defaultSampler);
    b.flushUpdatesSafe();
    return t.bindlessSlot;
}

void Editor::releaseTexture(GuiTexture& t) {
    if (t.valid() || t.image != VK_NULL_HANDLE) m_deadTextures.push_back({t, m_frame});
    t = GuiTexture{};
}

void Editor::collectTextures(bool all) {
    // Espera o número de frames em voo (mais folga) antes de destruir.
    constexpr uint64_t kDelay = 4;
    for (auto it = m_deadTextures.begin(); it != m_deadTextures.end();) {
        if (!all && m_frame - it->frame < kDelay) {
            ++it;
            continue;
        }
        GuiTexture& t = it->tex;
        if (t.set) ImGui_ImplVulkan_RemoveTexture(t.set);
        if (t.bindlessSlot != 0xFFFFFFFFu) m_engine->bindless().freeSlotSafe(t.bindlessSlot);
        if (t.view) vkDestroyImageView(m_engine->vulkan().device(), t.view, nullptr);
        if (t.image) vmaDestroyImage(m_engine->vulkan().allocator(), t.image, t.alloc);
        it = m_deadTextures.erase(it);
    }
}

// ---------------------------------------------------------------- miniaturas

void Editor::thumbnailWorker() {
    for (;;) {
        std::string path;
        {
            std::unique_lock<std::mutex> lock(m_thumbMutex);
            m_thumbCv.wait(lock, [this] { return m_thumbQuit || !m_thumbQueue.empty(); });
            if (m_thumbQuit) return;
            path = std::move(m_thumbQueue.front());
            m_thumbQueue.pop_front();
        }
        ThumbResult r;
        r.path = path;
        int w = 0, h = 0, ch = 0;
        std::vector<uint8_t> full = loadImage(path, w, h, ch);
        if (!full.empty()) r.rgba = downscale(full.data(), w, h, kThumbSize, r.w, r.h);
        std::lock_guard<std::mutex> lock(m_thumbMutex);
        m_thumbDone.push_back(std::move(r));
    }
}

void Editor::stopThumbnails() {
    {
        std::lock_guard<std::mutex> lock(m_thumbMutex);
        m_thumbQuit = true;
    }
    m_thumbCv.notify_all();
    if (m_thumbThread.joinable()) m_thumbThread.join();
}

const Editor::GuiTexture* Editor::thumbnail(const std::string& path) {
    auto it = m_thumbs.find(path);
    if (it == m_thumbs.end()) {
        if (!m_thumbThread.joinable()) m_thumbThread = std::thread(&Editor::thumbnailWorker, this);
        it = m_thumbs.emplace(path, Thumb{}).first;
        {
            std::lock_guard<std::mutex> lock(m_thumbMutex);
            m_thumbQueue.push_back(path);
        }
        m_thumbCv.notify_one();
    }
    it->second.lastUsed = m_frame;
    return it->second.state == 1 ? &it->second.tex : nullptr;
}

void Editor::uploadThumbnails() {
    // Poucas por frame: cada envio espera a GPU (immediateSubmit).
    for (int n = 0; n < 4; ++n) {
        ThumbResult r;
        {
            std::lock_guard<std::mutex> lock(m_thumbMutex);
            if (m_thumbDone.empty()) break;
            r = std::move(m_thumbDone.front());
            m_thumbDone.pop_front();
        }
        auto it = m_thumbs.find(r.path);
        if (it == m_thumbs.end()) continue;
        if (r.rgba.empty()) {
            it->second.state = 2;
            continue;
        }
        it->second.tex = makeTexture(r.rgba.data(), r.w, r.h, false);
        it->second.state = it->second.tex.valid() ? 1 : 2;
    }
    // Esquece as menos usadas quando passa do limite.
    if (m_thumbs.size() > kMaxThumbs) {
        std::vector<std::pair<uint64_t, std::string>> order;
        for (auto& [p, t] : m_thumbs)
            if (t.state != 0) order.push_back({t.lastUsed, p});
        std::sort(order.begin(), order.end());
        for (size_t i = 0; i < order.size() && m_thumbs.size() > kMaxThumbs * 3 / 4; ++i) {
            releaseTexture(m_thumbs[order[i].second].tex);
            m_thumbs.erase(order[i].second);
        }
    }
}

// ---------------------------------------------------------------- prévia

void Editor::closeAssetPreview() {
    AssetPreviewState& a = m_asset;
    for (GuiTexture* t : {&a.tex, &a.albedo, &a.mrahw, &a.normal, &a.embedded, &a.pbrChannels[0], &a.pbrChannels[1],
                          &a.pbrChannels[2], &a.pbrChannels[3]})
        releaseTexture(*t);
    if (a.materialReady) m_engine->m_spherePreview.setActive(false);
    a = AssetPreviewState{};
}

void Editor::selectAsset(const fs::path& path) {
    closeAssetPreview();
    m_selection = {SelectionKind::Asset, 0};
    AssetPreviewState& a = m_asset;
    a.path = path;
    std::error_code ec;
    if (isImageFile(path)) {
        a.kind = AssetPreviewState::Kind::Image;
        std::vector<uint8_t> full = loadImage(path, a.width, a.height, a.fileChannels);
        if (!full.empty()) {
            int w = 0, h = 0;
            a.rgba = downscale(full.data(), a.width, a.height, kPreviewMax, w, h);
            a.tex = makeTexture(a.rgba.data(), w, h, a.pixelated);
        }
        findPbrSiblings(path, a.albedoPath, a.mrahwPath, a.normalPath);
    } else if (isModelFile(path)) {
        a.kind = AssetPreviewState::Kind::Model;
        drawModelPreview(); // lê o cabeçalho na primeira chamada
    } else if (isTextFile(path)) {
        a.kind = AssetPreviewState::Kind::Text;
        std::ifstream f(path, std::ios::binary);
        std::string line;
        int n = 0;
        while (n < 400 && std::getline(f, line)) {
            a.text += line + "\n";
            ++n;
        }
    }
}

void Editor::drawImagePreview() {
    AssetPreviewState& a = m_asset;
    if (!a.tex.valid()) {
        ImGui::TextDisabled("Could not read this image.");
        return;
    }
    ImGui::Text("%d x %d, %d channel(s) in the file", a.width, a.height, a.fileChannels);
    const char* views[] = {"Color", "R", "G", "B", "A"};
    for (int i = 0; i < 5; ++i) {
        if (i) ImGui::SameLine(0, 2);
        const bool on = a.view == i;
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(kAccent.x * 0.7f, kAccent.y * 0.7f, kAccent.z * 0.7f, 1.0f));
        if (ImGui::SmallButton(views[i]) && !on) {
            a.view = i;
            const int w = a.tex.width, h = a.tex.height;
            releaseTexture(a.tex);
            if (i == 0) a.tex = makeTexture(a.rgba.data(), w, h, a.pixelated);
            else {
                const std::vector<uint8_t> ch = channelImage(a.rgba, i - 1);
                a.tex = makeTexture(ch.data(), w, h, a.pixelated);
            }
        }
        if (on) ImGui::PopStyleColor();
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("Pixels", &a.pixelated)) {
        const int w = a.tex.width, h = a.tex.height;
        releaseTexture(a.tex);
        const std::vector<uint8_t> ch = a.view == 0 ? a.rgba : channelImage(a.rgba, a.view - 1);
        a.tex = makeTexture(ch.data(), w, h, a.pixelated);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("Show pixels sharp (no filtering)");

    const float avail = ImGui::GetContentRegionAvail().x;
    const float scale = std::min(avail / a.tex.width, 320.0f / a.tex.height);
    const ImVec2 size(a.tex.width * scale, a.tex.height * scale);
    // Xadrez atrás, para enxergar a transparência.
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (float y = 0; y < size.y; y += 8)
        for (float x = 0; x < size.x; x += 8)
            dl->AddRectFilled(ImVec2(p.x + x, p.y + y), ImVec2(p.x + std::min(size.x, x + 8), p.y + std::min(size.y, y + 8)),
                              (static_cast<int>(x / 8 + y / 8) & 1) ? IM_COL32(70, 70, 70, 255) : IM_COL32(45, 45, 45, 255));
    ImGui::Image(reinterpret_cast<ImTextureID>(a.tex.set), size);
}

void Editor::drawMaterialPreview() {
    AssetPreviewState& a = m_asset;
    // Só imagens com mapas do motor ao lado (_mrahw / _normal) viram material.
    if (a.mrahwPath.empty() && a.normalPath.empty()) return;
    if (!ImGui::CollapsingHeader("Material (PBR)", ImGuiTreeNodeFlags_DefaultOpen)) return;

    if (!a.materialReady) {
        // Carrega uma vez: albedo, MRAH-W e normal, registrados para a esfera.
        auto load = [&](const fs::path& p, GuiTexture& out, std::vector<uint8_t>* keep) {
            if (p.empty()) return;
            int w = 0, h = 0, ch = 0;
            std::vector<uint8_t> full = loadImage(p, w, h, ch);
            if (full.empty()) return;
            int sw = 0, sh = 0;
            std::vector<uint8_t> small = downscale(full.data(), w, h, kPreviewMax, sw, sh);
            if (keep) *keep = small;
            small = opaque(std::move(small));
            out = makeTexture(small.data(), sw, sh, false);
        };
        std::vector<uint8_t> mrahwPixels;
        load(a.albedoPath, a.albedo, nullptr);
        load(a.mrahwPath, a.mrahw, &mrahwPixels);
        load(a.normalPath, a.normal, nullptr);
        if (!mrahwPixels.empty())
            for (int c = 0; c < 4; ++c) {
                const std::vector<uint8_t> chImg = channelImage(mrahwPixels, c);
                a.pbrChannels[c] = makeTexture(chImg.data(), a.mrahw.width, a.mrahw.height, false);
            }
        SpherePreview& sp = m_engine->m_spherePreview;
        sp.setMaterial(a.albedo.valid() ? bindTexture(a.albedo) : 0, a.mrahw.valid() ? bindTexture(a.mrahw) : 0, 1.0f,
                       a.normal.valid() ? bindTexture(a.normal) : 0);
        sp.setMaterialLabel(a.path.filename().string());
        sp.setWorldScale(false);
        sp.setShape(a.cube ? SpherePreview::Shape::Cube : SpherePreview::Shape::Sphere);
        sp.setSpin(true);
        sp.setActive(true);
        a.materialReady = true;
    }

    // Mapas lado a lado.
    struct Tile { const char* label; const GuiTexture* tex; };
    const Tile tiles[] = {
        {"Albedo", &a.albedo}, {"Normal", &a.normal}, {"Metal", &a.pbrChannels[0]},
        {"Roughness", &a.pbrChannels[1]}, {"Height", &a.pbrChannels[2]}, {"Wetness", &a.pbrChannels[3]},
    };
    const float tile = std::max(48.0f, (ImGui::GetContentRegionAvail().x - 16.0f) / 3.0f);
    int col = 0;
    for (const auto& t : tiles) {
        ImGui::BeginGroup();
        if (t.tex->valid()) ImGui::Image(reinterpret_cast<ImTextureID>(t.tex->set), ImVec2(tile, tile));
        else {
            ImGui::Dummy(ImVec2(tile, tile));
            const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddRect(mn, mx, IM_COL32(80, 80, 85, 255));
            ImGui::GetWindowDrawList()->AddText(ImVec2(mn.x + 6, mn.y + tile * 0.4f), IM_COL32(120, 120, 125, 255), "none");
        }
        ImGui::TextDisabled("%s", t.label);
        ImGui::EndGroup();
        if (++col % 3 != 0) ImGui::SameLine();
    }
    if (a.mrahwPath.empty())
        ImGui::TextDisabled("No _mrahw map next to this texture: the engine estimates PBR from the color.");

    // Esfera/cubo com luz própria (renderizada pelo motor a cada frame).
    ImGui::Spacing();
    SpherePreview& sp = m_engine->m_spherePreview;
    if (ImGui::RadioButton("Sphere", !a.cube)) { a.cube = false; sp.setShape(SpherePreview::Shape::Sphere); }
    ImGui::SameLine();
    if (ImGui::RadioButton("Cube", a.cube)) { a.cube = true; sp.setShape(SpherePreview::Shape::Cube); }
    ImGui::SameLine();
    bool spin = sp.spin();
    if (ImGui::Checkbox("Spin", &spin)) sp.setSpin(spin);
    if (void* id = sp.imguiTextureId()) {
        const float side = std::min(ImGui::GetContentRegionAvail().x, 300.0f);
        ImGui::Image(reinterpret_cast<ImTextureID>(id), ImVec2(side, side));
        if (ImGui::IsItemHovered()) {
            const ImGuiIO& io = ImGui::GetIO();
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) sp.orbitDrag(io.MouseDelta.x, io.MouseDelta.y);
            if (io.MouseWheel != 0.0f) sp.zoom(io.MouseWheel);
            ImGui::SetTooltip("Drag to turn, wheel to zoom");
        }
    }
}

void Editor::drawModelPreview() {
    AssetPreviewState& a = m_asset;
    if (a.modelInfo.empty()) {
        // Só o JSON do glTF (e, no .glb, a posição do bloco binário): rápido
        // mesmo para arquivos de centenas de MB.
        std::ifstream f(a.path, std::ios::binary);
        std::string json;
        size_t binStart = 0;
        if (a.path.extension() == ".glb") {
            uint32_t header[5] = {0, 0, 0, 0, 0};
            f.read(reinterpret_cast<char*>(header), sizeof(header));
            if (header[0] == 0x46546C67u && header[4] == 0x4E4F534Au) { // "glTF", "JSON"
                json.resize(header[3]);
                f.read(json.data(), static_cast<std::streamsize>(json.size()));
                binStart = 20 + header[3] + 8; // cabeçalho do bloco BIN
            }
        } else {
            std::ostringstream ss;
            ss << f.rdbuf();
            json = ss.str();
        }
        try {
            const nlohmann::json j = nlohmann::json::parse(json);
            size_t tris = 0, verts = 0, prims = 0;
            const auto& accessors = j.value("accessors", nlohmann::json::array());
            for (const auto& m : j.value("meshes", nlohmann::json::array()))
                for (const auto& p : m.value("primitives", nlohmann::json::array())) {
                    ++prims;
                    const auto& attr = p.value("attributes", nlohmann::json::object());
                    size_t vc = 0;
                    if (attr.contains("POSITION")) vc = accessors.at(attr["POSITION"].get<size_t>()).value("count", size_t(0));
                    verts += vc;
                    const size_t ic = p.contains("indices") ? accessors.at(p["indices"].get<size_t>()).value("count", size_t(0)) : vc;
                    if (p.value("mode", 4) == 4) tris += ic / 3;
                }
            char buf[512];
            std::error_code ec;
            std::snprintf(buf, sizeof(buf),
                          "File: %s\nMeshes: %zu (%zu parts)\nTriangles: %zu\nVertices: %zu\nNodes: %zu\nMaterials: %zu\nTextures: %zu\nAnimations: %zu",
                          humanSize(fs::file_size(a.path, ec)).c_str(), j.value("meshes", nlohmann::json::array()).size(), prims,
                          tris, verts, j.value("nodes", nlohmann::json::array()).size(),
                          j.value("materials", nlohmann::json::array()).size(), j.value("images", nlohmann::json::array()).size(),
                          j.value("animations", nlohmann::json::array()).size());
            a.modelInfo = buf;
            const auto& views = j.value("bufferViews", nlohmann::json::array());
            for (const auto& img : j.value("images", nlohmann::json::array())) {
                AssetPreviewState::Image im;
                im.name = img.value("name", std::string());
                im.mime = img.value("mimeType", std::string());
                im.uri = img.value("uri", std::string());
                if (img.contains("bufferView")) {
                    const auto& bv = views.at(img["bufferView"].get<size_t>());
                    im.offset = binStart + bv.value("byteOffset", size_t(0));
                    im.size = bv.value("byteLength", size_t(0));
                }
                if (im.name.empty()) im.name = im.uri.empty() ? "image " + std::to_string(a.images.size()) : im.uri;
                a.images.push_back(std::move(im));
            }
        } catch (const std::exception& e) {
            a.modelInfo = std::string("Could not read the glTF header: ") + e.what();
        }
        // Instâncias deste arquivo no mapa aberto.
        const std::string rel = a.path.is_absolute() ? a.path.lexically_relative(m_projectRoot).generic_string()
                                                     : a.path.generic_string();
        for (const auto& inst : m_engine->modelRenderer().getInstances())
            if (!inst.assetPath.empty() && (inst.assetPath == rel || inst.assetPath == a.path.generic_string())) ++a.instances;
        return;
    }

    ImGui::TextUnformatted(a.modelInfo.c_str());
    if (a.instances > 0) {
        ImGui::Text("Used %d time(s) in this map.", a.instances);
        ImGui::SameLine();
        if (ImGui::SmallButton("Select in scene")) {
            const std::string rel = a.path.is_absolute() ? a.path.lexically_relative(m_projectRoot).generic_string()
                                                         : a.path.generic_string();
            const auto& insts = m_engine->modelRenderer().getInstances();
            for (size_t i = 0; i < insts.size(); ++i)
                if (insts[i].assetPath == rel || insts[i].assetPath == a.path.generic_string()) {
                    closeAssetPreview();
                    select({SelectionKind::Model, static_cast<int>(i)});
                    frameSelection();
                    return;
                }
        }
    }
    if (a.images.empty()) return;
    if (!ImGui::CollapsingHeader("Embedded images", ImGuiTreeNodeFlags_DefaultOpen)) return;
    ImGui::BeginChild("##imgs", ImVec2(0, std::min(160.0f, a.images.size() * ImGui::GetTextLineHeightWithSpacing() + 8)), ImGuiChildFlags_Border);
    for (int i = 0; i < static_cast<int>(a.images.size()); ++i) {
        const auto& im = a.images[static_cast<size_t>(i)];
        if (ImGui::Selectable(im.name.c_str(), a.imageShown == i)) {
            a.imageShown = i;
            releaseTexture(a.embedded);
            std::vector<char> bytes;
            if (im.size > 0) {
                std::ifstream f(a.path, std::ios::binary);
                f.seekg(static_cast<std::streamoff>(im.offset));
                bytes.resize(im.size);
                f.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            } else if (!im.uri.empty()) {
                std::ifstream f(a.path.parent_path() / im.uri, std::ios::binary);
                bytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
            }
            int w = 0, h = 0, ch = 0;
            unsigned char* px = bytes.empty() ? nullptr
                : stbi_load_from_memory(reinterpret_cast<const unsigned char*>(bytes.data()), static_cast<int>(bytes.size()), &w, &h, &ch, 4);
            if (px) {
                int sw = 0, sh = 0;
                const std::vector<uint8_t> small = downscale(px, w, h, 512, sw, sh);
                stbi_image_free(px);
                a.embedded = makeTexture(small.data(), sw, sh, false);
            }
        }
    }
    ImGui::EndChild();
    if (a.embedded.valid()) {
        const float side = std::min(ImGui::GetContentRegionAvail().x, 300.0f);
        const float s = side / std::max(a.embedded.width, a.embedded.height);
        ImGui::Image(reinterpret_cast<ImTextureID>(a.embedded.set), ImVec2(a.embedded.width * s, a.embedded.height * s));
    }
}

void Editor::drawAssetPreview() {
    AssetPreviewState& a = m_asset;
    std::error_code ec;
    ImGui::TextColored(kAccent, "%s", a.path.filename().string().c_str());
    const fs::path shown = a.path.is_absolute() ? a.path.lexically_relative(m_projectRoot) : a.path;
    ImGui::TextDisabled("%s  (%s)", shown.generic_string().c_str(), humanSize(fs::file_size(a.path, ec)).c_str());
    if (ImGui::SmallButton("Open in VS Code")) openInExternalEditor(a.path);
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy path")) ImGui::SetClipboardText(a.path.generic_string().c_str());
    ImGui::Separator();
    switch (a.kind) {
        case AssetPreviewState::Kind::Image:
            drawImagePreview();
            drawMaterialPreview();
            break;
        case AssetPreviewState::Kind::Model:
            drawModelPreview();
            break;
        case AssetPreviewState::Kind::Text:
            ImGui::BeginChild("##text", ImVec2(0, 0), ImGuiChildFlags_Border, ImGuiWindowFlags_HorizontalScrollbar);
            ImGui::TextUnformatted(a.text.c_str());
            ImGui::EndChild();
            break;
        case AssetPreviewState::Kind::None:
            ImGui::TextDisabled("No preview for this kind of file.");
            break;
    }
}

} // namespace eruption
