#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace eruption {

// Simplificação de malha para LOD por distância.
//
// Motivo medido (parana_field, tempestade): o passe principal submete até
// 3.902.788 triângulos por frame vindos de apenas 713 instâncias visíveis -
// malhas de árvore com ~17k vértices instanciadas centenas de vezes.
// Compartilhar geometria entre instâncias resolveu MEMÓRIA (91M -> 583k
// vértices), mas não reduz o que é RASTERIZADO: cada instância continua
// desenhando a malha inteira. Só LOD reduz isso, e é o que decide se o mapa
// roda na GeForce 930M.
//
// A saída é apenas um NOVO INDEX BUFFER sobre o MESMO vertex buffer, então o
// custo de VRAM é uma fração do índice original, não uma cópia da malha.
//
// Detalhe importante: a malha que o parser entrega é DESINDEXADA (um vértice
// por canto de triângulo), e nenhum simplificador consegue colapsar uma malha
// assim - não há aresta compartilhada. Por isso soldamos por posição antes de
// simplificar. Ver docs/optimization_research.md (meshoptimizer, zeux).
// LIMITAÇÃO CONHECIDA (medida): em folhagem a métrica de erro geométrico
// subestima a mudança percebida. Tirar cartões de folha quase não move o erro,
// mas muda a silhueta visivelmente. Em castle_landscape todo o RMS de 3,178
// entre LOD ligado e desligado vem de UMA árvore; o resto do quadro é idêntico.
// Consequência prática: a distância de troca, que sai do erro, fica pequena
// para folhagem e o LOD entra cedo nela. Um simplificador ciente de cobertura
// alfa resolveria; não foi feito.
struct MeshLodResult {
    std::vector<uint32_t> indices;
    float error = 0.0f;      // erro geométrico relatado, em unidades de mundo
    bool valid = false;
    // Diagnostico de FOLHAGEM (ver MeshCardStats). Preenchido sempre.
    bool cardMesh = false;   // malha dominada por cartoes de alfa
    float areaKept = 1.0f;   // fracao da AREA original preservada pelo LOD
    uint32_t islandsKept = 0, islandsTotal = 0;
};

// Estatistica de TOPOLOGIA usada para reconhecer folhagem sem depender de
// nome de material.
//
// Uma folha, numa biblioteca CC0 qualquer, e' um QUAD (2 triangulos) com uma
// textura recortada por alfa. O quad e' uma ILHA: nao compartilha aresta com
// nada. Uma malha de arvore e' entao um tronco (uma ilha grande) mais milhares
// de ilhas de 2 triangulos.
//
// Isso importa porque decide o que a simplificacao PODE fazer:
//  - meshopt_simplify preserva topologia e trava bordas. Num quad TODA aresta
//    e' borda, entao ele nao consegue colapsar nada - devolve a malha inteira.
//  - meshopt_simplifySloppy ignora topologia: ele agrupa vertices por CELULA
//    ESPACIAL e funde cartoes VIZINHOS MAS DISTINTOS num triangulo so'. O UV
//    do resultado vem de um vertice arbitrario da celula, entao o recorte por
//    alfa passa a cortar um pedaco que nao tem nada a ver com a geometria.
//    E' exatamente o "as folhas ficam absurdamente estranhas ignorando o alfa".
struct MeshCardStats {
    uint32_t islands = 0;          // componentes conexas (por posicao soldada)
    uint32_t cardIslands = 0;      // ilhas com <= kCardMaxTris triangulos
    float cardTriFraction = 0.0f;  // fracao dos triangulos que vive em cartoes
    float cardAreaFraction = 0.0f; // fracao da AREA que vive em cartoes
    bool isCardMesh = false;       // veredito
    // Ilhas GRANDES (> kCardMaxTris): sao PECAS de um objeto montado
    // (parede, telhado, porta), nao cartoes de folhagem.
    uint32_t chunkyIslands = 0;
    // Veredito de ESTRUTURA: objeto montado de poucas pecas macicas. Ver a
    // nota em MeshLod.cpp - e' a malha em que o simplifySloppy solda peca
    // com peca e embaralha UV.
    bool isStructure = false;
};

// Analisa a topologia (solda + componentes conexas). Mesmos parametros de
// posicao/stride de simplifyMesh.
MeshCardStats analyzeCards(const float* positions,
                           size_t vertexCount,
                           size_t stride,
                           const std::vector<uint32_t>& indices,
                           size_t uvByteOffset = 0,
                           size_t texIdxByteOffset = 0);

