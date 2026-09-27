// ============================================================================
// Sprite Shadow System — CPU Unit Tests & Benchmark
// ============================================================================
// Compila com: ERUPTION_BUILD_TESTS=ON
// ============================================================================

#include <iostream>
#include <vector>
#include <cmath>
#include <cassert>
#include <chrono>
#include <random>

#include "math/Types.hpp"

using namespace eruption;

// ============================================================================
// Inline harness (adapted from docs/bug fix/shadows/SpriteShadow_Harness.hpp)
// ============================================================================

struct SpriteShadowDataHarness {
    alignas(16) Vec3 worldPos;
    float groundHeight;
    alignas(16) Vec3 lightDir;
    float shadowScale;
    alignas(16) Vec3 spriteRight;
    float blobAlpha;
    alignas(16) Vec3 spriteUp;
    uint32_t technique; // 0=planar, 1=blob, 2=proxy
    alignas(8) Vec2 spriteSize;
    float fadeStart;
    float fadeEnd;
};
static_assert(sizeof(SpriteShadowDataHarness) == 80, "SpriteShadowDataHarness must be 80 bytes");

class PlanarShadowMathHarness {
public:
    static Mat4 shadowMatrix(const Vec3& lightPos, float groundY) {
        float ly = lightPos.y - groundY;
        if (std::abs(ly) < 1e-6f) ly = 1e-6f;

        Mat4 m(1.0f);
        m[1][0] = -lightPos.x / ly;
        m[1][1] = 0.0f;
        m[1][2] = -lightPos.z / ly;
        m[1][3] = -1.0f / ly;

        m[3][0] = lightPos.x * groundY / ly;
        m[3][1] = groundY;
        m[3][2] = lightPos.z * groundY / ly;
        m[3][3] = groundY / ly;
        return m;
    }

    static bool verifyProjection(const Vec3& point, const Vec3& lightPos,
                                  float groundY, float epsilon = 1e-3f) {
        Mat4 sm = shadowMatrix(lightPos, groundY);
        Vec4 proj = sm * Vec4(point, 1.0f);
        proj /= proj.w;
        return std::abs(proj.y - groundY) < epsilon;
    }
};

struct TestResult {
    std::string name;
    bool passed;
    float durationMs;
    std::string message;
};

// ============================================================================
// Tests
// ============================================================================

TestResult test_planar_projection_correctness() {
    auto t0 = std::chrono::high_resolution_clock::now();
    bool pass = PlanarShadowMathHarness::verifyProjection(
        Vec3(2.0f, 3.0f, 1.0f),
        Vec3(5.0f, 10.0f, 5.0f),
        0.0f
    );
    auto t1 = std::chrono::high_resolution_clock::now();
    float ms = std::chrono::duration<float, std::milli>(t1 - t0).count();
    return {"PlanarProjection_Correctness", pass, ms,
            pass ? "Projecao no plano Y=0 validada" : "Falha na projecao"};
}

TestResult test_planar_degenerate_light() {
    auto t0 = std::chrono::high_resolution_clock::now();
    Mat4 sm = PlanarShadowMathHarness::shadowMatrix(Vec3(0.0f, 0.001f, 0.0f), 0.0f);
    auto t1 = std::chrono::high_resolution_clock::now();
    float ms = std::chrono::duration<float, std::milli>(t1 - t0).count();
    bool pass = !glm::isnan(sm[0][0]) && !glm::isinf(sm[0][0]);
    return {"PlanarProjection_DegenerateLight", pass, ms,
            pass ? "Luz horizontal tratada com epsilon" : "Matriz degenerada"};
}

TestResult test_billboard_basis_orthonormality() {
    auto t0 = std::chrono::high_resolution_clock::now();
    Vec3 camPos(10, 5, 10);
    Vec3 spritePos(0, 2, 0);
    Vec3 toCam = glm::normalize(camPos - spritePos);
    Vec3 up(0, 1, 0);
    Vec3 right = glm::normalize(glm::cross(up, toCam));
    Vec3 billboardUp = glm::normalize(glm::cross(toCam, right));

    bool pass =
        std::abs(glm::dot(right, billboardUp)) < 1e-3f &&
        std::abs(glm::dot(right, toCam)) < 1e-3f &&
        std::abs(glm::length(right) - 1.0f) < 1e-3f;
    auto t1 = std::chrono::high_resolution_clock::now();
    float ms = std::chrono::duration<float, std::milli>(t1 - t0).count();
    return {"BillboardBasis_Orthonormality", pass, ms,
            pass ? "Base ortonormal valida" : "Base degenerada"};
}

TestResult test_ssbo_alignment() {
    auto t0 = std::chrono::high_resolution_clock::now();
    bool pass = (sizeof(SpriteShadowDataHarness) % 16) == 0;
    auto t1 = std::chrono::high_resolution_clock::now();
    float ms = std::chrono::duration<float, std::milli>(t1 - t0).count();
    return {"SSBO_Alignment", pass, ms,
            pass ? "Alinhamento 16-byte OK" : "Alinhamento incorreto"};
}

