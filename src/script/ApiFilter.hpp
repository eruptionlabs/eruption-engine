#pragma once

// Filtro da API de script no jogo exportado (analise: analyzeApiUse em src/editor/GameExport.cpp).
//
// As tabelas luaL_Reg referenciam todas as funcoes, entao o linker nao joga
// nenhuma fora. ERUPTION_API_FN(world, nav_height) escreve a entrada
// {"nav_height", world_nav_height}; no build com ERUPTION_API_FILTER (so' o
// LEAN, com o header eruption_api_used.h gerado pelo export) a entrada de uma
// funcao que nenhum script do jogo usa fica com func nula: a funcao deixa de
// ser referenciada (o compilador e o linker a descartam, com tudo que so' ela
// chama) e quem a chamar recebe o erro "funcao nao exportada" em vez de
// um "attempt to call nil" ou de um crash. Sem o filtro (desenvolvimento e
// editor) a macro e' a entrada de sempre.

#if defined(ERUPTION_API_FILTER)
#include "eruption_api_used.h" // #define ERUPTION_API_USE_<global>_<funcao> 1

// IS_DEFINED a la Kconfig: 1 se ERUPTION_API_USE_g_f foi definido como 1, senao 0.
#define ERUPTION_API_PLACEHOLDER_1 0,
#define ERUPTION_API_TAKE2(a, b, ...) b
#define ERUPTION_API_PROBE3(x) ERUPTION_API_TAKE2(x 1, 0)
#define ERUPTION_API_PROBE2(v) ERUPTION_API_PROBE3(ERUPTION_API_PLACEHOLDER_##v)
#define ERUPTION_API_PROBE(v) ERUPTION_API_PROBE2(v)
#define ERUPTION_API_USED(g, f) ERUPTION_API_PROBE(ERUPTION_API_USE_##g##_##f)
#define ERUPTION_API_FN(g, f) {#f, ERUPTION_API_USED(g, f) ? g##_##f : nullptr}
#else
#define ERUPTION_API_FN(g, f) {#f, g##_##f}
#endif
