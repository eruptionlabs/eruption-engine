#pragma once

#include <cstdint>
#include <vector>

namespace eruption {

// ALTURA E NORMAL A PARTIR DO ALBEDO - fonte unica pros dois bakes (o de
// load, synthesizePbrFromPixels, e o cooker offline, generatePbrMaps).
//
// O que existia: altura = luminancia crua (cooker) ou luminancia menos um
// box de raio 3 (load). Nos dois, "claro = alto" pixel a pixel: rejunte
// branco virava crista, tabua escura virava vala, e um quadrado preto de
// janela virava cratera do tamanho dele (parede com rejunte
// branco e tijolo escuro saia com displacement estranho, inclusive nas
// janelas).
//
// O que se faz aqui segue a literatura classica de altura-a-partir-de-imagem
// (piramide de blur / diferenca de gaussianas + integracao de gradiente;
// Frankot & Chellappa 1988, pesquisa de 2026-09-29):
//  1. Luminancia LINEAR (a textura e' sRGB) e uma mascara de confianca:
//     preto puro (janela, buraco pintado) nao e' superficie, vira 0.
//  2. Delight: tira a banda baixa (blur grande) - sombra pintada grande sai,
//     rejunte e junta de pedra ficam.
//  3. Detalhe multi-escala: 4 bandas de diferenca-de-gaussianas (sigma 1, 2,
//     4, 8), cada uma normalizada pelo contraste LOCAL (RMS) - area clara e
//     area escura com o mesmo padrao relativo dao a mesma amplitude, e o
//     pixel mais escuro do mapa inteiro deixa de mandar na escala.
//  4. FORMA por gradiente (a parte que o autor pediu): Scharr na luminancia,
//     campo de gradiente EDITADO (so' borda forte, magnitude comprimida) e
//     integrado com Frankot-Chellappa (Poisson no dominio da frequencia,
//     textura periodica). Uma linha escura tem gradiente apontando pra
//     dentro dos dois lados - integrado vira SULCO; borda clara vira degrau.
//     E' o "reconhecimento de forma" por gradiente. Passa-alta depois tira a deriva
//     que toda integracao produz.
//  5. Duas alturas, misturadas pelo quanto a textura tem REDE de rejunte:
//     organica (forma + detalhe, tanh) e ESTRUTURAL - rejunte achado por
//     FORMA (linha fina pela Hessiana, escura OU clara, a polaridade que
//     forma a rede dominante), distancia exata ate' ele e perfil arredondado
//     a partir da borda: rejunte no fundo, pedra/tijolo alto, sem depender da
//     cor de cada pedra. Deviacao multiplicada pela confianca.
//  6. Normal HIBRIDA, multi-escala, a partir de h (nao da luminancia - luz e
//     relevo descrevem a MESMA superficie). Nao e' um Sobel de 3x3 por pixel:
//     e' a soma de gradientes de h em varias escalas (sigma 0.7 .. 12
//     texels), cada um com peso sigma^gamma (derivada normalizada por
//     escala, Lindeberg 1998 - sem isso a escala grossa some) e com uma
//     confianca por escala que comprime outlier e levanta linha fraca
//     (Fattal, Lischinski & Werman 2002). Somam-se os gradientes crus e
//     normaliza-se UMA vez no fim (combinacao linear de derivadas, nao slerp).
//     E' o "contexto" que o autor pediu: a normal de um texel depende da
//     vizinhanca em varias escalas, nao so' dos 8 vizinhos.
//     normalShapeMix: 0 = so' escala fina (grao), 1 = so' escala grossa
//     (forma), default 0.4.
//  7. Cavidade (AO): contraste local de h, so' o lado negativo.
//
// Referencias (artigos, liberados pra implementar):
//   Frankot & Chellappa 1988, "A method for enforcing integrability in shape
//     from shading algorithms", IEEE PAMI 10(4).
//   Lindeberg 1998, "Feature detection with automatic scale selection", IJCV.
//   Fattal, Lischinski & Werman 2002, "Gradient domain high dynamic range
//     compression", SIGGRAPH.
//   Agrawal, Raskar & Chellappa 2006, "What is the range of surface
//     reconstructions from a gradient field?", ECCV (integracao ponderada).
//   Queau, Durou & Aujol 2017, "Normal integration: a survey", JMIV.
//   Johnston 2002, "Lumo: illumination for cel animation", NPAR (normais de
//     desenho a mao: linha de tinta -> normais opostas -> sulco).
//   Frangi, Niessen, Vincken & Viergever 1998, "Multiscale vessel
//     enhancement filtering", MICCAI (linha fina pela Hessiana).
//   Felzenszwalb & Huttenlocher 2012, "Distance transforms of sampled
//     functions", Theory of Computing (distancia exata).
//   Sykora, Sedlacek, Jinchao, Dingliana & Collins 2010, "Adding depth to
//     cartoons using sparse depth (in)equalities", Eurographics.
struct HeightFromAlbedoResult {
    int width = 0, height = 0;
    std::vector<float> heightMap;   // [0,1], 0.5 = neutro
    std::vector<float> cavity;      // [0.35,1]
    std::vector<uint8_t> normalRgba; // xyz empacotado 0..255, a = 255
    float structureWeight = 0.0f;    // 0 = organico, 1 = rede de rejunte (diagnostico)
};

struct HeightFromAlbedoParams {
    float normalStrength = 3.0f; // inclinacao por unidade de dh/dx (texel)
    float normalShapeMix = 0.2f; // 0 = grao fino, 1 = forma grossa
    float shapeWeight = 0.6f;
    float detailWeight = 0.4f;
    float outputGain = 1.5f;     // dentro do tanh
    // Contexto do MATERIAL (categoria na tabela de perfis): grama, vegetacao,
    // terra e neve sao organicas - nunca viram "pedra com rejunte", mesmo que
    // os rabiscos fechem celulas (a imagem sozinha nao separa isso direito:
    // medido, cor/contraste/regularidade empatam grama com parede).
    bool forceOrganic = false;
};

HeightFromAlbedoResult heightFromAlbedo(const uint8_t* rgba, int width, int height,
                                        const HeightFromAlbedoParams& params = {});

} // namespace eruption
