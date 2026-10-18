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

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>

namespace eruption {

namespace {

namespace fs = std::filesystem;
using nlohmann::json;

// Objetos da cena citados em vars e em obj.data viram o caminho deles
// ("casa/porta") e voltam pelo caminho. Apontado durante salvar e carregar.
SceneObjects* g_scene = nullptr;

// Tabelas no caminho da conversão: uma que aparece dentro de si mesma
// (t.a = t) vira nada em vez de multiplicar o arquivo.
std::vector<const void*> g_converting;

json toJson(lua_State* L, int idx, int depth);

json tableToJson(lua_State* L, int idx, int depth) {
    const void* self = lua_topointer(L, idx);
    if (std::find(g_converting.begin(), g_converting.end(), self) != g_converting.end() || !lua_checkstack(L, 4))
        return nullptr;
    g_converting.push_back(self);
    // Lista (1..n sem buracos) vira array; o resto, objeto com chave texto.
    const int n = lua_objlen(L, idx);
    int keys = 0;
    lua_pushnil(L);
    while (lua_next(L, idx)) {
        ++keys;
        lua_pop(L, 1);
    }
    json out;
    if (n > 0 && keys == n) {
        out = json::array();
        for (int i = 1; i <= n; ++i) {
            lua_rawgeti(L, idx, i);
            out.push_back(toJson(L, -1, depth + 1));
            lua_pop(L, 1);
        }
    } else {
        out = json::object();
        lua_pushnil(L);
        while (lua_next(L, idx)) {
            if (lua_type(L, -2) == LUA_TSTRING) out[lua_tostring(L, -2)] = toJson(L, -1, depth + 1);
            else if (lua_type(L, -2) == LUA_TNUMBER) out["#" + std::to_string(lua_tonumber(L, -2))] = toJson(L, -1, depth + 1);
            lua_pop(L, 1);
        }
    }
    g_converting.pop_back();
    return out;
}

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
        case LUA_TTABLE: return tableToJson(L, idx, depth);
        case LUA_TUSERDATA: {
            ObjectId id;
            if (g_scene && scriptToObject(L, idx, id) && g_scene->world().alive(id))
                return json{{"$obj", g_scene->pathOf(id)}};
            return nullptr;
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
        if (j.contains("$obj") && j["$obj"].is_string()) {
            scriptPushObject(L, g_scene ? g_scene->findPath(j["$obj"].get<std::string>()) : ObjectId{});
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

json transformJson(const ObjectWorld::Transform& t) {
    return json::array({t.position.x, t.position.y, t.position.z, t.rotation.x, t.rotation.y, t.rotation.z,
                        t.rotation.w, t.scale.x, t.scale.y, t.scale.z});
}

ObjectWorld::Transform transformFrom(const json& j) {
    ObjectWorld::Transform t;
    if (!j.is_array() || j.size() != 10) return t;
    t.position = Vec3(j[0].get<float>(), j[1].get<float>(), j[2].get<float>());
    t.rotation = Quat(j[6].get<float>(), j[3].get<float>(), j[4].get<float>(), j[5].get<float>());
    t.scale = Vec3(j[7].get<float>(), j[8].get<float>(), j[9].get<float>());
    return t;
}

json tagsJson(const ObjectWorld& w, ObjectId id) {
    json a = json::array();
    for (const auto& t : w.tags(id)) a.push_back(t);
    return a;
}

// Objetos criados por script (com pai, transform, etiquetas e dados) e os
// dados/etiquetas que os scripts puseram nos objetos do mapa.
json scriptsJson(const ScriptHost& host, ObjectId id) {
    json a = json::array();
    for (const auto& m : host.behaviorsOf(id)) a.push_back(m);
    return a;
}

json captureObjects(lua_State* L, const ScriptHost& host, SceneObjects& scene) {
    ObjectWorld& w = scene.world();
    const std::vector<ObjectId> spawned = scriptObjects();
    json made = json::array();
    for (const ObjectId& id : spawned) {
        json o{{"name", w.name(id)}, {"local", transformJson(w.local(id))}, {"active", w.active(id)}};
        const std::string source = scene.sourceOf(id);
        if (!source.empty()) o["source"] = source;
        const ObjectId parent = w.parent(id);
        if (parent.valid()) {
            const auto it = std::find(spawned.begin(), spawned.end(), parent);
            if (it != spawned.end()) o["parent_made"] = it - spawned.begin();
            else o["parent"] = scene.pathOf(parent);
        }
        if (!w.tags(id).empty()) o["tags"] = tagsJson(w, id);
        if (!host.behaviorsOf(id).empty()) o["scripts"] = scriptsJson(host, id);
        if (scriptPushObjectDataIfAny(L, id)) {
            o["data"] = toJson(L, -1, 0);
            lua_pop(L, 1);
        }
        made.push_back(o);
    }
    json marked = json::array();
    std::vector<ObjectId> touched = scriptObjectsWithData(L);
    for (const ObjectId& id : w.tagged()) touched.push_back(id);
    for (const ObjectId& id : host.behaviorObjects())
        if (w.alive(id)) touched.push_back(id);
    std::sort(touched.begin(), touched.end(), [](const ObjectId& a, const ObjectId& b) {
        return a.index != b.index ? a.index < b.index : a.generation < b.generation;
    });
    touched.erase(std::unique(touched.begin(), touched.end()), touched.end());
    for (const ObjectId& id : touched) {
        if (std::find(spawned.begin(), spawned.end(), id) != spawned.end()) continue;
        json o{{"path", scene.pathOf(id)}};
        if (!w.tags(id).empty()) o["tags"] = tagsJson(w, id);
        if (!host.behaviorsOf(id).empty()) o["scripts"] = scriptsJson(host, id);
        if (scriptPushObjectDataIfAny(L, id)) {
            o["data"] = toJson(L, -1, 0);
            lua_pop(L, 1);
        }
        marked.push_back(o);
    }
    json j = json::object();
    if (!made.empty()) j["made"] = made;
    if (!marked.empty()) j["marked"] = marked;
    return j;
}

void restoreTagsAndData(lua_State* L, ScriptHost& host, ObjectWorld& w, ObjectId id, const json& o) {
    for (const auto& t : o.value("tags", json::array()))
        if (t.is_string()) w.addTag(id, t.get<std::string>());
    for (const auto& m : o.value("scripts", json::array())) {
        std::string error;
        if (m.is_string() && !host.attachBehavior(id, m.get<std::string>(), error))
            Logger::warning("Saved object script: %s", error.c_str());
    }
    if (o.contains("data")) {
        pushJson(L, o["data"], 0);
        if (lua_istable(L, -1)) scriptSetObjectData(L, id);
        else lua_pop(L, 1);
    }
}

void restoreObjects(lua_State* L, ScriptHost& host, Engine& engine, const json& j) {
    SceneObjects& scene = engine.sceneObjects();
    ObjectWorld& w = scene.world();
    releaseScriptObjects();
    // Primeiro todos na raiz (um filho pode ter sido criado antes do pai),
    // depois os pais, por fim os dados (que podem citar outros objetos).
    const json made = j.value("made", json::array());
    std::vector<ObjectId> ids;
    for (const auto& o : made) {
        const std::string name = o.value("name", std::string("object"));
        const ObjectWorld::Transform t = transformFrom(o.value("local", json::array()));
        const std::string source = o.value("source", std::string());
        std::string error;
        std::vector<SceneObjects::ObjectScript> prefabScripts;
        ObjectId id;
        if (source.empty()) {
            id = w.create(name, {}, t);
            scene.markMade(id);
        } else {
            id = scene.spawn(engine, source, name, {}, t, &error, &prefabScripts);
        }
        if (!id.valid()) Logger::warning("Saved object %s: %s", name.c_str(), error.c_str());
        else scriptAdoptObject(id);
        // Scripts que o prefab dá aos filhos (os da raiz também vêm em "scripts").
        for (const auto& [object, module] : prefabScripts)
            if (!host.attachBehavior(object, module, error)) Logger::warning("Saved object script: %s", error.c_str());
        ids.push_back(id);
    }
    for (size_t k = 0; k < made.size(); ++k) {
        if (!ids[k].valid()) continue;
        const json& o = made[k];
        ObjectId parent;
        if (o.contains("parent_made")) {
            const size_t p = o["parent_made"].get<size_t>();
            if (p < ids.size()) parent = ids[p];
        } else if (o.contains("parent")) {
            parent = scene.findPath(o["parent"].get<std::string>());
        }
        if (parent.valid()) w.setParent(ids[k], parent, false);
        w.setLocal(ids[k], transformFrom(o.value("local", json::array())));
        w.setActive(ids[k], o.value("active", true));
    }
    for (size_t k = 0; k < made.size(); ++k)
        if (ids[k].valid()) restoreTagsAndData(L, host, w, ids[k], made[k]);
    for (const auto& o : j.value("marked", json::array())) {
        const ObjectId id = scene.findPath(o.value("path", std::string()));
        if (id.valid()) restoreTagsAndData(L, host, w, id, o);
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

namespace {

// Configuracao do jogador (volumes, velocidade...): fora dos jogos salvos, que
// a apagam ou trocam. O ponto no nome nao sai de um nome de slot (slotPath
// troca '.' por '_'), entao nunca colide com um jogo salvo.
fs::path settingsPath() { return ScriptHost::saveDir() / "settings.prefs.json"; }

} // namespace

bool ScriptHost::saveSettings(lua_State* L, int idx, std::string& error) {
    if (!lua_istable(L, idx)) {
        error = "settings must be a table";
        return false;
    }
    const json j = toJson(L, idx, 0);
    std::error_code ec;
    fs::create_directories(saveDir(), ec);
    // Grava ao lado e troca: um desligamento no meio nao deixa o arquivo pela metade.
    const fs::path path = settingsPath(), tmp = path.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            error = "cannot write " + tmp.string();
            return false;
        }
        f << j.dump(1, ' ', false, json::error_handler_t::replace);
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        error = "cannot write " + path.string() + ": " + ec.message();
        return false;
    }
    return true;
}

void ScriptHost::pushSettings(lua_State* L) {
    std::ifstream f(settingsPath(), std::ios::binary);
    const json j = f ? json::parse(f, nullptr, false) : json();
    if (j.is_object()) pushJson(L, j, 0);
    else lua_newtable(L); // sem arquivo (ou estragado): tabela vazia, o jogo usa os padroes dele
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
    g_scene = &m_engine->sceneObjects();
    lua_getref(L, m_varsRef);
    const json j = toJson(L, -1, 0);
    lua_pop(L, 1);
    return j.is_null() ? "{}" : j.dump(-1, ' ', false, json::error_handler_t::replace);
}

// Variáveis do texto que contêm um objeto ({"$obj": ...} em qualquer nível)
// entram por cima das atuais; as outras ficam como estão.
static bool containsObjectRef(const json& j) {
    if (j.is_object()) {
        if (j.contains("$obj")) return true;
        for (const auto& [k, v] : j.items())
            if (containsObjectRef(v)) return true;
    } else if (j.is_array()) {
        for (const auto& v : j)
            if (containsObjectRef(v)) return true;
    }
    return false;
}

void ScriptHost::mergeObjectVars(const std::string& text) {
    const json j = json::parse(text, nullptr, false);
    if (!L || !j.is_object() || m_varsRef < 0) return;
    g_scene = &m_engine->sceneObjects();
    lua_getref(L, m_varsRef);
    for (const auto& [key, value] : j.items()) {
        if (!containsObjectRef(value)) continue;
        pushJson(L, value, 0);
        lua_setfield(L, -2, key.c_str());
    }
    lua_pop(L, 1);
}

bool ScriptHost::importVars(const std::string& text) {
    const json j = json::parse(text, nullptr, false);
    if (!L || !j.is_object()) return false;
    g_scene = &m_engine->sceneObjects();
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
        f << j.dump(1, ' ', false, json::error_handler_t::replace) << "\n";
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
    // Pedido de dentro de um script de objeto: carregar agora trocaria a lista
    // que está rodando (e destruiria o próprio self). Fica para o fim deles.
    if (m_inBehaviors) {
        if (!hasSave(slot)) {
            error = "no saved game in slot \"" + slot + "\"";
            return false;
        }
        m_deferredLoad = slot;
        return true;
    }
    try {
        return loadGameUnchecked(slot, error);
    } catch (const json::exception& e) {
        error = "saved game in slot \"" + slot + "\" is damaged (" + e.what() + ")";
        return false;
    }
}

bool ScriptHost::loadGameUnchecked(const std::string& slot, std::string& error) {
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
    // Variáveis entram de novo depois que os objetos do mapa voltam: uma
    // variável que guarda um objeto só acha o objeto depois disso.
    const std::string vars = j.contains("vars") ? j["vars"].dump(-1, ' ', false, json::error_handler_t::replace) : std::string();
    if (!vars.empty()) importVars(vars);
    m_mapStates.clear();
    if (j.contains("maps") && j["maps"].is_object())
        for (auto it = j["maps"].begin(); it != j["maps"].end(); ++it) m_mapStates[it.key()] = it.value().dump(-1, ' ', false, json::error_handler_t::replace);
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
        m_pendingVars = vars; // de novo em mapReady, com os objetos do mapa novo
    } else {
        m_pendingVars.clear(); // um load de outro mapa ainda não chegado perde para este
        // Mesmo mapa: as coisas voltam à foto do jogo salvo (ou ao começo do mapa).
        const auto it = m_mapStates.find(m_baselineMap);
        applyMapState(it != m_mapStates.end() ? it->second : std::string("{}"));
        if (!vars.empty()) importVars(vars);
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
    m_engine->sceneObjects().refresh(*m_engine); // o que entrou depois do último quadro (espalhados) já tem objeto
    attachSceneScripts(); // scripts de objeto que a cena do mapa declara
    const auto it = m_mapStates.find(map);
    if (it != m_mapStates.end()) applyMapState(it->second);
    // Jogo carregado em outro mapa: as variáveis já voltaram no game.load;
    // aqui só as que guardam objeto, que agora acham o dele (as outras o
    // script pode ter mudado depois do load).
    if (!m_pendingVars.empty()) {
        mergeObjectVars(m_pendingVars);
        m_pendingVars.clear();
    }
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
    if (L) {
        g_scene = &m_engine->sceneObjects();
        const json objects = captureObjects(L, *this, m_engine->sceneObjects());
        if (!objects.empty()) j["objects"] = objects;
    }
    return j.dump(-1, ' ', false, json::error_handler_t::replace);
}

// Estado salvo torto (arquivo editado à mão, versão antiga) não derruba o
// jogo: o que já voltou fica, o resto é avisado.
void ScriptHost::applyMapState(const std::string& text) {
    try {
        applyMapStateUnchecked(text);
    } catch (const json::exception& e) {
        Logger::error("Saved map state is damaged: %s", e.what());
    }
}

void ScriptHost::applyMapStateUnchecked(const std::string& text) {
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
    if (L) {
        g_scene = &m_engine->sceneObjects();
        // Objetos do mapa voltam ao que a cena declara (pai, etiquetas,
        // ligado; o que o jogo destruiu ganha objeto de novo) ANTES do que o
        // jogo salvo diz: as referências dele ("caixa", "casa/porta") precisam
        // existir. Scripts ligados no meio desta sessão param; voltam os da
        // cena e os que o jogo salvo pede.
        stopBehaviors();
        resetScriptObjectData(L); // obj.data volta só como o estado salvo diz
        m_engine->sceneObjects().restoreAuthored(*m_engine);
        restoreObjects(L, *this, *m_engine, j.value("objects", json::object()));
        attachSceneScripts();
    }
    if (applied) {
        ERUPTION_LOG_INFO("Map state: %d thing(s) back where the game left them", applied);
    }
}

} // namespace eruption
