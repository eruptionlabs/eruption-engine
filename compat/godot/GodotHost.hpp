#pragma once

// Host de extensoes GDExtension (API C publica do Godot 4, compativel com
// extensoes feitas com godot-cpp). Carrega o .so, entrega a tabela de
// funcoes da interface, recebe o registro de classes e roda os nos com
// _ready/_process sobre o host de compat (compat/host/Host.hpp).

#include <cstdint>
#include <string>
#include <vector>

namespace eruption::compat::godot {

// Valor simples para quem chama de fora (linha de comando, motor).
struct Valor {
    enum class Tipo { Nulo, Bool, Int, Float, Texto } tipo = Tipo::Nulo;
    bool b = false;
    int64_t i = 0;
    double f = 0.0;
    std::string s;

    // "10" -> Int, "1.5" -> Float, "true"/"false" -> Bool, resto -> Texto.
    static Valor deTexto(const std::string& t);
};

// Carrega uma extensao: arquivo .gdextension, ou "biblioteca.so:simbolo".
// Inicializa os niveis core, servers e scene (sem editor).
bool carregarExtensao(const std::string& caminho);

// Classes registradas pelas extensoes carregadas.
std::vector<std::string> classesRegistradas();

// Cria uma instancia (0 = falhou). O objeto fica vivo ate' liberar().
uint64_t instanciar(const std::string& classe);
// Pendura um no' na cena (pai 0 = raiz); dispara _enter_tree/_ready.
bool adicionarNaCena(uint64_t no, uint64_t pai = 0);
// Monta uma cena .tscn (formato de texto do Godot 4) e pendura a raiz dela na
// cena; devolve a raiz (0 = falhou). Cobre nos de classes nativas e de
// extensao, posicao/rotacao/escala/transform e propriedades simples (numero,
// bool, texto) das extensoes; script GDScript e cena instanciada dentro de
// cena ficam de fora, com aviso.
uint64_t carregarCena(const std::string& caminhoTscn);
// Chama um metodo pelo nome; o resultado vem como texto (como print()).
bool chamar(uint64_t objeto, const std::string& metodo, const std::vector<Valor>& args, std::string* resultado);
void liberar(uint64_t objeto);

// Um quadro: _physics_process/_process dos nos na cena e callbacks de laco.
void quadro(double dt);

// Descricao curta do objeto (classe, nome e posicao, se for no').
std::string descrever(uint64_t objeto);

// Uma linha por no' da cena GDExtension (descrever() de cada um).
std::vector<std::string> descreverCena();
// Multimeshes visiveis pedidas ao RenderingServer (partículas, por exemplo).
std::vector<std::string> descreverDesenho();

// Desliga as extensoes (niveis em ordem reversa) e descarrega.
void encerrar();

} // namespace eruption::compat::godot
