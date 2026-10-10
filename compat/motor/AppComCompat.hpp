#pragma once

// Liga a camada de compatibilidade ao laco do motor sem mexer na aplicacao:
// embrulha a App (onInit/onUpdate/onRender/onShutdown) e, em volta dela,
// carrega os plugins de ERUPTION_COMPAT_PLUGINS e repassa o quadro.
//
// Sem a variavel nada e' carregado e o embrulho so' repassa as chamadas.

namespace eruption::compat {

// Implementados em compat/motor/HostMotor.cpp (compilado junto do motor).
void iniciarNoMotor();
void quadroNoMotor(float dt);
void encerrarNoMotor();

template <typename App>
class AppComCompat {
public:
    explicit AppComCompat(App& app) : m_app(app) {}

    template <typename Engine>
    void onInit(Engine& engine) {
        m_app.onInit(engine);
        iniciarNoMotor();
    }
    void onUpdate(float dt) {
        m_app.onUpdate(dt);
        quadroNoMotor(dt);
    }
    template <typename Engine>
    void onRender(Engine& engine) {
        m_app.onRender(engine);
    }
    void onShutdown() {
        encerrarNoMotor();
        m_app.onShutdown();
    }

private:
    App& m_app;
};

} // namespace eruption::compat
