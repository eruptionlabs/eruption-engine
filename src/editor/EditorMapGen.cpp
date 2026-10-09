// Gerador de mapas (plugins/mapgen): receita em Luau mostrada como pilha de
// etapas, prévia rápida em baixa resolução e geração completa em segundo
// plano, que grava o mapa no formato do motor e o abre.
#include "editor/Editor.hpp"

#include "core/Engine.hpp"
#include "editor/EditorBlocks.hpp"
#include "editor/EditorTheme.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <thread>

namespace eruption {

namespace {

namespace fs = std::filesystem;

const char* kDefaultRecipe = R"(-- Map recipe: steps run top to bottom (Map Generator in the editor).
return {
    size = 1000, cell = 8, seed = 1,
    steps = {
        { "noise", scale = 320, height = 70 },
        { "ridges", scale = 600, height = 60 },
        { "erosion", drops = 60000 },
        { "smooth" },
        { "spawn" },
        { "walk", max_slope = 40 },
    },
}
)";

constexpr const char* kStepPayload = "ERUPTION_MAPGEN_STEP";
struct StepDrag { const mapgen::StepInfo* info; int index; }; // info: da paleta; index: bloco existente

// Cor do bloco pelo tipo de etapa.
ImU32 stepColor(const std::string& type) {
    if (type == "erosion" || type == "smooth") return IM_COL32(205, 120, 50, 255);
    if (type == "water") return IM_COL32(60, 120, 210, 255);
    if (type == "paint") return IM_COL32(110, 170, 70, 255);
    if (type == "scatter") return IM_COL32(60, 130, 90, 255);
    if (type == "spawn" || type == "walk") return IM_COL32(150, 100, 190, 255);
    return IM_COL32(160, 120, 80, 255); // forma do terreno
}

// "Morros: scale 320, height 70" (os parâmetros que a receita define).
std::string stepSummary(const mapgen::Step& s) {
    const mapgen::StepInfo* info = mapgen::findStep(s.type);
    std::string t = info ? info->label : s.type;
    std::string params;
    for (const auto& [k, v] : s.str) params += (params.empty() ? "" : ", ") + std::filesystem::path(v).filename().string();
    char buf[48];
    for (const auto& [k, v] : s.num) {
        std::snprintf(buf, sizeof(buf), "%s %g", k.c_str(), v);
        params += (params.empty() ? "" : ", ") + std::string(buf);
    }
    return params.empty() ? t : t + ": " + params;
}

ImU32 layerTint(size_t i) {
    static const ImU32 k[] = {IM_COL32(200, 180, 120, 255), IM_COL32(120, 120, 125, 255), IM_COL32(90, 150, 70, 255),
                              IM_COL32(210, 210, 220, 255), IM_COL32(150, 100, 70, 255), IM_COL32(60, 110, 60, 255),
                              IM_COL32(180, 150, 200, 255), IM_COL32(100, 140, 170, 255)};
    return k[i % 8];
}

// Relevo sombreado + camadas + água + pontos dos objetos.
std::vector<uint8_t> previewPixels(const mapgen::Result& r) {
    std::vector<uint8_t> px(size_t(r.w) * r.d * 4);
    const auto [lo, hi] = std::minmax_element(r.height.begin(), r.height.end());
    const float range = std::max(1e-3f, *hi - *lo);
    for (int z = 0; z < r.d; ++z)
        for (int x = 0; x < r.w; ++x) {
            const size_t g = size_t(z) * r.w + x;
            const int xl = std::max(0, x - 1), zu = std::max(0, z - 1);
            const float shade = std::clamp(0.75f + (r.h(x, z) - r.h(xl, zu)) / r.cell * 0.6f, 0.35f, 1.25f);
            float c[3] = {0.30f + 0.4f * (r.h(x, z) - *lo) / range, 0.36f + 0.3f * (r.h(x, z) - *lo) / range, 0.26f};
            for (size_t l = 0; l < r.layers.size(); ++l) {
                const float w = r.layers[l].weight[g];
                const ImU32 t = layerTint(l);
                c[0] += (((t >> 0) & 255) / 255.0f - c[0]) * w;
                c[1] += (((t >> 8) & 255) / 255.0f - c[1]) * w;
                c[2] += (((t >> 16) & 255) / 255.0f - c[2]) * w;
            }
            for (float& v : c) v *= shade;
            if (r.hasWater && r.h(x, z) < r.waterLevel) {
                const float depth = std::clamp((r.waterLevel - r.h(x, z)) / 20.0f, 0.2f, 1.0f);
                c[0] = 0.10f; c[1] = 0.30f + 0.2f * (1 - depth); c[2] = 0.55f + 0.2f * (1 - depth);
            }
            for (int k = 0; k < 3; ++k) px[g * 4 + k] = uint8_t(std::clamp(c[k], 0.0f, 1.0f) * 255.0f);
            px[g * 4 + 3] = 255;
        }
    for (const auto& grp : r.scatter)
        for (const auto& it : grp.items) {
            const int x = std::clamp(int(it.x / r.cell), 0, r.w - 1), z = std::clamp(int(it.z / r.cell), 0, r.d - 1);
            uint8_t* p = &px[(size_t(z) * r.w + x) * 4];
            p[0] = 30; p[1] = 70; p[2] = 30;
        }
    return px;
}

} // namespace

