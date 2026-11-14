// Janela > Criador de Assets: carrega um .glb, mostra as texturas que ele usa
// e gera, do albedo de cada uma, o par PBR do motor (normal + MRAH-W). O autor
// ajusta o relevo da normal e o perfil orgânico, vê o resultado e grava
// <stem>_mrahw.png / <stem>_normal.png onde a busca de PBR do motor
// (resolvePbrTexturePaths) acha esses mapas quando o modelo for carregado.
//
// A geração é a síntese analítica do próprio motor (synthesizePbrAnalytic):
// com força 1 e o mesmo perfil orgânico, os bytes saem iguais aos que o motor
// sintetizaria sozinho para essa textura.
#include "editor/Editor.hpp"
#include "editor/EditorTheme.hpp"
#include "editor/Translate.hpp"

#include "core/Engine.hpp"
#include "core/JobSystem.hpp"
#include "core/Project.hpp"
#include "formats/GltfParser.hpp"
#include "utils/EmbeddedTextureBake.hpp"
#include "utils/ImageUtils.hpp"
#include "utils/PbrMaterialProfile.hpp"
#include "utils/PbrTextureLoader.hpp"
#include "utils/ReadWholeFile.hpp"

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <future>
#include <unordered_map>

namespace eruption {

namespace fs = std::filesystem;

namespace {

constexpr int kThumbSide = 40;   // miniatura na lista
constexpr int kTileSide = 256;   // prévias enviadas à GPU (albedo, normal, canais)
constexpr int kThumbsPerFrame = 4; // cada envio espera a GPU (immediateSubmit)

// Uma textura do modelo, só dados (sai da thread de carga).
struct CreatorTexture {
    std::string key;      // nome da imagem no motor (GltfParser): a chave da busca de PBR
    std::string material; // primeiro material que usa a imagem (a busca tenta ele antes)
    std::string stem;     // nome dos arquivos gravados
    int fileW = 0, fileH = 0;
    int w = 0, h = 0;     // albedo guardado (no máximo o teto do motor)
    std::vector<uint8_t> albedo; // RGBA8
    bool organic = false; // perfil do material: grama, vegetação, terra, neve
    std::string existing; // _mrahw que a busca do motor acha hoje (vazio = nenhum)
};

struct CreatorModel {
    std::string path; // como foi pedido (vai para o visualizador de modelo)
    fs::path disk;
    std::vector<CreatorTexture> textures;
    std::string error;
};

struct GeneratedMaps {
    uint32_t generation = 0;
    bool ok = false;
    int w = 0, h = 0;
    std::vector<uint8_t> mrahw, normal;
    double ms = 0.0;
};

// Mesmo critério do bake de texturas embutidas (EmbeddedTextureBake.cpp).
bool organicCategory(const std::string& material) {
    if (material.empty()) return false;
    const std::string& cat = getPbrProfile(material).category;
    return cat == "grass" || cat == "vegetation" || cat == "dirt" || cat == "snow";
}

// Mapa que a busca do motor acha para esta textura, na ordem do ModelRenderer
// (nome do material primeiro, depois o nome da imagem).
std::string existingMapFor(const std::string& material, const std::string& key) {
    if (!material.empty()) {
        const PbrTexturePathPair p = resolvePbrTexturePaths(material);
        if (p.valid) return p.mrahw;
    }
    const PbrTexturePathPair p = resolvePbrTexturePaths(key);
    return p.valid ? p.mrahw : std::string();
}

// Uma imagem do GLB: albedo RGBA8 no teto de resolução, material, perfil
// orgânico, stem dos arquivos e o mapa que o motor já acha. Thread qualquer.
void decodeCreatorTexture(const EmbeddedTexture& et, const std::unordered_map<std::string, std::string>& materialOf,
                          int maxSide, CreatorTexture& t) {
    t.key = et.name;
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;
    if (et.isDecoded()) {
        w = et.width;
        h = et.height;
        const int ch = et.channels > 0 ? et.channels : 4;
        rgba.assign(static_cast<size_t>(w) * h * 4, 255);
        for (size_t p = 0; p < static_cast<size_t>(w) * h; ++p)
            for (int c = 0; c < std::min(ch, 4); ++c) rgba[p * 4 + c] = et.pixels[p * ch + c];
    } else if (et.hasEncodedData()) {
        ImageData img = ImageUtils::loadFromMemoryRaw(et.encodedData.data(), et.encodedData.size());
        if (!img.isValid()) return;
        w = img.width;
        h = img.height;
        rgba = std::move(img.pixels); // loadFromMemoryRaw sempre entrega RGBA
    }
    // Cor sólida (material sem textura vira 1x1): não há o que derivar.
    if (w < 4 || h < 4) return;
    t.fileW = w;
    t.fileH = h;
    ImageUtils::downscaleRGBA(w, h, rgba, maxSide);
    t.w = w;
    t.h = h;
    t.albedo = std::move(rgba);
    const auto mit = materialOf.find(et.name);
    if (mit != materialOf.end() && mit->second != et.name) t.material = mit->second;
    t.organic = organicCategory(t.material.empty() ? t.key : t.material);
    t.stem = pbrMapStemForAlbedoKey(t.key);
    t.existing = existingMapFor(t.material, t.key);
}

// Lê o .glb pelo mesmo parser do motor: os nomes das texturas saem iguais aos
// que o renderer usa como chave. Roda fora da thread da interface.
std::shared_ptr<CreatorModel> loadCreatorModel(const std::string& path) {
    auto model = std::make_shared<CreatorModel>();
    model->path = path;
    model->disk = project::gameRef(path);
    std::vector<uint8_t> data;
    {
        std::ifstream f(model->disk, std::ios::binary);
        readWholeFile(f, data);
    }
    if (data.size() < 12 || std::memcmp(data.data(), "glTF", 4) != 0) {
        model->error = "Not a .glb file: " + model->disk.string();
        return model;
    }
    ModelFile file = GltfParser::parse(data.data(), data.size(), model->disk.parent_path().string());
    data.clear();
    data.shrink_to_fit();
    if (file.embeddedTextures.empty()) {
        model->error = "This model has no textures.";
        return model;
    }

    // Material de cada imagem: textureNames e pbrTextureNames andam no mesmo índice.
    std::unordered_map<std::string, std::string> materialOf;
    for (const auto& node : file.nodes)
        for (size_t k = 0; k < node.textureNames.size() && k < node.pbrTextureNames.size(); ++k)
            materialOf.emplace(node.textureNames[k], node.pbrTextureNames[k]);

    // O mesmo teto de resolução que o bake aplica antes de sintetizar.
    const int maxSide = static_cast<int>(embeddedTextureMaxSize() > 0 ? embeddedTextureMaxSize() : 1024);
    std::vector<CreatorTexture> all(file.embeddedTextures.size());
    JobSystem::instance().parallelFor(static_cast<uint32_t>(all.size()), [&](uint32_t i) {
        decodeCreatorTexture(file.embeddedTextures[i], materialOf, maxSide, all[i]);
    });
    for (auto& t : all)
        if (!t.albedo.empty() && !t.stem.empty()) model->textures.push_back(std::move(t));
    if (model->textures.empty()) model->error = "No texture in this model is large enough to derive maps from.";
    return model;
}

GeneratedMaps generateMaps(const CreatorTexture& t, bool organic, float strength, uint32_t generation) {
    GeneratedMaps out;
    out.generation = generation;
    const auto t0 = std::chrono::steady_clock::now();
    PbrTextureData mrahw, normal;
    out.ok = synthesizePbrAnalytic(t.albedo, t.w, t.h, 4, mrahw, normal, organic, strength) &&
             mrahw.width == normal.width && mrahw.height == normal.height;
    if (out.ok) {
        out.w = mrahw.width;
        out.h = mrahw.height;
        out.mrahw = std::move(mrahw.pixels);
        out.normal = std::move(normal.pixels);
    }
    out.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return out;
}

fs::path mrahwFile(const fs::path& dir, const std::string& stem) { return dir / (stem + "_mrahw.png"); }
fs::path normalFile(const fs::path& dir, const std::string& stem) { return dir / (stem + "_normal.png"); }

bool writeMaps(const fs::path& dir, const std::string& stem, const GeneratedMaps& m, std::string& error) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        error = "Could not create " + dir.string() + ": " + ec.message();
        return false;
    }
    if (!ImageUtils::writePNG(mrahwFile(dir, stem).string(), m.w, m.h, 4, m.mrahw.data()) ||
        !ImageUtils::writePNG(normalFile(dir, stem).string(), m.w, m.h, 4, m.normal.data())) {
        error = "Could not write the maps of " + stem + " in " + dir.string();
        return false;
    }
    return true;
}

