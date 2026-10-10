#include "formats/SpriteSheet.hpp"

#include "core/Logger.hpp"
#include "core/Project.hpp"
#include "utils/ImageUtils.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>

namespace eruption {

namespace {

// Direção espelhada horizontalmente: esquerda <-> direita, frente/costas ficam.
int mirrorOf(int dir) { return (8 - dir) % 8; }

} // namespace

bool buildSpriteSheet(const SpriteSheetSpec& spec, SpriteSheetResult& out, std::string& error) {
    out = SpriteSheetResult{};
    ImageData img = ImageUtils::loadPNG(spec.image);
    if (!img.isValid()) {
        error = "Could not read " + spec.image;
        return false;
    }
    if (img.channels != 4) {
        // Normaliza para RGBA.
        std::vector<uint8_t> rgba(size_t(img.width) * img.height * 4);
        for (size_t i = 0; i < size_t(img.width) * img.height; ++i) {
            for (int c = 0; c < 3; ++c) rgba[i * 4 + c] = img.pixels[i * img.channels + std::min(c, img.channels - 1)];
            rgba[i * 4 + 3] = img.channels == 4 ? img.pixels[i * 4 + 3] : 255;
        }
        img.pixels = std::move(rgba);
        img.channels = 4;
    }
    if (spec.columns < 1 || spec.rows < 1 || img.width / spec.columns < 1 || img.height / spec.rows < 1) {
        error = "The grid does not fit the image.";
        return false;
    }
    if (spec.directions != 1 && spec.directions != 4 && spec.directions != 8) {
        error = "Directions must be 1, 4 or 8.";
        return false;
    }
    if (static_cast<int>(spec.rowDirection.size()) < spec.directions) {
        error = "Each row needs a direction.";
        return false;
    }
    out.cellW = img.width / spec.columns;
    const int fullH = img.height / spec.rows;
    // O vão transparente abaixo do pé sai da imagem: o motor põe a borda de
    // baixo do quadro no chão.
    out.cellH = std::max(1, fullH - std::clamp(spec.footOffset, 0, fullH - 1));
    out.sheetW = img.width;
    out.sheetH = img.height;
    out.strideH = fullH;
    out.scale = spec.worldHeight / (out.cellH * 0.15f);

    // Cada célula usada vira uma imagem (uma só vez, mesmo se repetida).
    std::map<std::pair<int, int>, uint32_t> cellImage;
    auto cell = [&](int row, int col) -> int {
        if (row < 0 || col < 0 || row >= spec.rows || col >= spec.columns) return -1;
        auto it = cellImage.find({row, col});
        if (it != cellImage.end()) return static_cast<int>(it->second);
        std::vector<uint8_t> px(size_t(out.cellW) * out.cellH * 4);
        for (int y = 0; y < out.cellH; ++y) {
            const uint8_t* src = &img.pixels[(size_t(row * fullH + y) * img.width + size_t(col) * out.cellW) * 4];
            std::memcpy(&px[size_t(y) * out.cellW * 4], src, size_t(out.cellW) * 4);
        }
        out.images.push_back(std::move(px));
        out.imageCell.push_back({row, col});
        const uint32_t idx = static_cast<uint32_t>(out.images.size() - 1);
        cellImage[{row, col}] = idx;
        return static_cast<int>(idx);
    };

    // Para cada estado: direção do motor -> (linha, espelhado).
    auto buildState = [&](const SpriteSheetSpec::Anim& a, std::vector<SpriteSheetResult::Action>& actions) -> bool {
        int rowFor[8];
        bool mirrored[8] = {};
        std::fill(std::begin(rowFor), std::end(rowFor), -1);
        for (int r = 0; r < spec.directions; ++r) {
            const int d = spec.rowDirection[r];
            if (d >= 0 && d < 8) rowFor[d] = a.firstRow + r;
        }
        if (spec.directions == 1) std::fill(std::begin(rowFor), std::end(rowFor), a.firstRow);
        for (int d = 0; d < 8; ++d) {
            if (rowFor[d] >= 0) continue;
            const int m = mirrorOf(d);
            if (spec.mirrorMissing && rowFor[m] >= 0 && m != d) {
                rowFor[d] = rowFor[m];
                mirrored[d] = true;
            }
        }
        // Diagonais que ainda faltam (folha de 4 direções): a vizinha mais próxima,
        // preferindo frente/costas (0/4), que leem melhor em diagonal.
        for (int d = 0; d < 8; ++d) {
            if (rowFor[d] >= 0) continue;
            const int cands[2] = {(d + 7) % 8, (d + 1) % 8};
            for (int c : cands) {
                if (rowFor[c] >= 0 && !mirrored[c] && (c == 0 || c == 4)) { rowFor[d] = rowFor[c]; break; }
            }
            if (rowFor[d] >= 0) continue;
            for (int c : cands) {
                if (rowFor[c] >= 0) { rowFor[d] = rowFor[c]; mirrored[d] = mirrored[c]; break; }
            }
            if (rowFor[d] < 0) rowFor[d] = a.firstRow;
        }
        for (int d = 0; d < 8; ++d) {
            SpriteSheetResult::Action act;
            act.delayMs = static_cast<uint32_t>(std::max(16, a.delayMs));
            for (int f = 0; f < std::max(1, a.frames); ++f) {
                const int idx = cell(rowFor[d], a.firstColumn + f);
                if (idx < 0) {
                    error = "Animation frames go past the grid (row " + std::to_string(rowFor[d] + 1) + ", column " +
                            std::to_string(a.firstColumn + f + 1) + ").";
                    return false;
                }
                act.frames.push_back({static_cast<uint32_t>(idx), mirrored[d]});
            }
            actions.push_back(std::move(act));
        }
        return true;
    };
    if (!buildState(spec.idle, out.actions)) return false;
    if (!buildState(spec.walk.enabled ? spec.walk : spec.idle, out.actions)) return false;
    for (const auto& e : spec.extra) {
        if (e.name.empty() || !e.anim.enabled) continue;
        const uint32_t first = static_cast<uint32_t>(out.actions.size());
        if (!buildState(e.anim, out.actions)) return false;
        out.named.push_back({e.name, first, e.loop});
    }
    return true;
}

bool writeEruptSprV2(const SpriteSheetResult& sheet, const std::string& path, std::string& error) {
    std::ofstream f(path, std::ios::binary);
    if (!f) {
        error = "Could not write " + path;
        return false;
    }
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto i32 = [&](int32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto f32 = [&](float v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    f.write("ERUPTSPR", 8);
    u32(sheet.named.empty() ? 2 : 3); // 3: com animações extras no fim
    u32(static_cast<uint32_t>(sheet.images.size()));
    for (const auto& px : sheet.images) {
        u32(static_cast<uint32_t>(sheet.cellW));
        u32(static_cast<uint32_t>(sheet.cellH));
        f.write(reinterpret_cast<const char*>(px.data()), static_cast<std::streamsize>(px.size()));
    }
    u32(static_cast<uint32_t>(sheet.actions.size()));
    for (const auto& a : sheet.actions) {
        u32(a.delayMs);
        u32(static_cast<uint32_t>(a.frames.size()));
        for (const auto& fr : a.frames) {
            u32(1);
            u32(fr.image);
            i32(0);
            i32(0);
            f32(sheet.scale);
            f32(sheet.scale);
            f32(1.0f); f32(1.0f); f32(1.0f); f32(1.0f);
            u32(fr.mirror ? 1u : 0u);
        }
    }
    if (!sheet.named.empty()) {
        u32(static_cast<uint32_t>(sheet.named.size()));
        for (const auto& n : sheet.named) {
            const uint16_t len = static_cast<uint16_t>(std::min<size_t>(n.name.size(), 255));
            f.write(reinterpret_cast<const char*>(&len), 2);
            f.write(n.name.data(), len);
            u32(n.firstAction);
            u32(n.loop ? 1u : 0u);
        }
    }
    if (!f) {
        error = "Could not write " + path;
        return false;
    }
    return true;
}


bool loadSpriteSheetRecipe(const std::string& jsonPath, SpriteSheetSpec& out, std::string& error) {
    namespace fs = std::filesystem;
    out = SpriteSheetSpec{};
    std::ifstream f(jsonPath);
    if (!f) {
        error = "Could not read " + jsonPath;
        return false;
    }
    nlohmann::json j;
    try {
        f >> j;
    } catch (const std::exception& e) {
        error = jsonPath + ": " + e.what();
        return false;
    }
    if (!j.is_object()) {
        error = jsonPath + ": the file must hold a JSON object";
        return false;
    }

    auto readAnim = [](const nlohmann::json& src, SpriteSheetSpec::Anim& a) {
        if (!src.is_object()) return;
        a.firstRow = src.value("firstRow", a.firstRow);
        a.firstColumn = src.value("firstColumn", a.firstColumn);
        a.frames = src.value("frames", a.frames);
        a.delayMs = src.value("delayMs", a.delayMs);
        a.enabled = src.value("enabled", a.enabled);
    };

    // PNG: o caminho da receita vale a partir do projeto; se nao estiver la,
    // procura ao lado do proprio .sheet.json (folha gerada numa pasta solta).
    const std::string image = j.value("image", std::string());
    if (image.empty()) {
        error = jsonPath + ": \"image\" is missing (the sheet PNG)";
        return false;
    }
    std::error_code ec;
    const fs::path inProject = project::gameRef(image);
    out.image = fs::exists(inProject, ec) ? inProject.string()
                                          : (fs::path(jsonPath).parent_path() / fs::path(image).filename()).string();
    if (!fs::exists(out.image, ec)) {
        error = jsonPath + ": sheet image not found: " + image;
        return false;
    }

    out.columns = j.value("columns", out.columns);
    out.rows = j.value("rows", out.rows);
    out.directions = j.value("directions", out.directions);
    out.mirrorMissing = j.value("mirrorMissing", out.mirrorMissing);
    out.worldHeight = j.value("worldHeight", out.worldHeight);
    out.footOffset = j.value("footOffset", out.footOffset);
    if (j.contains("rowDirection") && j["rowDirection"].is_array()) {
        out.rowDirection.clear();
        for (const auto& d : j["rowDirection"]) out.rowDirection.push_back(d.get<int>());
    }
    readAnim(j.value("idle", nlohmann::json::object()), out.idle);
    readAnim(j.value("walk", nlohmann::json::object()), out.walk);
    if (j.contains("extra") && j["extra"].is_array()) {
        for (const auto& e : j["extra"]) {
            if (!e.is_object()) continue;
            SpriteSheetSpec::NamedAnim n;
            n.name = e.value("name", std::string());
            if (n.name.empty()) continue;
            n.anim = out.idle; // herda o atraso/linha do parado e sobrescreve
            readAnim(e, n.anim);
            n.loop = e.value("loop", false);
            out.extra.push_back(std::move(n));
        }
    }

    if (out.columns < 1 || out.rows < 1) {
        error = jsonPath + ": columns and rows must be 1 or more";
        return false;
    }
    if (out.directions != 1 && out.directions != 4 && out.directions != 8) {
        error = jsonPath + ": \"directions\" must be 1, 4 or 8";
        return false;
    }
    if (static_cast<int>(out.rowDirection.size()) < out.directions) {
        error = jsonPath + ": \"rowDirection\" needs one entry per direction";
        return false;
    }
    return true;
}

bool importSpriteSheetRecipe(const std::string& jsonPath, std::string& error, std::string* writtenPath) {
    SpriteSheetSpec spec;
    if (!loadSpriteSheetRecipe(jsonPath, spec, error)) return false;
    SpriteSheetResult sheet;
    if (!buildSpriteSheet(spec, sheet, error)) return false;
    const std::string out = std::filesystem::path(spec.image).replace_extension(".spr").string();
    if (!writeEruptSprV2(sheet, out, error)) return false;
    if (writtenPath) *writtenPath = out;
    ERUPTION_LOG_WARN("[SPRITE] %s: %zu quadros, %zu acoes (%d x %d por quadro) -> %s", jsonPath.c_str(),
                      sheet.images.size(), sheet.actions.size(), sheet.cellW, sheet.cellH, out.c_str());
    return true;
}

} // namespace eruption
