#pragma once

#include "editor/RuleCatalog.hpp"

#include <string>
#include <vector>

namespace eruption {

// Uma chamada do catálogo dentro de uma regra. Os argumentos ficam como
// trechos de código Luau (literais, na maioria), na mesma ordem dos
// parâmetros. Sem `def`, é um trecho de código que a folha não sabe
// representar e só mostra (e devolve intacto ao salvar).
struct RuleCall {
    const RuleDef* def = nullptr;
    std::vector<std::string> args;
    bool negate = false;   // condições: NÃO
    std::string op = "=="; // vars.compare
    std::string code;      // sem def: o código original
};

// Uma linha da folha: QUANDO (evento) + SE (condições, todas valendo) +
// FAÇA (ações) + SENÃO (ações quando alguma condição falha).
struct Rule {
    RuleCall trigger;
    std::vector<RuleCall> conditions;
    std::vector<RuleCall> actions;
    std::vector<RuleCall> otherwise;
    bool raw = false;      // instrução que não é regra: guardada em `code`
    std::string code;
};

// Uma folha de regras = um arquivo .luau no formato que on.* entende. Ler e
// gravar passam pelo analisador do próprio Luau: o arquivo continua sendo um
// script comum, que também pode ser editado à mão ou por uma IA.
struct RuleSheet {
    std::string header;    // comentários do topo do arquivo
    std::vector<Rule> rules;

    // Falso se o código tem erro de sintaxe (mensagem em `error`).
    bool parse(const std::string& source, std::string& error);
    std::string generate() const;

    static RuleCall makeCall(const RuleDef& def);
};

// Valor mostrado na folha para um argumento (sem aspas, vetor por extenso).
std::string ruleArgDisplay(ParamType type, const std::string& code, UiLanguage lang);

} // namespace eruption
