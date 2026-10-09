#pragma once

#include "editor/RuleCatalog.hpp"

#include <string>

// Tradução da interface do editor. O texto em inglês é a chave; a tabela do
// idioma fica em data/lang/<idioma>.json ({"Inglês": "Tradução"}). Sem
// tradução, aparece o inglês. O "##id" do ImGui é mantido fora da tradução.
namespace eruption {

void setUiLanguage(UiLanguage lang);
UiLanguage uiLanguage();
// Texto visível traduzido (ponteiro válido até trocar de idioma).
const char* tr(const char* english);
// Janelas e abas: a tradução leva "###<inglês>" para a posição e a docagem
// continuarem as mesmas em qualquer idioma.
const char* trw(const char* english);

} // namespace eruption