// Pasta onde a busca do motor acha mapas: a árvore de texturas cozidas do
// jogo (projeto aberto; sem projeto, a do repositório), uma subpasta por
// modelo. A busca varre a árvore toda pelo stem (findPbrPairByStem).
fs::path engineMapDir(const fs::path& glb) { return project::game("assets/data/texture") / glb.stem(); }

// Média RGB de um mapa (resumo do teste).
void meanRgba(const std::vector<uint8_t>& px, double out[4]) {
    double sum[4] = {0, 0, 0, 0};
    const size_t n = px.size() / 4;
    for (size_t i = 0; i < n; ++i)
        for (int c = 0; c < 4; ++c) sum[c] += px[i * 4 + c];
    for (int c = 0; c < 4; ++c) out[c] = n ? sum[c] / static_cast<double>(n) : 0.0;
}

} // namespace

struct Editor::AssetCreatorState {
    char path[512] = {};
    std::shared_ptr<const CreatorModel> model;
    std::future<std::shared_ptr<CreatorModel>> loading;
    std::string status;
    bool statusError = false;
    struct Entry {
        float strength = 1.0f;
        bool organic = false;
        uint32_t generation = 0; // pedido mais novo; resultado de pedido velho é descartado
        std::future<GeneratedMaps> job;
        GeneratedMaps maps;      // último resultado pronto
        std::string existing;    // _mrahw que a busca do motor acha
        bool saved = false;
        bool previewDirty = true;
        GuiTexture thumb, albedo, normal, channels[4];
    };
    std::vector<Entry> entries;
    int selected = -1;
    fs::path saveDir;            // vazio = pasta da busca do motor (engineMapDir)
    bool saveAllPending = false;
    std::vector<int> confirmIndices; // gravação esperando confirmação
    std::vector<std::string> confirmFiles;
    bool openConfirm = false;
    std::vector<std::string> written; // arquivos gravados por esta janela (regravar não pede confirmação)
    bool testLayout = false;     // teste: janela grande e na frente (captura)

