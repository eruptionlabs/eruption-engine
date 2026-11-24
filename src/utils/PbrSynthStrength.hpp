#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace eruption {

// FORCA DA SINTESE ANALITICA DE PBR (relevo derivado do albedo).
//
// heightFromAlbedo() inventa relevo a partir da imagem. Numa foto de material
// (rocha, terra, folha, 512 px ou mais) isso acompanha o que a luz mostra. Numa
// textura PINTADA a mao em baixa resolucao (64..256 px, sombra pintada dentro
// da imagem, ampliada varias vezes na tela) a mesma conta le' o desenho como
// relevo: cada telha, tijolo e pincelada vira degrau, e o telhado inteiro sai
// com cara de papel alumínio amassado e mais escuro (AO da cavidade). O relevo
// so' e' confiavel quando ha' detalhe real por texel; entao a forca (0..1) e'
// medida POR TEXTURA, no bake, e escala a inclinacao da normal, a amplitude
// da altura e o escurecimento da cavidade. O albedo nao e' tocado.
//
// Dois sinais, medidos em 173 texturas pintadas e 163 fotograficas de dois
// projetos (numeros na funcao, em PbrSynthStrength.cpp):
//   1. Paleta. Textura pintada vem indexada (<= 256 cores distintas fora do
//      preto de fundo); foto tem milhares. Nas amostras: pintada 213..254 cores
//      (5 de 173 passam de 14 mil: sao fotograficas de fato), foto 2086 ou
//      mais. A banda 400..1500 cores faz a transicao.
//   2. Resolucao (menor lado). Pintada: ~0 ate' 256 px, sobe ate' 1 em 1024
//      (ampliada menos, o grao de um texel ja' nao e' do tamanho de um tijolo).
//      Qualquer textura: ~0 abaixo de 32 px, cheia a partir de 96.
// A energia de alta frequencia (grao de 1 texel) e a fracao de variancia na
// banda baixa (sombra pintada) foram medidas e NAO separam os grupos (foto de
// terra tem tanto grao quanto pintura; o atlas de foto com fundo preto tem mais
// banda baixa que a pintura), entao ficaram de fora.
struct PbrSynthSignals {
    int side = 0;          // menor lado analisado
    int colors = 0;        // cores distintas (sem o preto de fundo)
    float photo = 0.0f;    // 0 = paleta indexada, 1 = fotografica
    float resolution = 0.0f;
    float strength = 1.0f; // resultado, 0..1
};

// Forca automatica de uma textura (pixels RGB ou RGBA 8 bits).
float pbrAutoStrength(const uint8_t* pixels, int width, int height, int channels,
                      PbrSynthSignals* signals = nullptr);

// Botao global (data/graphics.json, "pbr_synthesis_strength", 0..1, padrao 1):
// multiplica a forca de toda textura sintetizada. Entra na chave do pacote
// assado (o pacote muda quando o botao muda).
void setPbrSynthesisStrength(float strength);
float pbrSynthesisStrength();

// Sidecar por modelo: "<modelo>.pbr.json" ao lado do .glb ("casa.glb" ->
// "casa.pbr.json"):
//   { "strength": 0..1, "materials": { "nome_do_material": 0..1 } }
// Os valores SUBSTITUEM a forca automatica (1 = a sintese cheia de antes, 0 =
// chapado); o material vence o modelo, o modelo vence o automatico. O botao
// global ainda multiplica. Chaves ausentes = automatico.
struct PbrSidecar {
    bool present = false;
    float strength = -1.0f;                                   // < 0: automatico
    std::vector<std::pair<std::string, float>> materials;    // nome -> forca
    uint32_t hash = 0;                                        // do conteudo, p/ a chave do pacote

    // < 0 quando nada se aplica ao material (usa o automatico).
    float forMaterial(const std::string& name) const;
};

// Parse puro (testavel). Falha de JSON ou chave de tipo errado = ignora a chave.
PbrSidecar parsePbrSidecar(const std::string& jsonText);
// Le' "<glb sem extensao>.pbr.json" (disco ou pacote do mapa); sem arquivo,
// present = false.
PbrSidecar loadPbrSidecar(const std::string& glbPath);

} // namespace eruption