// positions: base do array de posições (float x,y,z) dentro do vértice;
// stride: tamanho do vértice em bytes.
// ratio: fração do número de índices a manter (0.25 = um quarto).
// maxTriangles: TETO ABSOLUTO de triangulos do LOD (0 = so' a fracao).
// A fracao sozinha nao basta: no parana_field duas malhas de ~106k triangulos
// sao instanciadas 190 e 138 vezes, e reduzir 82% ainda deixa 19k CADA - o que
// da' 6,07 milhoes de triangulos so' com essas duas, 84% do passe inteiro.
// Uma malha desenhada centenas de vezes precisa de um teto por INSTANCIA, nao
// de uma porcentagem da propria complexidade.
MeshLodResult simplifyMesh(const float* positions,
                           size_t vertexCount,
                           size_t stride,
                           const std::vector<uint32_t>& indices,
                           float ratio,
                           float targetError,
                           uint32_t maxTriangles = 0,
                           // Offsets (em bytes, a partir do ponteiro de posicao)
                           // do UV (vec2) e do indice de textura (uint32) no
                           // vertice. 0 = ausente. Sem eles a solda e' por
                           // posicao pura e TROCA UV/textura em costuras.
                           size_t uvByteOffset = 0,
                           size_t texIdxByteOffset = 0,
                           // FOLHAGEM: quando a malha e' feita de cartoes com
                           // recorte por alfa, o LOD NAO pode colapsar
                           // geometria - tem que remover CARTOES INTEIROS.
                           // `alphaHint` forca esse caminho (vem do nome do
                           // material / categoria PBR); a topologia liga
                           // sozinha quando reconhece cartoes, mesmo sem hint.
                           bool alphaHint = false,
                           // Piso de AREA preservada no caminho de cartoes.
                           // O teto de triangulos e' ignorado abaixo deste
                           // piso: cortar 99% dos cartoes de uma copa nao e'
                           // "menos detalhe", e' a arvore sumindo.
                           float minAreaKeep = 0.6f);

// PODA DE CARTOES COM COMPENSACAO DE AREA (Cook, Halstead, Planck, Ryu 2007,
// "Stochastic Simplification of Aggregate Detail", Pixar).
//
// O caminho de cartao do simplifyMesh so' REMOVE cartoes, e por isso precisa
// do piso de area (minAreaKeep): sem ele a copa afina ate' sumir. Com o piso,
// o teto de triangulos e' letra morta em arvore densa - medido no parana_demo,
// uma arvore de 107 k tris parava em 45 k (piso 60%) e 155 instancias dela
// somavam 5,4 M tris por frame, ~5 triangulos por FRAGMENTO a pitch 30.
//
// Aqui a remocao e' a mesma (ilhas inteiras, recorte por alfa intacto), mas os
// cartoes SOBREVIVENTES CRESCEM em torno do proprio centroide por
// sqrt(area_total / area_mantida) (clamp em maxScale): a cobertura da copa se
// conserva, entao o teto pode ser honrado de verdade. Selecao por hash da
// ilha -> deterministica e ANINHADA: o conjunto de um teto menor e' subconjunto
// do teto maior, o que mantem a transicao lo->far coerente (nenhum cartao
// "volta" ao afastar). Pecas macicas (tronco, > 8 tris por ilha) ficam sempre.
//
// Como os vertices se movem, a saida e' um vertex buffer NOVO (compactado, so'
// os vertices referenciados, mesmo stride) + indices para ele.
struct CardPruneResult {
    std::vector<unsigned char> vertices; // vertices compactados, stride de entrada
    size_t vertexCount = 0;
    std::vector<uint32_t> indices;       // indexam `vertices`
    float scale = 1.0f;                  // fator aplicado aos cartoes mantidos
    float areaKeptRaw = 1.0f;            // fracao da area de cartoes ANTES de escalar
    uint32_t islandsKept = 0, islandsTotal = 0;
    bool valid = false;
};
CardPruneResult pruneCardsAreaPreserving(const void* vertexBytes,
                                          size_t vertexCount,
                                          size_t stride,
                                          size_t posByteOffset,   // offset da posicao no vertice
                                          const std::vector<uint32_t>& indices,
                                          uint32_t maxTriangles,  // teto ABSOLUTO do resultado
                                          size_t uvByteOffset = 0,      // absolutos no vertice; 0 = ausente
                                          size_t texIdxByteOffset = 0,
                                          float maxScale = 4.0f);

// true quando a engine foi compilada com meshoptimizer disponível.
bool meshLodAvailable();

// Reindexa uma malha DESINDEXADA (um vértice por canto de triângulo, que é o
// que o GltfParser produz) soldando vértices BITWISE idênticos e reordenando
// para o cache pós-transform.
//
// Motivo medido: o passe GBuffer custa 3,41 / 3,44 / 3,55 ms para 0,31 / 1,31 /
// 2,30 Mpx - **não escala com resolução**, logo não é fragment-bound, é
// vértice. Malha desindexada roda o vertex shader uma vez por CANTO em vez de
// uma vez por vértice (~2,3x a mais aqui) e zera o reuso do cache
// (ACMR 1.0 em vez de ~0.6; Forsyth, Kerbl et al. em
// docs/optimization_research.md).
//
// A solda é por igualdade de bytes do vértice inteiro, então é LOSSLESS: a
// imagem não muda. Roda no upload, depois de smoothNormalsByAngle e
// addSilhouetteBevels, então não interfere no pós-processamento do parser.
//
// vertexBytes: ponteiro para o array de vértices; count: quantos; stride:
// tamanho do vértice. Devolve false quando não vale a pena ou meshoptimizer
// não está disponível. Em caso de sucesso, outVertices recebe os vértices
// únicos (mesmo stride) e outIndices os índices.
bool reindexMesh(const void* vertexBytes, size_t count, size_t stride,
                 const std::vector<uint32_t>& indices,
                 std::vector<unsigned char>& outVertices,
                 std::vector<uint32_t>& outIndices);

} // namespace eruption
