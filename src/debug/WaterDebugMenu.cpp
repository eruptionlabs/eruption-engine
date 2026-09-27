#include "debug/WaterDebugMenu.hpp"
#include "core/Logger.hpp"
#include <imgui.h>
#include <ImGuizmo.h>
#include <glm/gtc/type_ptr.hpp>
#include <nlohmann/json.hpp>
#include <cfloat>
#include <cstdio>
#include <fstream>

namespace eruption {

using json = nlohmann::json;

static Vec3 readColor(const json& j, const char* key, const Vec3& def) {
    if (!j.contains(key) || !j[key].is_array() || j[key].size() < 3) return def;
    return Vec3(j[key][0], j[key][1], j[key][2]);
}

static void writeColor(json& j, const char* key, const Vec3& c) {
    j[key] = { c.r, c.g, c.b };
}

static constexpr int WATER_CONFIG_VERSION = 2;

void WaterDebugMenu::loadConfig() {
    std::ifstream file(CONFIG_PATH);
    if (!file.is_open()) {
        ERUPTION_LOG_INFO("Water config not found, using defaults");
        saveConfig();
        return;
    }
    try {
        json root;
        file >> root;

        if (!root.contains("water_settings") || !root["water_settings"].is_object()) {
            ERUPTION_LOG_WARN("Water config missing 'water_settings', rewriting defaults");
            saveConfig();
            return;
        }

        const json& j = root["water_settings"];
        int version = j.value("version", 0);
        if (version < WATER_CONFIG_VERSION) {
            ERUPTION_LOG_WARN("Water config version %d outdated (expected %d), rewriting defaults", version, WATER_CONFIG_VERSION);
            saveConfig();
            return;
        }

        config.enabled        = j.value("enabled", config.enabled);
        config.waterLevel     = j.value("waterLevel", config.waterLevel);

        if (j.contains("colors")) {
            const json& c = j["colors"];
            config.baseColorDeep    = readColor(c, "deep", config.baseColorDeep);
            config.baseColorShallow = readColor(c, "shallow", config.baseColorShallow);
        }

        if (j.contains("surface")) {
            const json& s = j["surface"];
            config.transparency       = s.value("transparency", config.transparency);
            config.refractionStrength = s.value("refractionStrength", config.refractionStrength);
            config.reflectivity       = s.value("reflectivity", config.reflectivity);
            config.roughness          = s.value("roughness", config.roughness);
        }

        if (j.contains("normal")) {
            const json& n = j["normal"];
            config.normalScale    = n.value("scale", config.normalScale);
            config.normalStrength = n.value("strength", config.normalStrength);
            // Migration: old normal.speed is now unified with waves.speed
            if (n.contains("speed")) {
                config.waveSpeed = n.value("speed", config.waveSpeed);
            }
        }

        if (j.contains("waves")) {
            const json& w = j["waves"];
            config.waveAmplitude = w.value("amplitude", config.waveAmplitude);
            config.waveFrequency = w.value("frequency", config.waveFrequency);
            config.waveSpeed     = w.value("speed", config.waveSpeed);
            if (w.contains("waveDir0") && w["waveDir0"].is_array() && w["waveDir0"].size() >= 2)
                config.waveDirections[0] = Vec2(w["waveDir0"][0], w["waveDir0"][1]);
            if (w.contains("waveDir1") && w["waveDir1"].is_array() && w["waveDir1"].size() >= 2)
                config.waveDirections[1] = Vec2(w["waveDir1"][0], w["waveDir1"][1]);
            if (w.contains("waveDir2") && w["waveDir2"].is_array() && w["waveDir2"].size() >= 2)
                config.waveDirections[2] = Vec2(w["waveDir2"][0], w["waveDir2"][1]);
            config.waveSteepness[0] = w.value("steepness0", config.waveSteepness[0]);
            config.waveSteepness[1] = w.value("steepness1", config.waveSteepness[1]);
            config.waveSteepness[2] = w.value("steepness2", config.waveSteepness[2]);
        }

        config.enableCaustics = j.value("enableCaustics", config.enableCaustics);
        if (j.contains("caustics")) {
            const json& c = j["caustics"];
            config.causticsIntensity = c.value("intensity", config.causticsIntensity);
            config.causticsDepthAttenuation = c.value("depthAttenuation", config.causticsDepthAttenuation);
        }

        if (j.contains("surfaceFoam")) {
            const json& sf = j["surfaceFoam"];
            config.enableSurfaceFoam = sf.value("enable", config.enableSurfaceFoam);
            config.foamScale      = sf.value("scale", config.foamScale);
            config.foamSpeed      = sf.value("speed", config.foamSpeed);
            config.foamRoughness  = sf.value("roughness", config.foamRoughness);
        }

        if (j.contains("normal")) {
            const json& n = j["normal"];
            config.normalOctaves = n.value("octaves", config.normalOctaves);
        }

        if (j.contains("foam")) {
            const json& f = j["foam"];
            config.enableFoam          = f.value("enable", config.enableFoam);
            config.foamEdgeDepth       = f.value("edgeDepth", config.foamEdgeDepth);
            config.contactFoamStrength = f.value("contactStrength", config.contactFoamStrength);
            config.contactFoamDecay    = f.value("decay", config.contactFoamDecay);
            config.contactFoamSpread   = f.value("spread", config.contactFoamSpread);
            if (f.contains("maskScale") && f["maskScale"].is_array() && f["maskScale"].size() >= 2) {
                config.foamMaskScale.x = f["maskScale"][0];
                config.foamMaskScale.y = f["maskScale"][1];
            }
        }

        if (j.contains("texture")) {
            const json& t = j["texture"];
            config.useProceduralTexture = t.value("useProcedural", config.useProceduralTexture);
            config.waterTexturePath     = t.value("path", config.waterTexturePath);
        }

        config.forceWater = j.value("forceWater", config.forceWater);

        // If colors are the old cartoon blue, reset them
        Vec3 oldDeep(0.06f, 0.18f, 0.38f);
        Vec3 oldShallow(0.18f, 0.55f, 0.65f);
        float eps = 0.01f;
        if (glm::distance(config.baseColorDeep, oldDeep) < eps &&
            glm::distance(config.baseColorShallow, oldShallow) < eps) {
            ERUPTION_LOG_INFO("Resetting old cartoon water colors to sky-matched defaults");
            config.baseColorDeep    = WaterRenderer::WaterSettings{}.baseColorDeep;
            config.baseColorShallow = WaterRenderer::WaterSettings{}.baseColorShallow;
            saveConfig();
        }

    } catch (const std::exception& e) {
        ERUPTION_LOG_WARN("Failed to parse water config: %s", e.what());
    }
}

void WaterDebugMenu::saveConfig() {
    json root;
    json& j = root["water_settings"];

    j["enabled"]    = config.enabled;
    j["waterLevel"] = config.waterLevel;

    writeColor(j["colors"], "deep", config.baseColorDeep);
    writeColor(j["colors"], "shallow", config.baseColorShallow);

    j["surface"] = {
        {"transparency", config.transparency},
        {"refractionStrength", config.refractionStrength},
        {"reflectivity", config.reflectivity},
        {"roughness", config.roughness}
    };

    j["normal"] = {
        {"scale", config.normalScale},
        {"strength", config.normalStrength}
    };

    j["waves"] = {
        {"amplitude", config.waveAmplitude},
        {"frequency", config.waveFrequency},
        {"speed", config.waveSpeed},
        {"waveDir0", {config.waveDirections[0].x, config.waveDirections[0].y}},
        {"waveDir1", {config.waveDirections[1].x, config.waveDirections[1].y}},
        {"waveDir2", {config.waveDirections[2].x, config.waveDirections[2].y}},
        {"steepness0", config.waveSteepness[0]},
        {"steepness1", config.waveSteepness[1]},
        {"steepness2", config.waveSteepness[2]}
    };

    j["enableCaustics"] = config.enableCaustics;

    j["caustics"] = {
        {"intensity", config.causticsIntensity},
        {"depthAttenuation", config.causticsDepthAttenuation}
    };

    j["surfaceFoam"] = {
        {"enable", config.enableSurfaceFoam},
        {"scale", config.foamScale},
        {"speed", config.foamSpeed},
        {"roughness", config.foamRoughness}
    };

    j["normal"]["octaves"] = config.normalOctaves;

    j["foam"] = {
        {"enable", config.enableFoam},
        {"edgeDepth", config.foamEdgeDepth},
        {"contactStrength", config.contactFoamStrength},
        {"decay", config.contactFoamDecay},
        {"spread", config.contactFoamSpread},
        {"maskScale", {config.foamMaskScale.x, config.foamMaskScale.y}}
    };

    j["texture"] = {
        {"useProcedural", config.useProceduralTexture},
        {"path", config.waterTexturePath}
    };

    j["version"] = WATER_CONFIG_VERSION;
    j["forceWater"] = config.forceWater;

    std::ofstream file(CONFIG_PATH);
    if (file.is_open()) {
        file << root.dump(4);
    }
}

void WaterDebugMenu::ensureDefaultLiquid() {
    if (liquids.empty()) {
        LiquidEntry e;
        e.name = "Liquid 0";
        e.kind = config.liquidKind;
        e.level = config.waterLevel;
        liquids.push_back(e);
    }
    if (selectedLiquid < 0 || selectedLiquid >= static_cast<int>(liquids.size())) {
        selectedLiquid = 0;
    }
}

void WaterDebugMenu::applySelectedLiquid() {
    ensureDefaultLiquid();
    const LiquidEntry& e = liquids[static_cast<size_t>(selectedLiquid)];
    config.liquidKind = e.kind;
    // So' Water usa config.waterLevel (o Y do plano do mapa inteiro) - Lava
    // sempre leu a propria altura de m_lavaLiquid.level. Escrever aqui pra
    // Lava era o mesmo bug que a UI tinha (ver "Surface Level" acima):
    // selecionar/sincronizar a lava sujava o nivel de agua global em
    // silencio, sem nenhum clique do usuario - foi assim que
    // data/water_config.json pegou um waterLevel de 399 numa sessao de
    // teste que nunca tocou o slider de agua.
    if (e.kind == 0) {
        config.waterLevel = e.level;
    }
}

void WaterDebugMenu::drawUI(const Mat4& view, const Mat4& proj, float screenW, float screenH) {
    ensureDefaultLiquid();

    if (ImGui::CollapsingHeader("Liquids", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextDisabled("%zu liquid surface(s) in this map", liquids.size());
        if (ImGui::BeginListBox("##liquidlist", ImVec2(-FLT_MIN, 4 * ImGui::GetTextLineHeightWithSpacing()))) {
            for (int i = 0; i < static_cast<int>(liquids.size()); ++i) {
                char label[128];
                snprintf(label, sizeof(label), "%s  [%s]  y=%.1f",
                         liquids[static_cast<size_t>(i)].name.c_str(),
                         WaterRenderer::liquidKindName(liquids[static_cast<size_t>(i)].kind),
                         liquids[static_cast<size_t>(i)].level);
                if (ImGui::Selectable(label, selectedLiquid == i)) {
                    selectedLiquid = i;
                    applySelectedLiquid();
                    settingsChanged = true;
                }
            }
            ImGui::EndListBox();
        }

        LiquidEntry& sel = liquids[static_cast<size_t>(selectedLiquid)];
        const char* kindNames[WaterRenderer::kLiquidKindCount] = { "Water", "Lava" };
        if (ImGui::Combo("Type", &sel.kind, kindNames, WaterRenderer::kLiquidKindCount)) {
            config.liquidKind = sel.kind;
            settingsChanged = true;
            saveConfig();
        }
        // "Surface Level" so' faz sentido pra Water: ele escreve em
        // config.waterLevel, que e' o Y do PLANO DE AGUA DO MAPA INTEIRO
        // (usado ate' pro culling de visibilidade da agua, Engine.cpp
        // "waterYNow"). Antes esse controle aparecia pra Lava tambem e
        // escrevia no MESMO lugar - mexer nele achando que ia subir a lava
        // na verdade subia a agua do mapa inteiro, que entrava visualmente
        // na area da cratera ("restou o espaco da lava"). A altura de
        // verdade da lava e' Height Offset, na secao Shape abaixo - o
        // draw da lava sempre le m_lavaLiquid.level, nunca config.waterLevel.
        if (sel.kind == 0) {
            if (ImGui::DragFloat("Surface Level", &sel.level, 0.5f)) {
                config.waterLevel = sel.level;
                settingsChanged = true;
                saveConfig();
            }
        }
        if (sel.kind == 1) {
            if (ImGui::SliderFloat("Emissive", &config.emissiveStrength, 0.0f, 8.0f, "%.2f")) {
                settingsChanged = true;
                saveConfig();
            }
            if (ImGui::SliderFloat("Flow Speed", &config.flowSpeed, 0.0f, 4.0f, "%.2f")) {
                settingsChanged = true;
                saveConfig();
            }

            // Posicao/rotacao do disco. So' Lava tem forma propria pra mover
            // (a agua principal e' um plano derivado do terreno inteiro, sem
            // centro). Isso e' um NUDGE por cima do .env - nunca escreve no
            // arquivo, some ao recarregar o mapa. Pra tornar permanente,
            // ainda e' preciso levar os valores de volta pro pipeline Python.
            //
            // Tudo aqui e' TEXT BOX (InputFloat), nao slider - pedido
            // explicito do autor. Os rotulos de X/Z/Y saem coloridos na
            // convencao RGB=XYZ dos editores 3D (X vermelho, Y verde, Z
            // azul), so' pra ajudar a associar qual campo mexe em qual eixo;
            // nao ha' gizmo nenhum aqui, o "alinhar com mouse" e' o proprio
            // click-and-drag que o ImGui::InputFloat ja' da' de graca no
            // texto (arrasta pra cima/baixo com o botao direito, ou digita
            // o numero direto).
            ImGui::Separator();
            ImGui::TextDisabled("Shape (live nudge, not saved to disk)");

            const float AXIS_STEP = 1.0f;
            const float AXIS_STEP_FAST = 10.0f;
            ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.35f, 1.0f), "X");
            ImGui::SameLine();
            if (ImGui::InputFloat("Center Offset##offX", &sel.offsetX, AXIS_STEP, AXIS_STEP_FAST, "%.1f")) {
                liquidShapeDirty = true;
            }
            ImGui::TextColored(ImVec4(0.35f, 0.85f, 0.35f, 1.0f), "Y");
            ImGui::SameLine();
            if (ImGui::InputFloat("Height Offset##offY", &sel.offsetY, AXIS_STEP, AXIS_STEP_FAST, "%.1f")) {
                liquidShapeDirty = true;
            }
            ImGui::TextColored(ImVec4(0.35f, 0.55f, 0.95f, 1.0f), "Z");
            ImGui::SameLine();
            if (ImGui::InputFloat("Center Offset##offZ", &sel.offsetZ, AXIS_STEP, AXIS_STEP_FAST, "%.1f")) {
                liquidShapeDirty = true;
            }
            // Sem clamp/wrap de proposito: 400 graus e 40 graus dao o
            // mesmo seno/cosseno no shader, entao nao ha' necessidade
            // funcional de normalizar - so' adicionaria complexidade.
            if (ImGui::InputFloat("Rotation (deg)", &sel.rotationDeg, 1.0f, 15.0f, "%.0f")) {
                liquidShapeDirty = true;
            }
            ImGui::SetItemTooltip("Spins the shore-radius profile (the lobed caldera outline) "
                                   "around the center. Position offsets move the whole disc.");

            // Nudge por teclado: setas movem no plano XZ, Ctrl+cima/baixo
            // move em Y. So' ativa com esta janela em foco e nenhum campo
            // de texto sendo editado no momento (senao roubaria as setas de
            // quem esta' digitando um numero ou navegando o cursor dentro
            // do proprio InputFloat).
            ImGui::TextDisabled("Arrows: move X/Z    Ctrl+Up/Down: move Y");
            if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                !ImGui::IsAnyItemActive()) {
                const float dt = ImGui::GetIO().DeltaTime;
                const float speed = ImGui::GetIO().KeyShift ? 120.0f : 30.0f; // u/s
                const float step = speed * dt;
                bool moved = false;
                if (ImGui::GetIO().KeyCtrl) {
                    if (ImGui::IsKeyDown(ImGuiKey_UpArrow))   { sel.offsetY += step; moved = true; }
                    if (ImGui::IsKeyDown(ImGuiKey_DownArrow)) { sel.offsetY -= step; moved = true; }
                } else {
                    // Up/Down andam em Z, Left/Right em X - mesma convencao
                    // de "cima do mapa" usada nos prints em pitch 89 desta
                    // sessao (Z cresce pra "baixo" na tela olhando de cima).
                    if (ImGui::IsKeyDown(ImGuiKey_UpArrow))    { sel.offsetZ -= step; moved = true; }
                    if (ImGui::IsKeyDown(ImGuiKey_DownArrow))  { sel.offsetZ += step; moved = true; }
                    if (ImGui::IsKeyDown(ImGuiKey_LeftArrow))  { sel.offsetX -= step; moved = true; }
                    if (ImGui::IsKeyDown(ImGuiKey_RightArrow)) { sel.offsetX += step; moved = true; }
                }
                if (moved) liquidShapeDirty = true;
            }

            if (ImGui::Button("Reset Shape")) {
                sel.offsetX = 0.0f;
                sel.offsetZ = 0.0f;
                sel.offsetY = 0.0f;
                sel.rotationDeg = 0.0f;
                liquidShapeDirty = true;
            }

            // Gizmo de viewport (setinhas tipo Unity/Blender/Unreal) - pedido
            // explicito do autor, por cima do InputFloat acima (os dois
            // convivem, o gizmo so' e' mais rapido pra ajustes grandes).
            // So' translacao e rotacao fazem sentido pro disco de lava (nao
            // ha' escala). Base do gizmo = valor autorado no .env (center*/
            // level, sincronizados em Engine::syncLiquidMenuFromMap) + o
            // nudge atual (offset*/rotationDeg) - igual a conta que
            // Engine::setupLavaLiquid faz pra construir a malha de verdade.
            ImGui::Separator();
            static ImGuizmo::OPERATION s_lavaGizmoOp = ImGuizmo::TRANSLATE;
            if (ImGui::RadioButton("Move##lavaGizmo", s_lavaGizmoOp == ImGuizmo::TRANSLATE)) s_lavaGizmoOp = ImGuizmo::TRANSLATE;
            ImGui::SameLine();
            if (ImGui::RadioButton("Rotate##lavaGizmo", s_lavaGizmoOp == ImGuizmo::ROTATE)) s_lavaGizmoOp = ImGuizmo::ROTATE;

            float gizT[3] = { sel.centerX + sel.offsetX, sel.level + sel.offsetY, sel.centerZ + sel.offsetZ };
            float gizR[3] = { 0.0f, sel.rotationDeg, 0.0f };
            float gizS[3] = { 1.0f, 1.0f, 1.0f };
            float gizmoMatrix[16];
            ImGuizmo::RecomposeMatrixFromComponents(gizT, gizR, gizS, gizmoMatrix);

            // Esta versao vendorizada do ImGuizmo (a mesma do exemplo do
            // tinygltf) tem Manipulate() retornando void, nao bool - usa-se
            // IsUsing() (true enquanto o mouse esta' arrastando o gizmo) pra
            // saber se algo mudou, em vez do valor de retorno de versoes
            // mais novas da lib.
            // Camera::rebuild() faz proj[1][1] *= -1 (convencao de NDC do
            // Vulkan, Y pra baixo) - o ImGuizmo foi escrito pra projecao
            // estilo OpenGL (Y pra cima) e nao sabe disso, entao sem desfazer
            // o flip aqui o gizmo desenha errado de um jeito que MUDA com o
            // angulo da camera (parece que as setinhas "andam" quando a
            // camera orbita, em vez de ficarem fixas no mundo). Motivo exato
            // do bug relatado pelo autor.
            Mat4 gizmoProj = proj;
            gizmoProj[1][1] *= -1.0f;

            ImGuizmo::SetRect(0, 0, screenW, screenH);
            ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(gizmoProj),
                                  s_lavaGizmoOp, ImGuizmo::WORLD, gizmoMatrix);
            if (ImGuizmo::IsUsing()) {
                float outT[3], outR[3], outS[3];
                ImGuizmo::DecomposeMatrixToComponents(gizmoMatrix, outT, outR, outS);
                sel.offsetX = outT[0] - sel.centerX;
                sel.offsetZ = outT[2] - sel.centerZ;
                sel.offsetY = outT[1] - sel.level;
                sel.rotationDeg = outR[1];
                liquidShapeDirty = true;
            }
        }

        if (ImGui::Button("Add")) {
            LiquidEntry e;
            char nm[64];
            snprintf(nm, sizeof(nm), "Liquid %zu", liquids.size());
            e.name = nm;
            e.kind = 0;
            e.level = config.waterLevel;
            liquids.push_back(e);
            selectedLiquid = static_cast<int>(liquids.size()) - 1;
            applySelectedLiquid();
            settingsChanged = true;
        }
        ImGui::SameLine();
        if (liquids.size() > 1 && ImGui::Button("Remove")) {
            liquids.erase(liquids.begin() + selectedLiquid);
            selectedLiquid = 0;
            applySelectedLiquid();
            settingsChanged = true;
        }
        ImGui::Separator();
    }

    // TODA esta secao (onda, normal map, espuma, optico - "Water Height
    // Offset" incluido) edita `config` direto, sem olhar qual liquido esta'
    // selecionado acima. lavaSettings so' sobrescreve 8 campos (level,
    // emissive, flowSpeed, liquidKind, e desliga foam/causticas/
    // transparencia) - TUDO daqui (amplitude/frequencia/velocidade de onda,
    // escala/forca do normal map, cores) e' herdado por copia direta pra
    // lava tambem. Com Lava selecionada, essa secao ficava visivel e
    // editavel mesmo assim - o autor reportou os dois sintomas exatos disso:
    // um controle com "Water" no nome aparecendo com Lava selecionada, e
    // mudar algo pensando que era so' da lava mudando a agua junto (porque
    // ERA o mesmo campo, sempre foi). Esconder e' mais honesto que deixar
    // visivel: hoje nao existe nenhum campo de onda/normal SO' da lava pra
    // mostrar aqui - ela usa exatamente os valores da agua ate' alguem
    // construir um conjunto independente.
    if (!liquids.empty() && liquids[static_cast<size_t>(selectedLiquid)].kind == 1) {
        ImGui::TextDisabled("Lava usa o nivel/emissivo/vazao dela (acima) - onda,");
        ImGui::TextDisabled("normal map e espuma ainda sao os da agua (ver Water,");
        ImGui::TextDisabled("selecione Water pra mexer nisso).");
        return;
    }

    if (!ImGui::CollapsingHeader("Surface Settings", ImGuiTreeNodeFlags_DefaultOpen))
        return;

    // Helpers with tooltips
    auto WSliderFloat = [&](const char* label, float* v, float min, float max, const char* fmt, const char* tip) {
        if (ImGui::SliderFloat(label, v, min, max, fmt)) { settingsChanged = true; saveConfig(); }
        if (tip) ImGui::SetItemTooltip("%s", tip);
    };
    auto WSliderInt = [&](const char* label, int* v, int min, int max, const char* tip) {
        if (ImGui::SliderInt(label, v, min, max)) { settingsChanged = true; saveConfig(); }
        if (tip) ImGui::SetItemTooltip("%s", tip);
    };
    auto WCheckbox = [&](const char* label, bool* v, const char* tip) {
        if (ImGui::Checkbox(label, v)) { settingsChanged = true; saveConfig(); }
        if (tip) ImGui::SetItemTooltip("%s", tip);
    };
    auto WColorEdit3 = [&](const char* label, float* v, const char* tip) {
        if (ImGui::ColorEdit3(label, v)) { settingsChanged = true; saveConfig(); }
        if (tip) ImGui::SetItemTooltip("%s", tip);
    };

    WCheckbox("Enable Water", &config.enabled, "Master toggle for water rendering.");
    if (!config.enabled) {
        ImGui::TextDisabled("Water rendering disabled.");
        return;
    }

    WCheckbox("Force Water (all tiles)", &config.forceWater, "Render water on every tile, ignoring map water data.");
    if (config.forceWater) {
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "WARNING: Water on every tile!");
    }

    // ===================== LAYER 1: WAVE BODY (Gerstner) =====================
    ImGui::Separator();
    if (ImGui::TreeNodeEx("LAYER 1: Wave Body (Gerstner)", ImGuiTreeNodeFlags_DefaultOpen)) {
        WSliderFloat("Amplitude", &config.waveAmplitude, 0.0f, 2.0f, "%.2f",
            "Height of the Gerstner waves. Higher = bigger swell.");
        WSliderFloat("Frequency", &config.waveFrequency, 0.1f, 5.0f, "%.2f",
            "How tightly packed the waves are. Higher = more wave crests per unit.");
        WSliderFloat("Speed", &config.waveSpeed, 0.0f, 3.0f, "%.2f",
            "Animation speed of waves and normal map scroll (unified).");
        WSliderFloat("Water Height Offset", &config.waterLevel, -5.0f, 5.0f, "%.2f",
            "Vertical offset added to the map's base water level.");
        for (int i = 0; i < 3; i++) {
            float angle = atan2(config.waveDirections[i].y, config.waveDirections[i].x) * 57.2958f;
            char label[32];
            snprintf(label, sizeof(label), "Wave %d Angle", i + 1);
            if (ImGui::SliderFloat(label, &angle, -180.0f, 180.0f, "%.0f deg")) {
                float rad = angle * 0.0174533f;
                config.waveDirections[i] = Vec2(cos(rad), sin(rad));
                settingsChanged = true; saveConfig();
            }
            ImGui::SetItemTooltip("Direction the wave travels, in degrees.");
            snprintf(label, sizeof(label), "Wave %d Steepness", i + 1);
            if (ImGui::SliderFloat(label, &config.waveSteepness[i], 0.0f, 1.0f, "%.2f")) {
                settingsChanged = true; saveConfig();
            }
            ImGui::SetItemTooltip("How sharply the wave peaks. 0 = round sine, 1 = sharp crest.");
        }
        ImGui::TreePop();
    }

    // ===================== LAYER 2: SURFACE DETAIL (Normal) =====================
    ImGui::Separator();
    if (ImGui::TreeNodeEx("LAYER 2: Surface Detail (Normal Map)", ImGuiTreeNodeFlags_DefaultOpen)) {
        WCheckbox("Use Procedural Normal", &config.useProceduralTexture,
            "ON: generate normal map from multi-octave Simplex noise in shader.\nOFF: use loaded texture.");
        WSliderFloat("Normal Scale", &config.normalScale, 0.01f, 5.0f, "%.2f",
            "Zoom level of the normal map. Higher = smaller, more detailed ripples.");
        WSliderFloat("Normal Strength", &config.normalStrength, 0.0f, 2.0f, "%.2f",
            "How much the normal perturbs the surface. 0 = flat mirror, 2 = very choppy.");
        WSliderInt("Normal Octaves", &config.normalOctaves, 1, 4,
            "Number of noise layers blended. More octaves = large swell + fine detail.");
        if (!config.useProceduralTexture) {
            static char texPath[256] = "";
            ImGui::InputText("Path", texPath, sizeof(texPath));
            ImGui::SameLine();
            if (ImGui::Button("Load")) {
                config.waterTexturePath = texPath;
                requestLoadTexture = true;
                settingsChanged = true; saveConfig();
            }
            ImGui::SetItemTooltip("Load an external PNG/JPG as the water normal map.");
            if (ImGui::Button("Generate Blue Noise")) {
                requestGenerateBlueNoise = true;
                config.useProceduralTexture = false;
                settingsChanged = true; saveConfig();
            }
            ImGui::SetItemTooltip("Generate a procedural blue noise texture for the normal map.");
        }
        ImGui::TreePop();
    }

    // ===================== LAYER 3: FOAM SYSTEM =====================
    ImGui::Separator();
    if (ImGui::TreeNodeEx("LAYER 3: Foam System", ImGuiTreeNodeFlags_DefaultOpen)) {
        WCheckbox("Enable Surface Foam", &config.enableSurfaceFoam,
            "Procedural foam on wave crests using Worley/Voronoi noise.");
        if (config.enableSurfaceFoam) {
            WSliderFloat("Foam Scale", &config.foamScale, 0.1f, 5.0f, "%.2f",
                "Size of foam cells. Higher = smaller, more dense foam patches.");
            WSliderFloat("Foam Speed", &config.foamSpeed, 0.0f, 2.0f, "%.2f",
                "How fast the foam pattern drifts across the surface.");
            WSliderFloat("Foam Roughness", &config.foamRoughness, 0.0f, 1.0f, "%.2f",
                "Roughness of the foam itself. 0 = wet/shiny foam, 1 = dry/matte foam.");
        }
        WCheckbox("Enable Edge/Contact Foam", &config.enableFoam,
            "Foam at shoreline edges and where objects touch the water.");
        if (config.enableFoam) {
            WSliderFloat("Edge Depth", &config.foamEdgeDepth, 0.0f, 2.0f, "%.2f",
                "How far from the shoreline the edge foam extends.");
            WSliderFloat("Contact Strength", &config.contactFoamStrength, 0.0f, 4.0f, "%.2f",
                "Intensity of foam created by objects interacting with water.");
            WSliderFloat("Contact Spread", &config.contactFoamSpread, 0.0f, 1.0f, "%.2f",
                "How much the contact foam spreads outward from the collision point.");
            WSliderFloat("Contact Decay", &config.contactFoamDecay, 0.0f, 1.0f, "%.3f",
                "How fast contact foam fades away over time.");
        }
        ImGui::TreePop();
    }

    // ===================== LAYER 4: OPTICAL PROPERTIES =====================
    ImGui::Separator();
    if (ImGui::TreeNodeEx("LAYER 4: Optical Properties", ImGuiTreeNodeFlags_DefaultOpen)) {
        WColorEdit3("Deep Color", &config.baseColorDeep.x,
            "Color of deep water. Seen where the floor is far below.");
        WColorEdit3("Shallow Color", &config.baseColorShallow.x,
            "Color of shallow water. Seen near shorelines and in puddles.");
        WSliderFloat("Transparency", &config.transparency, 0.0f, 1.0f, "%.2f",
            "0 = opaque water (solid color), 1 = fully see-through (refraction only).");
        WSliderFloat("Refraction", &config.refractionStrength, 0.0f, 0.1f, "%.3f",
            "How much the underwater background is distorted by wave ripples.");
        WSliderFloat("Reflectivity", &config.reflectivity, 0.0f, 1.0f, "%.2f",
            "Strength of sky reflection. 0 = no reflection, 1 = mirror-like at glancing angles.");
        WSliderFloat("Roughness", &config.roughness, 0.0f, 1.0f, "%.2f",
            "Surface roughness of the water itself. 0 = mirror, 1 = diffuse.");
        ImGui::TreePop();
    }

    // ===================== LAYER 5: LIGHTING EFFECTS =====================
    ImGui::Separator();
    if (ImGui::TreeNodeEx("LAYER 5: Lighting Effects", ImGuiTreeNodeFlags_DefaultOpen)) {
        WCheckbox("Enable Caustics", &config.enableCaustics,
            "Animated light patterns projected onto the underwater floor.");
        if (config.enableCaustics) {
            WSliderFloat("Caustics Intensity", &config.causticsIntensity, 0.0f, 1.0f, "%.2f",
                "Brightness multiplier of the caustics light filaments.");
            WSliderFloat("Depth Attenuation", &config.causticsDepthAttenuation, 0.05f, 1.0f, "%.2f",
                "How fast caustics fade with depth. Low = penetrates deep, High = fades quickly.");
        }
        ImGui::TreePop();
    }

    // ===================== ACTIONS =====================
    ImGui::Separator();
    if (ImGui::Button("Reload Water Shaders")) {
        requestReloadShaders = true;
    }
    ImGui::SetItemTooltip("Hot-reload liquid.vert and liquid.frag without restarting the game.");
}

} // namespace eruption
