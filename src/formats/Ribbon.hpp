#pragma once
// FAIXA POR SPLINE (estrada, trilha, rio raso). Pontos de controle no plano
// XZ viram uma Catmull-Rom centripeta (aberta ou fechada), amostrada a passo
// constante de comprimento de arco. Da' a malha da faixa (largura variavel,
// UV continua pelo comprimento) e responde onde um ponto esta' em relacao a
// ela: parametro ao longo, deslocamento lateral, distancia 'a borda.
//
// Deterministico entre maquinas: so' soma, produto, divisao e raiz (IEEE,
// arredondamento exato), sem pow/sin/cos da libm e sem contracao em FMA
// (Ribbon.cpp a desliga). Mesma entrada -> mesmos bits de amostra e de malha
// no PC e no ARM, o que deixa usar o hash da malha para conferir que dois
// aparelhos montaram a mesma faixa.
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace eruption {

struct RibbonParams {
    bool closed = false;
    float step = 4.0f;       // metros entre amostras da linha central
    float uvLength = 32.0f;  // metros por repeticao da textura ao longo
};

// Uma amostra da linha central. "Direita" e' o lado +X de quem anda para +Z
// (eixos do motor): para a tangente (tx, tz), a direita e' (tz, -tx).
struct RibbonSample {
    float x = 0.0f, z = 0.0f;   // posicao
    float tx = 0.0f, tz = 1.0f; // tangente unitaria
    float width = 1.0f;
    float s = 0.0f;             // metros desde o inicio
};

struct RibbonQuery {
    bool ok = false;
    float t = 0.0f;        // 0..1 ao longo da faixa
    float s = 0.0f;        // metros desde o inicio
    float lateral = 0.0f;  // com sinal, a partir do centro (+ = direita)
    float edge = 0.0f;     // distancia 'a borda mais proxima (< 0 = fora)
    float width = 0.0f;
    float nx = 0.0f, nz = 0.0f; // da borda mais proxima para dentro
    float tx = 0.0f, tz = 1.0f;
    float height = 0.0f;   // superficie da faixa no ponto (depois de assentar)
    float x = 0.0f, z = 0.0f; // ponto da linha central achado
    int segment = -1;      // segmento da linha central achado
};

// Vertice da malha da faixa (o motor converte para o formato dele).
struct RibbonVertex {
    float px, py, pz;
    float nx, ny, nz;
    float u, v;
};

class Ribbon {
public:
    // points: pares (x, z). widths: um por ponto, ou um so' para todos.
    bool build(const std::vector<float>& pointsXZ, const std::vector<float>& widths, const RibbonParams& params,
               std::string& error);

    const std::vector<RibbonSample>& samples() const { return m_samples; }
    float length() const { return m_length; }
    bool closed() const { return m_closed; }

    // Ponto mais proximo da linha central. Funcao pura: a mesma entrada da'
    // sempre o mesmo resultado (quem re-simula varias vezes por quadro depende
    // disso); empate de distancia fica com o segmento de menor indice.
    RibbonQuery query(float x, float z) const;

    // Ponto da linha central em t (0..1; a fechada da' a volta, a aberta
    // prende nas pontas): posicao, tangente, largura, s e altura no centro.
    RibbonQuery point(float t) const;

    // Malha assentada: height(x, z) devolve o chao; lift e' a folga acima
    // dele. Colunas atravessando a faixa a cada `step` (no minimo 2), para
    // a faixa acompanhar o relevo de lado a lado. Guarda a altura de cada
    // vertice para a consulta devolver height.
    void buildMesh(const std::function<float(float, float)>& height, float lift, std::vector<RibbonVertex>& vertices,
                   std::vector<uint32_t>& indices);

    // FNV-1a das amostras (e das alturas, depois de buildMesh).
    uint64_t hash() const;

private:
    void buildGrid();
    void segmentDistance(int seg, float x, float z, float& d2, float& u) const;
    int segmentCount() const;
    float heightAt(size_t a, size_t b, float u, float lateralFrac) const;

    std::vector<RibbonSample> m_samples;
    std::vector<float> m_heights; // [amostra * m_cols + coluna], depois de buildMesh
    int m_cols = 0;
    float m_length = 0.0f;
    float m_uvLength = 32.0f;
    float m_step = 4.0f;
    bool m_closed = false;

    // Grade uniforme de segmentos: cada celula lista os segmentos cuja caixa
    // (alargada pela meia-largura) a toca.
    float m_gridX0 = 0.0f, m_gridZ0 = 0.0f, m_cell = 1.0f;
    float m_minMargin = 0.0f; // menor alargamento de segmento na grade
    int m_gridW = 0, m_gridD = 0;
    std::vector<uint32_t> m_cellStart, m_cellSegs;
};

} // namespace eruption
