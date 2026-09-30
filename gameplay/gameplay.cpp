// The reloadable gameplay module: SYSTEMS ONLY (design doc §3.2, §3.3).
//
// No component definitions (they come from engine/ecs/components.h, owned by
// the host), no globals, no statics, no virtual functions, no GPU resources.
// Everything below is either a function or constant data.
//
// Built twice by CMake (GAMEPLAY_VERSION=1 and 2) so the tests can reload one
// over the other; edit and rebuild `gameplay` to hot reload it in the demo.
#include <cmath>
#include "modules/module_sdk.h"

#ifndef GAMEPLAY_VERSION
#define GAMEPLAY_VERSION 2
#endif

using namespace engine;
using namespace game;

#if GAMEPLAY_VERSION >= 2
constexpr float kMoveSpeed = 2.0f;      // v2: everything moves twice as fast
#else
constexpr float kMoveSpeed = 1.0f;
#endif

ENGINE_MODULE_EXPORT void System_Move(const EngineHostApi* api, const EngineChunk* c, float dt, void*)
{
    auto* pos = sdk::Column<Position>(api, c);
    const auto* vel = sdk::Column<const Velocity>(api, c);
    const uint32_t n = sdk::Count(api, c);
    for (uint32_t i = 0; i < n; ++i) {
        pos[i].x += vel[i].x * dt * kMoveSpeed;
        pos[i].y += vel[i].y * dt * kMoveSpeed;
        pos[i].z += vel[i].z * dt * kMoveSpeed;
    }
}

ENGINE_MODULE_EXPORT void System_Spin(const EngineHostApi* api, const EngineChunk* c, float dt, void*)
{
    auto* orient = sdk::Column<Orientation>(api, c);
    const auto* spin = sdk::Column<const Spin>(api, c);
    const uint32_t n = sdk::Count(api, c);
    for (uint32_t i = 0; i < n; ++i) {
        const float half = spin[i].radians_per_second * dt * 0.5f;
        const float s = std::sin(half);
        const float ax = spin[i].axis[0], ay = spin[i].axis[1], az = spin[i].axis[2];
        const float len = std::sqrt(ax * ax + ay * ay + az * az);
        if (len == 0.0f) continue;
        const float dx = ax / len * s, dy = ay / len * s, dz = az / len * s, dw = std::cos(half);
        float* q = orient[i].q;
        const float x = dw * q[0] + dx * q[3] + dy * q[2] - dz * q[1];
        const float y = dw * q[1] - dx * q[2] + dy * q[3] + dz * q[0];
        const float z = dw * q[2] + dx * q[1] - dy * q[0] + dz * q[3];
        const float w = dw * q[3] - dx * q[0] - dy * q[1] - dz * q[2];
        q[0] = x; q[1] = y; q[2] = z; q[3] = w;
    }
}

// Structural: destroys entities whose lifetime runs out (via the job's command buffer).
ENGINE_MODULE_EXPORT void System_Expire(const EngineHostApi* api, const EngineChunk* c, float dt, void* cmds)
{
    auto* life = sdk::Column<Lifetime>(api, c);
    const uint64_t* ents = api->chunk_entities(c);
    const uint32_t n = sdk::Count(api, c);
    for (uint32_t i = 0; i < n; ++i) {
        life[i].seconds -= dt;
        if (life[i].seconds <= 0.0f) api->cmd_destroy(cmds, ents[i]);
    }
}

#if GAMEPLAY_VERSION >= 2
// New in v2: appears in the pipeline after a reload, with no restart.
ENGINE_MODULE_EXPORT void System_Regen(const EngineHostApi* api, const EngineChunk* c, float dt, void*)
{
    auto* hp = sdk::Column<Health>(api, c);
    const uint32_t n = sdk::Count(api, c);
    for (uint32_t i = 0; i < n; ++i) hp[i].current = std::fmin(hp[i].max, hp[i].current + 5.0f * dt);
}
#endif

// ---------------------------------------------------------------- manifest (constant data only)

namespace {
constexpr const char* kMoveReads[] = {"game::Velocity", nullptr};
constexpr const char* kMoveWrites[] = {"game::Position", nullptr};
constexpr const char* kSpinReads[] = {"game::Spin", nullptr};
constexpr const char* kSpinWrites[] = {"game::Orientation", nullptr};
constexpr const char* kNone[] = {nullptr};
constexpr const char* kLifeWrites[] = {"game::Lifetime", nullptr};
#if GAMEPLAY_VERSION >= 2
constexpr const char* kHealthWrites[] = {"game::Health", nullptr};
#endif

constexpr EngineSystemDesc kSystems[] = {
    {"move", kMoveReads, kMoveWrites, 0, "System_Move", 256},
    {"spin", kSpinReads, kSpinWrites, 0, "System_Spin", 256},
    {"expire", kNone, kLifeWrites, 1, "System_Expire", 256},
#if GAMEPLAY_VERSION >= 2
    {"regen", kNone, kHealthWrites, 0, "System_Regen", 256},
#endif
};

constexpr EngineManifest kManifest = {
    ENGINE_MODULE_ABI_VERSION, "gameplay", GAMEPLAY_VERSION, uint32_t(sizeof(kSystems) / sizeof(kSystems[0])), kSystems,
};
}  // namespace

ENGINE_MODULE_EXPORT const EngineManifest* EngineModuleManifest() { return &kManifest; }
