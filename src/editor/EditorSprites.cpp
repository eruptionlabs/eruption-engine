// Importador de folha de sprites: recorta um PNG em grade, monta parado e
// andando em 8 direções (com espelho para as que faltam), mostra a prévia
// animada e grava o .spr (ERUPTSPR v2) ao lado do PNG.
#include "editor/Editor.hpp"
#include "editor/Translate.hpp"

#include "core/Engine.hpp"
#include "editor/EditorTheme.hpp"
#include "game/PlayerController.hpp"
#include "utils/ImageUtils.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>

namespace eruption {

namespace {

// Direções do motor, vistas pela câmera (2 = virado para a direita da tela).
const char* kDirNames[8] = {"Front", "Front-right", "Right", "Back-right", "Back", "Back-left", "Left", "Front-left"};

struct RowPreset {
    const char* label;
    int directions;
    int rows[8];
};
// Ordens de linha comuns em folhas prontas.
const RowPreset kPresets[] = {
    {"4 rows: front, left, right, back", 4, {0, 6, 2, 4}},
    {"4 rows: front, back, left, right", 4, {0, 4, 6, 2}},
    {"4 rows: back, left, front, right", 4, {4, 6, 0, 2}},
    {"8 rows: front, front-right, right... (turning right)", 8, {0, 1, 2, 3, 4, 5, 6, 7}},
    {"8 rows: front, front-left, left... (turning left)", 8, {0, 7, 6, 5, 4, 3, 2, 1}},
    {"1 row: same frames for every direction", 1, {0}},
};

} // namespace

void Editor::openSpriteImporter(const std::string& png) {
    SpriteImport& si = m_spriteImport;
    releaseTexture(si.sheetTex);
    si = SpriteImport{};
    si.spec.image = png;
    ImageData img = ImageUtils::loadPNG(png);
    if (!img.isValid()) {
        log(LogLevel::Error, "Could not read " + png);
        return;
    }
    if (img.channels != 4) {
        std::vector<uint8_t> rgba(size_t(img.width) * img.height * 4);
        for (size_t i = 0; i < size_t(img.width) * img.height; ++i) {
            for (int c = 0; c < 3; ++c) rgba[i * 4 + c] = img.pixels[i * img.channels + std::min(c, img.channels - 1)];
            rgba[i * 4 + 3] = 255;
        }
        img.pixels = std::move(rgba);
    }
    si.sheetTex = makeTexture(img.pixels.data(), img.width, img.height, true);
    // Palpite de grade: 4 linhas (o caso mais comum) e a largura de célula que
    // divide a folha exata e fica mais perto de 2/3 da altura (personagem em pé).
    si.spec.rows = 4;
    const float cellH = img.height / 4.0f;
    int best = 1;
    float bestErr = 1e9f;
    for (int c = 1; c <= 32; ++c) {
        if (img.width % c != 0) continue;
        const float err = std::abs(float(img.width / c) - cellH * 0.67f);
        if (err < bestErr) { bestErr = err; best = c; }
    }
    si.spec.columns = best;
    si.spec.walk.frames = si.spec.columns;
    si.dirty = true;
    m_showSpriteImporter = true;
}

void Editor::drawSpriteImporter() {
    if (!m_showSpriteImporter) return;
    SpriteImport& si = m_spriteImport;
    ImGui::SetNextWindowSize(ImVec2(900, 620), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(trw("Sprite Importer###SpriteImporter"), &m_showSpriteImporter)) {
        ImGui::End();
        return;
    }
    if (!si.sheetTex.valid()) {
        ImGui::TextDisabled("%s", tr("Right-click a PNG in the Project panel and choose Import as Sprite Sheet."));
        ImGui::End();
        return;
    }
    SpriteSheetSpec& sp = si.spec;
    ImGui::TextUnformatted(std::filesystem::path(sp.image).filename().string().c_str());
    ImGui::SameLine();
    ImGui::TextDisabled(tr("(%d x %d)"), si.sheetTex.width, si.sheetTex.height);

    // Coluna de controles
    ImGui::BeginChild("##controls", ImVec2(330, 0), ImGuiChildFlags_Border);
    bool changed = false;
    ImGui::SeparatorText(tr("Grid"));
    changed |= ImGui::InputInt(tr("Columns"), &sp.columns);
    changed |= ImGui::InputInt(tr("Rows"), &sp.rows);
    sp.columns = std::clamp(sp.columns, 1, 64);
    sp.rows = std::clamp(sp.rows, 1, 64);
    ImGui::SeparatorText(tr("Directions"));
    if (ImGui::BeginCombo(tr("Row order"), kPresets[si.preset].label)) {
        for (int i = 0; i < IM_ARRAYSIZE(kPresets); ++i)
            if (ImGui::Selectable(kPresets[i].label, si.preset == i)) {
                si.preset = i;
                changed = true;
            }
        ImGui::EndCombo();
    }
    const RowPreset& pr = kPresets[si.preset];
    sp.directions = pr.directions;
    sp.rowDirection.assign(pr.rows, pr.rows + pr.directions);
    changed |= ImGui::Checkbox(tr("Mirror missing sides"), &sp.mirrorMissing);
    ImGui::SetItemTooltip("%s", tr("A side that has no row is drawn by flipping the opposite one."));

    auto animUi = [&](const char* title, SpriteSheetSpec::Anim& a, bool optional) {
        ImGui::PushID(title);
        ImGui::SeparatorText(title);
        if (optional) changed |= ImGui::Checkbox(tr("Has this animation"), &a.enabled);
        ImGui::BeginDisabled(optional && !a.enabled);
        int row = a.firstRow + 1, col = a.firstColumn + 1;
        changed |= ImGui::InputInt(tr("First row"), &row);
        changed |= ImGui::InputInt(tr("First column"), &col);
        changed |= ImGui::InputInt(tr("Frames"), &a.frames);
        changed |= ImGui::InputInt(tr("Frame time (ms)"), &a.delayMs, 10, 50);
        a.firstRow = std::clamp(row, 1, sp.rows) - 1;
        a.firstColumn = std::clamp(col, 1, sp.columns) - 1;
        a.frames = std::clamp(a.frames, 1, 64);
        a.delayMs = std::clamp(a.delayMs, 16, 2000);
        ImGui::EndDisabled();
        ImGui::PopID();
    };
    animUi("Standing", sp.idle, false);
    animUi("Walking", sp.walk, true);

    // Animações extras: cada uma com nome, linhas da folha e se repete.
    ImGui::SeparatorText(tr("Extra animations"));
    ImGui::TextWrapped("%s", tr("Rules and scripts play them by name: characters.play(\"guard\", \"attack\")."));
    int removeAt = -1;
    for (size_t i = 0; i < sp.extra.size(); ++i) {
        auto& e = sp.extra[i];
        ImGui::PushID(static_cast<int>(i));
        char name[64];
        std::snprintf(name, sizeof(name), "%s", e.name.c_str());
        ImGui::SetNextItemWidth(150);
        if (ImGui::InputText(tr("Name"), name, sizeof(name))) {
            e.name = name;
            changed = true;
        }
        ImGui::SameLine();
        changed |= ImGui::Checkbox(tr("Repeat"), &e.loop);
        ImGui::SetItemTooltip("%s", tr("Off: plays once and goes back to standing."));
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("x"))) removeAt = static_cast<int>(i);
        animUi(("##extra" + std::to_string(i)).c_str(), e.anim, false);
        ImGui::PopID();
    }
    if (removeAt >= 0) {
        sp.extra.erase(sp.extra.begin() + removeAt);
        changed = true;
    }
    if (ImGui::SmallButton(tr("+ Animation"))) {
        SpriteSheetSpec::NamedAnim e;
        e.name = "action" + std::to_string(sp.extra.size() + 1);
        e.anim = sp.walk;
        sp.extra.push_back(e);
        changed = true;
    }

    ImGui::SeparatorText(tr("Size"));
    changed |= ImGui::DragFloat(tr("Height in world"), &sp.worldHeight, 0.5f, 1.0f, 500.0f, "%.1f");
    ImGui::SetItemTooltip("%s", tr("How tall one frame stands in the scene. The default character is about 25."));
    changed |= ImGui::InputInt(tr("Empty pixels below feet"), &sp.footOffset);
    sp.footOffset = std::max(0, sp.footOffset);

    if (changed) si.dirty = true;
    if (si.dirty) {
        si.error.clear();
        if (!buildSpriteSheet(sp, si.result, si.error)) si.result = SpriteSheetResult{};
        si.dirty = false;
    }
    if (!si.error.empty()) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", si.error.c_str());

    ImGui::Separator();
    const std::filesystem::path out = std::filesystem::path(sp.image).replace_extension(".spr");
    ImGui::BeginDisabled(si.result.images.empty());
    auto save = [&]() {
        std::string error;
        if (!writeEruptSprV2(si.result, out.string(), error)) {
            log(LogLevel::Error, error);
            return false;
        }
        log(LogLevel::Info, "Saved " + out.generic_string() + " (" + std::to_string(si.result.images.size()) + " frames)");
        return true;
    };
    if (ImGui::Button(tr("Save"))) save();
    ImGui::SetItemTooltip(tr("Writes %s"), out.filename().string().c_str());
    ImGui::SameLine();
    if (ImGui::Button(tr("Save and use for player")) && save()) usePlayerSprite(sp.image, true);
    ImGui::EndDisabled();
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##view", ImVec2(0, 0));
    const ImVec2 avail = ImGui::GetContentRegionAvail();

    // Prévia animada: direção e estado escolhidos, tocando no tempo do quadro.
    const float previewH = std::min(220.0f, avail.y * 0.45f);
    ImGui::SeparatorText(tr("Preview"));
    ImGui::SetNextItemWidth(160);
    {
        // Parado, andando e as extras pelo nome.
        std::vector<std::string> states{"Standing", "Walking"};
        for (const auto& n : si.result.named) states.push_back(n.name);
        si.previewWalk = std::clamp(si.previewWalk, 0, static_cast<int>(states.size()) - 1);
        if (ImGui::BeginCombo("##state", states[static_cast<size_t>(si.previewWalk)].c_str())) {
            for (size_t i = 0; i < states.size(); ++i)
                if (ImGui::Selectable(states[i].c_str(), si.previewWalk == static_cast<int>(i))) si.previewWalk = static_cast<int>(i);
            ImGui::EndCombo();
        }
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160);
    ImGui::SliderInt("##dir", &si.previewDir, 0, 7, kDirNames[std::clamp(si.previewDir, 0, 7)]);
    ImGui::SameLine();
    ImGui::Checkbox(tr("Turn"), &si.previewTurn);
    ImGui::SetItemTooltip("%s", tr("Cycle through the 8 directions"));
    const auto& res = si.result;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + avail.x, p0.y + previewH), IM_COL32(40, 42, 46, 255), 4.0f);
    const size_t actionIdx = si.previewWalk < 2 ? static_cast<size_t>(si.previewWalk * 8 + si.previewDir)
                                                 : si.result.named[static_cast<size_t>(si.previewWalk - 2)].firstAction +
                                                       static_cast<size_t>(si.previewDir);
    if (actionIdx < res.actions.size() && !res.actions[actionIdx].frames.empty()) {
        const auto& act = res.actions[actionIdx];
        si.previewTime += ImGui::GetIO().DeltaTime * 1000.0f;
        const float total = float(act.delayMs) * act.frames.size();
        if (si.previewTime >= total) {
            si.previewTime = std::fmod(si.previewTime, total);
            if (si.previewTurn) si.previewDir = (si.previewDir + 1) % 8;
        }
        const auto& fr = act.frames[std::min(act.frames.size() - 1, size_t(si.previewTime / act.delayMs))];
        const auto [row, col] = res.imageCell[fr.image];
        float u0 = float(col * res.cellW) / res.sheetW, u1 = float((col + 1) * res.cellW) / res.sheetW;
        const float v0 = float(row * res.strideH) / res.sheetH, v1 = float(row * res.strideH + res.cellH) / res.sheetH;
        if (fr.mirror) std::swap(u0, u1);
        const float sc = std::max(1.0f, std::floor((previewH - 16.0f) / res.cellH));
        const ImVec2 sz(res.cellW * sc, res.cellH * sc);
        const ImVec2 c(p0.x + avail.x * 0.5f, p0.y + previewH - 8.0f);
        dl->AddLine(ImVec2(c.x - sz.x, c.y), ImVec2(c.x + sz.x, c.y), IM_COL32(255, 255, 255, 40));
        dl->AddImage(reinterpret_cast<ImTextureID>(si.sheetTex.set), ImVec2(c.x - sz.x * 0.5f, c.y - sz.y),
                     ImVec2(c.x + sz.x * 0.5f, c.y), ImVec2(u0, v0), ImVec2(u1, v1));
        dl->AddText(ImVec2(p0.x + 8, p0.y + 6), IM_COL32(200, 200, 205, 255), kDirNames[si.previewDir]);
    }
    ImGui::Dummy(ImVec2(avail.x, previewH));

    // A folha com a grade e as células usadas.
    ImGui::SeparatorText(tr("Sheet"));
    const ImVec2 sheetAvail = ImGui::GetContentRegionAvail();
    const float scale = std::min(sheetAvail.x / si.sheetTex.width, sheetAvail.y / si.sheetTex.height);
    const ImVec2 s0 = ImGui::GetCursorScreenPos();
    const ImVec2 ssz(si.sheetTex.width * scale, si.sheetTex.height * scale);
    dl->AddRectFilled(s0, ImVec2(s0.x + ssz.x, s0.y + ssz.y), IM_COL32(30, 31, 34, 255));
    dl->AddImage(reinterpret_cast<ImTextureID>(si.sheetTex.set), s0, ImVec2(s0.x + ssz.x, s0.y + ssz.y));
    const float cw = ssz.x / sp.columns, ch = ssz.y / sp.rows;
    for (int x = 0; x <= sp.columns; ++x)
        dl->AddLine(ImVec2(s0.x + x * cw, s0.y), ImVec2(s0.x + x * cw, s0.y + ssz.y), IM_COL32(255, 255, 255, 60));
    for (int y = 0; y <= sp.rows; ++y)
        dl->AddLine(ImVec2(s0.x, s0.y + y * ch), ImVec2(s0.x + ssz.x, s0.y + y * ch), IM_COL32(255, 255, 255, 60));
    const ImU32 accent = ImGui::GetColorU32(kAccent);
    for (const auto& [row, col] : res.imageCell)
        dl->AddRect(ImVec2(s0.x + col * cw + 1, s0.y + row * ch + 1), ImVec2(s0.x + (col + 1) * cw - 1, s0.y + (row + 1) * ch - 1),
                    accent, 0.0f, 0, 1.5f);
    // Nome da direção de cada linha usada.
    auto labelRows = [&](const SpriteSheetSpec::Anim& a) {
        for (int r = 0; r < sp.directions; ++r) {
            const int row = a.firstRow + r;
            if (row >= sp.rows) break;
            dl->AddText(ImVec2(s0.x + 4, s0.y + row * ch + 2), IM_COL32(255, 220, 120, 255), kDirNames[sp.rowDirection[r]]);
        }
    };
    labelRows(sp.idle);
    if (sp.walk.enabled && sp.walk.firstRow != sp.idle.firstRow) labelRows(sp.walk);
    ImGui::Dummy(ssz);
    ImGui::EndChild();
    ImGui::End();
}

void Editor::usePlayerSprite(const std::string& png, bool persist) {
    std::error_code ec;
    const std::filesystem::path rel = std::filesystem::relative(png, std::filesystem::current_path(), ec);
    const std::string path = (!ec && !rel.empty() && rel.native().rfind("..", 0) != 0) ? rel.generic_string() : png;
    m_engine->m_charConfig.job = path;
    if (m_engine->m_playerController) m_engine->m_playerController->reloadSprite();
    if (persist) m_engine->m_charConfig.save("data/char_config.json");
    log(LogLevel::Info, "Player sprite: " + path);
}

} // namespace eruption