void Editor::mapGenLoad(const fs::path& file) {
    MapGenState& mg = m_mapgen;
    mg.file = file;
    mg.error.clear();
    std::error_code ec;
    mg.mtime = fs::last_write_time(file, ec);
    if (!mg.recipe.load(file.string(), mg.error)) mg.recipe = mapgen::Recipe{};
    std::snprintf(mg.mapName, sizeof(mg.mapName), "%s", fs::path(file).stem().stem().string().c_str());
    mg.previewDirty = true;
}

void Editor::mapGenSave() {
    MapGenState& mg = m_mapgen;
    std::ofstream(mg.file, std::ios::binary) << mg.recipe.serialize();
    std::error_code ec;
    mg.mtime = fs::last_write_time(mg.file, ec);
    mg.previewDirty = true;
}

bool Editor::mapGenStepParams(mapgen::Step& s) {
    const mapgen::StepInfo* info = mapgen::findStep(s.type);
    std::error_code ec;
    bool changed = false;
    if (info) {
        for (const char* t : info->texts) {
            char buf[260];
            std::snprintf(buf, sizeof(buf), "%s", s.text(t).c_str());
            if (ImGui::InputText(t, buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue)) { s.str[t] = buf; changed = true; }
            if (!m_projectSelected.empty() && ImGui::SmallButton("Use the file selected in Project")) {
                const fs::path rel = fs::relative(m_projectSelected, m_projectRoot, ec);
                s.str[t] = rel.empty() ? m_projectSelected : rel.generic_string();
                changed = true;
            }
        }
        for (const auto& p : info->params) {
            const bool has = s.num.count(p.name) != 0;
            const bool unbounded = std::abs(p.def) >= 1e8;
            float v = float(has ? s.num[p.name] : (unbounded ? (p.def < 0 ? p.min : p.max) : p.def));
            if (unbounded) {
                // Regra opcional: desligada até marcar.
                bool use = has;
                if (ImGui::Checkbox((std::string("##use") + p.name).c_str(), &use)) {
                    if (use) s.num[p.name] = v; else s.num.erase(p.name);
                    changed = true;
                }
                ImGui::SameLine();
                ImGui::BeginDisabled(!use);
            }
            if (ImGui::DragFloat(p.name, &v, float(std::max(0.01, (p.max - p.min) / 400.0)), float(p.min), float(p.max), "%.2f")) {
                s.num[p.name] = v;
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
            ImGui::SetItemTooltip("%s", p.help);
            if (unbounded) ImGui::EndDisabled();
        }
    }
    return changed;
}

void Editor::drawMapGenWindow() {
    if (!m_showMapGen) return;
    MapGenState& mg = m_mapgen;
    ImGui::SetNextWindowSize(ImVec2(980, 640), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Map Generator###MapGenerator", &m_showMapGen)) {
        ImGui::End();
        return;
    }
    const fs::path dir = m_projectRoot / "assets" / "mapgen";
    std::error_code ec;
    // Receitas
    ImGui::BeginChild("##recipes", ImVec2(190, 0), ImGuiChildFlags_Border | ImGuiChildFlags_ResizeX);
    ImGui::TextDisabled("Recipes");
    for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); ++it) {
        const std::string n = it->path().filename().string();
        if (n.size() < 13 || n.compare(n.size() - 12, 12, ".mapgen.luau") != 0) continue;
        if (ImGui::Selectable(n.substr(0, n.size() - 12).c_str(), it->path() == mg.file)) mapGenLoad(it->path());
    }
    ImGui::Separator();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##newrecipe", mg.newName, sizeof(mg.newName));
    if (ImGui::Button("+ New recipe", ImVec2(-1, 0)) && mg.newName[0]) {
        fs::create_directories(dir, ec);
        const fs::path f = dir / (std::string(mg.newName) + ".mapgen.luau");
        if (!fs::exists(f, ec)) std::ofstream(f) << kDefaultRecipe;
        mapGenLoad(f);
    }
    ImGui::EndChild();
    ImGui::SameLine();

    if (mg.file.empty()) {
        ImGui::TextDisabled("Create or pick a recipe on the left.");
        ImGui::End();
        return;
    }
    // Mudou fora (VS Code, IA): relê.
    if (fs::exists(mg.file, ec) && fs::last_write_time(mg.file, ec) != mg.mtime) mapGenLoad(mg.file);

    // Etapas
    ImGui::BeginChild("##steps", ImVec2(420, 0), ImGuiChildFlags_Border | ImGuiChildFlags_ResizeX);
    mapgen::Recipe& r = mg.recipe;
    bool changed = false;
    if (!mg.error.empty()) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", mg.error.c_str());
    ImGui::SeparatorText("Map");
    changed |= ImGui::DragFloat("Size", &r.size, 10, 64, 8000, "%.0f");
    changed |= ImGui::DragFloat("Grid step", &r.cell, 0.5f, 1, 64, "%.1f");
    int seed = int(r.seed);
    if (ImGui::InputInt("Seed", &seed)) { r.seed = uint32_t(std::max(0, seed)); changed = true; }
    ImGui::SameLine();
    if (ImGui::SmallButton("New")) { r.seed = uint32_t(std::rand()); changed = true; }
    ImGui::SeparatorText("Steps (top to bottom)");
    if (ImGui::RadioButton("Stack", !mg.blocks)) mg.blocks = false;
    ImGui::SameLine();
    if (ImGui::RadioButton("Blocks", mg.blocks)) mg.blocks = true;
    if (mg.blocks) {
        changed |= drawMapGenBlocks();
    } else {
    int removeAt = -1, moveUp = -1, moveDown = -1;
    for (size_t i = 0; i < r.steps.size(); ++i) {
        mapgen::Step& s = r.steps[i];
        const mapgen::StepInfo* info = mapgen::findStep(s.type);
        ImGui::PushID(int(i));
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        changed |= ImGui::Checkbox("##on", &s.enabled);
        ImGui::SameLine();
        const bool open = ImGui::TreeNodeEx("##step", ImGuiTreeNodeFlags_SpanAvailWidth, "%s", info ? info->label : s.type.c_str());
        if (info) ImGui::SetItemTooltip("%s", info->help);
        ImGui::SameLine(ImGui::GetContentRegionMax().x - 70);
        if (ImGui::SmallButton("^")) moveUp = int(i);
        ImGui::SameLine();
        if (ImGui::SmallButton("v")) moveDown = int(i);
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) removeAt = int(i);
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p0.x - 6, p0.y), ImVec2(p0.x - 2, p0.y + ImGui::GetFrameHeight()),
                                                  s.enabled ? ImGui::GetColorU32(kAccent) : IM_COL32(90, 90, 90, 255));
        if (open) {
            changed |= mapGenStepParams(s);
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    if (removeAt >= 0) { r.steps.erase(r.steps.begin() + removeAt); changed = true; }
    if (moveUp > 0) { std::swap(r.steps[size_t(moveUp)], r.steps[size_t(moveUp - 1)]); changed = true; }
    if (moveDown >= 0 && moveDown + 1 < int(r.steps.size())) { std::swap(r.steps[size_t(moveDown)], r.steps[size_t(moveDown + 1)]); changed = true; }
    if (ImGui::BeginCombo("##add", "+ Add step")) {
        for (const auto& info : mapgen::stepCatalog())
            if (ImGui::Selectable(info.label)) {
                mapgen::Step s;
                s.type = info.type;
                r.steps.push_back(s);
                changed = true;
            }
        ImGui::EndCombo();
    }
    }
    ImGui::EndChild();
    if (changed) mapGenSave();
    ImGui::SameLine();

    // Prévia e geração
    ImGui::BeginChild("##preview");
    if (mg.previewDirty && !mg.previewRunning) {
        mg.previewDirty = false;
        mg.previewRunning = true;
        const mapgen::Recipe copy = r;
        std::thread([this, copy] {
            mapgen::Result res;
            std::string err;
            const bool ok = mapgen::run(copy, 160, res, err);
            std::lock_guard<std::mutex> lock(m_mapgenMutex);
            m_mapgen.previewError = ok ? std::string() : err;
            if (ok) {
                m_mapgen.previewPixels = previewPixels(res);
                m_mapgen.previewSize = res.w;
            }
            m_mapgen.previewReady = ok;
            m_mapgen.previewRunning = false;
        }).detach();
    }
    {
        std::lock_guard<std::mutex> lock(m_mapgenMutex);
        if (mg.previewReady) {
            releaseTexture(mg.previewTex);
            mg.previewTex = makeTexture(mg.previewPixels.data(), mg.previewSize, mg.previewSize, false);
            mg.previewReady = false;
        }
        if (!mg.previewError.empty()) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", mg.previewError.c_str());
    }
    const float side = std::min(ImGui::GetContentRegionAvail().x, ImGui::GetContentRegionAvail().y - 110);
    if (mg.previewTex.valid()) ImGui::Image(reinterpret_cast<ImTextureID>(mg.previewTex.set), ImVec2(side, side));
    else ImGui::Dummy(ImVec2(side, side));
    if (mg.previewRunning) ImGui::TextDisabled("Updating preview...");
    ImGui::SeparatorText("Generate");
    ImGui::SetNextItemWidth(200);
    ImGui::InputText("Map name", mg.mapName, sizeof(mg.mapName));
    const bool exists = fs::exists(m_projectRoot / "assets" / "external" / mg.mapName, ec);
    if (exists) ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "Replaces the map \"%s\" and its scene edits.", mg.mapName);
    ImGui::BeginDisabled(mg.generating || !mg.mapName[0]);
    if (ImGui::Button("Generate map", ImVec2(160, 0))) mapGenGenerate();
    ImGui::EndDisabled();
    if (mg.generating) {
        ImGui::SameLine();
        std::lock_guard<std::mutex> lock(m_mapgenMutex);
        ImGui::ProgressBar(mg.progress, ImVec2(-1, 0), mg.stage.c_str());
    }
    ImGui::EndChild();
    ImGui::End();
}

