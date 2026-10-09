#pragma once

#include <string>

struct lua_State;

// Carrega arquivos .luau como fonte (compila na hora) ou como bytecode já
// compilado na exportação: o jogo exportado leva só a máquina virtual.
namespace eruption {

bool isLuauBytecode(const std::string& data);

// Empilha a função do trecho em L. Devolve false com a mensagem no topo.
bool loadLuauChunk(lua_State* L, const std::string& chunkName, const std::string& data);

#if ERUPTION_LUAU_COMPILER
// Fonte -> conteúdo do arquivo exportado (prefixo + bytecode). Vazio se não compila.
std::string compileLuauForExport(const std::string& source, std::string& error);
#endif

} // namespace eruption
