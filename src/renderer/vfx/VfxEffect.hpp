#pragma once

// EFEITO VISUAL (asset). Um efeito e' uma lista de emissores; cada emissor
// descreve COMO as particulas nascem (taxa, rajada, forma), COMO andam
// (gravidade, arrasto, turbulencia, colisao) e COMO sao desenhadas (cor e
// tamanho ao longo da vida, textura em folha de quadros, mistura, luz).
//
// O arquivo e' JSON legivel (<nome>.vfx.json): diff limpo, editavel a mao,
// pelo editor ou por IA. Conteudo do jogo fica em assets/effects/ do projeto;
// os modelos que vem com o motor ficam em data/vfx/.
//
// Parametro com faixa (Vec2 min/max) sorteia por particula no nascimento.
// Curvas e gradientes sao amostrados em 16 pontos ao subir para a GPU.

#include "math/Types.hpp"
#include "renderer/vfx/VfxGraph.hpp"

#include <string>
#include <vector>

namespace eruption {

// Curva escalar por trechos lineares, t em [0,1] (fracao da vida).
struct VfxCurve {
    std::vector<Vec2> keys; // (t, valor), em ordem de t
    float sample(float t) const;
    static VfxCurve constant(float v) { return VfxCurve{{Vec2(0.0f, v)}}; }
};

// Gradiente de cor RGBA por trechos lineares (cor linear, alfa reto).
struct VfxGradient {
    struct Key {
        float t = 0.0f;
        Vec4 color{1.0f};
    };
    std::vector<Key> keys;
    Vec4 sample(float t) const;
};

enum class VfxShape : uint32_t { Point = 0, Sphere = 1, Box = 2, Cone = 3, Disc = 4 };
// Ribbon: as particulas do emissor sao os pontos de uma fita continua (na
// ordem em que nasceram), virada para a camera - trilha de espada, rastro de
// magia, cauda de cometa. Combine com ratePerMeter para a fita seguir o efeito.
enum class VfxFacing : uint32_t { Camera = 0, Velocity = 1, Horizontal = 2, Ribbon = 3 };
// Todas viram UMA mistura na GPU (cor premultiplicada): aditiva = alfa 0 na
// saida. Por isso o sistema inteiro desenha num unico draw.
enum class VfxBlend : uint32_t { Additive = 0, Premultiplied = 1, Alpha = 2 };
// Folha de quadros: anima ao longo da vida, em quadros/s, ou sorteia um
// quadro fixo por particula (variacao sem animar: fumaca, pedrinhas).
enum class VfxFlipMode : uint32_t { Life = 0, Fps = 1, Random = 2 };

// Sub-emissor: quando uma particula morre (ou bate), nascem `count` filhas
// do emissor `emitter` (pelo nome, no mesmo efeito) no lugar dela. Tudo na
// GPU: o pai escreve um evento, um passe indireto faz as filhas.
struct VfxSubEmitter {
    std::string emitter;          // vazio = desligado
    uint32_t count = 4;           // filhas por evento (ate' 64)
    float probability = 1.0f;     // chance do evento acontecer
    float inheritVelocity = 0.3f; // fracao da velocidade do pai
};

struct VfxEmitterDesc {
    std::string name = "emissor";
    bool enabled = true;
    uint32_t capacity = 2000; // teto de particulas vivas deste emissor

    // --- Nascimento ---
    float rate = 20.0f;          // particulas por segundo
    float ratePerMeter = 0.0f;   // extra por metro andado pelo efeito (rastro)
    uint32_t burstCount = 0;     // rajada no inicio
    float burstInterval = 0.0f;  // repete a rajada a cada N s (0 = uma vez)
    float startDelay = 0.0f;
    float duration = 0.0f;       // tempo emitindo (0 = enquanto o efeito durar)

    // --- Forma ---
    VfxShape shape = VfxShape::Point;
    Vec3 shapeSize{1.0f};        // raio (esfera/cone/disco) ou meia-caixa
    Vec3 offset{0.0f};           // m, no espaco do efeito: onde a forma fica
    bool surfaceOnly = false;    // so' na casca da forma
    float coneAngle = 25.0f;     // graus (cone: abertura do jato)

    // --- Inicial ---
    Vec2 lifetime{1.0f, 2.0f};
    Vec2 speed{1.0f, 2.0f};
    Vec3 direction{0.0f, 1.0f, 0.0f}; // no espaco do efeito
    float spread = 0.2f;              // 0 = reto, 1 = qualquer direcao
    Vec2 size{0.5f, 1.0f};
    Vec2 rotation{0.0f, 360.0f};      // graus
    Vec2 angularVelocity{0.0f, 0.0f}; // graus/s
    float inheritVelocity = 0.0f;     // herda a velocidade do efeito

