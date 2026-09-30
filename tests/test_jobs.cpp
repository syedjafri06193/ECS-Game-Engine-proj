#include <doctest/doctest.h>
#include <algorithm>
#include <atomic>
#include <random>
#include <set>
#include <thread>
#include "ecs/components.h"
#include "jobs/deque.h"
#include "jobs/scheduler.h"
#include "jobs/system_graph.h"

using namespace engine;
using namespace game;

namespace {
unsigned Workers() { return std::max(4u, std::thread::hardware_concurrency()); }
}

TEST_CASE("deque: owner is LIFO, thieves take each item exactly once")
{
    WorkStealingDeque<int> d(1024);
    int items[4] = {0, 1, 2, 3};
    for (int& i : items) CHECK(d.Push(&i));
    CHECK(*d.Pop() == 3);
    CHECK(*d.Steal() == 0);                     // oldest
    CHECK(*d.Pop() == 2);
    CHECK(*d.Pop() == 1);
    CHECK(d.Pop() == nullptr);
    CHECK(d.Steal() == nullptr);

    // Concurrent: owner pushes/pops, 3 thieves steal. Every item consumed once.
    constexpr int kItems = 200000;
    std::vector<int> values(kItems);
    std::vector<std::atomic<int>> taken(kItems);
    WorkStealingDeque<int> q(4096);
    std::atomic<bool> done{false};
    std::vector<std::thread> thieves;
    for (int t = 0; t < 3; ++t)
        thieves.emplace_back([&] {
            while (!done.load()) {
                if (int* v = q.Steal()) taken[size_t(*v)].fetch_add(1);
            }
            while (int* v = q.Steal()) taken[size_t(*v)].fetch_add(1);
        });
    for (int i = 0; i < kItems; ++i) {
        values[size_t(i)] = i;
        while (!q.Push(&values[size_t(i)])) {
            if (int* v = q.Pop()) taken[size_t(*v)].fetch_add(1);
        }
        if (i % 3 == 0)
            if (int* v = q.Pop()) taken[size_t(*v)].fetch_add(1);
    }
    while (int* v = q.Pop()) taken[size_t(*v)].fetch_add(1);
    done = true;
    for (auto& t : thieves) t.join();
    int bad = 0;
    for (auto& t : taken) bad += t.load() != 1;
    CHECK(bad == 0);
}

TEST_CASE("scheduler: parallel sum, nested jobs and helping waits")
{
    Scheduler s(Workers());
    std::atomic<int64_t> sum{0};
    s.ParallelFor(100000, 1000, [&](size_t b, size_t e) {
        int64_t local = 0;
        for (size_t i = b; i < e; ++i) local += int64_t(i);
        sum += local;
    });
    CHECK(sum == int64_t(100000) * 99999 / 2);

    // Jobs that submit and wait on children (nested waits must not deadlock).
    std::atomic<int> leaves{0};
    JobCounter outer;
    for (int i = 0; i < 64; ++i)
        s.Submit([&] {
            JobCounter inner;
            for (int k = 0; k < 16; ++k) s.Submit([&] { leaves.fetch_add(1); }, &inner);
            s.Wait(inner);
        }, &outer);
    s.Wait(outer);
    CHECK(leaves == 64 * 16);
    CHECK(s.InFlight() == 0);
    CHECK(s.GetStats().executed > 0);
}

