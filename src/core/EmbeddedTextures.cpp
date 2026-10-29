// Texturas embutidas nos arquivos do mapa (.glb), por arquivo, e a previa de
// uma delas no cubo com o material que o mundo usa. Serve ao painel de assets
// do editor e a' lista "Textures" da depuracao (F1). O cubo tem o lado de uma
// repeticao da textura no mundo e o relevo sai da MESMA rotina do assado do
// mundo (ModelRenderer::bakeReliefPreview), entao o que o cubo mostra e' o
// que a malha do mapa faz com essa textura.
#include "core/Engine.hpp"
#include "core/Logger.hpp"
#include "formats/MapLoader.hpp"
#include "utils/BcCompressor.hpp"
#include "utils/PbrTextureLoader.hpp"

#include <backends/imgui_impl_vulkan.h>
#include <imgui.h>
#include <vk_mem_alloc.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include <vulkan/vulkan.h>

#ifdef ERUPTION_HAVE_BC_ENCODER
// Headers de terceiro: struct anonima dispara -Wpedantic que nao e' nosso.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include "rgbcx.h"
#include "bc7decomp.h"
#pragma GCC diagnostic pop
#endif

namespace eruption {

namespace {

const char* untranslated(const char* s) { return s; }

struct EmbeddedItem {
    std::string key;   // nome no cache de texturas do mundo
    std::string image; // nome da imagem no arquivo
    std::string name;  // legivel
    uint32_t slot = 0;
    int role = 0;      // ModelRenderer::groundRoleForSlot
};

struct EmbeddedGroup {
    std::string file;
    int role = 0; // o maior papel de chao entre as texturas (1 vem antes)
    std::vector<EmbeddedItem> items;
};

int roleOrder(int role) { return role == 1 ? 0 : (role == 2 ? 1 : 2); }

// Nome do material quando ele diz algo (tem letras); senao "Imagem N" e o
// nome da imagem sem o '#', os '_' da frente e a extensao.
std::string readableName(const std::string& image, const std::string& material, size_t index, const char* imageWord) {
    int letters = 0;
    for (char c : material) letters += std::isalpha(static_cast<unsigned char>(c)) ? 1 : 0;
    if (letters >= 3 && material != image) return material;
    std::string s = image;
    size_t a = 0;
    while (a < s.size() && (s[a] == '#' || s[a] == '_')) ++a;
    s = s.substr(a);
    const size_t dot = s.find_last_of('.');
    if (dot != std::string::npos && dot > 0) s.resize(dot);
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%s %zu", imageWord, index);
    return s.empty() ? std::string(buf) : std::string(buf) + " (" + s + ")";
}

// Reduz para no maximo `maxSide` (media por caixa); RGBA.
std::vector<uint8_t> shrink(const std::vector<uint8_t>& src, int& w, int& h, int maxSide) {
    if (w <= maxSide && h <= maxSide) return src;
    const int ow = std::max(1, w * maxSide / std::max(w, h)), oh = std::max(1, h * maxSide / std::max(w, h));
    std::vector<uint8_t> out(size_t(ow) * size_t(oh) * 4);
    for (int y = 0; y < oh; ++y) {
        const int y0 = y * h / oh, y1 = std::max(y0 + 1, (y + 1) * h / oh);
        for (int x = 0; x < ow; ++x) {
            const int x0 = x * w / ow, x1 = std::max(x0 + 1, (x + 1) * w / ow);
            unsigned sum[4] = {0, 0, 0, 0};
            for (int yy = y0; yy < y1; ++yy)
                for (int xx = x0; xx < x1; ++xx)
                    for (int c = 0; c < 4; ++c) sum[c] += src[(size_t(yy) * size_t(w) + size_t(xx)) * 4 + size_t(c)];
            const unsigned n = unsigned((y1 - y0) * (x1 - x0));
            for (int c = 0; c < 4; ++c) out[(size_t(y) * size_t(ow) + size_t(x)) * 4 + size_t(c)] = uint8_t(sum[c] / n);
        }
    }
    w = ow;
    h = oh;
    return out;
}

// Mip 0 de um .etex (RGBA8 cru, BC1, BC3 ou BC7) em RGBA8.
bool etexToRgba(const EtexData& e, std::vector<uint8_t>& rgba) {
    const size_t w = e.width, h = e.height;
    if (!e.valid() || w == 0 || h == 0) return false;
    rgba.assign(w * h * 4, 255);
    if (e.vkFormat == VK_FORMAT_R8G8B8A8_UNORM || e.vkFormat == VK_FORMAT_R8G8B8A8_SRGB) {
        if (e.payload.size() < rgba.size()) return false;
        std::copy(e.payload.begin(), e.payload.begin() + std::ptrdiff_t(rgba.size()), rgba.begin());
        return true;
    }
#ifdef ERUPTION_HAVE_BC_ENCODER
    const bool bc1 = e.vkFormat == VK_FORMAT_BC1_RGBA_UNORM_BLOCK || e.vkFormat == VK_FORMAT_BC1_RGB_UNORM_BLOCK ||
                     e.vkFormat == VK_FORMAT_BC1_RGBA_SRGB_BLOCK || e.vkFormat == VK_FORMAT_BC1_RGB_SRGB_BLOCK;
    const bool bc3 = e.vkFormat == VK_FORMAT_BC3_UNORM_BLOCK || e.vkFormat == VK_FORMAT_BC3_SRGB_BLOCK;
    const bool bc7 = e.vkFormat == VK_FORMAT_BC7_UNORM_BLOCK || e.vkFormat == VK_FORMAT_BC7_SRGB_BLOCK;
    if (!bc1 && !bc3 && !bc7) return false;
    const size_t bw = (w + 3) / 4, bh = (h + 3) / 4, bb = bc1 ? 8 : 16;
    if (e.payload.size() < bw * bh * bb) return false;
    bcCompressorInit();
    uint8_t px[64];
    for (size_t by = 0; by < bh; ++by)
        for (size_t bx = 0; bx < bw; ++bx) {
            const uint8_t* b = e.payload.data() + (by * bw + bx) * bb;
            if (bc1) rgbcx::unpack_bc1(b, px);
            else if (bc3) rgbcx::unpack_bc3(b, px);
            else bc7decomp::unpack_bc7(b, reinterpret_cast<bc7decomp::color_rgba*>(px));
            for (size_t y = 0; y < 4 && by * 4 + y < h; ++y)
                for (size_t x = 0; x < 4 && bx * 4 + x < w; ++x)
                    for (size_t c = 0; c < 4; ++c) rgba[((by * 4 + y) * w + bx * 4 + x) * 4 + c] = px[(y * 4 + x) * 4 + c];
        }
    return true;
#else
    return false;
#endif
}

std::vector<uint8_t> toRgba(const std::vector<uint8_t>& px, int w, int h, int channels) {
    const size_t n = size_t(w) * size_t(h);
    if (channels == 4) return px;
    std::vector<uint8_t> out(n * 4, 255);
    if (channels < 1 || px.size() < n * size_t(channels)) return {};
    for (size_t i = 0; i < n; ++i)
        for (int c = 0; c < 3; ++c) out[i * 4 + size_t(c)] = px[i * size_t(channels) + size_t(std::min(c, channels - 1))];
    return out;
}

bool pbrToRgba(const PbrTextureData& d, std::vector<uint8_t>& rgba, int& w, int& h) {
    w = d.width;
    h = d.height;
    if (d.etex.valid()) return etexToRgba(d.etex, rgba);
    rgba = toRgba(d.pixels, d.width, d.height, d.channels > 0 ? d.channels : 4);
    return !rgba.empty();
}

std::vector<uint8_t> channelGray(const std::vector<uint8_t>& rgba, int channel) {
    std::vector<uint8_t> out(rgba.size());
    for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
        out[i] = out[i + 1] = out[i + 2] = rgba[i + size_t(channel)];
        out[i + 3] = 255;
    }
    return out;
}

} // namespace

