#pragma once
// ESTADO DO CHAO POR CELULA: queimado, geada, molhado, neve e gelo sobre a agua,
// cada um com forca, duracao e um tempo de sumir. E' a LOGICA PURA (sem GPU,
// sem Engine): a lista de estados vivos, o ciclo de vida de cada um e a
// rasterizacao para uma textura de baixa resolucao em espaco de mundo que o
// passe de iluminacao le' (shaders/lighting/ground_state.glsl). A cola com a
// GPU fica em renderer/DeferredLighting.cpp e core/GroundStateEngine.cpp.
//
// Por que uma textura e nao um decal: o estado MODULA o material do que ja'
// esta' no chao (albedo, rugosidade, normal) pixel a pixel, entao serve ao
// terreno e a qualquer modelo de chao, tem borda macia, mistura com o vizinho
// (o mais forte vence por canal) e custa uma leitura por pixel de luz.
//
// Layout da imagem (RGBA8, 2W x W): a metade esquerda (W x W) guarda os quatro
// estados nos canais R = queimado, G = geada (e gelo sobre a agua), B = molhado,
// A = neve; a direita, a altura do chao em cada celula (16 bits em R e G), que
// o shader usa para so' tocar o que esta' rente ao chao (copa de arvore acima
// de uma marca nao escurece).
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace eruption::ground {

enum class Kind : uint8_t { Scorch = 0, Frost = 1, Wet = 2, Snow = 3, IceWater = 4 };

// "scorch", "frost", "wet", "snow", "ice_water"; false se o nome nao existe.
bool kindFromName(const std::string& name, Kind& out);
const char* kindName(Kind k);

inline constexpr uint32_t kMaxStates = 512;  // vivos ao mesmo tempo (o mais velho sai)
inline constexpr uint32_t kMaxIceAreas = 8;  // areas de gelo sobre a agua mandadas ao shader da agua

struct State {
    uint32_t id = 0;
    Kind kind = Kind::Scorch;
    float x = 0.0f, z = 0.0f;
    float radius = 1.0f;     // unidades do mundo (disco) ou meio lado (quadrado)
    bool square = false;     // uma celula quadrada no lugar do disco
    float strength = 1.0f;   // 0..1
    double born = 0.0;       // relogio do chao (s)
    float duration = 20.0f;  // s ate' sumir por inteiro
    float fade = 14.0f;      // s do fim da duracao gastos sumindo
    float y = 0.0f;          // altura do chao no centro
    // Altura do chao em uma grade grossa em volta do centro (nx x nz amostras
    // de `step` unidades, a partir de x0,z0): a mascara de altura do shader.
    std::vector<float> heights;
    int nx = 0, nz = 0;
    float x0 = 0.0f, z0 = 0.0f, step = 1.0f;
    // Gelo sobre a agua (ice_water): quem anda por cima fica na superficie da
    // agua enquanto a celula estiver congelada. `walkable` = false deixa so'
    // o visual. `solid` e' a trava com histerese (liga em kIceSolidOn, solta em
    // kIceSolidOff, sobre a forca do estado), `everSolid` = ja' aguentou gente
    // (so' entao ha' o afundar suave no degelo).
    bool walkable = true;
    bool solid = false;
    bool everSolid = false;
};

// Limiares do gelo que aguenta peso, sobre v = lifeAmount * perfil (0..1).
// Entra congelado em v >= kIceSolidOn; depois de solido, so' solta abaixo de
// kIceSolidOff (histerese: a borda nao pula quando v oscila em volta de um
// valor so'). Abaixo de kIceSolidOff e ate' kIceSinkLow o chao desce suave de
// volta ao leito (afundar no degelo, sem teleporte).
inline constexpr float kIceSolidOn = 0.6f;
inline constexpr float kIceSolidOff = 0.35f;
inline constexpr float kIceSinkLow = 0.05f;
inline constexpr float kIceBucket = 256.0f;

// Quanto do estado vale agora (forca x ciclo de vida), 0..1: sobe em ~0,5 s,
// segura ate' duration - fade e some em suavizado ate' duration.
float lifeAmount(const State& s, double now);
// Quando o frio ou a neve derretem, o chao fica molhado: 0 enquanto cheio,
// sobe no comeco do degelo, seca no fim. Zero para os outros tipos.
float meltWet(const State& s, double now);
// Perfil no espaco: 1 no miolo, cai suave ate' 0 na borda (disco ou quadrado).
float profile(const State& s, float px, float pz);
// Altura de referencia do chao em (x, z) pela grade do estado.
float groundAt(const State& s, float px, float pz);
// Acabou?
bool expired(const State& s, double now);

struct Rect {
    int x0 = 0, y0 = 0, x1 = -1, y1 = -1; // inclusivo; x1 < x0 = vazio
    bool valid() const { return x1 >= x0 && y1 >= y0; }
    void add(const Rect& r);
};

struct Params {
    uint32_t texels = 512;     // W (potencia de 2)
    float texelWorld = 8.0f;   // unidades do mundo por celula
};

class Field {
public:
    void configure(const Params& p);
    const Params& params() const { return m_p; }

    // Insere (o mais velho sai se passou de `cap`); devolve o id (s.id se != 0).
    // `groundY(x, z)` mede o chao para a mascara de altura (chamada ate' ~1500x).
    uint32_t add(State s, uint32_t id, const std::function<float(float, float)>& groundY, uint32_t cap);
    bool stop(uint32_t id);
    void clear();
    // Tira os vencidos; true se algum saiu.
    bool tick(double now);
    size_t size() const { return m_states.size(); }
    const std::vector<State>& states() const { return m_states; }

    // Mantem a janela em volta do foco (a camera): so' muda quando o foco passa
    // de 30% da janela do centro, em passos de 32 celulas. true se mudou.
    bool follow(float fx, float fz);
    float originX() const { return m_ox; }
    float originZ() const { return m_oz; }
    float windowSize() const { return float(m_p.texels) * m_p.texelWorld; }

    // Refaz a imagem no estado de `now`. Devolve o retangulo (em celulas da
    // metade esquerda) que mudou; invalido quando nada precisa subir.
    Rect rasterize(double now);
    const std::vector<uint8_t>& pixels() const { return m_px; }
    uint32_t imageWidth() const { return m_p.texels * 2; }
    uint32_t imageHeight() const { return m_p.texels; }
    // Mascara de altura gravada: altura = base + valor16/65535 * range.
    float heightBase() const { return m_hBase; }
    float heightRange() const { return m_hRange; }
    // Algum estado esta' mudando de valor agora (subindo, sumindo ou derretendo)?
    // Em regime (segurando) a imagem nao muda e nao precisa ser refeita.
    bool animating(double now) const;
    // Cobertura (0..1) de cada canal da imagem em (x, z): R queimado, G geada, B molhado,
    // A neve - a mesma metade esquerda que o shader de luz le', sem a mascara de altura
    // (quem pergunta aqui e' a base de uma planta, rente ao chao). Celula mais proxima,
    // uma leitura: custa quase nada por arvore. Fora da janela ou sem imagem: tudo 0.
    void coverAt(float x, float z, float out[4]) const;
    // Alguma coisa para o shader ler? (estado vivo, ou ainda a limpar)
    bool lit() const { return !m_states.empty() || m_dirtyLeft; }

    // Quanto o gelo sob (x, z) sustenta, 0..1, com o relogio do ultimo tick: 1 =
    // congelado (ande na superficie da agua), entre 0 e 1 = afundando no degelo,
    // 0 = agua. So' conta ice_water com `walkable`. Custo zero sem gelo vivo.
    float iceLift(float x, float z) const;
    bool hasIce() const { return m_iceLive > 0; }

    // Areas de gelo para a agua: (x, z, raio, quanto) das "ice_water" vivas.
    uint32_t iceAreas(double now, float out[][4], uint32_t cap) const;

private:
    Params m_p;
    std::vector<State> m_states;
    std::vector<uint8_t> m_px;
    float m_ox = 0.0f, m_oz = 0.0f;
    bool m_haveWindow = false;
    float m_hBase = 0.0f, m_hRange = 1.0f;
    Rect m_prevDirty;
    bool m_dirtyLeft = false;
    double m_now = 0.0;
    uint32_t m_iceLive = 0; // ice_water com walkable
    // Grade grossa (celulas de kIceBucket u) com os indices dos gelos que a cobrem:
    // iceLift custa o mesmo com 1 ou 400 lagos congelados (so' olha a celula).
    std::unordered_map<uint64_t, std::vector<uint16_t>> m_iceGrid;
    void countIce(); // refaz a contagem e a grade
};

} // namespace eruption::ground
