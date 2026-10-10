#pragma once

// Fachada da camada de compatibilidade: carrega plugins de qualquer origem
// pelo caminho e repassa o quadro para os hosts que estiverem ativos.
//
// Formas aceitas (separadas por ';' em ERUPTION_COMPAT_PLUGINS):
//   caminho/extensao.gdextension        GDExtension (Godot 4)
//   godot:biblioteca.so:simbolo         GDExtension sem o arquivo .gdextension
//   unity:biblioteca.so                 plugin nativo (UnityPluginLoad)
//   unity-cs:Scripts.dll                scripts C# compilados contra o UnityEngine da compat
//   caminho/Plugin.uplugin              plugin Unreal recompilado com compat/unreal/ferramentas
//   unreal:caminho/Plugin.uplugin       idem
//   biblioteca.so                       detecta pelos simbolos exportados
//
// Nada roda por padrao: sem a variavel, a camada nao carrega nada.

#include <string>
#include <vector>

namespace eruption::compat {

bool carregarPlugin(const std::string& especificacao);
// Le ERUPTION_COMPAT_PLUGINS (ou outra variavel) e carrega cada item.
// Devolve quantos carregaram.
int carregarDaVariavel(const char* variavel = "ERUPTION_COMPAT_PLUGINS");

// Instancia o que for pedido em ERUPTION_COMPAT_GODOT_CENA ("Classe;Classe",
// nos GDExtension) e em ERUPTION_COMPAT_UNITY_CENA
// ("Objeto:Componente:campo=valor:...;...", GameObjects C#). Devolve quantos.
int instanciarDaVariavel(const char* variavel = "ERUPTION_COMPAT_GODOT_CENA");

// Um quadro do jogo: avanca o relogio do host e chama os callbacks.
void quadro(double dt);
void encerrar();

bool algumCarregado();

} // namespace eruption::compat
