#pragma once
// Lua script systems (design doc §3.1, M7). This is where most of the hot
// reload value lives: gameplay rules reload instantly, with no layouts to
// migrate, no vtables to dangle and no jobs to quiesce.
//
// A script returns a table:
//
//   return {
//     name   = "bob",
//     reads  = { "Velocity" },
//     writes = { "Position" },
//     update = function(world, dt, state)
//       world:each({"Position", "Velocity"}, function(e, pos, vel)
//         pos.y = pos.y + math.sin(state.t) * dt
//       end)
//     end,
//   }
//
// Component proxies read and write fields through reflection. Writing a
// component not listed in `writes` is an error - the declaration is enforced,
// not advisory. Structural changes (spawn/destroy/add/remove) are recorded
// and applied after update() returns. `state` survives reloads; Lua globals
// do not. A script that fails to load or throws keeps the last good version
// running and reports the error instead of crashing.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "ecs/command_buffer.h"
#include "ecs/world.h"
#include "jobs/system_graph.h"

struct lua_State;

namespace engine {

class ScriptSystem {
public:
    ScriptSystem(World& world, std::string path);
    ~ScriptSystem();
    ScriptSystem(const ScriptSystem&) = delete;
    ScriptSystem& operator=(const ScriptSystem&) = delete;

    // Loads (or reloads) the file. On failure the previous version stays active.
    bool Load();
    // Reloads if the file's modification time changed. Returns true if reloaded.
    bool ReloadIfChanged();

    // Runs update(world, dt, state). Exclusive: call from the main thread or
    // register via MakeSystem(), which the pipeline runs alone in its stage.
    bool Update(float dt);
    System MakeSystem();

    const std::string& Name() const { return name_; }
    const std::string& LastError() const { return last_error_; }
    uint32_t ReloadCount() const { return reloads_; }
    bool Healthy() const { return healthy_; }
    const ComponentMask& Writes() const { return writes_; }
    const ComponentMask& Reads() const { return reads_; }

    // Internal: bindings need these.
    World& GetWorld() { return world_; }
    CommandBuffer& Deferred() { return deferred_; }
    ComponentId ResolveComponent(const char* name) const;

private:
    World& world_;
    std::string path_;
    std::string name_;
    lua_State* L_ = nullptr;
    int module_ref_ = -2;       // LUA_NOREF
    int state_ref_ = -2;
    int64_t mtime_ = 0;
    uint32_t reloads_ = 0;
    bool healthy_ = false;
    std::string last_error_;
    ComponentMask reads_, writes_;
    CommandBuffer deferred_;
};

}  // namespace engine
