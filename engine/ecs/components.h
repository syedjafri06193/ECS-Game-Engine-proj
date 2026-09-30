#pragma once
// HOST-OWNED component types (design doc §3.2).
//
// Every component is defined here and compiled into the host. Reloadable
// modules include this header but contain only systems - they never define a
// component - so reloading a module can never change a layout under data
// already stored in chunks. Changing a definition here requires a restart;
// world state survives via snapshot migration (ecs/snapshot.h), which maps
// fields by name. Bump the version number when you change a layout.
//
// Rules for components: trivially copyable, default constructible, no
// pointers into module memory, fields reflected with ENGINE_FIELD.
#include <cstdint>
#include "core/reflection/reflection.h"

namespace game {

struct Position    { float x = 0, y = 0, z = 0; };
struct Velocity    { float x = 0, y = 0, z = 0; };
struct Orientation { float q[4] = {0, 0, 0, 1}; };             // quaternion x,y,z,w
struct Scale       { float value = 1; };
struct Spin        { float axis[3] = {0, 1, 0}; float radians_per_second = 1; };
struct Health      { float current = 100, max = 100; };
struct Lifetime    { float seconds = 1; };
struct MeshRenderer {
    uint32_t mesh = 0;
    uint32_t material = 0;
    bool     transparent = false;
};
struct PointLight  { float color[3] = {1, 1, 1}; float intensity = 1; float radius = 5; };
struct Camera      { float fov_y = 1.0f; float z_near = 0.1f; float z_far = 200.0f; bool active = true; };

// Tags / volatile state
struct Stunned {};                      // toggled often -> sparse storage in practice (§4.1)
struct Dead {};

}  // namespace game

ENGINE_COMPONENT(game::Position, 1, ENGINE_FIELD(game::Position, x), ENGINE_FIELD(game::Position, y),
                 ENGINE_FIELD(game::Position, z));
ENGINE_COMPONENT(game::Velocity, 1, ENGINE_FIELD(game::Velocity, x), ENGINE_FIELD(game::Velocity, y),
                 ENGINE_FIELD(game::Velocity, z));
ENGINE_COMPONENT(game::Orientation, 1, ENGINE_FIELD(game::Orientation, q));
ENGINE_COMPONENT(game::Scale, 1, ENGINE_FIELD(game::Scale, value));
ENGINE_COMPONENT(game::Spin, 1, ENGINE_FIELD(game::Spin, axis), ENGINE_FIELD(game::Spin, radians_per_second));
ENGINE_COMPONENT(game::Health, 1, ENGINE_FIELD(game::Health, current), ENGINE_FIELD(game::Health, max));
ENGINE_COMPONENT(game::Lifetime, 1, ENGINE_FIELD(game::Lifetime, seconds));
ENGINE_COMPONENT(game::MeshRenderer, 1, ENGINE_FIELD(game::MeshRenderer, mesh),
                 ENGINE_FIELD(game::MeshRenderer, material), ENGINE_FIELD(game::MeshRenderer, transparent));
ENGINE_COMPONENT(game::PointLight, 1, ENGINE_FIELD(game::PointLight, color), ENGINE_FIELD(game::PointLight, intensity),
                 ENGINE_FIELD(game::PointLight, radius));
ENGINE_COMPONENT(game::Camera, 1, ENGINE_FIELD(game::Camera, fov_y), ENGINE_FIELD(game::Camera, z_near),
                 ENGINE_FIELD(game::Camera, z_far), ENGINE_FIELD(game::Camera, active));
ENGINE_TAG(game::Stunned);
ENGINE_TAG(game::Dead);