TestResult test_lod_selection() {
    auto t0 = std::chrono::high_resolution_clock::now();
    // Simulate LOD: 0-10 = proxy, 10-30 = planar, 30-80 = blob, >80 = none
    auto selectTechnique = [](float dist) -> uint32_t {
        if (dist > 80.0f) return 3; // none
        if (dist > 30.0f) return 1; // blob
        if (dist > 10.0f) return 0; // planar
        return 2; // proxy
    };

    bool pass = true;
    pass &= (selectTechnique(5.0f) == 2);
    pass &= (selectTechnique(20.0f) == 0);
    pass &= (selectTechnique(50.0f) == 1);
    pass &= (selectTechnique(100.0f) == 3);

    auto t1 = std::chrono::high_resolution_clock::now();
    float ms = std::chrono::duration<float, std::milli>(t1 - t0).count();
    return {"LOD_Selection", pass, ms,
            pass ? "LOD selection correct" : "LOD selection failed"};
}

// Replicates the billboard-planar-shadow vertex math and checks that the
// shear projection does not move the lower-left pivot point.
TestResult test_planar_foot_camera_invariant() {
    auto t0 = std::chrono::high_resolution_clock::now();

    // Sprite configuration matching Engine.cpp conventions
    Vec3 spritePos(0.0f, 0.0f, 0.0f);
    float spriteHeight = 4.0f;
    float centerY = spriteHeight * 0.5f;
    Vec3 anchorPoint = spritePos;
    anchorPoint.y += centerY;
    float xOff = 0.2f;
    float yOff = -0.3f;
    Vec2 scale(0.6f, 0.6f);

    Vec3 lightDir = glm::normalize(Vec3(0.3f, -0.8f, 0.5f));
    float epsilon = 1e-3f;

    bool pass = true;

    for (int flip = 0; flip < 2; ++flip) {
    for (int i = 0; i < 8; ++i) {
        float camYaw = float(i) * (glm::two_pi<float>() / 8.0f);
        float xOffSigned = (flip != 0) ? -xOff : xOff;
        float camDist = 10.0f;
        Vec3 camPos(
            camDist * std::sin(camYaw),
            5.0f,
            camDist * std::cos(camYaw)
        );
        Vec3 camTarget = anchorPoint;
        Mat4 view = glm::lookAt(camPos, camTarget, Vec3(0.0f, 1.0f, 0.0f));
        Mat4 invView = glm::inverse(view);

        // Same math as billboard_planar_shadow.vert
        Vec3 viewRight(-1.0f, 0.0f, 0.0f);
        Vec3 viewUp(0.0f, 1.0f, 0.0f);
        float tiltRad = glm::radians(15.0f);
        Vec3 tiltedUp = viewUp * std::cos(tiltRad) + Vec3(0.0f, 0.0f, -1.0f) * std::sin(tiltRad);

        float pivotX = (flip != 0) ? 0.5f : -0.5f;
        Vec3 pivotLocal = viewRight * (xOffSigned + pivotX * scale.x) +
                          tiltedUp  * (-yOff + (-0.5f) * scale.y - centerY);
        Vec4 pivotView = view * Vec4(anchorPoint, 1.0f) + Vec4(pivotLocal, 0.0f);
        Vec3 pivotWorld = Vec3(invView * pivotView);

        Vec3 feetLocal = tiltedUp * (-centerY);
        Vec4 feetView  = view * Vec4(anchorPoint, 1.0f) + Vec4(feetLocal, 0.0f);
        Vec3 feetWorld = Vec3(invView * feetView);
        float groundY = feetWorld.y;

        // Shear projection: pivot lies on the plane, so it must not move in X/Z.
        float denom = lightDir.y;
        if (std::abs(denom) < 0.001f) denom = std::copysign(0.001f, denom);
        Vec3 projectedPivot = pivotWorld;
        projectedPivot.y = groundY;
        float shear = (pivotWorld.y - groundY) / denom;
        projectedPivot.x += lightDir.x * shear;
        projectedPivot.z += lightDir.z * shear;

        float dxz = glm::length(Vec2(projectedPivot.x, projectedPivot.z) - Vec2(pivotWorld.x, pivotWorld.z));
        if (dxz > epsilon) {
            pass = false;
            std::cerr << "Pivot moved by projection at camYaw " << camYaw << " flip=" << flip << ": dxz=" << dxz << "\n";
        }
    }
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    float ms = std::chrono::duration<float, std::milli>(t1 - t0).count();
    return {"PlanarFoot_CameraInvariant", pass, ms,
            pass ? "Pivot stays pinned under projection" : "Pivot drifts under projection"};
}

TestResult test_shadow_intensity_follows_sun() {
    auto t0 = std::chrono::high_resolution_clock::now();

    auto shadowAlpha = [](float sunIntensity) -> float {
        return 0.45f * glm::clamp(sunIntensity, 0.0f, 1.0f);
    };

    bool pass = true;
    pass &= (shadowAlpha(0.0f) == 0.0f);
    pass &= (shadowAlpha(1.0f) == 0.45f);
    pass &= (shadowAlpha(0.5f) == 0.225f);
    pass &= (shadowAlpha(2.0f) == 0.45f);

    auto t1 = std::chrono::high_resolution_clock::now();
    float ms = std::chrono::duration<float, std::milli>(t1 - t0).count();
    return {"ShadowIntensityFollowsSun", pass, ms,
            pass ? "Shadow alpha scales with sun intensity" : "Shadow alpha does not scale with sun intensity"};
}

TestResult test_distance_fade() {
    auto t0 = std::chrono::high_resolution_clock::now();
    // smoothstep-based fade
    float fadeStart = 20.0f;
    float fadeEnd = 50.0f;
    auto fade = [&](float dist) -> float {
        return 1.0f - glm::smoothstep(fadeStart, fadeEnd, dist);
    };

    bool pass = true;
    pass &= (fade(10.0f) == 1.0f);
    pass &= (fade(20.0f) == 1.0f);
    pass &= (fade(35.0f) > 0.0f && fade(35.0f) < 1.0f);
    pass &= (fade(50.0f) == 0.0f);
    pass &= (fade(100.0f) == 0.0f);

    auto t1 = std::chrono::high_resolution_clock::now();
    float ms = std::chrono::duration<float, std::milli>(t1 - t0).count();
    return {"DistanceFade", pass, ms,
            pass ? "Distance fade correct" : "Distance fade failed"};
}

// ============================================================================
// Benchmark
// ============================================================================

struct BenchmarkResult {
    uint32_t spriteCount;
    float updateTimeMs;
    float totalTimeMs;
};

BenchmarkResult run_benchmark(uint32_t spriteCount) {
    std::mt19937 rng(42);
    std::uniform_real_distribution<float> dist(-200.0f, 200.0f);
    std::uniform_real_distribution<float> height(0.1f, 10.0f);

    std::vector<SpriteShadowDataHarness> sprites;
    sprites.reserve(spriteCount);
    for (uint32_t i = 0; i < spriteCount; ++i) {
        SpriteShadowDataHarness s{};
        s.worldPos = Vec3(dist(rng), height(rng), dist(rng));
        s.groundHeight = 0.0f;
        s.lightDir = glm::normalize(Vec3(0.3f, -1.0f, 0.5f));
        s.shadowScale = 1.0f;
        s.spriteSize = Vec2(1.0f, 1.0f);
        s.technique = (i % 3);
        s.fadeStart = 20.0f;
        s.fadeEnd = 50.0f;
        sprites.push_back(s);
    }

    Vec3 camPos(0, 10, 50);

    auto t0 = std::chrono::high_resolution_clock::now();

    // Simulate update: culling + LOD
    uint32_t active = 0;
    for (auto& s : sprites) {
        float d = glm::distance(camPos, s.worldPos);
        if (d < s.fadeEnd) {
            if (d < 10.0f) s.technique = 2;
            else if (d < 30.0f) s.technique = 0;
            else s.technique = 1;
            active++;
        }
    }

    auto t1 = std::chrono::high_resolution_clock::now();

    // Simulate shadow matrix calculations
    Vec3 lightPos(50, 100, 50);
    for (const auto& s : sprites) {
        if (s.technique == 0) {
            (void)PlanarShadowMathHarness::shadowMatrix(lightPos, s.groundHeight);
        }
    }

    auto t2 = std::chrono::high_resolution_clock::now();

    return {
        spriteCount,
        std::chrono::duration<float, std::milli>(t1 - t0).count(),
        std::chrono::duration<float, std::milli>(t2 - t0).count()
    };
}

// ============================================================================
// Main
// ============================================================================

int main() {
    std::cout << "\n========================================\n";
    std::cout << "  SPRITE SHADOW SYSTEM TESTS\n";
    std::cout << "========================================\n\n";

    std::vector<TestResult> tests;
    tests.push_back(test_planar_projection_correctness());
    tests.push_back(test_planar_degenerate_light());
    tests.push_back(test_billboard_basis_orthonormality());
    tests.push_back(test_ssbo_alignment());
    tests.push_back(test_lod_selection());
    tests.push_back(test_distance_fade());
    tests.push_back(test_planar_foot_camera_invariant());
    tests.push_back(test_shadow_intensity_follows_sun());

    std::cout << "[UNIT TESTS]\n";
    int passed = 0;
    for (const auto& t : tests) {
        std::cout << (t.passed ? "[PASS] " : "[FAIL] ")
                  << t.name << " (" << t.durationMs << " ms)\n";
        std::cout << "       " << t.message << "\n";
        if (t.passed) passed++;
    }
    std::cout << "\nResult: " << passed << "/" << tests.size() << " passed\n\n";

    std::cout << "[BENCHMARKS]\n";
    for (uint32_t n : {100, 500, 1000, 2000, 5000}) {
        auto b = run_benchmark(n);
        std::cout << "Sprites: " << b.spriteCount
                  << " | Update: " << b.updateTimeMs
                  << " ms | Total: " << b.totalTimeMs << " ms\n";
    }

    std::cout << "\n========================================\n";

    return (passed == (int)tests.size()) ? 0 : 1;
}
