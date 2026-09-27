#pragma once

#include "math/Types.hpp"
#include <vector>
#include <string>

namespace eruption {

// Generic map metadata (lights, water, bounds, model placements).
// This is authored as JSON and embedded into map bundles or loaded as companion .env files.

enum class WorldObjectType : uint32_t {
    Model = 1,
    Light = 2,
    Sound = 3,
    Effect = 4,
};

struct WorldModel {
    std::string name;
    std::string modelFile;
    Vec3 position;
    Vec3 rotation; // graus, ordem Y-X-Z
    Vec3 scale;

    // M_world = T * R_y * R_x * R_z * S
    Mat4 worldMatrix() const {
        Mat4 T = glm::translate(Mat4(1.0f), position);
        Mat4 Ry = glm::rotate(Mat4(1.0f), glm::radians(rotation.y), Vec3(0.0f, 1.0f, 0.0f));
        Mat4 Rx = glm::rotate(Mat4(1.0f), glm::radians(rotation.x), Vec3(1.0f, 0.0f, 0.0f));
        Mat4 Rz = glm::rotate(Mat4(1.0f), glm::radians(rotation.z), Vec3(0.0f, 0.0f, 1.0f));
        Mat4 S = glm::scale(Mat4(1.0f), scale);
        return T * Ry * Rx * Rz * S;
    }
};

struct WorldLight {
    std::string name;
    Vec3 position;
    Vec3 color;
    float range;
};

struct WorldSound {
    std::string name;
    std::string waveFile;
    Vec3 position;
    float volume;
    int32_t width;
    int32_t height;
    float range;
    float cycle = 0.0f;
};

struct WorldWater {
    float level = 0.0f;
    int32_t type = 0;
    float waveHeight = 0.2f;
    float waveSpeed = 2.0f;
    float wavePitch = 50.0f;
    float animSpeed = 3.0f; // float since v1.8 per spec
};

struct WorldLightInfo {
    int32_t longitude = 45;
    int32_t latitude = 45;
    Vec3 diffuse = Vec3(1.0f);
    Vec3 ambient = Vec3(0.3f);
    float opacity = 1.0f;

    Vec3 sunDirection() const {
        float lon = static_cast<float>(longitude);
        float lat = static_cast<float>(latitude);
        float theta = glm::radians(lon);
        float phi = glm::radians(lat);
        Vec3 dir;
        dir.x = cos(phi) * sin(theta);
        dir.y = -sin(phi); // negative to point down (from sky to ground)
        dir.z = cos(phi) * cos(theta);

        // Ensure it always points downwards into the scene (Y < 0 for Vulkan Y-down? Or Y-up?)
        // The prompt says: "Se a luz estiver vindo de BAIXO (y negativo), inverta o eixo Y: direction.y = -direction.y"
        // Wait, if Y is up, then light from sky points down (y < 0). If Y is down, light points up (y > 0).
        // Keep light direction pointing downward:
        if (dir.y > 0.0f) dir.y = -dir.y;

        return glm::normalize(dir);
    }
};

struct WorldGroundInfo {
    int32_t top = -500;
    int32_t bottom = 500;
    int32_t left = -500;
    int32_t right = 500;
};

struct WorldFile {
    uint8_t versionMajor = 0;
    uint8_t versionMinor = 0;
    uint32_t buildNumber = 0;
    WorldWater water;
    WorldLightInfo light;
    WorldGroundInfo ground;

    std::vector<WorldModel> models;
    std::vector<WorldLight> lights;
    std::vector<WorldSound> sounds;

    uint32_t totalObjectCount() const {
        return static_cast<uint32_t>(models.size() + lights.size() + sounds.size());
    }
};

} // namespace eruption