    fs::path targetDir() const { return saveDir.empty() && model ? engineMapDir(model->disk) : saveDir; }

    void setStatus(const std::string& text, bool error) {
        status = text;
        statusError = error;
    }

    static void releaseEntry(Editor& ed, Entry& e) {
        ed.releaseTexture(e.thumb);
        ed.releaseTexture(e.albedo);
        ed.releaseTexture(e.normal);
        for (auto& c : e.channels) ed.releaseTexture(c);
    }

    void clear(Editor& ed) {
        for (auto& e : entries) {
            if (e.job.valid()) e.job.wait(); // o job lê o albedo do modelo que sai agora
            releaseEntry(ed, e);
        }
        entries.clear();
        model.reset();
        selected = -1;
        saveAllPending = false;
        confirmIndices.clear();
        confirmFiles.clear();
    }

    void adopt(Editor& ed, std::shared_ptr<CreatorModel> loaded) {
        clear(ed);
        if (!loaded->error.empty()) {
            setStatus(loaded->error, true);
            return;
        }
        model = std::move(loaded);
        entries.resize(model->textures.size());
        for (size_t i = 0; i < entries.size(); ++i) {
            entries[i].organic = model->textures[i].organic;
            entries[i].existing = model->textures[i].existing;
        }
        selected = 0;
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%zu texture(s) in %s", entries.size(),
                      model->disk.filename().string().c_str());
        setStatus(buf, false);
    }

    void requestGenerate(int i) {
        if (!model || i < 0 || i >= static_cast<int>(entries.size())) return;
        Entry& e = entries[static_cast<size_t>(i)];
        const uint32_t gen = ++e.generation;
        // O job segura o modelo (shared_ptr): o albedo não é copiado.
        std::shared_ptr<const CreatorModel> keep = model;
        const bool organic = e.organic;
        const float strength = e.strength;
        e.job = JobSystem::instance().submit([keep, i, organic, strength, gen] {
            return generateMaps(keep->textures[static_cast<size_t>(i)], organic, strength, gen);
        });
    }

    bool busy() const {
        for (const auto& e : entries)
            if (e.job.valid()) return true;
        return false;
    }

    void poll(Editor& ed) {
        if (loading.valid() && loading.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
            adopt(ed, loading.get());
        for (auto& e : entries) {
            if (!e.job.valid() || e.job.wait_for(std::chrono::seconds(0)) != std::future_status::ready) continue;
            GeneratedMaps m = e.job.get();
            if (m.generation != e.generation) continue; // pedido velho (o autor mexeu de novo)
            if (!m.ok) setStatus("Could not generate maps from this texture.", true);
            e.maps = std::move(m);
            e.saved = false;
            e.previewDirty = true;
        }
        // A seleção sem mapas ainda: gera (a primeira vez que se olha a textura).
        if (selected >= 0 && selected < static_cast<int>(entries.size())) {
            Entry& e = entries[static_cast<size_t>(selected)];
            if (!e.job.valid() && e.maps.generation == 0) requestGenerate(selected);
        }
        if (saveAllPending && !busy()) {
            saveAllPending = false;
            std::vector<int> all;
            for (int i = 0; i < static_cast<int>(entries.size()); ++i)
                if (entries[static_cast<size_t>(i)].maps.ok) all.push_back(i);
            requestSave(ed, all);
        }
    }

    void refreshPreview(Editor& ed, Entry& e, const CreatorTexture& t) {
        if (!e.previewDirty) return;
        e.previewDirty = false;
        int w = 0, h = 0;
        if (!e.albedo.valid()) {
            const std::vector<uint8_t> a = opaque(downscale(t.albedo.data(), t.w, t.h, kTileSide, w, h));
            e.albedo = ed.makeTexture(a.data(), w, h, false);
        }
        ed.releaseTexture(e.normal);
        for (auto& c : e.channels) ed.releaseTexture(c);
        if (!e.maps.ok) return;
        // O alfa da normal guarda materialProps (quase 0) e o do MRAH-W a altura: sem alfa na prévia.
        const std::vector<uint8_t> n = opaque(downscale(e.maps.normal.data(), e.maps.w, e.maps.h, kTileSide, w, h));
        e.normal = ed.makeTexture(n.data(), w, h, false);
        const std::vector<uint8_t> m = downscale(e.maps.mrahw.data(), e.maps.w, e.maps.h, kTileSide, w, h);
        for (int c = 0; c < 4; ++c) {
            const std::vector<uint8_t> ch = channelImage(m, c);
            e.channels[c] = ed.makeTexture(ch.data(), w, h, false);
        }
    }

    // Grava, pedindo confirmação antes de sobrescrever mapa que esta janela não fez.
    void requestSave(Editor& ed, const std::vector<int>& indices) {
        if (!model || indices.empty()) return;
        const fs::path dir = targetDir();
        confirmFiles.clear();
        std::error_code ec;
        for (int i : indices) {
            const std::string& stem = model->textures[static_cast<size_t>(i)].stem;
            for (const fs::path& f : {mrahwFile(dir, stem), normalFile(dir, stem)})
                if (fs::exists(f, ec) && std::find(written.begin(), written.end(), f.string()) == written.end())
                    confirmFiles.push_back(f.string());
        }
        if (!confirmFiles.empty()) {
            confirmIndices = indices;
            openConfirm = true;
            return;
        }
        writeNow(ed, indices);
    }

    void writeNow(Editor& ed, const std::vector<int>& indices) {
        const fs::path dir = targetDir();
        size_t ok = 0;
        std::string error;
        for (int i : indices) {
            Entry& e = entries[static_cast<size_t>(i)];
            const CreatorTexture& t = model->textures[static_cast<size_t>(i)];
            if (!e.maps.ok) continue;
            if (!writeMaps(dir, t.stem, e.maps, error)) break;
            e.saved = true;
            ++ok;
            for (const fs::path& f : {mrahwFile(dir, t.stem), normalFile(dir, t.stem)})
                if (std::find(written.begin(), written.end(), f.string()) == written.end()) written.push_back(f.string());
            ERUPTION_LOG_INFO("Asset Creator: %s -> %s (%dx%d, normal strength %.2f%s)", t.key.c_str(),
                              mrahwFile(dir, t.stem).string().c_str(), e.maps.w, e.maps.h, e.strength,
                              e.organic ? ", organic" : "");
        }
        if (ok == 0 && error.empty()) return;
        // A busca guarda respostas e a varredura da árvore: sem esquecer, o
        // mapa novo só apareceria na próxima sessão.
        invalidatePbrLookupCaches();
        for (int i : indices) {
            const CreatorTexture& t = model->textures[static_cast<size_t>(i)];
            entries[static_cast<size_t>(i)].existing = existingMapFor(t.material, t.key);
        }
        // O pacote de texturas embutidas guarda o PBR antigo e só é chaveado
        // pelo .glb: apagado, a próxima carga reassa com os mapas do disco.
        if (saveDir.empty()) {
            const size_t packs = removeEmbeddedBakeCaches(model->disk.string());
            if (packs > 0)
                ed.log(LogLevel::Info, "Asset Creator: " + std::to_string(packs) + " texture pack(s) of " +
                                           model->disk.filename().string() + " removed (rebuilt on next load).");
        }
        char buf[512];
        if (!error.empty()) std::snprintf(buf, sizeof(buf), "%s", error.c_str());
        else std::snprintf(buf, sizeof(buf), "Saved %zu texture(s) in %s", ok, dir.string().c_str());
        setStatus(buf, !error.empty());
        ed.log(error.empty() ? LogLevel::Info : LogLevel::Error, std::string("Asset Creator: ") + buf);
    }
};

