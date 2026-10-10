#pragma once

// Interface unica entre a camada de compatibilidade (compat/) e o motor.
//
// Os hosts de plugin (godot/, unity/, unreal/) so' falam com o motor por
// aqui: log, relogio, nos de cena com transformacao, malhas e texturas.
// A implementacao padrao (HostPadrao) guarda tudo em memoria e escreve o log
// no terminal, o que basta para o executavel de teste (eruption-compat-run).
// O motor registra a propria implementacao com definirHost() ao iniciar.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace eruption::compat {

enum class Nivel { Info, Aviso, Erro };

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

struct Quat {
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;
};

// Transformacao LOCAL de um no' (relativa ao pai).
struct Transformacao {
    Vec3 posicao;
    Quat rotacao;
    Vec3 escala{1.0f, 1.0f, 1.0f};
};

using NoId = uint32_t;
constexpr NoId kSemNo = 0;

using RecursoId = uint32_t;
constexpr RecursoId kSemRecurso = 0;

class Host {
public:
    virtual ~Host() = default;

    // origem: quem falou ("godot", "unity", "unreal" ou o nome do plugin).
    virtual void log(Nivel nivel, const char* origem, const char* texto) = 0;

    // Relogio do jogo em segundos e duracao do quadro atual.
    virtual double tempo() const = 0;
    virtual double dt() const = 0;
    virtual uint64_t quadro() const = 0;

    // Cena: arvore de nos com nome e transformacao local.
    virtual NoId criarNo(const std::string& nome, NoId pai) = 0;
    virtual void destruirNo(NoId no) = 0;
    virtual void definirPai(NoId no, NoId pai) = 0;
    virtual void definirNome(NoId no, const std::string& nome) = 0;
    virtual void definirTransformacao(NoId no, const Transformacao& t) = 0;
    virtual Transformacao transformacao(NoId no) const = 0;

    // Recursos. Posicoes em xyz corrido; RGBA8 linha a linha.
    virtual RecursoId criarMalha(const std::vector<float>& posicoes, const std::vector<uint32_t>& indices) = 0;
    virtual RecursoId criarTextura(int largura, int altura, const std::vector<uint8_t>& rgba) = 0;
    virtual void liberarRecurso(RecursoId id) = 0;
};

// Implementacao em memoria, sem motor. Serve de base para a do motor, que
// so' precisa sobrescrever o que quiser espelhar (log e cena, por exemplo).
class HostPadrao : public Host {
public:
    void log(Nivel nivel, const char* origem, const char* texto) override;

    double tempo() const override { return m_tempo; }
    double dt() const override { return m_dt; }
    uint64_t quadro() const override { return m_quadro; }
    // Avanca o relogio; chamado uma vez por quadro por quem dirige o laco.
    void avancar(double dt);

    NoId criarNo(const std::string& nome, NoId pai) override;
    void destruirNo(NoId no) override;
    void definirPai(NoId no, NoId pai) override;
    void definirNome(NoId no, const std::string& nome) override;
    void definirTransformacao(NoId no, const Transformacao& t) override;
    Transformacao transformacao(NoId no) const override;

    RecursoId criarMalha(const std::vector<float>& posicoes, const std::vector<uint32_t>& indices) override;
    RecursoId criarTextura(int largura, int altura, const std::vector<uint8_t>& rgba) override;
    void liberarRecurso(RecursoId id) override;

    struct No {
        bool vivo = false;
        std::string nome;
        NoId pai = kSemNo;
        Transformacao t;
    };
    const std::vector<No>& nos() const { return m_nos; }

private:
    double m_tempo = 0.0;
    double m_dt = 0.0;
    uint64_t m_quadro = 0;
    std::vector<No> m_nos{No{}}; // indice 0 reservado (kSemNo)
    RecursoId m_proximoRecurso = 1;
};

// Host ativo. Sem definirHost(), usa um HostPadrao interno.
Host& host();
void definirHost(Host* h);

// Log formatado (printf) pelo host ativo.
void logf(Nivel nivel, const char* origem, const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;

} // namespace eruption::compat
