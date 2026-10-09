// File > New Map: cria assets/external/<nome>/<nome>.glb com um chão plano
// e um .env com a direção do sol, e abre o mapa novo.
#include "editor/Editor.hpp"

#include "core/Engine.hpp"
#include "formats/TerrainParser.hpp"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>
#include <fstream>

namespace eruption {

namespace {

namespace fs = std::filesystem;

template <typename T>
void appendBytes(std::vector<uint8_t>& out, const std::vector<T>& v) {
    const auto* p = reinterpret_cast<const uint8_t*>(v.data());
    out.insert(out.end(), p, p + v.size() * sizeof(T));
}

// Passo da grade do chão: o mesmo do mapa de exemplo, e o .ter tem um ponto
// por vértice (o pincel de terreno edita os dois juntos).
constexpr float kGridStep = 8.0f;

// Chão de size x size unidades, de (0,0) a (size,size) no plano XZ.
bool writeFlatGroundGlb(const fs::path& file, float size, const Vec3& color, std::string& error) {
    const int cells = std::max(1, static_cast<int>(size / kGridStep));
    const int side = cells + 1;
    std::vector<float> pos, nrm, uv;
    pos.reserve(side * side * 3);
    for (int z = 0; z < side; ++z)
        for (int x = 0; x < side; ++x) {
            pos.insert(pos.end(), {x * kGridStep, 0.0f, z * kGridStep});
            nrm.insert(nrm.end(), {0.0f, 1.0f, 0.0f});
            uv.insert(uv.end(), {x * kGridStep / 32.0f, z * kGridStep / 32.0f});
        }
    std::vector<uint32_t> idx;
    idx.reserve(cells * cells * 6);
    for (int z = 0; z < cells; ++z)
        for (int x = 0; x < cells; ++x) {
            const uint32_t a = z * side + x, b = a + 1, c = a + side, d = c + 1;
            // Anti-horário visto de cima (+Y).
            idx.insert(idx.end(), {a, c, b, b, c, d});
        }

    std::vector<uint8_t> bin;
    const size_t offPos = bin.size();
    appendBytes(bin, pos);
    const size_t offNrm = bin.size();
    appendBytes(bin, nrm);
    const size_t offUv = bin.size();
    appendBytes(bin, uv);
    const size_t offIdx = bin.size();
    appendBytes(bin, idx);
    while (bin.size() % 4) bin.push_back(0);

    using nlohmann::json;
    const int nVerts = side * side;
    json j;
    j["asset"] = {{"version", "2.0"}, {"generator", "Eruption Editor"}};
    j["scene"] = 0;
    j["scenes"] = json::array({{{"nodes", {0}}}});
    j["nodes"] = json::array({{{"name", "Ground"}, {"mesh", 0}}});
    j["meshes"] = json::array({{{"name", "Ground"},
                                {"primitives", json::array({{{"attributes", {{"POSITION", 0}, {"NORMAL", 1}, {"TEXCOORD_0", 2}}},
                                                             {"indices", 3},
                                                             {"material", 0}}})}}});
    j["materials"] = json::array({{{"name", "eruption_ground"},
                                   {"pbrMetallicRoughness",
                                    {{"baseColorFactor", {color.r, color.g, color.b, 1.0f}},
                                     {"metallicFactor", 0.0f},
                                     {"roughnessFactor", 0.9f}}}}});
    j["buffers"] = json::array({{{"byteLength", bin.size()}}});
    j["bufferViews"] = json::array({
        {{"buffer", 0}, {"byteOffset", offPos}, {"byteLength", pos.size() * 4}, {"target", 34962}},
        {{"buffer", 0}, {"byteOffset", offNrm}, {"byteLength", nrm.size() * 4}, {"target", 34962}},
        {{"buffer", 0}, {"byteOffset", offUv}, {"byteLength", uv.size() * 4}, {"target", 34962}},
        {{"buffer", 0}, {"byteOffset", offIdx}, {"byteLength", idx.size() * 4}, {"target", 34963}},
    });
    j["accessors"] = json::array({
        {{"bufferView", 0}, {"componentType", 5126}, {"count", nVerts}, {"type", "VEC3"},
         {"min", {0.0f, 0.0f, 0.0f}}, {"max", {cells * kGridStep, 0.0f, cells * kGridStep}}},
        {{"bufferView", 1}, {"componentType", 5126}, {"count", nVerts}, {"type", "VEC3"}},
        {{"bufferView", 2}, {"componentType", 5126}, {"count", nVerts}, {"type", "VEC2"}},
        {{"bufferView", 3}, {"componentType", 5125}, {"count", idx.size()}, {"type", "SCALAR"}},
    });
    std::string text = j.dump();
    while (text.size() % 4) text.push_back(' ');

    auto u32 = [](std::vector<uint8_t>& out, uint32_t v) {
        for (int i = 0; i < 4; ++i) out.push_back(uint8_t(v >> (8 * i)));
    };
    std::vector<uint8_t> glb;
    const uint32_t total = 12 + 8 + uint32_t(text.size()) + 8 + uint32_t(bin.size());
    u32(glb, 0x46546C67); // "glTF"
    u32(glb, 2);
    u32(glb, total);
    u32(glb, uint32_t(text.size()));
    u32(glb, 0x4E4F534A); // "JSON"
    glb.insert(glb.end(), text.begin(), text.end());
    u32(glb, uint32_t(bin.size()));
    u32(glb, 0x004E4942); // "BIN\0"
    glb.insert(glb.end(), bin.begin(), bin.end());

    std::ofstream f(file, std::ios::binary);
    f.write(reinterpret_cast<const char*>(glb.data()), std::streamsize(glb.size()));
    if (!f) {
        error = "Could not write " + file.generic_string();
        return false;
    }
    return true;
}

bool validMapName(const char* s) {
    if (!*s) return false;
    for (const char* p = s; *p; ++p)
        if (!(std::isalnum(static_cast<unsigned char>(*p)) || *p == '_' || *p == '-')) return false;
    return true;
}

} // namespace

bool Editor::createFlatMap(const std::string& name, float size, std::string& error) {
    const fs::path dir = fs::path("assets/external") / name;
    std::error_code ec;
    if (fs::exists(dir / (name + ".glb"), ec)) {
        error = "A map named \"" + name + "\" already exists.";
        return false;
    }
    fs::create_directories(dir, ec);
    if (ec) {
        error = "Could not create " + dir.generic_string() + ": " + ec.message();
        return false;
    }
    if (!writeFlatGroundGlb(dir / (name + ".glb"), size, Vec3(0.30f, 0.34f, 0.27f), error)) return false;
    // Grade de altura (.ter) com um ponto por vértice do chão: altura do
    // jogador, raios do editor e o pincel de terreno.
    TerrainFile ter;
    ter.version = 2;
    ter.width = ter.height = static_cast<uint32_t>(std::max(1, static_cast<int>(size / kGridStep))) + 1;
    ter.scale = kGridStep;
    ter.cubes.assign(size_t(ter.width) * ter.height, TerrainCube{{0.0f, 0.0f, 0.0f, 0.0f}, -1, -1, -1});
    if (!TerrainParser::writeEruptGndBin(ter, (dir / (name + ".ter")).string())) {
        error = "Could not write " + (dir / (name + ".ter")).generic_string();
        return false;
    }
    // Sol a ~55 graus de altura; o resto do ambiente usa os padrões do motor.
    nlohmann::json env;
    env["light"] = {{"direction", {0.47f, -0.82f, 0.33f}}};
    std::ofstream(dir / (name + ".env")) << env.dump(1) << "\n";
    m_engine->populateMaps();
    return true;
}

void Editor::drawNewMapDialog() {
    if (m_newMapOpen) {
        ImGui::OpenPopup("New Map");
        m_newMapOpen = false;
        m_newMapError.clear();
    }
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("New Map", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

    static const char* kSizes[] = {"Small (500 x 500)", "Medium (1000 x 1000)", "Large (2000 x 2000)"};
    static const float kSizeValues[] = {500.0f, 1000.0f, 2000.0f};
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    const bool enter = ImGui::InputText("Name", m_newMapName, sizeof(m_newMapName),
                                        ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::Combo("Size", &m_newMapSize, kSizes, IM_ARRAYSIZE(kSizes));
    ImGui::TextDisabled("A flat ground. Letters, digits, _ and - in the name.");
    const bool ok = validMapName(m_newMapName);
    if (!m_newMapError.empty()) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", m_newMapError.c_str());

    ImGui::Separator();
    ImGui::BeginDisabled(!ok);
    const bool create = ImGui::Button("Create", ImVec2(120, 0)) || (enter && ok);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    if (create) {
        std::string error;
        if (createFlatMap(m_newMapName, kSizeValues[m_newMapSize], error)) {
            log(LogLevel::Info, std::string("Created map ") + m_newMapName);
            m_pendingMap = m_newMapName;
            m_newMapName[0] = '\0';
            ImGui::CloseCurrentPopup();
        } else {
            m_newMapError = error;
        }
    }
    ImGui::EndPopup();
}

} // namespace eruption