Editor::AssetCreatorState& Editor::assetCreator() {
    if (!m_assetCreator) m_assetCreator = std::make_shared<AssetCreatorState>();
    return *m_assetCreator;
}

void Editor::shutdownAssetCreator() {
    if (!m_assetCreator) return;
    AssetCreatorState& s = *m_assetCreator;
    if (s.loading.valid()) s.loading.wait();
    s.clear(*this);
}

void Editor::openAssetCreator(const std::string& path) {
    AssetCreatorState& s = assetCreator();
    m_showAssetCreator = true;
    m_pendingFocus.push_back("###AssetCreator");
    // Caminho dentro do projeto fica relativo (como a cena e o visualizador usam).
    fs::path p = path;
    if (p.is_absolute() && !m_projectRoot.empty()) {
        const fs::path rel = p.lexically_relative(m_projectRoot);
        if (!rel.empty() && rel.native()[0] != '.') p = rel;
    }
    std::snprintf(s.path, sizeof(s.path), "%s", p.generic_string().c_str());
    if (s.loading.valid()) return; // uma carga por vez
    s.setStatus("Loading " + p.generic_string() + "...", false);
    s.loading = std::async(std::launch::async, loadCreatorModel, p.generic_string());
}

void Editor::assetCreatorTestRun(const std::string& spec) {
    const size_t comma = spec.rfind(',');
    if (comma == std::string::npos) {
        ERUPTION_LOG_ERROR("ERUPTION_TEST_ASSET_CREATOR: expected <model.glb>,<folder>, got '%s'", spec.c_str());
        return;
    }
    AssetCreatorState& s = assetCreator();
    if (s.loading.valid()) s.loading.wait();
    std::snprintf(s.path, sizeof(s.path), "%s", spec.substr(0, comma).c_str());
    const auto t0 = std::chrono::steady_clock::now();
    s.adopt(*this, loadCreatorModel(s.path));
    if (!s.model) {
        ERUPTION_LOG_ERROR("Asset Creator test: %s", s.status.c_str());
        return;
    }
    const double loadMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    s.saveDir = spec.substr(comma + 1);
    // Mesma geração da janela, todas de uma vez.
    std::shared_ptr<const CreatorModel> model = s.model;
    JobSystem::instance().parallelFor(static_cast<uint32_t>(s.entries.size()), [&](uint32_t i) {
        auto& e = s.entries[i];
        e.generation = 1;
        e.maps = generateMaps(model->textures[i], e.organic, e.strength, 1);
    });
    std::vector<int> all;
    for (int i = 0; i < static_cast<int>(s.entries.size()); ++i) all.push_back(i);
    s.writeNow(*this, all); // pasta de teste: sem confirmação
    size_t ok = 0;
    for (size_t i = 0; i < s.entries.size(); ++i) {
        const auto& e = s.entries[i];
        const CreatorTexture& t = model->textures[i];
        double n[4], m[4];
        meanRgba(e.maps.normal, n);
        meanRgba(e.maps.mrahw, m);
        ok += e.saved ? 1 : 0;
        ERUPTION_LOG_WARN("Asset Creator test: [%zu] key='%s' material='%s' stem='%s' albedo %dx%d (file %dx%d) "
                          "maps %dx%d %.0f ms organic=%d | normal mean RGB %.1f %.1f %.1f | MRAH-W mean %.1f %.1f "
                          "%.1f %.1f | saved=%d | engine lookup: %s",
                          i, t.key.c_str(), t.material.c_str(), t.stem.c_str(), t.w, t.h, t.fileW, t.fileH, e.maps.w,
                          e.maps.h, e.maps.ms, e.organic ? 1 : 0, n[0], n[1], n[2], m[0], m[1], m[2], m[3],
                          e.saved ? 1 : 0, e.existing.empty() ? "(none)" : e.existing.c_str());
    }
    ERUPTION_LOG_WARN("Asset Creator test: %zu of %zu texture(s) saved in %s (load %.0f ms)", ok, s.entries.size(),
                      s.targetDir().string().c_str(), loadMs);
    m_showAssetCreator = true;
    s.testLayout = true;
    s.selected = 0;
}