// Cor, MRAH-W e normal na CPU, do jeito que o mundo sobe: o bake comprimido
// do mapa quando existe, senao a mesma sintese da subida da textura.
bool Engine::embeddedMaterialImages(const std::string& image, std::vector<uint8_t> rgba[3], int w[3], int h[3]) {
    for (int i = 0; i < 3; ++i) {
        rgba[i].clear();
        w[i] = h[i] = 0;
    }
    std::vector<uint8_t> px;
    int pw = 0, ph = 0, ch = 0;
    if (!findEmbeddedAlbedoPixels(image, px, pw, ph, ch)) return false;
    rgba[0] = toRgba(px, pw, ph, ch);
    w[0] = pw;
    h[0] = ph;
    auto it = m_embeddedBaked.find(image);
    if (it != m_embeddedBaked.end() && it->second.mrahw.valid() && it->second.normal.valid()) {
        w[1] = int(it->second.mrahw.width);
        h[1] = int(it->second.mrahw.height);
        w[2] = int(it->second.normal.width);
        h[2] = int(it->second.normal.height);
        etexToRgba(it->second.mrahw, rgba[1]);
        etexToRgba(it->second.normal, rgba[2]);
    } else {
        PbrTextureData mrahw, normal;
        if (synthesizePbrFromPixels(px, pw, ph, ch, mrahw, normal)) {
            pbrToRgba(mrahw, rgba[1], w[1], h[1]);
            pbrToRgba(normal, rgba[2], w[2], h[2]);
        }
    }
    return !rgba[0].empty();
}

