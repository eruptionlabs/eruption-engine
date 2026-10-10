// Host de compatibilidade dentro do motor: o log vai para o Logger do motor
// (console do editor incluso). A cena ainda e' a arvore em memoria do
// HostPadrao; espelhar os nos em entidades do motor e' o passo seguinte.
//
// Compilado junto do executavel (precisa de src/), nao da biblioteca compat.

#include "core/Logger.hpp"
#include "godot/GodotHost.hpp"
#include "host/Compat.hpp"
#include "host/Host.hpp"
#include "motor/AppComCompat.hpp"

#include <string>

namespace eruption::compat {

namespace {

class HostMotor : public HostPadrao {
public:
    void log(Nivel nivel, const char* origem, const char* texto) override {
        const std::string linha = std::string("[compat:") + origem + "] " + texto;
        const LogLevel l = nivel == Nivel::Erro ? LogLevel::Error : nivel == Nivel::Aviso ? LogLevel::Warning : LogLevel::Info;
        // message() ignora o filtro de nivel: o que o plugin imprime aparece sempre.
        Logger::message(l, linha.c_str());
    }
};

HostMotor& hostMotor() {
    static HostMotor h;
    return h;
}

bool g_ativo = false;

} // namespace

void iniciarNoMotor() {
    definirHost(&hostMotor());
    const int n = carregarDaVariavel("ERUPTION_COMPAT_PLUGINS");
    if (n > 0) {
        instanciarDaVariavel("ERUPTION_COMPAT_GODOT_CENA");
        g_ativo = true;
    }
}

void quadroNoMotor(float dt) {
    if (!g_ativo) return;
    quadro(dt);
    // Marcos de progresso no log (1, 10, 100, 1000... quadros): prova de vida
    // dos plugins mesmo quando o processo e' encerrado de fora.
    static uint64_t n = 0, proximo = 1;
    if (++n == proximo) {
        proximo *= 10;
        logf(Nivel::Info, "compat", "quadro %llu (t=%.2fs)", static_cast<unsigned long long>(n), host().tempo());
        for (const std::string& linha : godot::descreverCena()) logf(Nivel::Info, "compat", "cena: %s", linha.c_str());
    }
}

void encerrarNoMotor() {
    if (g_ativo) encerrar();
    g_ativo = false;
    definirHost(nullptr);
}

} // namespace eruption::compat
