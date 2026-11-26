#pragma once
// AFECCOES DO LUGAR: um efeito (acao) com um elemento (qualidade), uma forca e
// uma area (quantidade) e uma duracao (tempo) deixa uma marca no chao e no
// entorno; a reacao depende da superficie do lugar (substancia).
//
// Este arquivo e' a LOGICA PURA (sem GPU, sem Engine): a tabela data-driven
// (data/affects.json: matriz elemento x superficie -> reacao, cada reacao uma
// lista de blocos reutilizaveis), a escolha da reacao, o anel de afeccoes
// vivas com teto fixo (a mais velha sai) e o anel de rajadas de vento. A cola
// com o VFX, o molhado e o shader de vegetacao fica em core/AffectsEngine.cpp.
// Nada de efeito escrito no codigo: trocar a tabela troca o comportamento.
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace eruption::affects {

// Teto duro das duas filas (o das rajadas e' o tamanho do array no UBO do
// quadro; o das afeccoes e' o maximo que um preset pode pedir).
inline constexpr uint32_t kHardMaxAffects = 96;
inline constexpr uint32_t kMaxGusts = 8;

// Qualidade do preset (data/graphics.json, affect_*): 0 = low (decal simples
// e vento), 1 = medium, 2 = high (deslocamento de verdade, poca com reflexo,
// mais particulas). Um bloco vale para quem estiver entre minQuality e maxQuality.
struct Quality {
    int level = 0;
    uint32_t maxAffects = 48;   // afeccoes vivas ao mesmo tempo
    uint32_t maxGusts = 4;      // rajadas vivas (<= kMaxGusts)
    float particles = 0.5f;     // multiplica a contagem das particulas
};

struct DecalBlock {
    std::string texture = "builtin:mk_scorch";
    bool additive = false;       // soma luz (brasa, brilho); senao mistura com o chao
    float color[3] = {0.05f, 0.04f, 0.03f};
    float colorEnd[3] = {0.05f, 0.04f, 0.03f}; // cor no fim da vida (brasa que esfria)
    float alpha = 0.8f;
    float size = 1.0f;           // meio lado, em fracao do raio da afeccao
    float sizeJitter = 0.15f;
    float sizeStart = 1.0f, sizeEnd = 1.0f; // multiplicador no nascer e no fim
    uint32_t count = 1;
    float scatter = 0.0f;        // espalha as marcas, em fracao do raio
    float life = 1.0f;           // fracao da duracao da afeccao
    float fadeIn = 0.04f;        // fracao da vida da marca
    float hold = 0.35f;          // fracao da vida antes de comecar a sumir
    float delay = 0.0f;          // fracao da duracao ate' a marca nascer
    bool directional = false;    // a ponta da textura segue o dir do pedido
    uint32_t atlas = 1;          // textura em NxN desenhos: um e' sorteado por marca
    float lit = 0.8f;
    float strengthAlpha = 1.0f;  // quanto a forca mexe no alfa (0 = nada)
    int minQuality = 0, maxQuality = 2;
};

struct ParticleBlock {
    std::string texture = "builtin:puff";
    std::string mesh;            // malha no lugar do sprite (pedrinha, estilhaco)
    bool additive = false;
    float color[3] = {0.6f, 0.55f, 0.45f};
    float colorEnd[3] = {0.6f, 0.55f, 0.45f};
    float alpha = 0.5f;
    uint32_t count = 8;
    float life[2] = {0.8f, 1.6f};     // s
    float speed[2] = {0.1f, 0.5f};    // em raios por segundo
    float size[2] = {0.1f, 0.2f};     // em raios
    float sizeEnd = 1.5f;             // multiplicador no fim da vida
    float gravity = 0.0f;             // raios/s2 (negativo = cai)
    float drag = 1.0f;
    float spread = 0.9f;
    float areaRadius = 0.5f;          // disco de nascimento, em raios
    float burstTime = 0.0f;           // fracao da duracao
    float rate = 0.0f;                // por segundo, no lugar da rajada (borbulhar)
    float duration = 0.0f;            // fracao da duracao emitindo (com rate)
    bool collide = false;             // quica no plano do chao
    float lit = 0.8f;
    bool outward = false;             // sai do centro para fora, rente ao chao
    // Distorcao de tela (calor, onda de choque): > 0 o emissor nao desenha cor,
    // entorta a cena atras (fracao da altura da tela, ver VfxEmitterDesc).
    float distortion = 0.0f;
    float distortionRadial = 0.0f;    // 0 = ondulacao de calor, 1 = anel que empurra para fora
    // Luz pontual do emissor (clarao): acende o chao e a copa em volta.
    bool light = false;
    float lightColor[3] = {1.0f, 0.9f, 0.6f};
    float lightIntensity = 2.0f;
    float lightRadius = 1.5f;         // em raios da afeccao
    float lightFlicker = 0.3f;
    float lightHeight = 0.6f;         // altura da luz, em raios
    int minQuality = 0, maxQuality = 2;
};

// Relevo que sobe e assenta: peças de malha (pedaco de chao) que sobem da terra
// e afundam de novo. So' nos presets com qualidade >= minQuality.
struct RaiseBlock {
    std::string mesh = "builtin:rock";
    float color[3] = {0.35f, 0.3f, 0.25f};
    uint32_t count = 6;
    float ring = 0.6f;           // em fracao do raio (0 = ao centro)
    float size = 0.25f;          // em raios
    float rise = 0.12f;          // quanto sobe, em raios
    float riseTime = 0.15f;      // fracao da vida subindo
    float life = 0.6f;           // fracao da duracao da afeccao
    float lit = 1.0f;
    int minQuality = 2, maxQuality = 2;
};

struct WetBlock {
    float amount = 1.0f;         // 0..1 (vezes a forca)
    float radius = 1.0f;         // em fracao do raio
    float dry = 1.0f;            // secar em fracao da duracao
    bool mud = false;            // marca de lama no lugar de so' molhar
};

// Forma da rajada (os valores de antes = rajada de vento comum). Uma onda de
// impacto e' a mesma rajada com a frente fina e quase nada atras dela.
struct GustShape {
    float width = 0.35f;   // largura da frente, em fracao do raio
    float floor = 0.35f;   // fracao da forca que sobra atras da frente (0..1)
    float shake = 0.0f;    // tremor da copa quando a frente passa (0..1)
    float lift = 0.3f;     // sopro para cima nas particulas (0..1)
    // Giro em volta do centro (nevasca, redemoinho de area): `swirl` e' a forca
    // tangencial (+ anti-horario visto de cima, - horario), `inward` puxa para o
    // centro (+) ou empurra (-), ambos sem frente de onda (valem o raio todo, com a
    // mesma queda). `radial` escala o empurrao para fora da rajada comum (1 = como
    // sempre, 0 = so' o giro). Com 0, 0 e 1 a rajada e a imagem ficam como eram.
    float swirl = 0.0f;    // -1..1
    float inward = 0.0f;   // -1..1
    float radial = 1.0f;   // 0..1
};

struct WindBlock {
    float strength = 1.0f;       // multiplica a forca do pedido
    float radius = 1.5f;         // em fracao do raio
    float duration = 0.1f;       // em fracao da duracao (a rajada e' curta)
    float durationMin = 0.8f;    // s
    GustShape shape;
};

// Aneis na superficie da agua dentro do raio (so' vale onde ha' agua).
struct RippleBlock {
    float radius = 1.0f;         // alcance final do anel, em fracao do raio
    float life = 1.6f;           // s
    float strength = 1.0f;       // 0..2, vezes a forca do pedido
    uint32_t rings = 3;          // aneis seguidos (1..4)
};

// Estado do chao (core/GroundState.hpp): queimado, geada, molhado, neve, gelo
// sobre a agua. Modula o material do chao por celula; some por conta propria.
struct GroundBlock {
    std::string kind = "scorch"; // scorch, frost, wet, snow, ice_water
    float amount = 1.0f;         // vezes a forca do pedido
    float radius = 1.0f;         // em fracao do raio
    float duration = 1.0f;       // vezes a duracao do pedido
    float fade = 0.7f;           // fracao da duracao do estado gasta sumindo
};

struct Block {
    enum class Type { Decal, Particles, Raise, Wet, Wind, Ripple, Ground } type = Type::Decal;
    DecalBlock decal;
    ParticleBlock particles;
    RaiseBlock raise;
    WetBlock wet;
    WindBlock wind;
    RippleBlock ripple;
    GroundBlock ground;
};

struct Reaction {
    std::string name;
    std::vector<Block> blocks;
    // A reacao precisa do dir do pedido? (muda a chave da variante do VFX)
    bool usesDir() const;
};

// A tabela inteira.
class Table {
public:
    // Le o JSON; devolve false (e uma mensagem em *err) se estiver torto.
    // Chaves faltando ficam nos padroes; reacao citada na matriz e ausente e' erro.
    bool parse(const std::string& jsonText, std::string* err = nullptr);

    const std::vector<std::string>& elements() const { return m_elements; }
    const std::vector<std::string>& surfaces() const { return m_surfaces; }
    bool hasElement(const std::string& e) const;
    // Elemento da matriz para um nome ou apelido ("aliases" do JSON: heat -> fire,
    // cold -> ice...); o proprio nome se nao e' apelido.
    std::string canonical(const std::string& e) const;

    // Reacao do par; se o elemento nao tem a superficie, usa a "default" dele.
    // nullptr: elemento desconhecido ou sem reacao nenhuma.
    const Reaction* select(const std::string& element, const std::string& surface) const;

    // Superficie pelo nome de um material ou textura (dicas em surface_hints,
    // minusculas, por trecho do nome). "default" se nada casar.
    std::string surfaceFromName(const std::string& materialOrTexture) const;
    // Um tipo de regiao por textura ("sand", "grass", ...) e' uma superficie?
    std::string surfaceFromRegion(const std::string& regionType) const;

    bool empty() const { return m_reactions.empty(); }
    size_t reactionCount() const { return m_reactions.size(); }

private:
    std::vector<std::string> m_elements, m_surfaces;
    std::unordered_map<std::string, std::string> m_aliases;
    std::unordered_map<std::string, Reaction> m_reactions;
    // element -> surface -> nome da reacao
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> m_matrix;
    std::vector<std::pair<std::string, std::vector<std::string>>> m_hints; // superficie -> trechos
};

// ------------------------------------------------------------------ anel

// Anel das afeccoes vivas: teto fixo, o mais antigo sai. Cada afeccao guarda
// os ids do VFX (e o que mais a cola precisar) para parar tudo junto.
struct LiveAffect {
    uint32_t id = 0;
    double born = 0.0, expire = 0.0;
    std::vector<uint32_t> vfx;   // instancias do VFX
    std::vector<uint32_t> gusts; // rajadas de vento que a afeccao soltou
    std::vector<uint32_t> grounds; // estados do chao (world.ground_state) que a afeccao deixou
    std::string element, surface;
    float x = 0.0f, z = 0.0f;
};

class AffectRing {
public:
    // Insere; devolve as afeccoes descartadas por causa do teto (as mais velhas).
    std::vector<LiveAffect> add(LiveAffect a, uint32_t cap);
    // Tira as que passaram do prazo; devolve as que sairam.
    std::vector<LiveAffect> tick(double now);
    // Para uma; false se o id nao existe (ja' acabou).
    bool stop(uint32_t id, LiveAffect* out = nullptr);
    void clear() { m_live.clear(); }
    size_t size() const { return m_live.size(); }
    const std::vector<LiveAffect>& live() const { return m_live; }

private:
    std::vector<LiveAffect> m_live; // em ordem de nascimento
};

// ----------------------------------------------------------------- vento

struct Gust {
    uint32_t id = 0;
    float x = 0.0f, z = 0.0f, radius = 1.0f, strength = 1.0f, duration = 1.0f;
    float age = 0.0f;
    GustShape shape;
};

// Dois vec4 por rajada no FrameUBO dos modelos (FrameUBO::gusts):
//   a = (x, z, raio, forca agora)
//   b = (frente 0..1, largura da frente, forca atras da frente 0..1, tremor 0..1)
// As particulas (VfxFrameParams::gusts) leem a e (b.x, b.y, c.x, b.z): o sopro
// para cima no lugar do tremor.
struct GustPacked {
    float a[4];
    float b[4];
    float c[4];
    float d[4];   // (giro, para dentro, escala radial, 0): so' as particulas e o vegetal leem
};

// Forca da rajada no tempo t = idade/duracao: sobe rapido e cai (1-t)^1.5.
float gustEnvelope(float t);
// Frente da onda (fracao do raio) no tempo t: anda rapido e desacelera.
float gustFront(float t);

class GustRing {
public:
    // Insere com o id dado (o mesmo contador das afeccoes, para world.affect_stop
    // valer para os dois); *dropped = true se uma mais velha saiu pelo teto.
    void add(uint32_t id, float x, float z, float radius, float strength, float duration, uint32_t cap,
             bool* dropped = nullptr, const GustShape& shape = GustShape());
    void tick(float dt);
    bool stop(uint32_t id);
    void clear() { m_gusts.clear(); }
    size_t size() const { return m_gusts.size(); }
    const std::vector<Gust>& gusts() const { return m_gusts; }
    // Preenche ate' kMaxGusts entradas para o UBO; devolve quantas valem.
    uint32_t pack(GustPacked* out) const;

private:
    std::vector<Gust> m_gusts;
};

} // namespace eruption::affects
