#pragma once

// GRAFO DO EMISSOR no editor de efeitos: nos (entradas, constantes,
// matematica, vetor, saidas) ligados por fios, desenhados com o imnodes. O
// modelo do grafo e' o VfxGraph do proprio emissor; a janela so' desenha e
// edita. Botao direito no fundo adiciona no; Delete apaga o selecionado;
// arrastar de um pino a outro liga; entrada solta tem valor editavel.

#include "renderer/vfx/VfxGraph.hpp"

#include <set>
#include <string>

struct ImNodesContext;

namespace eruption {

class VfxGraphEditor {
public:
    void init();
    void shutdown();
    // Desenha a janela do grafo; devolve true se o grafo mudou. `key` muda
    // quando o grafo editado e' outro (outro emissor/efeito).
    bool draw(const char* title, const std::string& key, VfxGraph& g, bool* open);

private:
    bool drawNode(VfxGraphNode& n, const VfxGraph& g);
    ImNodesContext* m_ctx = nullptr;
    std::string m_key;
    std::set<int> m_placed; // nos ja' posicionados neste contexto
    std::string m_status;
};

} // namespace eruption
