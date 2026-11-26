// Afeccoes do lugar (core/Affects): a tabela data/affects.json, a escolha da
// reacao por elemento x superficie e os dois aneis de teto fixo (afeccoes e
// rajadas). Logica pura, sem GPU.
//
// O que se verifica, e por que cada um importa:
//   1. a tabela do motor le', e todo par elemento x superficie tem reacao  - world.affect
//                                                                            nunca cai no vazio
//   2. a escolha cai na celula certa, ou na "default" do elemento          - a reacao depende
//                                                                            da substancia
//   3. a superficie sai do nome do material/textura e de tipos de regiao   - o chao decide
//   4. tabela torta e' recusada com a causa                                - json de jogo errado
//                                                                            nao derruba o motor
//   5. passou do teto, a afeccao mais velha sai                            - custo limitado
//   6. a afeccao vencida sai do anel, e stop acha o id                     - o anel nao vaza
//   7. rajadas: teto, envelope e frente da onda                            - o vento acaba e
//                                                                            nao pula
//
//   g++ -std=c++17 -I src -I <nlohmann>/include tests/test_affects.cpp src/core/Affects.cpp
#include "core/Affects.hpp"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

using namespace eruption::affects;

static int g_falhas = 0;

static void checa(bool ok, const char* titulo) {
    std::printf("  [%s] %s\n", ok ? "ok" : "FALHA", titulo);
    if (!ok) ++g_falhas;
}

static std::string le(const char* path) {
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// Tabela minima para os testes de escolha: duas superficies e uma reacao por celula.
static const char* kMini = R"({
  "elements": ["fire", "water"],
  "surfaces": ["grass", "snow", "default"],
  "surface_hints": {"grass": ["Grass", "relva"], "snow": ["snow"]},
  "matrix": {
    "fire": {"default": "queima", "snow": "derrete"},
    "water": {"grass": "molha"}
  },
  "reactions": {
    "queima": {"blocks": [{"type": "decal", "texture": "builtin:mk_scorch", "directional": true}]},
    "derrete": {"blocks": [{"type": "particles", "life": [1, 2]}, {"type": "wet", "amount": 0.5}]},
    "molha": {"blocks": [{"type": "wind", "strength": 2}]}
  }
})";

