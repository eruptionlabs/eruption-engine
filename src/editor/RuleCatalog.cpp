#include "editor/RuleCatalog.hpp"

#include <cstring>

namespace eruption {

const std::vector<RuleDef>& ruleCatalog() {
    using T = ParamType;
    static const std::vector<RuleDef> kCatalog = {
        // Quando
        {"on.start", RuleKind::Trigger, "events", {},
         "When the game starts", "Quando o jogo começar"},
        {"on.every", RuleKind::Trigger, "events", {{T::Seconds, "1"}},
         "Every {1} s", "A cada {1} s"},
        {"on.key", RuleKind::Trigger, "events", {{T::Key, "\"space\""}},
         "When key {1} is pressed", "Quando apertar a tecla {1}"},
        {"on.near", RuleKind::Trigger, "events", {{T::Model, "\"\""}, {T::Number, "40"}},
         "When the player comes within {2} of {1}", "Quando o jogador chegar a {2} de {1}"},
        {"on.hour", RuleKind::Trigger, "events", {{T::Hour, "18"}},
         "When the clock reaches {1}h", "Quando o relógio marcar {1}h"},
        {"on.update", RuleKind::Trigger, "events", {},
         "Every frame", "A cada quadro"},

        // Se
        {"input.down", RuleKind::Condition, "input", {{T::Key, "\"space\""}},
         "Key {1} is held", "A tecla {1} está apertada"},
        {"player.near", RuleKind::Condition, "player", {{T::Model, "\"\""}, {T::Number, "40"}},
         "The player is within {2} of {1}", "O jogador está a menos de {2} de {1}"},
        {"world.time_between", RuleKind::Condition, "world", {{T::Hour, "18"}, {T::Hour, "6"}},
         "The time is between {1}h and {2}h", "A hora está entre {1}h e {2}h"},
        {"world.is_weather", RuleKind::Condition, "world", {{T::Weather, "\"rainy\""}},
         "The weather is {1}", "O clima é {1}"},
        {"models.visible", RuleKind::Condition, "models", {{T::Model, "\"\""}},
         "{1} is visible", "{1} está visível"},
        {kVarCompareId, RuleKind::Condition, "variables", {{T::Variable, "\"points\""}, {T::Value, "0"}},
         "Variable {1} {op} {2}", "A variável {1} {op} {2}"},

        // Faça
        {"ui.message", RuleKind::Action, "screen", {{T::Text, "\"Hello!\""}, {T::Seconds, "3"}},
         "Show the message {1} for {2} s", "Mostrar a mensagem {1} por {2} s"},
        {"log", RuleKind::Action, "screen", {{T::Text, "\"...\""}},
         "Write {1} in the console", "Escrever {1} no console"},
        {"vars.set", RuleKind::Action, "variables", {{T::Variable, "\"points\""}, {T::Value, "0"}},
         "Set variable {1} to {2}", "Mudar a variável {1} para {2}"},
        {"vars.add", RuleKind::Action, "variables", {{T::Variable, "\"points\""}, {T::Number, "1"}},
         "Add {2} to variable {1}", "Somar {2} à variável {1}"},
        {"models.move_by", RuleKind::Action, "models",
         {{T::Model, "\"\""}, {T::Offset, "vector.create(0, 20, 0)"}, {T::Seconds, "1"}},
         "Move {1} by {2} in {3} s", "Mover {1} em {2} durante {3} s"},
        {"models.rotate_by", RuleKind::Action, "models", {{T::Model, "\"\""}, {T::Number, "90"}, {T::Seconds, "1"}},
         "Turn {1} by {2} degrees in {3} s", "Girar {1} em {2} graus durante {3} s"},
        {"models.show", RuleKind::Action, "models", {{T::Model, "\"\""}},
         "Show {1}", "Mostrar {1}"},
        {"models.hide", RuleKind::Action, "models", {{T::Model, "\"\""}},
         "Hide {1}", "Esconder {1}"},
        {"lights.turn_on", RuleKind::Action, "lights", {{T::Light, "1"}},
         "Turn on light {1}", "Acender a luz {1}"},
        {"lights.turn_off", RuleKind::Action, "lights", {{T::Light, "1"}},
         "Turn off light {1}", "Apagar a luz {1}"},
        {"world.set_time", RuleKind::Action, "world", {{T::Hour, "12"}},
         "Set the time to {1}h", "Mudar a hora para {1}h"},
        {"world.set_weather", RuleKind::Action, "world", {{T::Weather, "\"clear\""}},
         "Change the weather to {1}", "Mudar o clima para {1}"},
        {"player.set_position", RuleKind::Action, "player", {{T::Offset, "vector.create(0, 0, 0)"}},
         "Move the player to {1}", "Levar o jogador para {1}"},
    };
    return kCatalog;
}

const RuleDef* findRuleDef(const std::string& id) {
    for (const auto& d : ruleCatalog())
        if (id == d.id) return &d;
    return nullptr;
}

const char* ruleText(const RuleDef& def, UiLanguage lang) {
    return lang == UiLanguage::Portuguese ? def.pt : def.en;
}

const char* ruleUi(const char* key, UiLanguage lang) {
    static const struct { const char* key; const char* en; const char* pt; } kTexts[] = {
        {"when", "WHEN / IF", "QUANDO / SE"},
        {"do", "DO", "FAÇA"},
        {"else", "OTHERWISE", "SENÃO"},
        {"add_rule", "+ Add rule", "+ Nova regra"},
        {"add_condition", "+ Condition", "+ Condição"},
        {"add_action", "+ Action", "+ Ação"},
        {"add_else", "+ Otherwise", "+ Senão"},
        {"not", "NOT", "NÃO"},
        {"code", "Code", "Código"},
        {"delete", "Delete", "Apagar"},
        {"move_up", "Move up", "Subir"},
        {"move_down", "Move down", "Descer"},
        {"negate", "Invert (NOT)", "Inverter (NÃO)"},
        {"duplicate", "Duplicate", "Duplicar"},
        {"new_sheet", "+ New rule sheet", "+ Nova folha de regras"},
        {"new_name", "Name of the new sheet:", "Nome da nova folha:"},
        {"no_sheet", "Choose a rule sheet on the left, or create one.", "Escolha uma folha de regras à esquerda ou crie uma."},
        {"empty", "No rules yet. Click \"+ Add rule\" to start.", "Nenhuma regra ainda. Clique em \"+ Nova regra\" para começar."},
        {"raw", "Code (not editable here)", "Código (não editável aqui)"},
        {"syntax_error", "This file has a syntax error. Fix it in the code editor:", "Este arquivo tem um erro de sintaxe. Corrija no editor de código:"},
        {"search", "Search...", "Buscar..."},
        {"show_code", "Show code", "Ver código"},
        {"saved", "Saved", "Salvo"},
        {"yes", "true", "verdadeiro"},
        {"no", "false", "falso"},
        {"open_vscode", "Open in VS Code", "Abrir no VS Code"},
        {"edit_code", "Edit code", "Editar código"},
    };
    for (const auto& t : kTexts)
        if (std::strcmp(t.key, key) == 0) return lang == UiLanguage::Portuguese ? t.pt : t.en;
    return key;
}

const char* categoryName(const char* category, UiLanguage lang) {
    static const struct { const char* key; const char* en; const char* pt; } kNames[] = {
        {"events", "Events", "Eventos"}, {"input", "Keyboard", "Teclado"}, {"player", "Player", "Jogador"},
        {"world", "World", "Mundo"}, {"models", "Objects", "Objetos"}, {"variables", "Variables", "Variáveis"},
        {"screen", "Screen", "Tela"}, {"lights", "Lights", "Luzes"},
    };
    for (const auto& n : kNames)
        if (std::strcmp(n.key, category) == 0) return lang == UiLanguage::Portuguese ? n.pt : n.en;
    return category;
}

} // namespace eruption