void Engine::releaseEmbeddedTiles() {
    EmbeddedPreview& st = m_embPreview;
    bool any = false;
    for (int i = 0; i < 6; ++i) any = any || st.tiles[i].image != VK_NULL_HANDLE;
    if (!any) return;
    // Clique raro: espera os quadros em voo largarem as imagens.
    vkDeviceWaitIdle(m_vulkan.device());
    for (int i = 0; i < 6; ++i) {
        if (st.sets[i]) ImGui_ImplVulkan_RemoveTexture(st.sets[i]);
        if (st.tiles[i].view) vkDestroyImageView(m_vulkan.device(), st.tiles[i].view, nullptr);
        if (st.tiles[i].image) vmaDestroyImage(m_vulkan.allocator(), st.tiles[i].image, st.tiles[i].alloc);
        st.sets[i] = VK_NULL_HANDLE;
        st.tiles[i] = TextureResource{};
    }
}

void Engine::closeEmbeddedPreview() {
    releaseEmbeddedTiles();
    if (m_embPreview.slot != 0) m_spherePreview.setActive(false);
    m_embPreview.map = nullptr;
    m_embPreview.key.clear();
    m_embPreview.image.clear();
    m_embPreview.label.clear();
    m_embPreview.slot = 0;
}

void Engine::openEmbeddedPreview(const std::string& key, const std::string& image, const std::string& label,
                                 uint32_t slot, int groundRole, const std::string& file) {
    releaseEmbeddedTiles();
    EmbeddedPreview& st = m_embPreview;
    st.map = m_currentMap.get();
    st.key = key;
    st.image = image;
    st.label = label;
    st.slot = slot;
    st.groundRole = groundRole;
    st.worldAmplitude = m_modelRenderer.reliefAmplitudeForSlot(slot);
    st.amplitude = st.worldAmplitude;
    st.baked = st.worldAmplitude > 0.0f;
    // ERUPTION_TEST_CUBE_AMP=<u> (teste/prints): abre o cubo com essa amplitude.
    if (const char* e = std::getenv("ERUPTION_TEST_CUBE_AMP")) st.amplitude = std::max(0.0f, float(std::atof(e)));
    // Grade: a do chao jogavel deste arquivo com esta textura (a malha com mais
    // triangulos de origem), e com ela o mesmo filtro de Nyquist, que segue a grade.
    const ModelRenderer::ReliefGridInfo grid = m_modelRenderer.reliefGridForSlot(slot, file);
    st.spacing = grid.dominant > 0.0f ? grid.dominant : m_modelRenderer.reliefBake().spacing;
    st.gridMin = grid.minSpacing;
    st.gridMax = grid.maxSpacing;
    st.gridMeshes = grid.meshes;
    const float uvpu = m_modelRenderer.uvPerUnitForSlot(slot);
    st.edge = uvpu > 0.0f ? 1.0f / uvpu : 16.0f;

    // Canais: cor, normal e os quatro do MRAH-W (R metal, G aspereza,
    // B oclusao, A altura), reduzidos para a tela.
    std::vector<uint8_t> img[3];
    int w[3], h[3];
    embeddedMaterialImages(image, img, w, h);
    auto upload = [&](int tile, const std::vector<uint8_t>& rgba, int tw, int th) {
        if (rgba.empty() || tw <= 0 || th <= 0) return;
        int sw = tw, sh = th;
        const std::vector<uint8_t> small = shrink(rgba, sw, sh, 256);
        if (!createTextureFromPixels(small.data(), sw, sh, st.tiles[tile])) return;
        VkSampler sampler = m_uiDefaultSampler != VK_NULL_HANDLE ? m_uiDefaultSampler : m_defaultSampler;
        st.sets[tile] = ImGui_ImplVulkan_AddTexture(sampler, st.tiles[tile].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    };
    upload(0, img[0], w[0], h[0]);
    upload(1, img[2], w[2], h[2]);
    if (!img[1].empty())
        for (int c = 0; c < 4; ++c) upload(2 + c, channelGray(img[1], c), w[1], h[1]);

    const PbrTextureSlots pbr = m_modelRenderer.pbrForTexSlot(slot);
    m_spherePreview.setMaterial(slot, pbr.mrahw.index, 0.0f, pbr.normal.index);
    m_spherePreview.setMaterialLabel(key);
    m_spherePreview.setSpin(false);
    m_spherePreview.setOrbitDegrees(45.0f, 30.0f);
    m_spherePreview.setOrbitDistance(11.0f);
    m_spherePreview.setActive(true);
    rebuildEmbeddedCube();
}

// Cubo de uma repeticao da textura por face, no tamanho do mundo, assado com
// a amplitude escolhida (ou liso).
void Engine::rebuildEmbeddedCube() {
    EmbeddedPreview& st = m_embPreview;
    const PbrTextureSlots pbr = m_modelRenderer.pbrForTexSlot(st.slot);
    std::vector<TerrainVertex> v;
    std::vector<uint32_t> idx;
    auto build = [&](int grid) {
        SpherePreview::cubeMesh(st.edge, grid, v, idx);
        for (TerrainVertex& t : v) {
            t.texIndex = static_cast<uint16_t>(st.slot);
            t.pbrIndex = static_cast<uint16_t>(pbr.mrahw.index);
            t.normalIndex = static_cast<uint16_t>(pbr.normal.index);
        }
    };
    build(4);
    const auto t0 = std::chrono::steady_clock::now();
    bool baked = false;
    if (st.baked && st.amplitude > 0.0f)
        baked = m_modelRenderer.bakeReliefPreview(v, idx, st.slot, st.key, st.amplitude, st.spacing, &st.cubeSpacing);
    st.bakeFailed = st.baked && st.amplitude > 0.0f && !baked;
    if (!baked) build(10);
    st.triangles = idx.size() / 3;
    if (!baked) st.cubeSpacing = 0.0f;
    // Deslocamento de verdade nos vertices: quanto cada um saiu da face dele
    // (o eixo de maior coordenada), para conferir o controle de amplitude.
    float dMin = 0.0f, dMax = 0.0f;
    double dSum = 0.0, dSq = 0.0;
    for (const TerrainVertex& t : v) {
        const Vec3 a = glm::abs(t.position);
        const float d = std::max(a.x, std::max(a.y, a.z)) - 0.5f * st.edge;
        dMin = std::min(dMin, d);
        dMax = std::max(dMax, d);
        dSum += d;
        dSq += double(d) * d;
    }
    const double n = double(std::max<size_t>(v.size(), 1));
    const double dStd = std::sqrt(std::max(dSq / n - (dSum / n) * (dSum / n), 0.0));
    ERUPTION_LOG_WARN("[RELEVO_CUBO] %s: amplitude %.2f u, grade do mundo %.2f u (%u malhas, %.2f a %.2f u), "
                      "grade do cubo %.2f u, %zu tris; deslocamento %.2f a %.2f u (desvio %.2f u), %.0f ms", st.key.c_str(),
                      double(st.amplitude), double(st.spacing), st.gridMeshes, double(st.gridMin), double(st.gridMax),
                      double(st.cubeSpacing), st.triangles, double(dMin), double(dMax), dStd,
                      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    m_spherePreview.setCustomMesh(std::move(v), std::move(idx), st.edge);
}

void Engine::drawEmbeddedTextures(UiText tr, const std::string& file, bool inlineCube) {
    if (!tr) tr = untranslated;
    EmbeddedPreview& st = m_embPreview;
    if (st.slot != 0 && st.map != m_currentMap.get()) closeEmbeddedPreview(); // o mapa trocou: slots velhos
    if (!m_currentMap) {
        if (file.empty()) ImGui::TextDisabled("%s", tr("No map loaded."));
        return;
    }

    // Grupos: um por arquivo, com as texturas que o mundo subiu.
    const auto& cache = m_modelRenderer.getTextureCache();
    auto slotOf = [&](const std::string& image, std::string& key) -> uint32_t {
        auto it = cache.find(image);
        if (it != cache.end() && it->second != 0) {
            key = it->first;
            return it->second;
        }
        const std::string tail = "|" + (image.size() > 1 && image[0] == '#' ? image.substr(1) : image);
        for (const auto& [k, s] : cache)
            if (s != 0 && k.size() > tail.size() && k.compare(k.size() - tail.size(), tail.size(), tail) == 0) {
                key = k;
                return s;
            }
        return 0;
    };
    const std::string wanted = file.empty() ? std::string() : std::filesystem::path(file).filename().string();
    std::vector<EmbeddedGroup> groups;
    for (const ModelFile& model : m_currentMap->models) {
        if (model.embeddedTextures.empty()) continue;
        const std::string fname = std::filesystem::path(model.filePath.empty() ? model.name : model.filePath).filename().string();
        if (!wanted.empty() && fname != wanted) continue;
        std::unordered_map<std::string, std::string> material;
        for (const auto& node : model.nodes)
            for (size_t i = 0; i < node.textureNames.size() && i < node.pbrTextureNames.size(); ++i)
                material.emplace(node.textureNames[i], node.pbrTextureNames[i]);
        EmbeddedGroup g;
        g.file = fname;
        g.role = 3;
        for (size_t i = 0; i < model.embeddedTextures.size(); ++i) {
            const EmbeddedTexture& et = model.embeddedTextures[i];
            EmbeddedItem it;
            it.image = et.name;
            it.slot = slotOf(et.name, it.key);
            if (it.slot == 0) continue; // o mundo nao usa (mascara, imagem solta)
            auto m = material.find(et.name);
            it.name = readableName(et.name, m != material.end() ? m->second : std::string(), i, tr("Image"));
            it.role = m_modelRenderer.groundRoleForSlot(it.slot);
            g.role = std::min(g.role, roleOrder(it.role));
            g.items.push_back(std::move(it));
        }
        if (g.items.empty()) continue;
        std::stable_sort(g.items.begin(), g.items.end(), [](const EmbeddedItem& a, const EmbeddedItem& b) {
            return roleOrder(a.role) < roleOrder(b.role);
        });
        groups.push_back(std::move(g));
    }
    std::stable_sort(groups.begin(), groups.end(),
                     [](const EmbeddedGroup& a, const EmbeddedGroup& b) { return a.role < b.role; });
    if (groups.empty()) {
        if (file.empty()) ImGui::TextDisabled("%s", tr("No embedded textures in the loaded map."));
        return;
    }
    if (!file.empty() && !ImGui::CollapsingHeader(tr("In the world (material on the cube)"), ImGuiTreeNodeFlags_DefaultOpen))
        return;

    ImGui::TextWrapped("%s", tr("Click a texture: it opens on the cube with the material the world uses."));
    // ERUPTION_TEST_MAP_TEXTURE=<n> (teste/prints): abre uma vez a n-esima
    // textura da lista (0 = a primeira, o chao do mapa quando o mapa tem).
    static int s_testPick = [] {
        const char* e = std::getenv("ERUPTION_TEST_MAP_TEXTURE");
        return e ? std::atoi(e) : -1;
    }();
    int counter = 0;
    bool openListed = false;
    // Lista com altura limitada: o material aberto fica logo abaixo, sem rolar.
    size_t rows = 0;
    for (const EmbeddedGroup& g : groups) rows += g.items.size() + 1;
    const float listH = std::min(float(rows) * ImGui::GetTextLineHeightWithSpacing() + 8.0f, 230.0f);
    ImGui::BeginChild("##embedded", ImVec2(0, listH), ImGuiChildFlags_Border);
    for (const EmbeddedGroup& g : groups) {
        ImGui::PushID(g.file.c_str());
        if (ImGui::TreeNodeEx(g.file.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
            for (const EmbeddedItem& it : g.items) {
                std::string label = it.name;
                if (it.role == 1) label = std::string(tr("Map ground")) + ": " + label;
                else if (it.role == 2) label = std::string(tr("Map ground layer")) + ": " + label;
                label += "##" + it.key;
                const bool sel = st.slot == it.slot && st.key == it.key;
                openListed = openListed || sel;
                if (ImGui::Selectable(label.c_str(), sel) || counter == s_testPick) {
                    if (counter == s_testPick) s_testPick = -1;
                    openEmbeddedPreview(it.key, it.image, it.name, it.slot, it.role, g.file);
                    openListed = true;
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", it.key.c_str());
                ++counter;
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    if (st.slot == 0 || !openListed) return;
    st.inlineCube = inlineCube;

    // Material aberto.
    ImGui::Separator();
    ImGui::TextUnformatted(st.label.c_str());
    static const char* const kTiles[6] = {"Albedo", "Normal", "Metal", "Roughness", "AO", "Height"};
    const float tile = std::max(48.0f, std::min(128.0f, (ImGui::GetContentRegionAvail().x - 16.0f) / 3.0f));
    for (int i = 0; i < 6; ++i) {
        ImGui::BeginGroup();
        if (st.sets[i]) ImGui::Image(reinterpret_cast<ImTextureID>(st.sets[i]), ImVec2(tile, tile));
        else ImGui::Dummy(ImVec2(tile, tile));
        ImGui::TextDisabled("%s", tr(kTiles[i]));
        ImGui::EndGroup();
        if (i % 3 != 2) ImGui::SameLine();
    }
    if (!st.sets[2]) ImGui::TextDisabled("%s", tr("No MRAH-W for this texture in the world."));

    if (st.worldAmplitude > 0.0f) {
        ImGui::Text(tr("World relief: %.1f u, grid %.2f u"), double(st.worldAmplitude), double(st.spacing));
        if (st.gridMeshes > 1)
            ImGui::TextDisabled(tr("%u meshes in this file, grid %.2f to %.2f u"), st.gridMeshes, double(st.gridMin),
                                double(st.gridMax));
        if (st.cubeSpacing > 0.0f)
            ImGui::TextDisabled(tr("Cube: grid %.2f u, %zu triangles"), double(st.cubeSpacing), st.triangles);
    } else {
        ImGui::TextDisabled("%s", tr("The world bakes no relief from this texture."));
    }
    bool rebuild = false;
    if (ImGui::RadioButton(tr("Baked relief"), st.baked) && !st.baked) {
        st.baked = true;
        rebuild = true;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton(tr("No relief"), !st.baked) && st.baked) {
        st.baked = false;
        rebuild = true;
    }
    // ERUPTION_TEST_CUBE_WORLD_LIGHT=1 (teste/prints): abre com a luz do mundo.
    static bool s_worldLight = std::getenv("ERUPTION_TEST_CUBE_WORLD_LIGHT") != nullptr;
    if (s_worldLight) {
        m_spherePreview.setWorldLight(true);
        s_worldLight = false;
    }
    bool worldLight = m_spherePreview.worldLight();
    if (ImGui::Checkbox(tr("World light"), &worldLight)) m_spherePreview.setWorldLight(worldLight);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("%s", tr("The sun and ambient of the open map, as on the ground. Off: fixed studio light."));
    ImGui::BeginDisabled(!st.baked);
    const float top = std::max(24.0f, st.worldAmplitude * 2.0f);
    ImGui::SetNextItemWidth(std::max(80.0f, ImGui::GetContentRegionAvail().x * 0.5f));
    const bool dragged = ImGui::SliderFloat(tr("Amplitude (u)"), &st.amplitude, 0.0f, top, "%.1f");
    // Assa tambem durante o arrasto, no maximo a cada 150 ms: so' ao soltar, o
    // cubo parecia nao responder ao controle. O cubo de 24 mil triangulos na
    // grade do mundo assa em ~15 ms (altura ja' filtrada fica em cache).
    static std::chrono::steady_clock::time_point s_lastDragBake;
    if (dragged && ImGui::IsItemActive()) {
        const auto now = std::chrono::steady_clock::now();
        if (now - s_lastDragBake >= std::chrono::milliseconds(150)) {
            s_lastDragBake = now;
            rebuild = true;
        }
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) rebuild = true;
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("World value"))) {
        st.amplitude = st.worldAmplitude;
        rebuild = true;
    }
    ImGui::EndDisabled();
    if (rebuild) rebuildEmbeddedCube();
    ImGui::TextDisabled(tr("Cube edge = one texture repeat in the world: %.1f u, %zu triangles"), double(st.edge),
                        st.triangles);
    if (st.bakeFailed) ImGui::TextDisabled("%s", tr("No height in this texture: the cube stays flat."));

    if (!inlineCube) return;
    if (void* id = m_spherePreview.imguiTextureId()) {
        const float side = std::min(ImGui::GetContentRegionAvail().x, 300.0f);
        ImGui::Image(reinterpret_cast<ImTextureID>(id), ImVec2(side, side));
        if (ImGui::IsItemHovered()) {
            const ImGuiIO& io = ImGui::GetIO();
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) m_spherePreview.orbitDrag(io.MouseDelta.x, io.MouseDelta.y);
            if (io.MouseWheel != 0.0f) m_spherePreview.zoom(io.MouseWheel);
            ImGui::SetTooltip("%s", tr("Drag to turn, wheel to zoom"));
        }
    }
}

} // namespace eruption
