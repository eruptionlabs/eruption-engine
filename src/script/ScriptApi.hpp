#pragma once

#include "core/Input.hpp"
#include "math/Types.hpp"

#include <string>
#include <vector>

struct lua_State;

namespace eruption {

class Engine;

// Registra as bibliotecas do motor (world, player, camera, input, models,
// lights, log) como globais do estado Luau.
void registerScriptApi(lua_State* L, Engine& engine);

// As mesmas bibliotecas descritas em sintaxe de declaração do Luau, para o
// autocompletar e a checagem de tipos no editor de código.
std::string scriptApiDefinitions();

// Tabela `vfx` (ScriptApiVfx.cpp): efeitos de particula, e o bloco dela nas
// declaracoes de tipo.
void registerVfxApi(lua_State* L, Engine& engine);
const char* vfxApiDefinitions();
// Tabela `objects` e o tipo Object (ScriptApiObjects.cpp): os objetos da
// cena. releaseScriptObjects destrói os que os scripts criaram (Stop, recarga).
void registerObjectsApi(lua_State* L, Engine& engine);
const char* objectsApiDefinitions();
// Faixas por spline em `world` (ScriptApiRibbon.cpp): world.ribbon e as
// consultas; as declaracoes sao linhas do bloco `declare world`.
void registerRibbonApi(lua_State* L, Engine& engine);
std::string ribbonApiDefinitions();
// Tabela `vision` (ScriptApiVision.cpp): visao de equipe.
void registerVisionApi(lua_State* L, Engine& engine);
const char* visionApiDefinitions();
// Tabela `telemetry` (ScriptApiTelemetry.cpp): eventos do jogo em JSONL.
// openTelemetryFromEnv liga pelo ERUPTION_GAME_TELEMETRY no inicio do jogo.
void registerTelemetryApi(lua_State* L, Engine& engine);
const char* telemetryApiDefinitions();
void openTelemetryFromEnv();
// Tabela `net` (ScriptApiNet.cpp): partida em rede. ScriptHost entrega os
// eventos da rede aos net.on* pela chave ("@join", "@leave", "@fail" ou o
// nome do evento); scriptPushNetEvent empilha os argumentos e diz quantos.
struct NetScriptEvent;
void registerNetApi(lua_State* L, Engine& engine);
const char* netApiDefinitions();
std::string netEventKey(const NetScriptEvent& event);
int scriptPushNetEvent(lua_State* L, const NetScriptEvent& event);
void releaseScriptObjects();
// Empilha o Object (nil se o id não vale).
struct ObjectId;
void scriptPushObject(lua_State* L, ObjectId id);
// Jogo salvo (SaveGame.cpp): os objetos dos scripts e as tabelas obj.data.
bool scriptToObject(lua_State* L, int idx, ObjectId& out);
std::vector<ObjectId> scriptObjects();          // criados por script, vivos
void scriptAdoptObject(ObjectId id);            // passa a ser dos scripts (some no Stop)
std::vector<ObjectId> scriptObjectsWithData(lua_State* L);
bool scriptPushObjectDataIfAny(lua_State* L, ObjectId id);
void scriptSetObjectData(lua_State* L, ObjectId id); // tira a tabela do topo da pilha
void resetScriptObjectData(lua_State* L);            // jogo novo: nenhum obj.data

// Tecla pelo nome usado nos scripts ("A", "space", "left"...).
Key scriptKeyFromName(const char* name);
// Índice (base 0) do primeiro modelo com esse nome, ou -1.
int scriptFindModel(const std::string& name);
// Posição do jogador (zero sem jogador).
Vec3 scriptPlayerPosition();

} // namespace eruption
