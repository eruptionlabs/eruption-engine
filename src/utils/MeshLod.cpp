#include "utils/MeshLod.hpp"
#include "core/Logger.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <unordered_map>

#ifdef ERUPTION_HAVE_MESHOPT
#include <meshoptimizer.h>
#endif

namespace eruption {

bool meshLodAvailable() {
#ifdef ERUPTION_HAVE_MESHOPT
    return true;
#else
    return false;
#endif
}

#ifdef ERUPTION_HAVE_MESHOPT
namespace {

// Solda vértices coincidentes por posição. A malha do GltfParser é
// desindexada (node.indices é 0,1,2,3...), então sem isto o simplificador não
// encontra nenhuma aresta compartilhada e não consegue colapsar nada.
// Quantizar em 1e-4 de unidade de mundo junta o que é de fato o mesmo ponto
// sem grudar detalhe real.
// A solda por POSICAO PURA trocava UV e ate' TEXTURA no LOD: numa costura de
// UV (ou na fronteira entre primitivas de materiais diferentes fundidas no
// mesmo stream) ha' dois vertices co-locados com UV/texIndex distintos; soldar
// os dois elege um canonico arbitrario e a ilha inteira desenha com o UV/a
// textura do OUTRO lado da costura - "o UV troca todo" com zoom out
// (reportado pelo autor na casa de vila-A). A chave agora inclui UV
// (quantizado) e texIndex quando o chamador informa os offsets; vertices
// duplicados pelo merge de primitivas do MESMO material continuam soldando.
std::vector<uint32_t> weldByPosition(const float* positions, size_t vertexCount, size_t stride,
                                     size_t uvByteOffset, size_t texIdxByteOffset) {
    std::vector<uint32_t> canonical(vertexCount);
    struct KeyHash { size_t operator()(const std::array<int64_t,4>& k) const {
        size_t h = 1469598103934665603ull;
        for (int64_t v : k) { h ^= static_cast<size_t>(v); h *= 1099511628211ull; }
        return h; } };
    std::unordered_map<std::array<int64_t,4>, uint32_t, KeyHash> seen;
    seen.reserve(vertexCount * 2);
    const uint8_t* base = reinterpret_cast<const uint8_t*>(positions);
    for (size_t i = 0; i < vertexCount; ++i) {
        const float* p = reinterpret_cast<const float*>(base + i * stride);
        const int64_t qx = static_cast<int64_t>(std::llround(p[0] * 10000.0));
        const int64_t qy = static_cast<int64_t>(std::llround(p[1] * 10000.0));
        const int64_t qz = static_cast<int64_t>(std::llround(p[2] * 10000.0));
        int64_t extra = 0;
        if (uvByteOffset > 0) {
            const float* uv = reinterpret_cast<const float*>(base + i * stride + uvByteOffset);
            // 1/1024 de UV: separa costura real sem impedir a solda de
            // duplicatas exatas do parser.
            extra = (static_cast<int64_t>(std::llround(uv[0] * 1024.0)) << 24) ^
                     static_cast<int64_t>(std::llround(uv[1] * 1024.0));
        }
        if (texIdxByteOffset > 0) {
            const uint32_t* t = reinterpret_cast<const uint32_t*>(base + i * stride + texIdxByteOffset);
            extra = (extra << 20) ^ static_cast<int64_t>(*t);
        }
        const std::array<int64_t,4> key{(qx << 21) ^ qy, qz, extra, 0};
        auto it = seen.find(key);
        if (it == seen.end()) {
            seen.emplace(key, static_cast<uint32_t>(i));
            canonical[i] = static_cast<uint32_t>(i);
        } else {
            canonical[i] = it->second;
        }
    }
    return canonical;
}

// ---------------------------------------------------------------------------
// TOPOLOGIA: componentes conexas ("ilhas") sobre a malha soldada.
// ---------------------------------------------------------------------------
// Uma folha e' um QUAD com recorte por alfa, e um quad e' uma ilha de 2
// triangulos que nao encosta em nada. Saber disso e' o que separa "posso
// colapsar aresta" de "so' posso remover cartao inteiro".
constexpr uint32_t kCardMaxTris = 8;      // ate' 8 tris ainda e' cartao/ripa
constexpr float kCardTriFractionMin = 0.30f;
constexpr float kCardAreaFractionMin = 0.15f;

// ESTRUTURA = objeto montado de poucas pecas macicas e desconexas: casa
// (parede + telhado + porta + chamine), poco, portao, carroca. O
// simplifySloppy solda vertice por CELULA ESPACIAL, e numa malha dessas a
// celula fica grossa em relacao a distancia entre as pecas - o telhado
// funde na parede e o UV do resultado sai de um vertice arbitrario. E' o
// defeito reportado: "telhado fora de lugar, em certa distancia o UV buga
// tudo, tudo torto".
//
// Os dois limites existem pra NAO pegar a malha gigante de mapa inteiro
// (castle_landscape: 1,5 M de triangulos e milhares de cascas), que depende
// do sloppy pra ter LOD de distancia e onde a celula e' fina perto do todo.
constexpr uint32_t kStructureMaxParts = 512;     // acima disso e' cena, nao objeto
constexpr size_t   kStructureMaxTris = 200000;   // a maior casa aqui tem 43 k

struct Islands {
    std::vector<uint32_t> parent;       // union-find por VERTICE canonico
    std::vector<uint32_t> triIsland;    // ilha de cada triangulo (raiz)
    std::unordered_map<uint32_t, uint32_t> triCount;  // raiz -> triangulos
    std::unordered_map<uint32_t, float>    area;      // raiz -> area
    float totalArea = 0.0f;
};

uint32_t ufFind(std::vector<uint32_t>& p, uint32_t a) {
    while (p[a] != a) { p[a] = p[p[a]]; a = p[a]; }
    return a;
}

float triArea(const float* positions, size_t stride, uint32_t a, uint32_t b, uint32_t c) {
    const uint8_t* base = reinterpret_cast<const uint8_t*>(positions);
    const float* pa = reinterpret_cast<const float*>(base + size_t(a) * stride);
    const float* pb = reinterpret_cast<const float*>(base + size_t(b) * stride);
    const float* pc = reinterpret_cast<const float*>(base + size_t(c) * stride);
    const float ux = pb[0]-pa[0], uy = pb[1]-pa[1], uz = pb[2]-pa[2];
    const float vx = pc[0]-pa[0], vy = pc[1]-pa[1], vz = pc[2]-pa[2];
    const float cx = uy*vz - uz*vy, cy = uz*vx - ux*vz, cz = ux*vy - uy*vx;
    return 0.5f * std::sqrt(cx*cx + cy*cy + cz*cz);
}

Islands buildIslands(const float* positions, size_t stride,
                     const std::vector<uint32_t>& welded, size_t vertexCount) {
    Islands is;
    is.parent.resize(vertexCount);
    std::iota(is.parent.begin(), is.parent.end(), 0u);
    for (size_t i = 0; i + 2 < welded.size(); i += 3) {
        const uint32_t a = welded[i], b = welded[i+1], c = welded[i+2];
        if (a >= vertexCount || b >= vertexCount || c >= vertexCount) continue;
        uint32_t ra = ufFind(is.parent, a), rb = ufFind(is.parent, b), rc = ufFind(is.parent, c);
        if (ra != rb) { is.parent[rb] = ra; rb = ra; }
        if (ra != rc) { is.parent[rc] = ra; }
    }
    is.triIsland.resize(welded.size() / 3, 0u);
    for (size_t t = 0; t * 3 + 2 < welded.size(); ++t) {
        const uint32_t a = welded[t*3];
        if (a >= vertexCount) { is.triIsland[t] = 0xFFFFFFFFu; continue; }
        const uint32_t root = ufFind(is.parent, a);
        is.triIsland[t] = root;
        ++is.triCount[root];
        const float ar = triArea(positions, stride, welded[t*3], welded[t*3+1], welded[t*3+2]);
        is.area[root] += ar;
        is.totalArea += ar;
    }
    return is;
}

MeshCardStats statsFromIslands(const Islands& is) {
    MeshCardStats st;
    st.islands = static_cast<uint32_t>(is.triCount.size());
    size_t cardTris = 0, allTris = 0;
    float cardArea = 0.0f;
    for (const auto& kv : is.triCount) {
        allTris += kv.second;
        if (kv.second > kCardMaxTris) ++st.chunkyIslands;
        if (kv.second <= kCardMaxTris) {
            ++st.cardIslands;
            cardTris += kv.second;
            auto it = is.area.find(kv.first);
            if (it != is.area.end()) cardArea += it->second;
        }
    }
    st.cardTriFraction = allTris ? float(cardTris) / float(allTris) : 0.0f;
    st.cardAreaFraction = is.totalArea > 0.0f ? cardArea / is.totalArea : 0.0f;
    // DOIS criterios, os dois necessarios: contagem sozinha dispara em malha
    // com lixo solto irrelevante; area sozinha dispara em malha de 3 pecas.
    st.isCardMesh = st.cardIslands >= 16 &&
                    st.cardTriFraction >= kCardTriFractionMin &&
                    st.cardAreaFraction >= kCardAreaFractionMin;
    // Pelo menos duas pecas macicas separadas (telhado solto da parede) e
    // pequeno/simples o bastante pra ser UM objeto, nao uma cena inteira.
    // NAO condicionar a !isCardMesh: o predio era classificado como cartao
    // ANTES desta linha, e a guarda virava codigo morto - medido, so' pegava
    // weed_plant, nettle e anthurium, exatamente o oposto da intencao.
    st.isStructure = st.chunkyIslands >= 2 &&
                     st.chunkyIslands <= kStructureMaxParts &&
                     allTris <= kStructureMaxTris;
    return st;
}

// Remove CARTOES INTEIROS ate' caber no alvo, nunca partindo um cartao.
//
// Ordem de remocao: MENOR AREA primeiro. A distancia em que este LOD e' usado,
// o cartao pequeno e' sub-pixel - some sem que a copa mude de forma - enquanto
// o cartao grande (e o tronco, que e' a maior ilha de todas) e' a silhueta.
// Colapsar aresta faria o contrario: destruiria o recorte de TODOS.
//
// PISO DE AREA: o teto de triangulos so' vale enquanto a copa continua sendo
// uma copa. Abaixo de `minAreaKeep` da area original a remocao para, mesmo que
// o alvo nao tenha sido atingido.
std::vector<uint32_t> dropCards(const std::vector<uint32_t>& welded,
                                const Islands& is, size_t targetIndices,
                                float minAreaKeep, float* outAreaKept,
                                uint32_t* outKept, uint32_t* outTotal) {
    std::vector<std::pair<float, uint32_t>> byArea;  // (area, raiz)
    byArea.reserve(is.area.size());
    for (const auto& kv : is.area) byArea.emplace_back(kv.second, kv.first);
    std::sort(byArea.begin(), byArea.end());

    size_t tris = welded.size() / 3;
    float areaLeft = is.totalArea;
    const float areaFloor = is.totalArea * std::clamp(minAreaKeep, 0.05f, 1.0f);
    std::unordered_map<uint32_t, uint8_t> dropped;
    for (const auto& pr : byArea) {
        if (tris * 3 <= targetIndices) break;
        if (areaLeft - pr.first < areaFloor) break;
        auto itc = is.triCount.find(pr.second);
        if (itc == is.triCount.end()) continue;
        // Nunca remova a MAIOR ilha (tronco/casco): e' a silhueta inteira.
        if (&pr == &byArea.back()) break;
        dropped[pr.second] = 1;
        tris -= itc->second;
        areaLeft -= pr.first;
    }
    std::vector<uint32_t> out;
    out.reserve(tris * 3);
    for (size_t t = 0; t * 3 + 2 < welded.size(); ++t) {
        const uint32_t isl = is.triIsland[t];
        if (isl == 0xFFFFFFFFu || dropped.count(isl)) continue;
        out.push_back(welded[t*3]); out.push_back(welded[t*3+1]); out.push_back(welded[t*3+2]);
    }
    if (outAreaKept) *outAreaKept = is.totalArea > 0.0f ? areaLeft / is.totalArea : 1.0f;
    if (outTotal) *outTotal = static_cast<uint32_t>(is.triCount.size());
    if (outKept) *outKept = static_cast<uint32_t>(is.triCount.size() - dropped.size());
    return out;
}

} // namespace
#endif

MeshCardStats analyzeCards(const float* positions,
                           size_t vertexCount,
                           size_t stride,
                           const std::vector<uint32_t>& indices,
                           size_t uvByteOffset,
                           size_t texIdxByteOffset) {
    MeshCardStats st;
#ifdef ERUPTION_HAVE_MESHOPT
    if (!positions || vertexCount < 3 || indices.size() < 3) return st;
    const std::vector<uint32_t> canonical =
        weldByPosition(positions, vertexCount, stride, uvByteOffset, texIdxByteOffset);
    std::vector<uint32_t> welded(indices.size());
    for (size_t i = 0; i < indices.size(); ++i) {
        const uint32_t src = indices[i];
        welded[i] = (src < canonical.size()) ? canonical[src] : src;
    }
    st = statsFromIslands(buildIslands(positions, stride, welded, vertexCount));
#else
    (void)positions; (void)vertexCount; (void)stride; (void)indices;
    (void)uvByteOffset; (void)texIdxByteOffset;
#endif
    return st;
}

MeshLodResult simplifyMesh(const float* positions,
                           size_t vertexCount,
                           size_t stride,
                           const std::vector<uint32_t>& indices,
                           float ratio,
                           float targetError,
                           uint32_t maxTriangles,
                           size_t uvByteOffset,
                           size_t texIdxByteOffset,
                           bool alphaHint,
                           float minAreaKeep) {
    MeshLodResult out;
#ifdef ERUPTION_HAVE_MESHOPT
    if (!positions || vertexCount < 3 || indices.size() < 3 || indices.size() % 3 != 0) return out;
    if (ratio <= 0.0f || ratio >= 1.0f) return out;

    const std::vector<uint32_t> canonical =
        weldByPosition(positions, vertexCount, stride, uvByteOffset, texIdxByteOffset);
    std::vector<uint32_t> welded(indices.size());
    for (size_t i = 0; i < indices.size(); ++i) {
        const uint32_t src = indices[i];
        welded[i] = (src < canonical.size()) ? canonical[src] : src;
    }

    size_t target = static_cast<size_t>(indices.size() * ratio) / 3 * 3;
    // Teto absoluto: o que decide o custo e' triangulo POR INSTANCIA vezes o
    // numero de instancias, nao a fracao da malha original.
    if (maxTriangles > 0) {
        const size_t cap = static_cast<size_t>(maxTriangles) * 3;
        if (target > cap) target = cap;
    }
    if (target < 3) return out;

    // TOPOLOGIA ANTES DE SIMPLIFICAR. E' o que decide se esta malha aceita
    // colapso de aresta ou se so' aceita remocao de cartao inteiro.
    const Islands islands = buildIslands(positions, stride, welded, vertexCount);
    const MeshCardStats cardStats = statsFromIslands(islands);
    // O caminho de CARTAO apaga ILHAS INTEIRAS (dropCards). O veredito
    // isCardMesh e' puramente TOPOLOGICO - "muitas ilhas pequenas" descreve
    // um telhado de telhas soltas tao bem quanto uma copa de arvore. Medido:
    // dka_telha, dka_madeira, dka_metal e ate' Apple e WellWater caiam aqui,
    // todos com sway 0, e o telhado do castelo SUMIA. Agora o caminho exige
    // alphaHint, que o chamador deriva do swayAmount (folhagem 0,45-1,00,
    // arquitetura 0,00) - topologia sozinha nao decide mais.
    const bool cardPath = alphaHint && (cardStats.isCardMesh || cardStats.cardIslands >= 8);
    // Escape hatch pra medicao de performance: com a guarda ligada, objeto
    // montado reduz menos (so' o que o conservador entrega). Isto permite
    // um A/B honesto sem recompilar a receita do mapa.
    static const bool kNoStructGuard = std::getenv("ERUPTION_LOD_NO_STRUCTURE_GUARD") != nullptr;
    const bool structurePath = cardStats.isStructure && !kNoStructGuard;
    out.cardMesh = cardPath;
    out.islandsTotal = static_cast<uint32_t>(islands.triCount.size());
    out.islandsKept = out.islandsTotal;

    static const bool kCardDbg = std::getenv("ERUPTION_DEBUG_LODCARDS") != nullptr;

    std::vector<uint32_t> simplified(indices.size());
    float resultError = 0.0f;
    size_t n = 0;
    if (uvByteOffset > 0) {
        // COM UV. O simplify que so' ve posicao colapsa aresta interna sem
        // saber o que aquilo custa na textura: numa parede com UV que
        // REPETE (os predios chegam a 10 repeticoes), o triangulo
        // sobrevivente passa a cobrir um trecho de UV muito maior, a
        // textura estica e a derivada de UV muda - e' a selecao de mipmap
        // que vai junto, o "mipmap zoado quando o LOD entra". Passar UV
        // como atributo faz o simplificador pagar por essa distorcao.
        const float* attrs = reinterpret_cast<const float*>(
            reinterpret_cast<const uint8_t*>(positions) + uvByteOffset);
        const float weights[2] = {0.5f, 0.5f};
        n = meshopt_simplifyWithAttributes(simplified.data(),
                                           welded.data(), welded.size(),
                                           positions, vertexCount, stride,
                                           attrs, stride, weights, 2,
                                           nullptr, target, targetError, 0, &resultError);
    }
    if (n == 0) {
        n = meshopt_simplify(simplified.data(),
                             welded.data(), welded.size(),
                             positions, vertexCount, stride,
                             target, targetError, 0, &resultError);
    }

    // ------------------------------------------------------------------
    // CAMINHO DE FOLHAGEM (cartoes com recorte por alfa)
    // ------------------------------------------------------------------
    // Aqui o sloppy esta' PROIBIDO. Ele funde vertices por celula espacial e
    // gruda cartoes vizinhos num triangulo so'; como o UV do resultado sai de
    // um vertice arbitrario, o `discard` do model.frag passa a recortar uma
    // regiao sem relacao com a geometria. E' a queixa literal do autor:
    // "as folhas ficam absurdamente estranhas ignorando totalmente o alfa".
    //
    // O que sobra e' honesto: o quad e' irredutivel (toda aresta e' borda,
    // meshopt_simplify nao encosta nele), entao a unica reducao possivel e'
    // REMOVER CARTOES INTEIROS. Cada cartao que fica continua com o recorte
    // exato do original, e a copa so' AFINA em vez de virar poligono torto.
    if (cardPath) {
        std::vector<uint32_t> kept = dropCards(welded, islands, target, minAreaKeep,
                                               &out.areaKept, &out.islandsKept,
                                               &out.islandsTotal);
        if (kept.size() >= 3 && kept.size() < indices.size()) {
            simplified.assign(kept.begin(), kept.end());
            n = kept.size();
            // O erro geometrico do meshopt nao descreve remocao de cartao
            // (nenhum vertice se moveu). O que muda e' COBERTURA. Reporta um
            // erro proporcional a area perdida vezes o raio tipico do cartao,
            // para que a distancia de troca cresca quando se remove muito.
            resultError = std::max(resultError, (1.0f - out.areaKept) * 0.25f);
        } else {
            if (kCardDbg)
                ERUPTION_LOG_WARN("[LODCARDS] cartoes: nada a remover (tris=%zu alvo=%zu)",
                                  welded.size()/3, target/3);
            return out;
        }
        if (kCardDbg)
            ERUPTION_LOG_WARN("[LODCARDS] CARTAO ilhas=%u (cartoes=%u tri%%=%.2f area%%=%.2f) "
                              "tris %zu -> %zu, area preservada %.2f, hint=%d",
                              cardStats.islands, cardStats.cardIslands,
                              cardStats.cardTriFraction, cardStats.cardAreaFraction,
                              welded.size()/3, n/3, out.areaKept, (int)alphaHint);
    } else

    // meshopt_simplify preserva topologia e trava bordas. A malha que vem do
    // GltfParser é um merge de várias primitivas num único stream, então tem
    // muitas cascas desconexas - e o simplificador conservador mal encosta
    // nela: medido em castle_landscape, 1.484.191 -> 1.284.555 triângulos,
    // apenas 13% com 65% pedidos. Para LOD de DISTÂNCIA isso não serve.
    //
    // simplifySloppy ignora topologia e atinge o alvo de forma confiável. A
    // silhueta fica pior de perto, que é exatamente onde este LOD nunca é
    // usado. Só caímos nele quando o conservador não entregou - e NUNCA em
    // malha de cartoes (ver acima).
    if (n > target + target / 2 && structurePath) {
        // ESTRUTURA: para aqui. O conservador entrega menos reducao do que o
        // alvo pedido, e tudo bem - predio derretido a media distancia e' um
        // defeito visivel, LOD 20% mais pesado num punhado de instancias nao
        // e'. Mesma escolha ja' feita pro cartao de folhagem logo acima.
        if (kCardDbg)
            ERUPTION_LOG_WARN("[LODCARDS] ESTRUTURA: sloppy bloqueado (pecas=%u tris=%zu -> %zu, "
                              "alvo %zu)", cardStats.chunkyIslands, welded.size()/3, n/3, target/3);
    } else if (n > target + target / 2) {
        std::vector<uint32_t> sloppy(indices.size());
        float sloppyError = 0.0f;
        const size_t ns = meshopt_simplifySloppy(sloppy.data(),
                                                 welded.data(), welded.size(),
                                                 positions, vertexCount, stride,
                                                 target, targetError * 4.0f, &sloppyError);
        if (ns >= 3 && ns < n) {
            simplified.swap(sloppy);
            n = ns;
            resultError = sloppyError;
        }
    }

    if (n < 3 || n >= indices.size()) return out; // não valeu a pena
    simplified.resize(n);

    // Ordem amigável ao cache pós-transform (Forsyth / meshopt): a malha de LOD
    // é desenhada muitas vezes, então a ordem importa tanto quanto o tamanho.
    meshopt_optimizeVertexCache(simplified.data(), simplified.data(), n, vertexCount);

    out.indices = std::move(simplified);
    out.error = resultError;
    out.valid = true;
#else
    (void)positions; (void)vertexCount; (void)stride;
    (void)indices; (void)ratio; (void)targetError;
#endif
    return out;
}


bool reindexMesh(const void* vertexBytes, size_t count, size_t stride,
                 const std::vector<uint32_t>& indices,
                 std::vector<unsigned char>& outVertices,
                 std::vector<uint32_t>& outIndices) {
#ifdef ERUPTION_HAVE_MESHOPT
    if (!vertexBytes || count < 3 || indices.size() < 3 || stride == 0) return false;

    std::vector<uint32_t> remap(count);
    const size_t unique = meshopt_generateVertexRemap(remap.data(),
                                                      indices.data(), indices.size(),
                                                      vertexBytes, count, stride);
    if (unique == 0 || unique >= count) return false; // nada a ganhar

    outIndices.resize(indices.size());
    meshopt_remapIndexBuffer(outIndices.data(), indices.data(), indices.size(), remap.data());

    outVertices.resize(unique * stride);
    meshopt_remapVertexBuffer(outVertices.data(), vertexBytes, count, stride, remap.data());

    // Ordem amigável ao cache pós-transform, depois ordem de fetch dos
    // vértices: a primeira reduz invocações do vertex shader, a segunda
    // melhora a localidade da leitura do vertex buffer.
    meshopt_optimizeVertexCache(outIndices.data(), outIndices.data(), outIndices.size(), unique);
    meshopt_optimizeVertexFetch(outVertices.data(), outIndices.data(), outIndices.size(),
                                outVertices.data(), unique, stride);
    return true;
#else
    (void)vertexBytes; (void)count; (void)stride; (void)indices;
    (void)outVertices; (void)outIndices;
    return false;
#endif
}

CardPruneResult pruneCardsAreaPreserving(const void* vertexBytes,
                                          size_t vertexCount,
                                          size_t stride,
                                          size_t posByteOffset,
                                          const std::vector<uint32_t>& indices,
                                          uint32_t maxTriangles,
                                          size_t uvByteOffset,
                                          size_t texIdxByteOffset,
                                          float maxScale) {
    CardPruneResult out;
#ifdef ERUPTION_HAVE_MESHOPT
    if (!vertexBytes || vertexCount < 3 || indices.size() < 3 || indices.size() % 3 != 0 ||
        stride == 0 || maxTriangles == 0) return out;
    const unsigned char* base = static_cast<const unsigned char*>(vertexBytes);
    const float* positions = reinterpret_cast<const float*>(base + posByteOffset);
    // weldByPosition/buildIslands recebem offsets RELATIVOS ao ponteiro de posicao.
    const size_t uvRel  = (uvByteOffset  > posByteOffset) ? uvByteOffset  - posByteOffset : 0;
    const size_t texRel = (texIdxByteOffset > posByteOffset) ? texIdxByteOffset - posByteOffset : 0;
    const std::vector<uint32_t> canonical =
        weldByPosition(positions, vertexCount, stride, uvRel, texRel);
    std::vector<uint32_t> welded(indices.size());
    for (size_t i = 0; i < indices.size(); ++i) {
        const uint32_t src = indices[i];
        welded[i] = (src < canonical.size()) ? canonical[src] : src;
    }
    const Islands is = buildIslands(positions, stride, welded, vertexCount);
    const MeshCardStats st = statsFromIslands(is);
    out.islandsTotal = static_cast<uint32_t>(is.triCount.size());
    // Mesmo criterio de entrada do caminho de cartao do simplifyMesh.
    if (!(st.isCardMesh || st.cardIslands >= 8)) return out;

    const size_t totalTris = welded.size() / 3;
    if (totalTris <= maxTriangles) return out; // ja' cabe: nada a fazer

    // O que e' PODAVEL: qualquer ilha que nao seja a MAIOR (tronco/casco) e
    // que seja agregado - ate' kPrunableMaxTris e menos de 5% da area. O
    // criterio de cartao (<= 8 tris) e' apertado demais aqui: medido no
    // island_tree_01, 106 k tris em 9,4 k ilhas (11 tris por ilha - cachos
    // de folhas soldados por posicao); com <= 8 quase tudo virava "macico"
    // e a poda parava no piso, com MAIS triangulos que o dropCards.
    constexpr uint32_t kPrunableMaxTris = 512;
    uint32_t largestRoot = 0, largestTris = 0;
    for (const auto& kv : is.triCount)
        if (kv.second > largestTris) { largestTris = kv.second; largestRoot = kv.first; }
    const float solidAreaMin = is.totalArea * 0.05f;
    struct Card { uint32_t root; uint32_t tris; float area; uint32_t prio; };
    std::vector<Card> cards;
    cards.reserve(is.triCount.size());
    std::unordered_map<uint32_t, uint8_t> prunable;
    size_t solidTris = 0, cardTrisTotal = 0;
    float cardAreaTotal = 0.0f;
    for (const auto& kv : is.triCount) {
        const auto ita = is.area.find(kv.first);
        const float ar = (ita != is.area.end()) ? ita->second : 0.0f;
        if (kv.first != largestRoot && kv.second <= kPrunableMaxTris && ar < solidAreaMin) {
            prunable[kv.first] = 1;
            // Hash de 32 bits do indice canonico da raiz (murmur3 fmix):
            // deterministico por malha, sem correlacao espacial -> a poda e'
            // uniforme sobre a copa, e ordenar por ele da' subconjuntos
            // ANINHADOS entre tetos diferentes.
            uint32_t h = kv.first * 0x9E3779B9u;
            h ^= h >> 16; h *= 0x85EBCA6Bu; h ^= h >> 13; h *= 0xC2B2AE35u; h ^= h >> 16;
            cards.push_back({kv.first, kv.second, ar, h});
            cardTrisTotal += kv.second;
            cardAreaTotal += ar;
        } else {
            solidTris += kv.second;
        }
    }
    if (cards.empty() || cardAreaTotal <= 0.0f) return out;

    // PECAS MACICAS (tronco, galhos grossos): simplificacao CONSERVADORA ate'
    // METADE do teto, para a copa ficar com a outra metade. Sem isto o tronco
    // engole o orcamento: medido, island_tree_01 tem 16 k tris de tronco para
    // um teto de 12 k - a copa caia no piso de 1/64 e a escala nao alcancava.
    // Tronco e' malha fechada, o simplify que preserva topologia funciona nele
    // (o problema do sloppy era cartao com cartao, nao isto).
    std::vector<uint32_t> solidIdx;
    solidIdx.reserve(solidTris * 3);
    for (size_t t = 0; t < is.triIsland.size(); ++t) {
        const uint32_t isl = is.triIsland[t];
        if (isl == 0xFFFFFFFFu || prunable.count(isl)) continue;
        solidIdx.push_back(welded[t * 3]);
        solidIdx.push_back(welded[t * 3 + 1]);
        solidIdx.push_back(welded[t * 3 + 2]);
    }
    const size_t solidBudget = std::max<size_t>(maxTriangles / 2, 64) * 3;
    if (solidIdx.size() > solidBudget) {
        std::vector<uint32_t> simp(solidIdx.size());
        float err = 0.0f;
        size_t n = 0;
        if (uvRel > 0) {
            const float* attrs = reinterpret_cast<const float*>(
                reinterpret_cast<const unsigned char*>(positions) + uvRel);
            const float weights[2] = {0.5f, 0.5f};
            n = meshopt_simplifyWithAttributes(simp.data(), solidIdx.data(), solidIdx.size(),
                                               positions, vertexCount, stride,
                                               attrs, stride, weights, 2, nullptr,
                                               solidBudget, 1.0f, 0, &err);
        }
        if (n == 0) {
            n = meshopt_simplify(simp.data(), solidIdx.data(), solidIdx.size(),
                                 positions, vertexCount, stride, solidBudget, 1.0f, 0, &err);
        }
        if (n >= 3 && n < solidIdx.size()) {
            simp.resize(n);
            solidIdx.swap(simp);
        }
    }
    solidTris = solidIdx.size() / 3;

    // Orcamento dos cartoes = teto menos o que as pecas macicas ja' gastam.
    // Piso de 1/64 dos cartoes: abaixo disso nao ha' escala que devolva uma
    // copa - vira tronco pelado com meia duzia de folhas gigantes.
    size_t budget = (maxTriangles > solidTris) ? (maxTriangles - solidTris) : 0;
    budget = std::max(budget, cardTrisTotal / 64);
    std::sort(cards.begin(), cards.end(),
              [](const Card& a, const Card& b) { return a.prio < b.prio; });
    std::unordered_map<uint32_t, uint8_t> kept;
    size_t keptTris = 0;
    float keptArea = 0.0f;
    for (const Card& c : cards) {
        if (keptTris > 0 && keptTris + c.tris > budget) break;
        kept[c.root] = 1;
        keptTris += c.tris;
        keptArea += c.area;
    }
    if (keptArea <= 0.0f || kept.size() == cards.size()) return out;
    out.areaKeptRaw = keptArea / cardAreaTotal;
    out.scale = std::clamp(std::sqrt(cardAreaTotal / keptArea), 1.0f, std::max(1.0f, maxScale));
    out.islandsKept = static_cast<uint32_t>(kept.size() + (is.triCount.size() - cards.size()));

    // Centroide de cada cartao mantido (media dos vertices canonicos unicos).
    struct Cent { float s[3] = {0.0f, 0.0f, 0.0f}; uint32_t n = 0; };
    std::unordered_map<uint32_t, Cent> cent;
    std::vector<unsigned char> seenV(vertexCount, 0);
    const size_t triTotal = is.triIsland.size();
    for (size_t t = 0; t < triTotal; ++t) {
        const uint32_t isl = is.triIsland[t];
        if (isl == 0xFFFFFFFFu || !kept.count(isl)) continue;
        for (int k = 0; k < 3; ++k) {
            const uint32_t v = welded[t * 3 + k];
            if (v >= vertexCount || seenV[v]) continue;
            seenV[v] = 1;
            float p[3];
            std::memcpy(p, base + size_t(v) * stride + posByteOffset, sizeof(p));
            Cent& c = cent[isl];
            c.s[0] += p[0]; c.s[1] += p[1]; c.s[2] += p[2]; ++c.n;
        }
    }

    // Saida COMPACTADA: so' vertices referenciados; cartao mantido e' escalado
    // no proprio centroide (UV intacto -> o recorte de alfa cresce junto, que
    // e' o efeito desejado: menos folhas, cada uma maior).
    std::vector<uint32_t> remap(vertexCount, 0xFFFFFFFFu);
    out.indices.reserve((solidTris + keptTris) * 3);
    out.vertices.reserve((solidTris + keptTris) * 2 * stride);
    // Copia o vertice `v` uma vez so'; com centroide (`c`), escala a posicao
    // em torno dele. Ilhas sao disjuntas em vertices, entao um vertice nunca
    // pertence a dois cartoes (nem a cartao e tronco ao mesmo tempo).
    auto emitVertex = [&](uint32_t v, const Cent* c) -> uint32_t {
        if (remap[v] == 0xFFFFFFFFu) {
            remap[v] = static_cast<uint32_t>(out.vertices.size() / stride);
            const size_t off = out.vertices.size();
            out.vertices.insert(out.vertices.end(),
                                base + size_t(v) * stride, base + size_t(v + 1) * stride);
            if (c && c->n > 0 && out.scale > 1.0f) {
                const float inv = 1.0f / float(c->n);
                float p[3];
                std::memcpy(p, out.vertices.data() + off + posByteOffset, sizeof(p));
                for (int a = 0; a < 3; ++a) {
                    const float cc = c->s[a] * inv;
                    p[a] = cc + (p[a] - cc) * out.scale;
                }
                std::memcpy(out.vertices.data() + off + posByteOffset, p, sizeof(p));
            }
        }
        return remap[v];
    };
    // (a) pecas macicas, ja' simplificadas (indices no vertex buffer original)
    for (size_t i = 0; i + 2 < solidIdx.size(); i += 3) {
        if (solidIdx[i] >= vertexCount || solidIdx[i + 1] >= vertexCount || solidIdx[i + 2] >= vertexCount) continue;
        for (int k = 0; k < 3; ++k) out.indices.push_back(emitVertex(solidIdx[i + k], nullptr));
    }
    // (b) cartoes mantidos, escalados no proprio centroide
    for (size_t t = 0; t < triTotal; ++t) {
        const uint32_t isl = is.triIsland[t];
        if (isl == 0xFFFFFFFFu || !prunable.count(isl) || !kept.count(isl)) continue;
        bool ok = true;
        for (int k = 0; k < 3; ++k) if (welded[t * 3 + k] >= vertexCount) ok = false;
        if (!ok) continue;
        const auto itcen = cent.find(isl);
        const Cent* c = (itcen != cent.end()) ? &itcen->second : nullptr;
        for (int k = 0; k < 3; ++k) out.indices.push_back(emitVertex(welded[t * 3 + k], c));
    }
    out.vertexCount = out.vertices.size() / stride;
    if (out.indices.size() < 3 || out.indices.size() >= indices.size() || out.vertexCount == 0) {
        out = CardPruneResult{};
        return out;
    }
    meshopt_optimizeVertexCache(out.indices.data(), out.indices.data(), out.indices.size(), out.vertexCount);
    out.valid = true;
#else
    (void)vertexBytes; (void)vertexCount; (void)stride; (void)posByteOffset; (void)indices;
    (void)maxTriangles; (void)uvByteOffset; (void)texIdxByteOffset; (void)maxScale;
#endif
    return out;
}

} // namespace eruption
