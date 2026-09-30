// Throughput benchmarks: the "done when" criteria of milestones M1 and M2.
//
//   ./build/apps/bench/engine_bench [entities] [workers]
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include "ecs/command_buffer.h"
#include "ecs/components.h"
#include "ecs/world.h"
#include "jobs/scheduler.h"
#include "jobs/system_graph.h"

using namespace engine;
using namespace game;
using Clock = std::chrono::steady_clock;

namespace {
double Ms(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }
void Row(const char* what, double ms, double items, const char* unit)
{
    std::printf("  %-44s %9.2f ms   %8.2f ns/%s   %7.1f M%s/s\n", what, ms, ms * 1e6 / items, unit, items / ms / 1e3, unit);
}
}  // namespace

int main(int argc, char** argv)
{
    const int n = argc > 1 ? std::atoi(argv[1]) : 1'000'000;
    const unsigned workers = argc > 2 ? unsigned(std::atoi(argv[2])) : std::max(1u, std::thread::hardware_concurrency());
    std::printf("ECS benchmark: %d entities, %u workers, %zu-byte chunks\n\n", n, workers, kChunkSize);

    World w;
    w.Register<Position, Velocity, Health, Stunned>();
    auto t0 = Clock::now();
    for (int i = 0; i < n; ++i) w.Create(Position{float(i), 0, 0}, Velocity{1, 2, 3});
    Row("create (Position, Velocity)", Ms(t0), n, "ent");
    std::printf("  %-44s %zu chunks, %u entities/chunk\n", "  storage", w.ChunkCount(),
                w.ArchetypeOf(Entity{0, 1})->ChunkCapacity());

    auto q = w.Query<Position, const Velocity>();
    t0 = Clock::now();
    q.ForEach([](Entity, Position& p, const Velocity& v) { p.x += v.x * 0.016f; p.y += v.y * 0.016f; p.z += v.z * 0.016f; });
    Row("iterate, single thread (ForEach)", Ms(t0), n, "ent");

    t0 = Clock::now();
    float sum = 0;
    for (auto [e, p, v] : q) { (void)e; sum += p.x + v.x; }
    Row("iterate, single thread (range-for)", Ms(t0), n, "ent");

    Scheduler sched(workers);
    SystemPipeline pipe(w, sched);
    pipe.Each<Position, const Velocity>("move", [](SystemContext& c, Entity, Position& p, const Velocity& v) {
        p.x += v.x * c.dt; p.y += v.y * c.dt; p.z += v.z * c.dt;
    });
    pipe.Run(0.016f);                                  // warm up
    t0 = Clock::now();
    constexpr int kFrames = 10;
    FrameStats fs;
    for (int f = 0; f < kFrames; ++f) fs = pipe.Run(0.016f);
    Row("iterate, parallel pipeline (per frame)", Ms(t0) / kFrames, n, "ent");
    std::printf("  %-44s %u jobs/frame\n", "  ", fs.jobs);

    // Structural changes through command buffers.
    std::vector<CommandBuffer> bufs(64, CommandBuffer(&w));
    const int changes = n / 10;
    {
        StructureLock lock(w);
        sched.ParallelFor(size_t(changes), size_t(changes / 64 + 1), [&](size_t b, size_t e) {
            CommandBuffer& cb = bufs[b / size_t(changes / 64 + 1)];
            for (size_t i = b; i < e; ++i) cb.AddComponent(Entity{uint32_t(i * 10), 1}, Health{});
        });
    }
    t0 = Clock::now();
    const size_t applied = w.ApplyCommandBuffers(std::span<CommandBuffer>(bufs));
    Row("apply: add component via command buffers", Ms(t0), double(applied), "cmd");

    // Tag churn: archetype storage vs sparse set (design doc §4.1), on
    // "fat" entities - toggling a tag in archetype storage copies them all.
    const int churn = std::min(n, 100'000);
    World a;
    a.Register<Position, Velocity, Orientation, Spin, Health, MeshRenderer, Stunned>();
    for (int i = 0; i < churn; ++i) a.Create(Position{}, Velocity{}, Orientation{}, Spin{}, Health{}, MeshRenderer{});
    t0 = Clock::now();
    for (int i = 0; i < churn; ++i) a.Add<Stunned>(Entity{uint32_t(i), 1});
    for (int i = 0; i < churn; ++i) a.Remove<Stunned>(Entity{uint32_t(i), 1});
    Row("tag toggle, archetype storage (6 comps)", Ms(t0), 2.0 * churn, "op");

    World s;
    s.Register<Position, Velocity, Orientation, Spin, Health, MeshRenderer>();
    s.RegisterSparse<Stunned>();
    for (int i = 0; i < churn; ++i) s.Create(Position{}, Velocity{}, Orientation{}, Spin{}, Health{}, MeshRenderer{});
    t0 = Clock::now();
    for (int i = 0; i < churn; ++i) s.AddSparse<Stunned>(Entity{uint32_t(i), 1});
    for (int i = 0; i < churn; ++i) s.RemoveSparse<Stunned>(Entity{uint32_t(i), 1});
    Row("tag toggle, sparse set (6 comps, hybrid)", Ms(t0), 2.0 * churn, "op");

    // Scheduler overhead.
    const int jobs = 200'000;
    JobCounter c;
    t0 = Clock::now();
    for (int i = 0; i < jobs; ++i) sched.Submit([] {}, &c);
    sched.Wait(c);
    Row("scheduler: empty jobs", Ms(t0), jobs, "job");
    const auto st = sched.GetStats();
    std::printf("  %-44s %llu executed, %llu stolen\n", "  ", static_cast<unsigned long long>(st.executed), static_cast<unsigned long long>(st.stolen));

    std::string err;
    std::printf("\nworld valid: %s  (checksum %.1f)\n", w.Validate(&err) ? "yes" : err.c_str(), double(sum));
    return 0;
}
