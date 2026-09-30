#include "jobs/system_graph.h"
#include <algorithm>

namespace engine {

std::vector<std::vector<uint32_t>> BuildExecutionStages(std::span<const SystemAccess> systems)
{
    std::vector<std::vector<uint32_t>> stages;
    std::vector<uint32_t> remaining(systems.size());
    for (uint32_t i = 0; i < remaining.size(); ++i) remaining[i] = i;

    while (!remaining.empty()) {
        std::vector<uint32_t> stage, waiting;
        for (uint32_t i : remaining) {
            const SystemAccess& s = systems[i];
            if (s.exclusive) {
                if (stage.empty()) stage.push_back(i);   // alone in its stage
                break;                                   // nothing jumps over it
            }
            bool blocked = false;
            for (uint32_t j : stage)
                if (Conflicts(systems[j], s)) { blocked = true; break; }
            if (!blocked)
                for (uint32_t j : waiting)               // earlier, not yet scheduled
                    if (Conflicts(systems[j], s)) { blocked = true; break; }
            if (blocked) {
                if (s.structural) break;                 // barrier: later systems must see its changes
                waiting.push_back(i);
                continue;
            }
            // A structural system must not overtake an earlier waiting system:
            // that system would then observe changes registered after it.
            if (s.structural && !waiting.empty()) break;
            stage.push_back(i);
            // Commands apply after the stage; anything registered later must
            // run after that sync point to observe them.
            if (s.structural) break;
        }
        stages.push_back(stage);
        std::erase_if(remaining, [&](uint32_t i) { return std::find(stage.begin(), stage.end(), i) != stage.end(); });
    }
    return stages;
}

uint32_t SystemPipeline::Add(System s)
{
    ENGINE_ASSERT(bool(s.chunk_fn) != bool(s.single_fn), "a system has exactly one of chunk_fn / single_fn");
    s.access.reads |= s.access.writes;
    systems_.push_back(std::move(s));
    dirty_ = true;
    return uint32_t(systems_.size() - 1);
}

size_t SystemPipeline::RemoveOwnedBy(uint32_t owner)
{
    const size_t n = std::erase_if(systems_, [&](const System& s) { return s.owner == owner; });
    if (n) dirty_ = true;
    return n;
}

const std::vector<std::vector<uint32_t>>& SystemPipeline::Stages()
{
    if (dirty_) {
        std::vector<SystemAccess> acc;
        acc.reserve(systems_.size());
        for (const auto& s : systems_) acc.push_back(s.access);
        stages_ = BuildExecutionStages(acc);
        dirty_ = false;
    }
    return stages_;
}

FrameStats SystemPipeline::Run(float dt)
{
    ENGINE_ZONE("SystemPipeline::Run");
    FrameStats stats;
    for (const auto& stage : Stages()) {
        const auto t0 = std::chrono::steady_clock::now();
        RunStage(stage, dt, stats);
        stats.stage_ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        ++stats.stages;
    }
    return stats;
}

void SystemPipeline::RunStage(const std::vector<uint32_t>& stage, float dt, FrameStats& stats)
{
    // Exclusive systems own the world: run inline, structural changes allowed.
    if (stage.size() == 1 && systems_[stage[0]].access.exclusive) {
        CommandBuffer cb(&world_);
        SystemContext ctx{world_, dt, cb, 0};
        systems_[stage[0]].single_fn(ctx);
        stats.commands_applied += world_.ApplyCommandBuffers(std::span<CommandBuffer>(&cb, 1));
        return;
    }

    // Plan jobs: chunk batches per system, in system order - that order is
    // also the command-buffer apply order, so results are deterministic.
    struct JobPlan { const System* sys; size_t first_view, view_count; };
    std::vector<JobPlan> plans;
    std::vector<ChunkView> views;
    bool structural = false;
    for (uint32_t si : stage) {
        const System& s = systems_[si];
        structural |= s.access.structural;
        if (s.single_fn) { plans.push_back({&s, 0, 0}); continue; }
        const auto& archs = world_.Cached(s.include, s.exclude).matched;
        size_t batch_start = views.size(), batch_entities = 0;
        for (Archetype* a : archs)
            for (Chunk* c : a->Chunks()) {
                views.push_back({a, c});
                batch_entities += c->count;
                if (batch_entities >= s.min_batch_entities) {
                    plans.push_back({&s, batch_start, views.size() - batch_start});
                    batch_start = views.size();
                    batch_entities = 0;
                }
            }
        if (views.size() > batch_start) plans.push_back({&s, batch_start, views.size() - batch_start});
    }

    if (buffers_.size() < plans.size()) buffers_.resize(plans.size());
    for (size_t i = 0; i < plans.size(); ++i) buffers_[i].Bind(&world_);

    {
        StructureLock lock(world_);                       // structural change now asserts (§5.3)
        JobCounter counter;
        for (size_t i = 0; i < plans.size(); ++i) {
            const JobPlan* plan = &plans[i];
            CommandBuffer* cb = &buffers_[i];
            const ChunkView* v = views.data();
            World* w = &world_;
            const uint32_t job = uint32_t(i);
            scheduler_.Submit([plan, cb, v, w, dt, job] {
                SystemContext ctx{*w, dt, *cb, job};
                if (plan->sys->single_fn) { plan->sys->single_fn(ctx); return; }
                for (size_t k = 0; k < plan->view_count; ++k) plan->sys->chunk_fn(v[plan->first_view + k], ctx);
            }, &counter);
        }
        scheduler_.Wait(counter);
    }
    stats.jobs += uint32_t(plans.size());

    if (structural) {
        // SYNC POINT: single-threaded, deterministic order (§5.3, §5.4).
        const auto t0 = std::chrono::steady_clock::now();
        std::vector<CommandBuffer*> ptrs;
        for (size_t i = 0; i < plans.size(); ++i) ptrs.push_back(&buffers_[i]);
        stats.commands_applied += world_.ApplyCommandBuffers(std::span<CommandBuffer* const>(ptrs));
        stats.apply_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        ++stats.sync_points;
    } else {
        for (size_t i = 0; i < plans.size(); ++i)
            ENGINE_ASSERT(buffers_[i].Empty(), "system recorded commands but is not declared structural");
    }
}

}  // namespace engine