int main(int argc, char** argv) {
    // 1. A tabela do motor.
    std::printf("tabela do motor\n");
    const std::string real = le(argc > 1 ? argv[1] : "data/affects.json");
    Table t;
    std::string err;
    checa(!real.empty() && t.parse(real, &err), "data/affects.json le' sem erro");
    if (!err.empty()) std::printf("    %s\n", err.c_str());
    static const char* kElementos[] = {"fire", "earth", "lightning", "water", "impact", "wind", "ice", "poison", "holy", "shadow"};
    static const char* kSuperficies[] = {"grass", "dirt", "sand", "stone", "snow", "water", "wood", "default"};
    bool todosElementos = true, todosPares = true;
    for (const char* e : kElementos) {
        todosElementos = todosElementos && t.hasElement(e);
        for (const char* s : kSuperficies) {
            const Reaction* r = t.select(e, s);
            if (!r || r->blocks.empty()) todosPares = false;
        }
    }
    checa(todosElementos, "os dez elementos minimos existem");
    checa(todosPares, "todo elemento x superficie tem reacao com blocos");
    const Reaction* fogoNeve = t.select("fire", "snow");
    const Reaction* fogoPedra = t.select("fire", "stone");
    const Reaction* fogoAgua = t.select("fire", "water");
    checa(fogoNeve && fogoPedra && fogoNeve != fogoPedra, "fogo na neve nao e' a reacao do fogo na pedra");
    checa(fogoAgua && fogoAgua != fogoPedra, "fogo na agua tem reacao propria (vapor)");
    bool aguaTemMolhado = false, lamaNaTerra = false;
    if (const Reaction* r = t.select("water", "dirt"))
        for (const Block& b : r->blocks)
            if (b.type == Block::Type::Wet && b.wet.mud) lamaNaTerra = true;
    if (const Reaction* r = t.select("water", "stone"))
        for (const Block& b : r->blocks)
            if (b.type == Block::Type::Wet) aguaTemMolhado = true;
    checa(aguaTemMolhado, "agua na pedra molha");
    checa(lamaNaTerra, "agua na terra vira lama");
    bool vento = false;
    if (const Reaction* r = t.select("wind", "grass"))
        for (const Block& b : r->blocks) vento = vento || b.type == Block::Type::Wind;
    checa(vento, "vento na grama solta rajada");
    bool impactoRaise = false;
    if (const Reaction* r = t.select("impact", "stone"))
        for (const Block& b : r->blocks)
            if (b.type == Block::Type::Raise) impactoRaise = b.raise.minQuality == 2;
    checa(impactoRaise, "o relevo que sobe so' vale na qualidade alta");
    checa(t.select("nao_existe", "grass") == nullptr, "elemento desconhecido nao tem reacao");

    // 2. Escolha na tabela pequena.
    std::printf("escolha da reacao\n");
    Table m;
    checa(m.parse(kMini, &err), "tabela minima le'");
    checa(m.select("fire", "snow") && m.select("fire", "snow")->name == "derrete", "celula exata");
    checa(m.select("fire", "grass") && m.select("fire", "grass")->name == "queima", "sem celula: usa a default do elemento");
    checa(m.select("water", "grass") && m.select("water", "grass")->name == "molha", "outro elemento, outra celula");
    checa(m.select("water", "snow") == nullptr, "elemento sem default e sem celula: nada");
    checa(m.select("fire", "snow")->blocks.size() == 2, "a reacao traz os dois blocos");
    checa(m.select("fire", "grass")->usesDir(), "decal direcional pede o dir do pedido");
    checa(!m.select("fire", "snow")->usesDir(), "sem decal direcional nao precisa de dir");
    const Block& vb = m.select("water", "grass")->blocks[0];
    checa(vb.type == Block::Type::Wind && vb.wind.strength == 2.0f, "parametro do bloco vem da tabela");

    // 3. Superficie pelo nome.
    std::printf("superficie\n");
    checa(m.surfaceFromName("Terrain_GRASS_01.png") == "grass", "dica casa sem diferenciar maiusculas");
    checa(m.surfaceFromName("relva_seca") == "grass", "dica em outra lingua");
    checa(m.surfaceFromName("fresh_snow") == "snow", "neve pelo nome");
    checa(m.surfaceFromName("metal_plate") == "default", "nome sem dica: default");
    checa(m.surfaceFromName("") == "default", "nome vazio: default");
    checa(m.surfaceFromRegion("snow") == "snow", "tipo de regiao que e' superficie");
    checa(m.surfaceFromRegion("window").empty(), "tipo de regiao que nao e' superficie");

    // 4. Tabela torta.
    std::printf("tabela torta\n");
    Table x;
    checa(!x.parse("{ nao e json", &err) && !err.empty(), "JSON quebrado");
    checa(!x.parse(R"({"elements":["a"],"surfaces":["default"],"matrix":{"a":{"default":"zzz"}},"reactions":{}})", &err) &&
              err.find("zzz") != std::string::npos,
          "matriz cita reacao que nao existe");
    checa(!x.parse(R"({"elements":["a"],"surfaces":["default"],"matrix":{"b":{}},"reactions":{}})", &err), "matriz cita elemento fora da lista");
    checa(!x.parse(R"({"elements":["a"],"surfaces":["default"],"matrix":{},"reactions":{"r":{"blocks":[{"type":"magica"}]}}})", &err), "bloco de tipo desconhecido");
    checa(x.empty(), "tabela recusada nao deixa lixo");

    // 5. O anel de afeccoes.
    std::printf("anel de afeccoes\n");
    AffectRing ring;
    auto afeccao = [](uint32_t id, double born, double expire) {
        LiveAffect a;
        a.id = id;
        a.born = born;
        a.expire = expire;
        a.vfx = {id * 10};
        return a;
    };
    std::vector<LiveAffect> fora;
    for (uint32_t i = 1; i <= 3; ++i) fora = ring.add(afeccao(i, double(i), 100.0), 3);
    checa(ring.size() == 3 && fora.empty(), "abaixo do teto ninguem sai");
    fora = ring.add(afeccao(4, 4.0, 100.0), 3);
    checa(ring.size() == 3 && fora.size() == 1 && fora[0].id == 1, "no teto, a mais velha sai");
    checa(fora[0].vfx.size() == 1 && fora[0].vfx[0] == 10, "a que saiu leva os ids do VFX para parar");
    fora = ring.add(afeccao(5, 5.0, 100.0), 2);
    checa(ring.size() == 2 && fora.size() == 2 && fora[0].id == 2 && fora[1].id == 3, "teto menor descarta varias, da mais velha");
    checa(ring.live().front().id == 4 && ring.live().back().id == 5, "ficam as mais novas, em ordem");

    // 6. Vencimento e stop.
    AffectRing r2;
    r2.add(afeccao(1, 0.0, 10.0), 8);
    r2.add(afeccao(2, 0.0, 30.0), 8);
    r2.add(afeccao(3, 0.0, 20.0), 8);
    checa(r2.tick(5.0).empty() && r2.size() == 3, "antes do prazo nada vence");
    const auto vencidas = r2.tick(25.0);
    checa(vencidas.size() == 2 && r2.size() == 1 && r2.live()[0].id == 2, "vencem so' as do prazo");
    LiveAffect parada;
    checa(r2.stop(2, &parada) && parada.id == 2 && r2.size() == 0, "stop acha o id e entrega os ids do VFX");
    checa(!r2.stop(2), "stop de id que nao vive mais devolve falso");
    r2.add(afeccao(9, 0.0, 10.0), 1000);
    checa(r2.size() == 1, "o teto pedido e' limitado pelo teto duro");
    for (uint32_t i = 0; i < kHardMaxAffects + 20; ++i) r2.add(afeccao(100 + i, double(i), 1e9), 1000);
    checa(r2.size() == kHardMaxAffects, "nunca passa do teto duro");

    // 7. Rajadas.
    std::printf("rajadas\n");
    GustRing g;
    bool largou = false;
    for (uint32_t i = 1; i <= 3; ++i) g.add(i, float(i), 0.0f, 5.0f, 1.0f, 2.0f, 3, &largou);
    checa(g.size() == 3 && !largou, "abaixo do teto");
    g.add(4, 9.0f, 0.0f, 5.0f, 1.0f, 2.0f, 3, &largou);
    checa(g.size() == 3 && largou && g.gusts().front().id == 2, "no teto, a mais velha sai");
    g.add(5, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 100, &largou);
    checa(g.size() <= kMaxGusts, "o teto pedido e' limitado ao do UBO");
    checa(g.stop(5) && !g.stop(5), "stop acha a rajada uma vez");
    GustRing h;
    h.add(7, 10.0f, 20.0f, 6.0f, 0.8f, 2.0f, 4);
    GustPacked p[kMaxGusts];
    checa(h.pack(p) == 1 && p[0].a[0] == 10.0f && p[0].a[1] == 20.0f && p[0].a[2] == 6.0f, "pack leva centro e raio");
    checa(p[0].a[3] < 0.05f, "no instante zero a forca ainda nao subiu (sem estalo)");
    h.tick(0.3f);
    h.pack(p);
    const float meio = p[0].a[3], frente = p[0].b[0];
    h.tick(0.9f);
    h.pack(p);
    checa(p[0].a[3] < meio, "a forca decai com o tempo");
    checa(p[0].b[0] > frente && p[0].b[0] <= 1.0f, "a frente da onda anda para fora");
    checa(gustEnvelope(0.0f) == 0.0f && gustEnvelope(1.0f) == 0.0f, "o envelope abre e fecha em zero");
    checa(gustEnvelope(0.1f) > gustEnvelope(0.9f), "forte no comeco, fraco no fim");
    h.tick(1.0f);
    checa(h.size() == 0 && h.pack(p) == 0, "a rajada some quando acaba o tempo");

    // 8. Giro da rajada (swirl, inward, radial): o padrao nao muda nada e os limites valem.
    std::printf("giro da rajada\n");
    GustRing w;
    w.add(1, 0.0f, 0.0f, 5.0f, 1.0f, 2.0f, 4);
    checa(w.pack(p) == 1 && p[0].d[0] == 0.0f && p[0].d[1] == 0.0f && p[0].d[2] == 1.0f, "sem pedido: giro 0, para dentro 0, radial 1");
    GustShape giro;
    giro.swirl = 5.0f;
    giro.inward = -5.0f;
    giro.radial = -1.0f;
    w.add(2, 0.0f, 0.0f, 5.0f, 1.0f, 2.0f, 4, nullptr, giro);
    checa(w.pack(p) == 2 && p[1].d[0] == 1.0f && p[1].d[1] == -1.0f && p[1].d[2] == 0.0f, "swirl e inward ficam em -1..1 e radial em 0..1");
    giro.swirl = 0.4f;
    giro.inward = 0.2f;
    giro.radial = 0.0f;
    w.add(3, 0.0f, 0.0f, 5.0f, 1.0f, 2.0f, 4, nullptr, giro);
    checa(w.pack(p) == 3 && p[2].d[0] == 0.4f && p[2].d[1] == 0.2f && p[2].d[2] == 0.0f, "valores dentro dos limites passam como estao");

    std::printf(g_falhas ? "\n%d FALHA(S)\n" : "\ntudo certo\n", g_falhas);
    return g_falhas ? 1 : 0;
}
