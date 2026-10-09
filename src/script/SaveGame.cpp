// Progresso do jogo: game.save / game.load. Um JSON por espaço ("1", "auto"...)
// na pasta de dados do usuário, com o mapa, o jogador, as variáveis dos
// scripts, a hora, o clima, a música e o estado de cada mapa visitado (o que
// os scripts mudaram nos objetos e personagens). Valores das variáveis:
// número, texto, verdadeiro/falso, vector e tabelas deles.
#include "script/ScriptHost.hpp"
#include "script/ScriptApi.hpp"

#include "core/Engine.hpp"
#include "core/Logger.hpp"
#include "game/PlayerController.hpp"
#include "renderer/ModelRenderer.hpp"
#include "renderer/WeatherTypes.hpp"

#include <lua.h>
#include <lualib.h>
#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdlib>
#include <fstream>

namespace eruption {

namespace {

namespace fs = std::filesystem;
using nlohmann::json;

json toJson(lua_State* L, int idx, int depth) {
    if (depth > 16) return nullptr;
    idx = lua_absindex(L, idx);
    switch (lua_type(L, idx)) {
        case LUA_TBOOLEAN: return lua_toboolean(L, idx) != 0;
        case LUA_TNUMBER: return lua_tonumber(L, idx);
        case LUA_TSTRING: return std::string(lua_tostring(L, idx));
        case LUA_TVECTOR: {
            const float* v = lua_tovector(L, idx);
            return json{{"$vec", {v[0], v[1], v[2]}}};
        }
        case LUA_TTABLE: {
            // Lista (1..n sem buracos) vira array; o resto, objeto com chave texto.
            const int n = lua_objlen(L, idx);
            int keys = 0;
            lua_pushnil(L);
            while (lua_next(L, idx)) {
                ++keys;
                lua_pop(L, 1);
            }
            if (n > 0 && keys == n) {
                json a = json::array();
                for (int i = 1; i <= n; ++i) {
                    lua_rawgeti(L, idx, i);
                    a.push_back(toJson(L, -1, depth + 1));
                    lua_pop(L, 1);
                }
                return a;
            }
            json o = json::object();
            lua_pushnil(L);
            while (lua_next(L, idx)) {
                if (lua_type(L, -2) == LUA_TSTRING) o[lua_tostring(L, -2)] = toJson(L, -1, depth + 1);
                else if (lua_type(L, -2) == LUA_TNUMBER) o["#" + std::to_string(lua_tonumber(L, -2))] = toJson(L, -1, depth + 1);
                lua_pop(L, 1);
            }
            return o;
        }
        default: return nullptr; // função, nil: não se salva
    }
}

void pushJson(lua_State* L, const json& j, int depth) {
    if (depth > 16) {
        lua_pushnil(L);
        return;
    }
    if (j.is_boolean()) lua_pushboolean(L, j.get<bool>());
    else if (j.is_number()) lua_pushnumber(L, j.get<double>());
    else if (j.is_string()) lua_pushstring(L, j.get<std::string>().c_str());
    else if (j.is_array()) {
        lua_createtable(L, int(j.size()), 0);
        for (size_t i = 0; i < j.size(); ++i) {
            pushJson(L, j[i], depth + 1);
            lua_rawseti(L, -2, int(i + 1));
        }
    } else if (j.is_object()) {
        if (j.contains("$vec") && j["$vec"].is_array() && j["$vec"].size() == 3) {
            lua_pushvector(L, j["$vec"][0].get<float>(), j["$vec"][1].get<float>(), j["$vec"][2].get<float>());
            return;
        }
        lua_createtable(L, 0, int(j.size()));
        for (auto it = j.begin(); it != j.end(); ++it) {
            const std::string& k = it.key();
            if (!k.empty() && k[0] == '#') lua_pushnumber(L, std::atof(k.c_str() + 1));
            else lua_pushstring(L, k.c_str());
            pushJson(L, it.value(), depth + 1);
            lua_settable(L, -3);
        }
    } else {
        lua_pushnil(L);
    }
}

fs::path slotPath(const std::string& slot) {
    std::string safe;
    for (char c : slot.empty() ? std::string("1") : slot)
        safe += (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-') ? c : '_';
    return ScriptHost::saveDir() / (safe + ".json");
}

} // namespace

fs::path ScriptHost::saveDir() {
    // ERUPTION_SAVE_DIR (testes) ou a pasta de dados do usuário, por jogo.
    if (const char* d = std::getenv("ERUPTION_SAVE_DIR"); d && *d) return d;
    const std::string game = fs::current_path().filename().string();
#ifdef _WIN32
    if (const char* a = std::getenv("APPDATA"); a && *a) return fs::path(a) / game / "saves";
#else
    if (const char* x = std::getenv("XDG_DATA_HOME"); x && *x) return fs::path(x) / game / "saves";
    if (const char* h = std::getenv("HOME"); h && *h) return fs::path(h) / ".local" / "share" / game / "saves";
#endif
    return fs::path("saves");
}

bool ScriptHost::hasSave(const std::string& slot) {
    std::error_code ec;
    return fs::exists(slotPath(slot), ec);
}

bool ScriptHost::deleteSave(const std::string& slot) {
    std::error_code ec;
    return fs::remove(slotPath(slot), ec);
}

std::string ScriptHost::exportVars() {
    if (!L || m_varsRef < 0) return "{}";
    lua_getref(L, m_varsRef);
    const json j = toJson(L, -1, 0);
    lua_pop(L, 1);
    return j.is_null() ? "{}" : j.dump();
}

bool ScriptHost::importVars(const std::string& text) {
    const json j = json::parse(text, nullptr, false);
    if (!L || !j.is_object()) return false;
    pushJson(L, j, 0);
    if (m_varsRef >= 0) lua_unref(L, m_varsRef);
    m_varsRef = lua_ref(L, -1);
    lua_pop(L, 1);
    return true;
}

bool ScriptHost::saveGame(const std::string& slot, std::string& error) {
    json j;
    j["version"] = 1;
    j["map"] = m_engine->currentMapName();
    if (PlayerController* p = m_engine->m_playerController) {
        const Vec3 pos = p->pos();
        j["player"] = {pos.x, pos.y, pos.z};
    }
    j["vars"] = json::parse(exportVars(), nullptr, false);
    rememberMapState();
    json maps = json::object();
    for (const auto& [name, state] : m_mapStates) maps[name] = json::parse(state, nullptr, false);
    j["maps"] = maps;
    j["time"] = m_engine->dayNightCycle().timeOfDay() * 24.0f;
    j["weather"] = weatherTypeName(m_engine->weatherSystem().currentType());
    if (!m_engine->audio().currentMusic().empty()) j["music"] = m_engine->audio().currentMusic();
    const fs::path path = slotPath(slot);
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    // Grava ao lado e troca: um jogo salvo nunca fica pela metade.
    const fs::path tmp = fs::path(path).concat(".tmp");
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f << j.dump(1) << "\n";
        if (!f) {
            error = "could not write " + tmp.string();
            return false;
        }
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        error = "could not write " + path.string() + ": " + ec.message();
        return false;
    }
    ERUPTION_LOG_INFO("Game saved: %s", path.string().c_str());
    return true;
}

bool ScriptHost::loadGame(const std::string& slot, std::string& error) {
    std::ifstream f(slotPath(slot), std::ios::binary);
    if (!f) {
        error = "no saved game in slot \"" + slot + "\"";
        return false;
    }
    const json j = json::parse(f, nullptr, false);
    if (!j.is_object()) {
        error = "saved game in slot \"" + slot + "\" is damaged";
        return false;
    }
    if (j.contains("vars")) importVars(j["vars"].dump());
    m_mapStates.clear();
    if (j.contains("maps") && j["maps"].is_object())
        for (auto it = j["maps"].begin(); it != j["maps"].end(); ++it) m_mapStates[it.key()] = it.value().dump();
    if (j.contains("time") && j["time"].is_number())
        m_engine->dayNightCycle().setTimeOfDay(j["time"].get<float>() / 24.0f);
    if (j.contains("weather") && j["weather"].is_string())
        m_engine->applyWeatherTypeFull(weatherTypeFromName(j["weather"].get<std::string>().c_str()), 1.0f);
    if (j.contains("music") && j["music"].is_string()) m_engine->audio().music(j["music"].get<std::string>());
    Vec3 pos(0.0f);
    const bool hasPos = j.contains("player") && j["player"].is_array() && j["player"].size() == 3;
    if (hasPos) pos = Vec3(j["player"][0].get<float>(), j["player"][1].get<float>(), j["player"][2].get<float>());
    const std::string map = j.value("map", std::string());
    if (!map.empty() && map != m_engine->currentMapName()) {
        m_engine->requestMap(map, hasPos ? &pos : nullptr); // o jogador vai para lá depois da troca
    } else {
        // Mesmo mapa: as coisas voltam à foto do jogo salvo (ou ao começo do mapa).
        const auto it = m_mapStates.find(m_baselineMap);
        applyMapState(it != m_mapStates.end() ? it->second : std::string("{}"));
        if (hasPos && m_engine->m_playerController) m_engine->m_playerController->setPos(pos);
    }
    return true;
}

void ScriptHost::mapReady() {
    // Foto do mapa como a cena o deixou (antes dos scripts mexerem).
    const std::string map = m_engine->currentMapName();
    if (map.empty()) return;
    const auto& insts = m_engine->modelRenderer().getInstances();
    m_baseModels.resize(insts.size());
    for (size_t i = 0; i < insts.size(); ++i)
        m_baseModels[i] = {insts[i].transform, insts[i].enabled, m_engine->isSolid(static_cast<uint32_t>(i))};
    m_baseCharacters.clear();
    for (const auto& c : m_engine->characters())
        if (!c.removed && !c.fromNeighbor) m_baseCharacters.push_back({c.name, c.position, c.facing});
    m_baselineMap = map;
    const auto it = m_mapStates.find(map);
    if (it != m_mapStates.end()) applyMapState(it->second);
}

void ScriptHost::rememberMapState() {
    if (!m_baselineMap.empty() && m_baselineMap == m_engine->currentMapName())
        m_mapStates[m_baselineMap] = captureMapState();
}

std::string ScriptHost::captureMapState() const {
    // Só o que mudou desde a foto: índice + nome (o nome confere se a cena mudou).
    json models = json::array();
    const auto& insts = m_engine->modelRenderer().getInstances();
    const size_t n = std::min(insts.size(), m_baseModels.size());
    for (size_t i = 0; i < n; ++i) {
        if (m_engine->isRegionInstance(static_cast<uint32_t>(i))) continue;
        const BaseModel& b = m_baseModels[i];
        const bool solid = m_engine->isSolid(static_cast<uint32_t>(i));
        if (insts[i].transform == b.transform && insts[i].enabled == b.enabled && solid == b.solid) continue;
        json m{{"i", i}, {"name", insts[i].name}};
        if (insts[i].transform != b.transform) {
            json t = json::array();
            for (int c = 0; c < 4; ++c)
                for (int r = 0; r < 4; ++r) t.push_back(insts[i].transform[c][r]);
            m["transform"] = t;
        }
        if (insts[i].enabled != b.enabled) m["visible"] = insts[i].enabled;
        if (solid != b.solid) m["solid"] = solid;
        models.push_back(m);
    }
    json chars = json::array();
    for (const auto& b : m_baseCharacters) {
        const int idx = m_engine->findCharacter(b.name);
        if (idx < 0) continue;
        const auto& c = m_engine->characters()[static_cast<size_t>(idx)];
        if (glm::length(c.position - b.position) < 0.01f && std::abs(c.facing - b.facing) < 0.01f) continue;
        chars.push_back({{"name", c.name}, {"position", {c.position.x, c.position.y, c.position.z}}, {"facing", c.facing}});
    }
    json j = json::object();
    if (!models.empty()) j["models"] = models;
    if (!chars.empty()) j["characters"] = chars;
    return j.dump();
}

void ScriptHost::applyMapState(const std::string& text) {
    const json j = json::parse(text, nullptr, false);
    if (!j.is_object()) return;
    ModelRenderer& mr = m_engine->modelRenderer();
    m_tweens.clear();
    // Primeiro tudo volta à foto do mapa; depois o que o estado diz.
    const auto& insts = mr.getInstances();
    for (size_t i = 0; i < std::min(insts.size(), m_baseModels.size()); ++i) {
        const BaseModel& b = m_baseModels[i];
        const auto idx = static_cast<uint32_t>(i);
        if (insts[i].transform != b.transform) mr.setInstanceTransform(idx, b.transform);
        if (insts[i].enabled != b.enabled) mr.setInstanceEnabled(idx, b.enabled);
        if (m_engine->isSolid(idx) != b.solid) m_engine->setSolid(idx, b.solid);
    }
    for (const auto& b : m_baseCharacters) {
        const int idx = m_engine->findCharacter(b.name);
        if (idx < 0) continue;
        auto& c = m_engine->characters()[static_cast<size_t>(idx)];
        c.position = b.position;
        c.facing = b.facing;
        c.walking = false;
    }
    int applied = 0;
    for (const auto& m : j.value("models", json::array())) {
        const std::string name = m.value("name", std::string());
        size_t i = m.value("i", size_t(0));
        if (i >= insts.size() || insts[i].name != name) {
            const int found = scriptFindModel(name);
            if (found < 0) continue;
            i = static_cast<size_t>(found);
        }
        const auto idx = static_cast<uint32_t>(i);
        if (m.contains("transform") && m["transform"].is_array() && m["transform"].size() == 16) {
            Mat4 t(1.0f);
            for (int c = 0; c < 4; ++c)
                for (int r = 0; r < 4; ++r) t[c][r] = m["transform"][c * 4 + r].get<float>();
            mr.setInstanceTransform(idx, t);
        }
        if (m.contains("visible")) mr.setInstanceEnabled(idx, m["visible"].get<bool>());
        if (m.contains("solid")) m_engine->setSolid(idx, m["solid"].get<bool>());
        ++applied;
    }
    for (const auto& ch : j.value("characters", json::array())) {
        const int idx = m_engine->findCharacter(ch.value("name", std::string()));
        const json& p = ch.value("position", json::array());
        if (idx < 0 || !p.is_array() || p.size() != 3) continue;
        auto& c = m_engine->characters()[static_cast<size_t>(idx)];
        c.position = Vec3(p[0].get<float>(), p[1].get<float>(), p[2].get<float>());
        c.facing = ch.value("facing", c.facing);
        c.walking = false;
        ++applied;
    }
    if (applied) {
        ERUPTION_LOG_INFO("Map state: %d thing(s) back where the game left them", applied);
    }
}

} // namespace eruption