    // --- Movimento ---
    Vec3 gravity{0.0f};
    float drag = 0.0f;
    float noiseStrength = 0.0f;  // turbulencia (campo de rotacional, sem divergencia)
    float noiseFrequency = 0.25f;
    float noiseSpeed = 0.5f;
    bool collidePlane = false;   // plano horizontal na altura planeY (mundo)
    float planeY = 0.0f;
    bool collideDepth = false;   // colide com o que esta' na tela (profundidade)
    float bounce = 0.3f;
    float friction = 0.2f;
    float collisionLifeLoss = 0.0f; // fracao da vida perdida a cada batida

    // --- Ao longo da vida ---
    VfxGradient color;
    VfxCurve sizeOverLife = VfxCurve::constant(1.0f);

    // --- Desenho ---
    VfxBlend blend = VfxBlend::Additive;
    VfxFacing facing = VfxFacing::Camera;
    std::string texture;       // PNG (folha de quadros); vazio = disco suave
    uint32_t flipCols = 1, flipRows = 1;
    VfxFlipMode flipMode = VfxFlipMode::Life;
    float flipFps = 12.0f;     // modo fps
    bool flipBlend = true;     // mistura o quadro seguinte (menos quadros, mais fluido)
    bool flipRandomStart = false;
    float stretch = 0.0f;      // estica na direcao da velocidade (faisca)
    float softDistance = 0.5f; // some perto da geometria (m); 0 = corte seco
    float emissive = 1.0f;     // multiplica a cor (HDR: >1 brilha no bloom)
    float lit = 0.0f;          // 0 = luz propria, 1 = iluminada pelo sol/ceu
    float maxScreenSize = 0.6f;// teto de tamanho na tela (fracao da altura)
    float nearFade = 0.5f;     // some perto da camera (m)
    std::string mesh;          // malha no lugar do sprite ("builtin:rock"...):
                               // opaca, no G-buffer, com a luz da cena
    bool lowRes = false;       // desenha em meia resolucao (fumaca, poeira:
                               // 1/4 dos pixels; o ganho maior em GPU fraca)
    bool decal = false;        // deitada e PROJETADA no que esta' na tela
                               // (circulo de magia, marca): segue rampa e
                               // degrau em vez de cortar ou flutuar; gira com
                               // a particula e com o efeito
    bool translucent = false;  // malha translucida (gelo, vidro, cristal):
                               // depois da luz, refrata a cena atras, cor da
                               // particula = tinta pela espessura, borda de
                               // Fresnel e brilho do sol
    float refraction = 0.04f;  // desvio da cena atras (fracao da tela)
    float absorption = 1.0f;   // tinta por metro de espessura (0 = incolor)

    // --- Sub-emissores ---
    VfxSubEmitter onDeath;
    VfxSubEmitter onCollision;

    // --- Luz: UMA luz pontual por emissor (nao por particula), no centro do
    // efeito, que acompanha o quanto ele esta' vivo e tremula. O fogo
    // ilumina o chao e as paredes em volta pelo passe de luzes da cena.
    bool light = false;
    Vec3 lightColor{1.0f, 0.55f, 0.2f};
    float lightIntensity = 2.0f;
    float lightRadius = 6.0f;    // m
    float lightFlicker = 0.3f;   // 0 = fixa, 1 = tremula forte
    Vec3 lightOffset{0.0f, 0.5f, 0.0f}; // m, no espaco do efeito

    // --- Grafo (expressoes por particula; vazio = so' os modulos acima) ---
    VfxGraph graph;
};

struct VfxEffectDesc {
    std::string name;
    float duration = 0.0f; // 0 = sem fim (ate' parar)
    bool loop = true;      // com duration > 0: recomeca
    bool vision = true;    // false: cenario; a visao de equipe so' escurece, nao esconde
    std::vector<VfxEmitterDesc> emitters;

    // Maior vida possivel (para saber quando a ultima particula morreu).
    float maxLifetime() const;
};

// Leitura/escrita do arquivo .vfx.json. Erros vao em *err.
bool loadVfxEffect(const std::string& path, VfxEffectDesc& out, std::string* err = nullptr);
bool saveVfxEffect(const std::string& path, const VfxEffectDesc& fx, std::string* err = nullptr);
// O mesmo formato em texto (desfazer do editor, copiar/colar).
std::string vfxEffectToJson(const VfxEffectDesc& fx);
bool vfxEffectFromJson(const std::string& text, VfxEffectDesc& out, std::string* err = nullptr);

} // namespace eruption
