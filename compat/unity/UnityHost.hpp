#pragma once

// Host de plugins nativos do Unity (bibliotecas .so que exportam
// UnityPluginLoad/UnityPluginUnload e, opcionalmente, GetRenderEventFunc).
// Entrega a tabela de interfaces (graficos, log), dispara os eventos de
// dispositivo e de renderizacao e permite chamar funcoes C exportadas, como
// os scripts C# fazem via DllImport.

#include <string>
#include <vector>

namespace eruption::compat::unity {

// Dispositivo que os plugins enxergam (valor de Rendering.GraphicsDeviceType).
// Padrao 4 (nulo): o plugin desliga o proprio caminho de render. Quem embute
// o host e ja' tem um contexto OpenGL atual pode anunciar 17 (OpenGLCore)
// ANTES de carregar os plugins.
void definirRenderizador(int tipo);

bool carregarPlugin(const std::string& caminhoSo);

// Chama uma funcao exportada. `assinatura`: tipo de retorno e argumentos,
// ex.: "v(f)" = void(float), "i()" = int(), "v(i,i)" = void(int, int).
// Tipos: v void, i int32, l int64, f float, d double, p ponteiro (numero), s texto.
// Os valores vem como texto. Devolve o retorno formatado em `resultado`.
bool chamarFuncao(const std::string& nome, const std::string& assinatura, const std::vector<std::string>& valores,
                  std::string* resultado);

// GL.IssuePluginEvent: chama a funcao de GetRenderEventFunc() com o id.
// Devolve quantos plugins receberam o evento.
int emitirEventoRender(int id);

// Eventos de render pedidos a cada quadro (ERUPTION_COMPAT_UNITY_EVENTOS="1;2").
void definirEventosPorQuadro(const std::vector<int>& ids);
void quadro(double dt);

void encerrar();
bool algumCarregado();

} // namespace eruption::compat::unity
