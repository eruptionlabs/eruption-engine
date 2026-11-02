#include "math/Camera.hpp"

#include <algorithm>
#include <cmath>

namespace eruption {

Camera::Camera() {
    rebuildView();
    rebuildProjection();
    updateFrustum();
}

void Camera::setOrthographic(float left, float right, float bottom, float top, float nearZ, float farZ) {
    m_orthographic = true;
    m_orthoLeft = left;
    m_orthoRight = right;
    m_orthoBottom = bottom;
    m_orthoTop = top;
    m_near = nearZ;
    m_far = farZ;
    rebuildProjection();
    updateFrustum();
}

void Camera::setPerspective(float fov, float aspect, float nearZ, float farZ) {
    m_orthographic = false;
    m_fov = fov;
    m_aspect = aspect;
    m_near = nearZ;
    m_far = farZ;
    rebuildProjection();
    updateFrustum();
}

float Camera::zoomPercent() const {
    constexpr float kFovDeRefDeg = 60.0f;
    const float ref = std::tan(glm::radians(kFovDeRefDeg) * 0.5f);
    const float atual = std::tan(glm::radians(m_fov) * 0.5f);
    // Distancia que daria ESTE mesmo quadro na abertura de referencia.
    const float equivalente = m_orbitDistance * (atual / ref);
    return std::clamp(1.0f - (equivalente - 10.0f) / (1000.0f - 10.0f), 0.0f, 1.0f);
}

void Camera::setFov(float fovDeg) {
    m_orthographic = false;
    m_fov = std::clamp(fovDeg, 1.0f, 170.0f);
    rebuildProjection();
    updateFrustum();
}

void Camera::setPosition(const Vec3& pos) {
    m_position = pos;
    rebuildView();
    updateFrustum();
}

void Camera::setTarget(const Vec3& target) {
    m_target = target;
    rebuildView();
    updateFrustum();
}

void Camera::setUp(const Vec3& up) {
    m_up = up;
    rebuildView();
    updateFrustum();
}

void Camera::pan(float dx, float dy) {
    Vec3 r = right();
    Vec3 u = up();
    Vec3 delta = r * dx + u * dy;
    m_position += delta;
    m_target += delta;
    rebuildView();
    updateFrustum();
}

void Camera::zoom(float factor) {
    // Fator <= 0 (rolagem grande demais num quadro, p.ex. pinca rapida no
    // celular) invertia ou zerava a distancia e a camera pulava longe.
    factor = std::clamp(factor, 0.5f, 2.0f);
    m_zoomLevel *= factor;
    if (m_zoomLevel < 0.1f) m_zoomLevel = 0.1f;
    if (m_zoomLevel > 10.0f) m_zoomLevel = 10.0f;

    if (m_orthographic) {
        float cx = (m_orthoLeft + m_orthoRight) * 0.5f;
        float cy = (m_orthoBottom + m_orthoTop) * 0.5f;
        float hw = (m_orthoRight - m_orthoLeft) * 0.5f / factor;
        float hh = (m_orthoTop - m_orthoBottom) * 0.5f / factor;
        m_orthoLeft = cx - hw;
        m_orthoRight = cx + hw;
        m_orthoBottom = cy - hh;
        m_orthoTop = cy + hh;
        rebuildProjection();
    } else {
        // For perspective, zoom scales the orbit distance
        m_orbitDistance /= factor;
        if (m_debugFreeCamera) {
            // Free camera: full zoom range (0%..100%), no HUD layout clamp.
            if (m_orbitDistance < 10.0f) m_orbitDistance = 10.0f;
            if (m_orbitDistance > m_maxOrbitDistance) m_orbitDistance = m_maxOrbitDistance;
        } else {
            // Clamp the on-screen "Zoom" percentage (computed as
            // 1 - (orbitDistance - 10) / (1000 - 10)) between 70% and 95%.
            // This keeps bind-to-player HUD widgets inside the designed layout
            // bounds at both zoom extremes.
            const float minHudDistance = 10.0f + (1.0f - 0.95f) * (1000.0f - 10.0f); // 59.5
            // O afastamento vai ao menos a 1,5x a distancia padrao: a camera
            // nasce a 1000 e o teto antigo de 307 cortava o primeiro gesto de
            // zoom ali, sem volta para a vista inicial (celular, pinca).
            const float maxHudDistance = std::min(
                std::max(10.0f + (1.0f - 0.70f) * (1000.0f - 10.0f), m_defaultDistance * 1.5f), m_maxOrbitDistance);
            if (m_orbitDistance < minHudDistance) m_orbitDistance = minHudDistance;
            if (m_orbitDistance > maxHudDistance) m_orbitDistance = maxHudDistance;
        }
        rebuildPositionFromOrbit();
    }
    updateFrustum();
}

void Camera::orbit(float deltaYaw, float deltaPitch) {
    if (m_orthographic) return;

    Vec3 offset = m_position - m_target;
    float radius = glm::length(offset);
    if (radius < 0.001f) return;

    // Spherical coordinates
    float yaw = std::atan2(offset.z, offset.x) + deltaYaw;
    float pitch = std::asin(offset.y / radius) + deltaPitch;
    pitch = glm::clamp(pitch, -glm::half_pi<float>() + 0.01f, glm::half_pi<float>() - 0.01f);

    offset.x = radius * std::cos(pitch) * std::cos(yaw);
    offset.y = radius * std::sin(pitch);
    offset.z = radius * std::cos(pitch) * std::sin(yaw);

    m_position = m_target + offset;
    rebuildView();
    updateFrustum();
}

void Camera::setOrbit(float yaw, float pitch, float distance) {
    m_orbitYaw = yaw;
    // CLAMPA igual ao arrasto (orbitPitch): se este caminho deixasse a camera
    // fora da faixa, o primeiro frame de arrasto a traria de volta de um golpe
    // - era o "jump" de 19 graus vindo da visao tatica em 89.
    m_orbitPitch = clampPitch(pitch);
    m_orbitDistance = distance;
    if (m_orbitDistance > m_maxOrbitDistance) m_orbitDistance = m_maxOrbitDistance;
    // NOTE: does NOT touch m_default* — setting a pose (map swap, tactical
    // preset) must not redefine the default view. Use setDefaultOrbit() for
    // that; the application HUD zoomScale references defaultOrbitDistance() and
    // breaks if it drifts.
    rebuildPositionFromOrbit();
}

void Camera::setDefaultOrbit(float yaw, float pitch, float distance) {
    m_defaultYaw = yaw;
    m_defaultPitch = pitch;
    m_defaultDistance = distance;
    // O teto de afastamento acompanha o padrao: com abertura estreita a
    // distancia padrao ja' bate no antigo teto fixo de 1000 e a roda do
    // mouse nao afastava mais nada.
    m_maxOrbitDistance = std::max(1000.0f, distance * 7.0f);
}

void Camera::orbitYaw(float delta) {
    m_orbitYaw += delta;
    rebuildPositionFromOrbit();
}

void Camera::orbitPitch(float delta) {
    // Mesma faixa que o setOrbit usa (ver Camera::clampPitch): camera livre
    // vai a -89..89, jogo normal fica nos limites correntes (10..70 por
    // padrao; a visao tatica sobe o teto pra 89 enquanto esta' ativa).
    m_orbitPitch = clampPitch(m_orbitPitch + delta);
    rebuildPositionFromOrbit();
}

void Camera::setOrbitTarget(const Vec3& target) {
    m_target = target;
    m_orbitTarget = target;
    rebuildPositionFromOrbit();
}

void Camera::resetOrbit() {
    m_orbitYaw = m_defaultYaw;
    m_orbitPitch = m_defaultPitch;
    m_orbitDistance = m_defaultDistance;
    m_target = m_orbitTarget;
    rebuildPositionFromOrbit();
}

void Camera::setOrbitOverride(float dist, float pitchLift) {
    if (dist == m_orbitOverride && pitchLift == m_orbitLift) return;
    m_orbitOverride = dist;
    m_orbitLift = pitchLift;
    if (!m_orthographic) rebuildPositionFromOrbit();
}

void Camera::rebuildPositionFromOrbit() {
    const float d = m_orbitOverride < FLT_MAX ? m_orbitOverride : m_orbitDistance;
    const float p = m_orbitPitch + m_orbitLift;
    m_position = m_target + Vec3(
        d * std::cos(p) * std::cos(m_orbitYaw),
        d * std::sin(p),
        d * std::cos(p) * std::sin(m_orbitYaw)
    );
    rebuildView();
    updateFrustum();
}

void Camera::moveTarget(const Vec3& delta) {
    m_target += delta;
    m_orbitTarget = m_target;
    rebuildPositionFromOrbit();
}

void Camera::setFarPlane(float farZ) {
    m_far = farZ;
    rebuildProjection();
    updateFrustum();
}

void Camera::setNearPlane(float nearZ) {
    m_near = nearZ;
    rebuildProjection();
    updateFrustum();
}

void Camera::setAspect(float aspect) {
    m_aspect = aspect;
    if (m_orthographic) {
        float cx = (m_orthoLeft + m_orthoRight) * 0.5f;
        float hh = (m_orthoTop - m_orthoBottom) * 0.5f;
        float hw = hh * aspect;
        m_orthoLeft = cx - hw;
        m_orthoRight = cx + hw;
        rebuildProjection();
        updateFrustum();
    } else {
        rebuildProjection();
        updateFrustum();
    }
}

Vec3 Camera::forward() const {
    return glm::normalize(m_target - m_position);
}

Vec3 Camera::right() const {
    return glm::normalize(glm::cross(m_up, forward()));
}

Vec3 Camera::up() const {
    return glm::normalize(glm::cross(right(), forward()));
}

void Camera::rebuildView() {
    m_view = glm::lookAt(m_position + m_shakeOffset, m_target + m_shakeOffset, m_up);
}

void Camera::shake(float amplitude, float seconds) {
    amplitude = std::max(0.0f, amplitude);
    seconds = std::max(0.0f, seconds);
    // Tremor em andamento com mais forca continua; o novo so' entra se bater mais.
    const float running = m_shakeLeft > 0.0f ? m_shakeAmp * (m_shakeLeft / std::max(1e-4f, m_shakeTotal)) : 0.0f;
    if (amplitude <= running) return;
    m_shakeAmp = amplitude;
    m_shakeTotal = m_shakeLeft = seconds;
    m_shakeTime = 0.0f;
}

void Camera::tickShake(float dt) {
    if (m_shakeLeft <= 0.0f) {
        if (m_shakeOffset != Vec3(0.0f)) {
            m_shakeOffset = Vec3(0.0f);
            rebuildView();
        }
        return;
    }
    m_shakeLeft = std::max(0.0f, m_shakeLeft - dt);
    m_shakeTime += dt;
    // Soma de senos em frequencias que nao batem (nao volta ao mesmo lugar) e
    // decaimento linear ate' o fim: sem RNG, mesmo tremor em qualquer maquina.
    const float decay = m_shakeLeft / std::max(1e-4f, m_shakeTotal);
    const float a = m_shakeAmp * decay * decay;
    const float t = m_shakeTime;
    const Vec3 r = right(), u = up();
    const float sx = std::sin(t * 57.3f) * 0.6f + std::sin(t * 31.1f + 1.7f) * 0.4f;
    const float sy = std::sin(t * 43.7f + 0.9f) * 0.6f + std::sin(t * 71.3f + 2.3f) * 0.4f;
    m_shakeOffset = r * (sx * a) + u * (sy * a);
    rebuildView();
}

void Camera::rebuildProjection() {
    if (m_orthographic) {
        m_proj = glm::ortho(m_orthoLeft, m_orthoRight, m_orthoBottom, m_orthoTop, m_near, m_far);
    } else {
        m_proj = glm::perspective(glm::radians(m_fov), m_aspect, m_near, m_far);
    }
    // Vulkan Y-flip
    m_proj[1][1] *= -1;
}

Mat4 Camera::projJittered() const {
    Mat4 p = m_proj;
    // Translacao no espaco de clip proporcional a w, entao o deslocamento em
    // NDC e' exatamente m_jitterNdc. Perspectiva (LEFT_HANDED): w = z de
    // vista, termo na coluna 2. Ortografica: w = 1, termo na coluna 3.
    if (m_orthographic) {
        p[3][0] += m_jitterNdc.x;
        p[3][1] += m_jitterNdc.y;
    } else {
        p[2][0] += m_jitterNdc.x;
        p[2][1] += m_jitterNdc.y;
    }
    return p;
}

void Camera::updateFrustum() {
    m_frustum.extractFromMatrix(viewProjNoJitter());
}

} // namespace eruption
