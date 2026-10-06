#pragma once

#include <algorithm>

#include "math/Types.hpp"
#include "math/Frustum.hpp"

namespace eruption {

class Camera {
public:
    Camera();

    // Isometric setup
    void setOrthographic(float left, float right, float bottom, float top, float nearZ, float farZ);
    void setPerspective(float fov, float aspect, float nearZ, float farZ);

    // Position / target
    void setPosition(const Vec3& pos);
    void setTarget(const Vec3& target);
    void setUp(const Vec3& up);

    // Pan / zoom helpers
    void pan(float dx, float dy);       // Move camera in XY plane
    void zoom(float factor);            // Scale ortho size
    void orbit(float deltaYaw, float deltaPitch); // Orbit around target (perspective only)

    // legacy-style isometric orbit controls
    void setOrbit(float yaw, float pitch, float distance);
    // Defines the default view (used by resetOrbit and by HUD zoomScale).
    // Called only at boot / initial-camera setup — never on map swap.
    void setDefaultOrbit(float yaw, float pitch, float distance);
    void orbitYaw(float delta);
    void orbitPitch(float delta);
    void setOrbitTarget(const Vec3& target);
    void moveTarget(const Vec3& delta);
    void resetOrbit();
    float zoomLevel() const { return m_zoomLevel; }
    float orbitYaw() const { return m_orbitYaw; }
    float orbitPitch() const { return m_orbitPitch; }
    float orbitDistance() const { return m_orbitDistance; }
    float defaultOrbitDistance() const { return m_defaultDistance; }
    void setMaxOrbitDistance(float maxDist) { m_maxOrbitDistance = maxDist; }
    float maxOrbitDistance() const { return m_maxOrbitDistance; }

    // LIMITES DE PITCH - fonte unica, respeitada TANTO por setOrbit() quanto
    // por orbitPitch(). Antes so' o orbitPitch (arrasto do mouse) clampava:
    // a visao tatica do F7 chamava setOrbit(.., 89 graus, ..), a camera ficava
    // FORA da faixa que o arrasto aceita, e o primeiro frame de arrasto a
    // puxava de 89 pra 70 de uma vez - 19 graus e ~330 unidades de mundo num
    // frame. E' o "jump" que o autor reproduzia no parana_demo varrendo o
    // pitch (2026-09-04). Quem quer uma faixa maior (a propria visao tatica,
    // e os hooks de teste de camera) SOBE o limite em vez de furar o clamp.
    void setPitchLimits(float minRad, float maxRad) {
        m_pitchMin = std::min(minRad, maxRad);
        m_pitchMax = std::max(minRad, maxRad);
    }
    float pitchMin() const { return m_pitchMin; }
    float pitchMax() const { return m_pitchMax; }
    // Faixa ATIVA: a camera livre (botao do meio) tem a sua propria, mais
    // ampla, e continua valendo por cima dos limites de jogo.
    float clampPitch(float pitch) const {
        const float lo = m_debugFreeCamera ? glm::radians(-89.0f) : m_pitchMin;
        const float hi = m_debugFreeCamera ? glm::radians(89.0f) : m_pitchMax;
        return glm::clamp(pitch, lo, hi);
    }

    // Matrices
    //
    // DUAS projecoes, nomes explicitos de proposito (nao existe "a" projecao):
    //   NoJitter  - logica: culling (frustum), LOD, ajuste de sombra, HUD,
    //               picking, gizmos, motion blur, motion vectors.
    //   Jittered  - RASTERIZACAO na resolucao de render: tudo que desenha ou
    //               reconstroi posicao a partir do depth buffer do frame. O
    //               depth do G-buffer e' gerado com ESTA matriz.
    // Sem jitter ligado (setJitterNdc nunca chamado ou zero) as duas sao
    // iguais. O jitter so' faz sentido com um acumulador temporal (FSR).
    const Mat4& viewMatrix() const { return m_view; }
    const Mat4& projNoJitter() const { return m_proj; }
    Mat4 viewProjNoJitter() const { return m_proj * m_view; }
    Mat4 projJittered() const;
    Mat4 viewProjJittered() const { return projJittered() * m_view; }
    Mat4 skyboxViewProjJittered() const {
        // Remove translation so the skybox stays at infinity and the camera position
        // does not warp reconstructed ray directions.
        Mat4 view = m_view;
        view[3] = Vec4(0.0f, 0.0f, 0.0f, 1.0f);
        return projJittered() * view;
    }

    // Deslocamento sub-pixel da rasterizacao, em NDC do Vulkan (x direita,
    // y para BAIXO; 1 pixel = 2/largura). Valido ate' o proximo set.
    void setJitterNdc(const Vec2& jitter) { m_jitterNdc = jitter; }
    const Vec2& jitterNdc() const { return m_jitterNdc; }

    // Frustum
    void updateFrustum();
    const Frustum& frustum() const { return m_frustum; }

    // Accessors
    Vec3 position() const { return m_position; }
    Vec3 target() const { return m_target; }
    Vec3 forward() const;
    Vec3 right() const;
    Vec3 up() const;

    float nearPlane() const { return m_near; }
    float farPlane() const { return m_far; }
    void setFarPlane(float farZ);
    void setNearPlane(float nearZ);
    float fov() const { return m_fov; }
    float aspect() const { return m_aspect; }
    void setAspect(float aspect);

    bool isOrthographic() const { return m_orthographic; }

    void setDebugFreeCamera(bool enabled) { m_debugFreeCamera = enabled; }
    bool debugFreeCamera() const { return m_debugFreeCamera; }

private:
    Vec3 m_position = Vec3(0.0f, 0.0f, 10.0f);
    Vec3 m_target = Vec3(0.0f, 0.0f, 0.0f);
    Vec3 m_up = Vec3(0.0f, 1.0f, 0.0f);

    Mat4 m_view = Mat4(1.0f);
    Mat4 m_proj = Mat4(1.0f); // sempre SEM jitter
    Vec2 m_jitterNdc = Vec2(0.0f);
    Frustum m_frustum;

    float m_near = 0.1f;
    float m_far = 1000.0f;
    float m_fov = 60.0f;
    float m_aspect = 16.0f / 9.0f;
    float m_zoomLevel = 1.0f;

    // Ortho params
    float m_orthoLeft = -10.0f;
    float m_orthoRight = 10.0f;
    float m_orthoBottom = -10.0f;
    float m_orthoTop = 10.0f;

    bool m_orthographic = true;
    bool m_debugFreeCamera = false;
    // Faixa de pitch do JOGO (a camera livre usa a dela, ver clampPitch).
    // 10 graus = nao entrar no chao; 70 = acima disso e' visao tatica/mapa,
    // que sobe o limite explicitamente ao entrar.
    float m_pitchMin = glm::radians(10.0f);
    float m_pitchMax = glm::radians(70.0f);

    // Orbit state for isometric controls
    float m_orbitYaw = 0.0f;
    float m_orbitPitch = 0.0f;
    float m_orbitDistance = 208.0f;
    Vec3 m_orbitTarget = Vec3(0.0f);
    float m_defaultYaw = 0.0f;
    float m_defaultPitch = 0.0f;
    float m_defaultDistance = 208.0f;
    float m_maxOrbitDistance = 1000.0f;

    void rebuildView();
    void rebuildProjection();
    void rebuildPositionFromOrbit();
};

} // namespace eruption