void Editor::drawAssetCreator() {
    if (!m_showAssetCreator) return;
    AssetCreatorState& s = assetCreator();
    s.poll(*this);
    if (s.testLayout) {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + 24, vp->WorkPos.y + 24), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(std::min(1040.0f, vp->WorkSize.x - 48), std::min(640.0f, vp->WorkSize.y - 48)),
                                 ImGuiCond_Always);
        ImGui::SetNextWindowFocus();
    } else {
        ImGui::SetNextWindowSize(ImVec2(980, 640), ImGuiCond_FirstUseEver);
    }
    if (!ImGui::Begin(trw("Asset Creator###AssetCreator"), &m_showAssetCreator)) {
        ImGui::End();
        return;
    }

    // Modelo
    const bool loading = s.loading.valid();
    ImGui::SetNextItemWidth(std::max(120.0f, ImGui::GetContentRegionAvail().x - 250.0f));
    const bool enter = ImGui::InputTextWithHint("##path", tr("model .glb (project path or absolute)"), s.path,
                                                sizeof(s.path), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    ImGui::BeginDisabled(loading || s.path[0] == '\0');
    if (ImGui::Button(tr("Load Model")) || (enter && !loading && s.path[0])) openAssetCreator(s.path);
    ImGui::EndDisabled();
    ImGui::SameLine();
    const bool assetIsModel = m_asset.kind == AssetPreviewState::Kind::Model && !m_asset.path.empty();
    ImGui::BeginDisabled(loading || !assetIsModel);
    if (ImGui::Button(tr("Use Selected"))) openAssetCreator(m_asset.path.string());
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("%s", tr("The .glb selected in the Project panel."));
    if (!s.status.empty()) {
        if (s.statusError) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", s.status.c_str());
        else ImGui::TextDisabled("%s", s.status.c_str());
    }
    if (!s.model) {
        ImGui::Spacing();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", tr("Load a model to see its textures. For each one the engine derives a normal map "
                                     "and an MRAH-W map (metallic, roughness, ambient occlusion, height) from the "
                                     "color, the same way it does at load time when no map exists."));
        ImGui::PopTextWrapPos();
        ImGui::End();
        return;
    }
    const fs::path dir = s.targetDir();
    ImGui::TextDisabled(tr("Maps are saved in %s"), dir.generic_string().c_str());
    ImGui::Separator();

    // Lista das texturas
    const float listW = 270.0f;
    ImGui::BeginChild("##textures", ImVec2(listW, 0), ImGuiChildFlags_Border);
    int thumbsMade = 0;
    for (int i = 0; i < static_cast<int>(s.entries.size()); ++i) {
        auto& e = s.entries[static_cast<size_t>(i)];
        const CreatorTexture& t = s.model->textures[static_cast<size_t>(i)];
        ImGui::PushID(i);
        const ImVec2 start = ImGui::GetCursorPos();
        if (ImGui::Selectable("##row", s.selected == i, ImGuiSelectableFlags_AllowOverlap,
                              ImVec2(0, static_cast<float>(kThumbSide))))
            s.selected = i;
        const bool visible = ImGui::IsItemVisible();
        ImGui::SetCursorPos(start);
        if (visible && !e.thumb.valid() && thumbsMade < kThumbsPerFrame) {
            int w = 0, h = 0;
            const std::vector<uint8_t> px = opaque(downscale(t.albedo.data(), t.w, t.h, kThumbSide, w, h));
            e.thumb = makeTexture(px.data(), w, h, false);
            ++thumbsMade;
        }
        if (e.thumb.valid()) ImGui::Image(reinterpret_cast<ImTextureID>(e.thumb.set), ImVec2(kThumbSide, kThumbSide));
        else ImGui::Dummy(ImVec2(kThumbSide, kThumbSide));
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::TextUnformatted(t.stem.c_str());
        const char* state = e.job.valid() ? tr("generating...")
                            : e.saved     ? tr("saved")
                            : !e.existing.empty() ? tr("has maps")
                                                  : "";
        ImGui::TextDisabled("%d x %d  %s", t.fileW, t.fileH, state);
        ImGui::EndGroup();
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::SameLine();

    // Textura escolhida
    ImGui::BeginChild("##detail", ImVec2(0, 0));
    if (s.selected >= 0 && s.selected < static_cast<int>(s.entries.size())) {
        auto& e = s.entries[static_cast<size_t>(s.selected)];
        const CreatorTexture& t = s.model->textures[static_cast<size_t>(s.selected)];
        s.refreshPreview(*this, e, t);
        ImGui::TextColored(kAccent, "%s", t.stem.c_str());
        ImGui::TextDisabled(tr("Texture key: %s"), t.key.c_str());
        if (!t.material.empty()) ImGui::TextDisabled(tr("Material: %s"), t.material.c_str());
        ImGui::TextDisabled(tr("Files: %s_mrahw.png, %s_normal.png"), t.stem.c_str(), t.stem.c_str());
        if (!e.existing.empty()) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled(tr("The engine uses: %s"), e.existing.c_str());
            ImGui::PopTextWrapPos();
        }

        ImGui::SeparatorText(tr("Generate"));
        ImGui::SetNextItemWidth(220.0f);
        ImGui::SliderFloat(tr("Normal strength"), &e.strength, 0.0f, 4.0f, "%.2f");
        const bool strengthDone = ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SetItemTooltip("%s", tr("Relief of the normal map. 1 = what the engine generates by itself."));
        ImGui::SameLine();
        const bool organicChanged = ImGui::Checkbox(tr("Organic material"), &e.organic);
        ImGui::SetItemTooltip("%s", tr("Grass, foliage, soil, snow: the height never looks for tile joints."));
        ImGui::SameLine();
        if (ImGui::Button(tr("Regenerate")) || strengthDone || organicChanged) s.requestGenerate(s.selected);
        if (e.job.valid()) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", tr("generating..."));
        } else if (e.maps.ok) {
            ImGui::SameLine();
            ImGui::TextDisabled(tr("%d x %d, %.0f ms"), e.maps.w, e.maps.h, e.maps.ms);
        }

        // Mapas lado a lado (o mesmo jeito da prévia de material do Inspetor).
        struct Tile { const char* label; const GuiTexture* tex; };
        const Tile tiles[] = {
            {"Albedo", &e.albedo},          {"Normal", &e.normal},        {"Metallic (R)", &e.channels[0]},
            {"Roughness (G)", &e.channels[1]}, {"AO (B)", &e.channels[2]}, {"Height (A)", &e.channels[3]},
        };
        // Duas linhas de mapas e a linha dos botões cabem sem rolar.
        const ImVec2 room = ImGui::GetContentRegionAvail();
        const float below = 2.0f * ImGui::GetTextLineHeightWithSpacing() + ImGui::GetFrameHeightWithSpacing() + 24.0f;
        const float tile = std::clamp(std::min((room.x - 16.0f) / 3.0f, (room.y - below) / 2.0f), 48.0f,
                                      static_cast<float>(kTileSide));
        int col = 0;
        for (const auto& tl : tiles) {
            ImGui::BeginGroup();
            if (tl.tex->valid()) {
                ImGui::Image(reinterpret_cast<ImTextureID>(tl.tex->set), ImVec2(tile, tile));
            } else {
                ImGui::Dummy(ImVec2(tile, tile));
                const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
                ImGui::GetWindowDrawList()->AddRect(mn, mx, IM_COL32(80, 80, 85, 255));
            }
            ImGui::TextDisabled("%s", tr(tl.label));
            ImGui::EndGroup();
            if (++col % 3 != 0) ImGui::SameLine();
        }

        ImGui::Separator();
        ImGui::BeginDisabled(!e.maps.ok || e.job.valid());
        if (ImGui::Button(tr("Save"))) s.requestSave(*this, {s.selected});
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(s.saveAllPending);
        if (ImGui::Button(tr("Save All"))) {
            // As que ainda não têm mapas são geradas primeiro; grava quando todas chegarem.
            for (int i = 0; i < static_cast<int>(s.entries.size()); ++i) {
                auto& o = s.entries[static_cast<size_t>(i)];
                if (!o.job.valid() && !o.maps.ok) s.requestGenerate(i);
            }
            s.saveAllPending = true;
            s.setStatus("Generating all maps before saving...", false);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(tr("Open in Model Viewer"))) openModelViewer(s.model->path, -1);
        ImGui::SetItemTooltip("%s", tr("Shows the model with the engine's rendering. A model already open in this "
                                       "session keeps its maps until it is loaded again."));
    }
    ImGui::EndChild();

    // Confirmação antes de sobrescrever mapa feito por outra pessoa/ferramenta.
    if (s.openConfirm) {
        ImGui::OpenPopup("###AssetCreatorOverwrite");
        s.openConfirm = false;
    }
    if (ImGui::BeginPopupModal((std::string(tr("Overwrite maps?")) + "###AssetCreatorOverwrite").c_str(), nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(tr("These maps already exist and were not made in this window:"));
        for (const auto& f : s.confirmFiles) ImGui::BulletText("%s", f.c_str());
        if (ImGui::Button(tr("Overwrite"))) {
            s.writeNow(*this, s.confirmIndices);
            s.confirmIndices.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Cancel"))) {
            s.confirmIndices.clear();
            s.setStatus("Nothing saved.", false);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::End();
}

} // namespace eruption