bool Editor::drawMapGenBlocks() {
    MapGenState& mg = m_mapgen;
    mapgen::Recipe& r = mg.recipe;
    bool changed = false;
    // Recebe um bloco arrastado na posição `at` (-1 = fim).
    auto accept = [&](int at) {
        const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kStepPayload);
        if (!p) return;
        const StepDrag d = *static_cast<const StepDrag*>(p->Data);
        mapgen::Step step;
        if (d.info) {
            step.type = d.info->type;
        } else {
            if (d.index < 0 || d.index >= int(r.steps.size())) return;
            step = r.steps[size_t(d.index)];
            r.steps.erase(r.steps.begin() + d.index);
            if (at > d.index) --at;
        }
        if (at < 0 || at > int(r.steps.size())) at = int(r.steps.size());
        r.steps.insert(r.steps.begin() + at, step);
        mg.selected = at;
        changed = true;
    };
    // Paleta (soltar um bloco aqui apaga).
    ImGui::BeginChild("##mgpalette", ImVec2(150, 300), ImGuiChildFlags_Border);
    for (const auto& info : mapgen::stepCatalog()) {
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const ImVec2 size(ImGui::GetContentRegionAvail().x, ImGui::GetTextLineHeight() + 10);
        ImGui::PushID(info.type);
        ImGui::InvisibleButton("##pal", size);
        blockShape(ImGui::GetWindowDrawList(), p0, ImVec2(p0.x + size.x, p0.y + size.y), stepColor(info.type), 1);
        ImGui::GetWindowDrawList()->AddText(ImVec2(p0.x + 8, p0.y + 5), IM_COL32(255, 255, 255, 255), info.label);
        ImGui::SetItemTooltip("%s", info.help);
        if (ImGui::BeginDragDropSource()) {
            StepDrag d{&info, -1};
            ImGui::SetDragDropPayload(kStepPayload, &d, sizeof(d));
            ImGui::TextUnformatted(info.label);
            ImGui::EndDragDropSource();
        }
        ImGui::PopID();
        ImGui::Dummy(ImVec2(0, 3));
    }
    ImGui::EndChild();
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kStepPayload)) {
            const StepDrag d = *static_cast<const StepDrag*>(p->Data);
            if (!d.info && d.index >= 0 && d.index < int(r.steps.size())) {
                r.steps.erase(r.steps.begin() + d.index);
                mg.selected = -1;
                changed = true;
            }
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::SameLine();
    // A receita: chapéu do mapa e as etapas encaixadas embaixo.
    ImGui::BeginChild("##mgstack", ImVec2(0, 300), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    {
        char title[96];
        std::snprintf(title, sizeof(title), "Map %.0f x %.0f, seed %u", r.size, r.size, r.seed);
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const ImVec2 size(std::max(200.0f, ImGui::CalcTextSize(title).x + 24), ImGui::GetTextLineHeight() + 18);
        ImGui::Dummy(size);
        blockShape(dl, p0, ImVec2(p0.x + size.x, p0.y + size.y), ImGui::GetColorU32(kAccent), 0);
        dl->AddText(ImVec2(p0.x + 10, p0.y + 9), IM_COL32(255, 255, 255, 255), title);
    }
    for (size_t i = 0; i < r.steps.size(); ++i) {
        mapgen::Step& st = r.steps[i];
        const std::string text = stepSummary(st);
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const ImVec2 size(std::max(200.0f, ImGui::CalcTextSize(text.c_str()).x + 24), ImGui::GetTextLineHeight() + 12);
        ImGui::PushID(int(i));
        ImGui::InvisibleButton("##blk", size);
        const bool hovered = ImGui::IsItemHovered();
        ImU32 col = stepColor(st.type);
        if (!st.enabled) col = IM_COL32(95, 95, 100, 255);
        blockShape(dl, p0, ImVec2(p0.x + size.x, p0.y + size.y), hovered ? darker(col, 1.12f) : col, 1);
        if (mg.selected == int(i)) dl->AddRect(p0, ImVec2(p0.x + size.x, p0.y + size.y), IM_COL32(255, 255, 255, 220), 5.0f, 0, 2.0f);
        dl->AddText(ImVec2(p0.x + 10, p0.y + 6), IM_COL32(255, 255, 255, 255), text.c_str());
        const ImGuiIO& io = ImGui::GetIO();
        if (ImGui::IsItemDeactivated() && hovered && io.MouseDragMaxDistanceSqr[0] < io.MouseDragThreshold * io.MouseDragThreshold)
            mg.selected = int(i);
        if (ImGui::BeginDragDropSource()) {
            StepDrag d{nullptr, int(i)};
            ImGui::SetDragDropPayload(kStepPayload, &d, sizeof(d));
            ImGui::TextUnformatted(text.c_str());
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget()) { accept(int(i)); ImGui::EndDragDropTarget(); }
        ImGui::PopID();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::Button("+ drop a step here", ImVec2(200, 0));
    ImGui::PopStyleColor();
    if (ImGui::BeginDragDropTarget()) { accept(-1); ImGui::EndDragDropTarget(); }
    ImGui::EndChild();
    // Parâmetros do bloco escolhido.
    if (mg.selected >= 0 && mg.selected < int(r.steps.size())) {
        mapgen::Step& st = r.steps[size_t(mg.selected)];
        const mapgen::StepInfo* info = mapgen::findStep(st.type);
        ImGui::SeparatorText(info ? info->label : st.type.c_str());
        changed |= ImGui::Checkbox("Enabled", &st.enabled);
        changed |= mapGenStepParams(st);
    } else {
        ImGui::TextDisabled("Click a block to change it. Drag blocks to reorder; drop on the palette to remove.");
    }
    return changed;
}

void Editor::mapGenGenerate() {
    MapGenState& mg = m_mapgen;
    if (mg.generating) return;
    mg.generating = true;
    mg.progress = 0.0f;
    const mapgen::Recipe recipe = mg.recipe;
    const std::string name = mg.mapName;
    const std::string root = m_projectRoot.string();
    std::thread([this, recipe, name, root] {
        mapgen::Result res;
        std::string err;
        bool ok = mapgen::run(recipe, 0, res, err, [this](float t, const char* stage) {
            std::lock_guard<std::mutex> lock(m_mapgenMutex);
            m_mapgen.progress = t;
            m_mapgen.stage = stage;
            return true;
        });
        if (ok) ok = mapgen::writeMap(res, root, name, err);
        std::lock_guard<std::mutex> lock(m_mapgenMutex);
        m_mapgen.generating = false;
        if (ok) {
            m_mapgen.generatedMap = name;
            log(LogLevel::Info, "Generated map " + name + " (" + std::to_string(res.w) + " x " + std::to_string(res.d) + " grid)");
        } else {
            log(LogLevel::Error, "Map generation failed: " + err);
        }
    }).detach();
}

void Editor::mapGenPoll() {
    std::string map;
    {
        std::lock_guard<std::mutex> lock(m_mapgenMutex);
        map.swap(m_mapgen.generatedMap);
    }
    if (map.empty()) return;
    m_engine->populateMaps();
    m_pendingMap = map; // abre o mapa novo
}

} // namespace eruption
