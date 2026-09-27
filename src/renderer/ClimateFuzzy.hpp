#pragma once

#include "renderer/WeatherTypes.hpp"

#include <string>
#include <vector>

namespace eruption {

// -----------------------------------------------------------------------------
// ClimateFuzzy — selecao de clima por logica difusa sobre estado atmosferico
// -----------------------------------------------------------------------------
// POR QUE ARISTOTELES E' LITERALMENTE A CLASSIFICACAO REAL
//
// Aristoteles classifica a materia por duas qualidades opostas - QUENTE/FRIO e
// UMIDO/SECO - e os quatro elementos sao as quatro combinacoes.
//
// A meteorologia classifica MASSA DE AR exatamente pelos mesmos dois eixos:
// temperatura (Arctic / Polar / Tropical) x umidade (continental = seco /
// maritime = umido), produzindo cA, cP, mP, cT, mT. Nao e' analogia: e' o
// mesmo plano. Uma massa continental tropical (cT) E' o "quente e seco"
// aristotelico, e da' deserto, onda de calor e tempestade de areia. Uma
// maritima tropical (mT) e' "quente e umido", e da' abafado, tempestade
// convectiva e trovoada.
//
// Por isso o sistema nao precisa escolher entre "ser aristotelico" e "ser
// realista" - as duas coisas pedem a mesma estrutura. O que a meteorologia
// acrescenta e' um terceiro eixo que Aristoteles nao tinha: ESTABILIDADE
// atmosferica, que decide se ar umido vira neblina parada ou trovoada, e um
// quarto pratico, o VENTO.
//
// COMO FUNCIONA
//
// O mundo tem um ESTADO atmosferico continuo (temperatura, umidade, vento,
// instabilidade). Cada tipo de clima declara em que REGIAO desse espaco ele
// vive, por funcao de pertinencia trapezoidal em cada eixo. A pertinencia do
// tipo e' o produto das pertinencias por eixo, e o clima ativo e' o de maior
// pertinencia - com os vizinhos entrando na mistura, que e' o que da'
// transicao suave em vez de troca em degrau.
//
// NUCLEO x SUPORTE, e o que "sem overlap" quer dizer
//
// Cada eixo tem quatro pontos: [suporteMin, nucleoMin, nucleoMax, suporteMax].
// Dentro do nucleo a pertinencia e' 1; entre suporte e nucleo ela sobe/desce
// suave; fora do suporte e' 0.
//
//   - Os SUPORTES PRECISAM se sobrepor. E' a sobreposicao deles que produz a
//     transicao continua entre climas vizinhos.
//   - Os NUCLEOS NAO PODEM se sobrepor. Dois climas com nucleo em comum sao
//     ambiguos: existe um estado atmosferico em que os dois estao 100% certos,
//     e a escolha entre eles vira empate arbitrario.
//
// E' isso que `validate()` verifica, e por isso ele so' reclama de nucleo.
struct FuzzyRange {
    // Trapezio: 0 fora de [supportMin, supportMax], 1 dentro de [coreMin, coreMax].
    float supportMin = -1.0f;
    float coreMin    = -1.0f;
    float coreMax    =  1.0f;
    float supportMax =  1.0f;

    float membership(float x) const;
    bool coreOverlaps(const FuzzyRange& o) const;
    // Um eixo declarado como "nao importa" nao restringe nada e tambem nao
    // conta como sobreposicao: senao todo clima colidiria com todo clima nos
    // eixos que nenhum dos dois usa.
    bool isWildcard() const;
};

// Onde um tipo de clima vive no espaco atmosferico.
struct ClimateEnvelope {
    WeatherType type = WeatherType::Clear;
    bool enabled = true;         // fora da selecao automatica quando false
    float priority = 1.0f;       // desempate quando a pertinencia empata

    FuzzyRange temperature;      // -1 (arctico) .. +1 (tropical)
    FuzzyRange humidity;         //  0 (continental/seco) .. 1 (maritimo/umido)
    FuzzyRange wind;             //  0 (calmo) .. 1 (vendaval)
    FuzzyRange instability;      //  0 (estavel) .. 1 (convectivo)

    float membership(const WeatherSignature& s) const;
    // Distancia do estado ao nucleo (0 se dentro). Usada para achar o clima
    // mais proximo quando nenhum envelope cobre o estado.
    float distanceTo(const WeatherSignature& s) const;
    // Centro do nucleo: o estado atmosferico que representa este clima. E' o
    // que o mundo assume quando o autor ESCOLHE um clima na mao.
    WeatherSignature centroid() const;
};

struct ClimateOverlap {
    WeatherType a = WeatherType::Clear;
    WeatherType b = WeatherType::Clear;
};

class ClimateFuzzySet {
public:
    // Carrega os envelopes. Arquivo ausente ou invalido nao e' fatal: a
    // selecao automatica fica desligada e o clima continua sendo escolhido a
    // mao, como antes.
    bool loadFromFile(const std::string& path);
    bool reloadIfModified();

    bool empty() const { return m_envelopes.empty(); }
    const std::vector<ClimateEnvelope>& envelopes() const { return m_envelopes; }
    const ClimateEnvelope* find(WeatherType t) const;

    // Clima mais adequado a um estado atmosferico. `outConfidence` recebe a
    // proximidade ao nucleo do vencedor: 1 dentro dele, caindo suave para fora.
    // Continua em todo o espaco, entao serve para cross-fade sem degrau.
    WeatherType classify(const WeatherSignature& s, float* outConfidence = nullptr) const;

    // Os N mais pertinentes, normalizados para somar 1. E' com isto que se faz
    // mistura suave em vez de troca em degrau.
    struct Blend { WeatherType type; float weight; };
    std::vector<Blend> classifyBlend(const WeatherSignature& s, int maxTypes = 3) const;

    // Pares cujos NUCLEOS se sobrepoem em TODOS os eixos: climas ambiguos.
    std::vector<ClimateOverlap> findOverlaps() const;

    // Fracao do espaco que nenhum envelope cobre ESTRITAMENTE. Informativo, nao
    // defeito: ha' muito mais combinacoes de eixos do que climas nomeados, e
    // classify() resolve esses estados pelo clima mais proximo.
    float coverageGaps(int samplesPerAxis = 7) const;

private:
    std::vector<ClimateEnvelope> m_envelopes;
    std::string m_path;
    long long m_mtime = 0;
};

} // namespace eruption
