#pragma once
// Systems, static access declarations and the dependency graph (§5.5, §13.2).
//
// Each system declares what it reads and writes (derived from its query's
// types where possible) and whether it makes structural changes. The graph
// packs non-conflicting systems into stages that run in parallel; a sync
// point - apply all command buffers - happens only after stages that contain
// a structural system (§5.4: sync points are where parallelism dies).
#include <chrono>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>
#include "ecs/archetype.h"
#include "ecs/command_buffer.h"
#include "ecs/query.h"
#include "ecs/world.h"
#include "jobs/scheduler.h"

namespace engine {

struct SystemAccess {
    ComponentMask reads;           // includes everything written
    ComponentMask writes;
    bool structural = false;       // records into command buffers -> needs a sync point after
    bool exclusive = false;        // needs the whole world (scripts, debug UI): runs alone
};

// Two systems conflict if either writes something the other touches (§5.5).
inline bool Conflicts(const SystemAccess& a, const SystemAccess& b)
{
    if (a.exclusive || b.exclusive) return true;
    return a.writes.Intersects(b.reads) || a.writes.Intersects(b.writes) || b.writes.Intersects(a.reads);
}

// Greedy stage packing that PRESERVES REGISTRATION ORDER between conflicting
// systems. A system may join the current stage only if it conflicts with no
// system in the stage AND with no earlier system still waiting - otherwise a
// later reader could run before an earlier writer. (The design doc's §13.2
// reference omits the second check; tests/test_jobs.cpp shows the resulting
// reordering.) Structural systems are barriers: nothing jumps over them.
std::vector<std::vector<uint32_t>> BuildExecutionStages(std::span<const SystemAccess> systems);

struct SystemContext {
    World&         world;
    float          dt;
    CommandBuffer& commands;       // this job's buffer; applied at the next sync point
    uint32_t       job_index;
};

using ChunkSystemFn  = std::function<void(const ChunkView&, SystemContext&)>;
using SingleSystemFn = std::function<void(SystemContext&)>;

struct System {
    std::string    name;
    SystemAccess   access;
    ComponentMask  include, exclude;   // chunk systems: which archetypes to visit
    ChunkSystemFn  chunk_fn;           // run once per chunk batch, in parallel
    SingleSystemFn single_fn;          // or once per frame
    uint32_t       min_batch_entities = 256;   // §5.2: batch small chunks into one job
    uint32_t       owner = 0;          // 0 = host; otherwise the module that registered it
};

struct FrameStats {
    uint32_t stages = 0;
    uint32_t sync_points = 0;
    uint32_t jobs = 0;
    size_t   commands_applied = 0;
    double   apply_ms = 0;
    std::vector<double> stage_ms;
};

class SystemPipeline {
public:
    SystemPipeline(World& w, Scheduler& s) : world_(w), scheduler_(s) {}

    uint32_t Add(System s);

    // Typed helper: fn(SystemContext&, Entity, Ts&...) per entity, access
    // derived from Ts (const = read). Structural systems use ctx.commands.
    template <Component... Ts, typename Fn>
    uint32_t Each(std::string name, Fn fn, bool structural = false, uint32_t owner = 0)
    {
        auto q = world_.Query<Ts...>();
        System s;
        s.name = std::move(name);
        s.access.reads = q.Reads();
        s.access.writes = q.Writes();
        s.access.structural = structural;
        s.include = q.Include();
        s.owner = owner;
        s.chunk_fn = [q, fn](const ChunkView& v, SystemContext& ctx) {
            q.RunChunk(v.archetype, v.chunk, [&](Entity e, Ts&... c) { fn(ctx, e, c...); });
        };
        return Add(std::move(s));
    }

    // Removes every system a module registered (before unloading it, §3.4).
    size_t RemoveOwnedBy(uint32_t owner);
    void   Clear() { systems_.clear(); dirty_ = true; }

    const std::vector<std::vector<uint32_t>>& Stages();
    const std::vector<System>& Systems() const { return systems_; }

    // Runs one frame: stages in order, parallel within a stage, a sync point
    // after any stage containing a structural system.
    FrameStats Run(float dt);

private:
    void RunStage(const std::vector<uint32_t>& stage, float dt, FrameStats& stats);

    World& world_;
    Scheduler& scheduler_;
    std::vector<System> systems_;
    std::vector<std::vector<uint32_t>> stages_;
    bool dirty_ = true;
    std::vector<CommandBuffer> buffers_;     // reused across frames
};

}  // namespace engine
