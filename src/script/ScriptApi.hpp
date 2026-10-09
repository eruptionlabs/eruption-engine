#pragma once

#include "core/Input.hpp"

#include <string>

struct lua_State;

namespace eruption {

class Engine;

// Registra as bibliotecas do motor (world, player, camera, input, models,
// lights, log) como globais do estado Luau.
void registerScriptApi(lua_State* L, Engine& engine);

// As mesmas bibliotecas descritas em sintaxe de declaração do Luau, para o
// autocompletar e a checagem de tipos no editor de código.
std::string scriptApiDefinitions();

// Tecla pelo nome usado nos scripts ("A", "space", "left"...).
Key scriptKeyFromName(const char* name);
// Índice (base 0) do primeiro modelo com esse nome, ou -1.
int scriptFindModel(const std::string& name);

} // namespace eruption
