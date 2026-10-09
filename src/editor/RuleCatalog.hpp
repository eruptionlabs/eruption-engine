#pragma once

#include <string>
#include <vector>

namespace eruption {

// Idioma das frases da folha de eventos (e, aos poucos, do editor todo).
enum class UiLanguage { English, Portuguese };

enum class RuleKind { Trigger, Condition, Action };

enum class ParamType {
    Text,     // "texto"
    Number,
    Model,    // nome do modelo no mapa
    Key,      // "A", "space"...
    Weather,  // "clear", "rainy"...
    Light,    // índice da luz (1..)
    Variable, // nome da variável
    Value,    // número, texto ou verdadeiro/falso
    Offset,   // vector.create(x, y, z)
    Hour,     // 0..24
    Seconds,
};

struct RuleParam {
    ParamType type;
    const char* defaultCode; // literal Luau inicial
};

// Uma entrada do catálogo: a frase em cada idioma (com {1}, {2}... no lugar
// dos parâmetros) e a função Luau que ela chama.
struct RuleDef {
    const char* id;          // função Luau: "on.near", "models.move_by"...
    RuleKind kind;
    const char* category;
    std::vector<RuleParam> params;
    const char* en;
    const char* pt;
};

// Condição especial "variável <op> valor" (vars.get(nome) == 3).
inline constexpr const char* kVarCompareId = "vars.compare";

const std::vector<RuleDef>& ruleCatalog();
const RuleDef* findRuleDef(const std::string& id);

const char* ruleText(const RuleDef& def, UiLanguage lang);
// Texto curto da interface da folha (botões, cabeçalhos).
const char* ruleUi(const char* key, UiLanguage lang);
const char* categoryName(const char* category, UiLanguage lang);

} // namespace eruption