TEST_CASE("scheduler: quiesce waits for queued work and blocks external submitters")
{
    Scheduler s(Workers());
    std::atomic<int> ran{0};
    JobCounter c;
    for (int i = 0; i < 2000; ++i)
        s.Submit([&] {
            std::this_thread::sleep_for(std::chrono::microseconds(20));
            ran.fetch_add(1);
        }, &c);

    s.Quiesce();                                // must wait for all 2000, not just stop dispatch
    CHECK(ran == 2000);
    CHECK(s.InFlight() == 0);
    CHECK(s.Quiesced());

    std::atomic<bool> submitted{false};
    std::thread external([&] {
        s.Submit([&] { ran.fetch_add(1); });   // blocks until Resume
        submitted = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK_FALSE(submitted.load());
    CHECK(ran == 2000);
    s.Resume();
    external.join();
    CHECK(submitted.load());
    s.Quiesce();
    CHECK(ran == 2001);
    s.Resume();
    s.Wait(c);
}

// ---------------------------------------------------------------- dependency graph

namespace {
struct Fixture {
    World w;
    ComponentId pos, vel, hp, life;
    Fixture()
    {
        pos = w.Id<Position>(); vel = w.Id<Velocity>(); hp = w.Id<Health>(); life = w.Id<Lifetime>();
    }
    SystemAccess A(std::initializer_list<ComponentId> r, std::initializer_list<ComponentId> wr, bool structural = false)
    {
        SystemAccess a;
        for (auto c : r) a.reads.Set(c);
        for (auto c : wr) { a.writes.Set(c); a.reads.Set(c); }
        a.structural = structural;
        return a;
    }
};

// The design doc's §13.2 reference, verbatim in structure, for comparison.
std::vector<std::vector<uint32_t>> DocReferenceStages(std::span<const SystemAccess> systems)
{
    std::vector<std::vector<uint32_t>> stages;
    std::vector<bool> scheduled(systems.size(), false);
    size_t remaining = systems.size();
    while (remaining > 0) {
        std::vector<uint32_t> stage;
        ComponentMask stage_writes, stage_reads;
        for (size_t i = 0; i < systems.size(); ++i) {
            if (scheduled[i]) continue;
            const auto& s = systems[i];
            if (s.structural && !stage.empty()) continue;
            if (s.writes.Intersects(stage_reads) || s.writes.Intersects(stage_writes) || s.reads.Intersects(stage_writes))
                continue;
            stage.push_back(uint32_t(i));
            stage_writes |= s.writes;
            stage_reads |= s.reads;
            scheduled[i] = true;
            --remaining;
            if (s.structural) break;
        }
        stages.push_back(stage);
    }
    return stages;
}

size_t StageOf(const std::vector<std::vector<uint32_t>>& stages, uint32_t sys)
{
    for (size_t i = 0; i < stages.size(); ++i)
        if (std::find(stages[i].begin(), stages[i].end(), sys) != stages[i].end()) return i;
    return ~size_t(0);
}
}  // namespace

TEST_CASE("stages: independent systems run together")
{
    Fixture f;
    std::vector<SystemAccess> s = {
        f.A({f.vel}, {f.pos}),      // 0 move
        f.A({}, {f.hp}),            // 1 regen
        f.A({f.pos}, {}),           // 2 reads pos -> after 0
        f.A({}, {f.life}),          // 3 independent
    };
    auto stages = BuildExecutionStages(s);
    REQUIRE(stages.size() == 2);
    CHECK(stages[0] == std::vector<uint32_t>{0, 1, 3});
    CHECK(stages[1] == std::vector<uint32_t>{2});
}

TEST_CASE("stages: registration order is preserved between conflicting systems")
{
    // A writes X; B reads X and writes Y; C reads Y.  Required order A < B < C.
    Fixture f;
    const ComponentId X = f.pos, Y = f.vel;
    std::vector<SystemAccess> s = {f.A({}, {X}), f.A({X}, {Y}), f.A({Y}, {})};

    auto doc = DocReferenceStages(s);
    // The doc's greedy packer puts C in stage 0 with A: C reads Y before B wrote it.
    CHECK(StageOf(doc, 2) < StageOf(doc, 1));

    auto ours = BuildExecutionStages(s);
    CHECK(StageOf(ours, 0) < StageOf(ours, 1));
    CHECK(StageOf(ours, 1) < StageOf(ours, 2));
}

TEST_CASE("stages: structural systems are barriers but may share a stage with earlier work")
{
    Fixture f;
    std::vector<SystemAccess> s = {
        f.A({}, {f.pos}),                 // 0
        f.A({f.life}, {}, true),          // 1 structural (despawn)
        f.A({}, {f.hp}),                  // 2 independent, but registered after the despawn
    };
    auto stages = BuildExecutionStages(s);
    REQUIRE(stages.size() == 2);
    CHECK(stages[0] == std::vector<uint32_t>{0, 1});
    CHECK(stages[1] == std::vector<uint32_t>{2});   // must observe the despawn
}

TEST_CASE("pipeline: parallel chunk systems, one sync point, deterministic results")
{
    auto run = [](unsigned workers) {
        Scheduler sched(workers);
        World w;
        w.Register<Position, Velocity, Lifetime, Health>();
        for (int i = 0; i < 20000; ++i)
            w.Create(Position{0, 0, 0}, Velocity{1, float(i % 7), 0}, Lifetime{float(i % 50) * 0.1f});
        SystemPipeline p(w, sched);
        p.Each<Position, const Velocity>("move", [](SystemContext& ctx, Entity, Position& pos, const Velocity& v) {
            pos.x += v.x * ctx.dt; pos.y += v.y * ctx.dt;
        });
        p.Each<Lifetime>("age", [](SystemContext& ctx, Entity e, Lifetime& l) {
            l.seconds -= ctx.dt;
            if (l.seconds <= 0) {
                ctx.commands.DestroyEntity(e);
                Entity spawn = ctx.commands.CreateEntity();       // placeholder, resolved at apply
                ctx.commands.AddComponent(spawn, Health{float(e.index)});
            }
        }, /*structural=*/true);

        FrameStats last;
        for (int frame = 0; frame < 30; ++frame) last = p.Run(0.1f);
        CHECK(last.sync_points == 1);
        CHECK(last.stages == 1);              // move + age run together, then one sync
        std::string err;
        CHECK_MESSAGE(w.Validate(&err), err);

        // Fingerprint of the whole world.
        uint64_t h = 1469598103934665603ull;
        for (auto [e, hp] : w.Query<const Health>()) h = (h ^ (e.Bits() * 31 + uint64_t(hp.current))) * 1099511628211ull;
        for (auto [e, pos] : w.Query<const Position>()) h = (h ^ (e.Bits() + uint64_t(pos.x * 1000))) * 1099511628211ull;
        return std::make_pair(h, w.EntityCount());
    };
    const auto a = run(1);
    const auto b = run(Workers());
    const auto c = run(Workers());
    CHECK(a == b);                            // same result regardless of thread count
    CHECK(b == c);                            // and run to run
    CHECK(a.second == 20000);
}

TEST_CASE("concurrent structural changes never corrupt the world (design doc 14.2)")
{
    World w;
    Scheduler s(Workers());
    w.Register<Position>();
    w.Register<Dead>();
    std::vector<Entity> entities;
    for (int i = 0; i < 10000; ++i) entities.push_back(w.Create(Position{}));

    std::vector<CommandBuffer> buffers;
    const int iterations = 200;
    for (int iter = 0; iter < iterations; ++iter) {
        const size_t n = entities.size(), batch = 256;
        buffers.assign((n + batch - 1) / batch, CommandBuffer(&w));
        {
            StructureLock lock(w);
            s.ParallelFor(n, batch, [&](size_t b, size_t e) {
                CommandBuffer& cb = buffers[b / batch];
                for (size_t i = b; i < e; ++i) {
                    if ((i + size_t(iter)) % 3 == 0) cb.AddComponent(entities[i], Dead{});
                    if ((i + size_t(iter)) % 5 == 0) cb.RemoveComponent<Dead>(entities[i]);
                    if ((i + size_t(iter)) % 97 == 0) cb.DestroyEntity(entities[i]);
                    if (i % 101 == 0) cb.AddComponent(cb.CreateEntity(), Position{float(i)});
                }
            });
        }
        w.ApplyCommandBuffers(std::span<CommandBuffer>(buffers));
        std::string err;
        REQUIRE_MESSAGE(w.Validate(&err), err);
        std::erase_if(entities, [&](Entity e) { return !w.IsAlive(e); });
        for (auto [e, p] : w.Query<const Position>()) { (void)p; if (entities.size() < 10000) entities.push_back(e); }
        std::sort(entities.begin(), entities.end(), [](Entity a, Entity b) { return a.Bits() < b.Bits(); });
        entities.erase(std::unique(entities.begin(), entities.end()), entities.end());
    }
    CHECK(w.Validate());
}
