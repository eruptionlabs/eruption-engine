#pragma once

// REGIOES POR TEXTURA: um mapa de IDs desenhado em cima de uma textura do mapa
// (o mesmo conceito dos ID maps do Substance). Cada regiao diz como aquele
// pedaco da textura se comporta: "window" acende de noite, "lamp" tambem e
// traz uma luz pontual, "sand"/"snow" sao tipos de superficie.
//
// Logica pura (sem GPU nem ImGui): rasteriza as formas, monta a mascara
// emissiva a partir da luminancia (a mesma regra de tools/emissive_windows.py)
// e tem o laco magnetico (live-wire) do editor. Tudo em UV 0..1, v de cima
// para baixo, como a imagem; assim as regioes valem para qualquer resolucao
// que o pacote entregue da mesma textura.

#include <cstdint>
#include <string>
#include <vector>

namespace eruption::texregion {

enum class Shape { Poly, Rect, Ellipse, All, Bitmap };

// Como a luminancia do pixel vira brilho dentro da regiao.
//   Dark    - escuro brilha (vidro preto de janela): 1,1 - lum, piso 0,15
//   Light   - claro brilha (vidro de lampiao): lum 0,5 -> 0,7 vira 0 -> 1
//   Paper   - papel claro em trelica: o papel brilha, a madeira fica fraca
//   Uniform - a regiao inteira brilha igual
enum class Glow { Dark, Light, Paper, Uniform };

// Tipos conhecidos. A lista e' extensivel: um tipo desconhecido e' guardado e
// devolvido como veio (nao brilha, nao muda superficie), entao arquivo de uma
// versao nova nao se perde ao ser salvo por uma antiga.
inline constexpr const char* kTypeWindow = "window"; // janela com luz
inline constexpr const char* kTypeLamp = "lamp";     // poste / lampiao / tocha
inline constexpr const char* kTypeSand = "sand";     // superficie: areia
inline constexpr const char* kTypeSnow = "snow";     // superficie: neve
inline constexpr const char* kTypeGrass = "grass";   // superficie: grama
inline constexpr const char* kTypeDirt = "dirt";     // superficie: terra
inline constexpr const char* kTypeStone = "stone";   // superficie: pedra
inline constexpr const char* kTypeWood = "wood";     // superficie: madeira

bool isGlowType(const std::string& type);
bool isSurfaceType(const std::string& type);

struct Region {
    std::string type = kTypeWindow;
    Shape shape = Shape::Poly;
    // Poly: u,v,u,v,... ; Rect: u0,v0,u1,v1 ; Ellipse: centro u,v e raios u,v.
    std::vector<float> pts;
    // Bitmap (pincel/borracha, varinha): cobertura 0..255 em bw x bh. Vai para
    // um PNG ao lado do arquivo do mapa; `bitmapFile` e' o nome dele.
    std::string bitmapFile;
    std::vector<uint8_t> bitmap;
    int bw = 0, bh = 0;

    // Brilho (window, lamp).
    float color[3] = {1.0f, 0.72f, 0.36f};
    float intensity = 1.0f; // multiplica a cor; o resultado e' cortado em 1 (8 bits)
    Glow glow = Glow::Dark;
    // Horario de acender e apagar (hora do dia, 0..24); negativo = so' a curva
    // do sol do motor, como as luzes artificiais de sempre.
    float onHour = -1.0f, offHour = -1.0f;
    // Cintilacao: vale na luz do poste e na textura emissiva (o shader multiplica
    // a emissiva da imagem por um fator por quadro; uma imagem tem um fator so',
    // o da regiao acesa de maior cintilacao).
    float flicker = 0.0f, flickerSpeed = 6.0f;

    // Luz pontual (lamp): vive no mundo, entre as luzes do mapa; aqui ficam
    // so' os valores com que o editor cria a luz.
    float lightRadius = 40.0f, lightIntensity = 3.0f;
    // Brilho do objeto em volta da luz: a superficie a ate' glowRadius dela
    // (unidades) acende com a cor da luz vezes glowStrength. 0 = desligado.
    float glowRadius = 0.0f, glowStrength = 0.0f;
};

struct ImageRegions {
    std::string image; // nome da imagem do glTF (como o ModelFile a chama)
    bool enabled = true;
    std::vector<Region> regions;
};

// A regiao esta acesa na hora dada (0..24)?
bool activeAt(const Region& r, float hour);

// Cobertura 0..1 da regiao em w x h (pixels). Poligono pelo par-impar.
void rasterize(const Region& r, int w, int h, std::vector<float>& coverage);

// Converte a regiao em Bitmap (para pincel e borracha editarem pixel a pixel).
void toBitmap(Region& r, int w, int h);
// Carimbo redondo no bitmap (raio em pixels); add=false apaga.
void stamp(Region& r, float cx, float cy, float radiusPx, bool add);
// Traco de (x0,y0) a (x1,y1) em pixels, carimbado a cada meio raio.
void strokeLine(Region& r, float x0, float y0, float x1, float y1, float radiusPx, bool add);
// Varinha magica: pixels com cor a ate' `tolerance` (0..255, por canal) da cor
// do pixel clicado. contiguous: so' os ligados a ele (preenchimento); falso:
// a imagem inteira (todo pixel parecido, ligado ou nao).
void wandFill(const uint8_t* rgba, int w, int h, int channels, int sx, int sy, int tolerance, bool contiguous,
              Region& out);

// Luminancia 0..1 de um pixel (mesmos pesos do script).
float luminance(const uint8_t* px);

// Mascara emissiva RGB8 (w*h*3) a partir do albedo: soma (maximo) das regioes
// de brilho ativas na hora `hour` (negativa = ignora o horario). O desfoque
// leve (sigma 0,6) e' o do script.
void buildEmissive(const uint8_t* albedo, int w, int h, int channels, const ImageRegions& img, float hour,
                   std::vector<uint8_t>& rgb);

// Mapa de superficie (w*h, 0 = nada; N = 1 + indice do tipo em surfaceTypes).
// Gancho: o motor ainda nao liga os tipos a pegada/efeito, so' guarda.
std::vector<std::string> surfaceTypes(const ImageRegions& img);

// LACO MAGNETICO (live-wire, Mortensen e Barrett, SIGGRAPH 95). Custo local
// por pixel: cruzamento por zero do laplaciano (0,43), magnitude do gradiente
// de Sobel invertida (0,43) e direcao do gradiente (0,14). Dijkstra a partir
// do ultimo ponto fixado, so' numa janela em volta dele.
class LiveWire {
public:
    // Prepara os custos de uma imagem (reduzida para no maximo `maxSide`).
    void setImage(const uint8_t* rgba, int w, int h, int channels, int maxSide = 512);
    // Fixa a semente (UV) e calcula as distancias na janela.
    void setSeed(float u, float v, int windowRadiusPx = 160);
    // Caminho da semente ate (u,v), em UV; vazio se fora da janela.
    std::vector<float> pathTo(float u, float v) const;
    bool ready() const { return m_w > 0; }

private:
    int m_w = 0, m_h = 0;
    std::vector<float> m_fz, m_fg, m_dx, m_dy; // custos e direcao (perpendicular ao gradiente)
    std::vector<float> m_dist;
    std::vector<int32_t> m_prev;
    int m_seed = -1, m_x0 = 0, m_y0 = 0, m_x1 = 0, m_y1 = 0;
    float linkCost(int p, int q) const;
};

} // namespace eruption::texregion
