#include <doctest/doctest.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>
#include "ecs/components.h"
#include "jobs/system_graph.h"
#include "modules/loader.h"

using namespace engine;
using namespace game;
namespace fs = std::filesystem;

#ifndef GAMEPLAY_V1_PATH
#error "tests/CMakeLists.txt passes the module paths"
#endif

namespace {

struct LiveModule {
    fs::path path = fs::temp_directory_path() / ("ecs_test_live_gameplay" + platform::SharedLibraryExtension());
    ~LiveModule() { std::error_code ec; fs::remove(path, ec); }
    // Simulate "the build just wrote a new version".
    void Install(const char* built)
    {
        const bool existed = fs::exists(path);
        const auto before = existed ? fs::last_write_time(path) : fs::file_time_type{};
        fs::copy_file(built, path, fs::copy_options::overwrite_existing);
        if (existed) fs::last_write_time(path, before + std::chrono::seconds(1));
    }
    void InstallGarbage()
    {
        const auto before = fs::last_write_time(path);
        std::ofstream(path, std::ios::binary | std::ios::trunc) << "this is not a shared library";
        fs::last_write_time(path, before + std::chrono::seconds(1));
    }
};

void Populate(World& w, int n)
{
    w.Register<Position, Velocity, Orientation, Spin, Lifetime, Health>();
    for (int i = 0; i < n; ++i) {
        Entity e = w.Create(Position{}, Velocity{1, 0, 0}, Health{10, 100});
        if (i % 4 == 0) w.Add(e, Lifetime{0.45f});
        if (i % 3 == 0) { w.Add(e, Orientation{}); w.Add(e, Spin{}); }
    }
}

bool PollUntilReloaded(ModuleHost& host, std::string* err = nullptr)
{
    for (int i = 0; i < 3; ++i)
        if (host.PollAndReload(0, err)) return true;
    return false;
}

}  // namespace

TEST_CASE("hot reload: new behaviour and new systems, world preserved")
{
    Scheduler sched(4);
    World w;
    Populate(w, 4000);
    SystemPipeline pipeline(w, sched);
    ModuleHost host(w, sched, pipeline);
    LiveModule live;
    live.Install(GAMEPLAY_V1_PATH);

    std::string err;
    REQUIRE_MESSAGE(host.Load(live.path.string(), &err), err);
    CHECK(host.ModuleVersion() == 1);
    CHECK(host.SystemNames() == std::vector<std::string>{"move", "spin", "expire"});

    for (int i = 0; i < 5; ++i) pipeline.Run(0.1f);
    CHECK(w.EntityCount() == 3000);                      // Lifetime 0.45 expired via command buffers
    for (auto [e, p] : w.Query<const Position>()) { (void)e; CHECK(p.x == doctest::Approx(0.5f)); }

    live.Install(GAMEPLAY_V2_PATH);
    REQUIRE_MESSAGE(PollUntilReloaded(host, &err), err);
    CHECK(host.ModuleVersion() == 2);
    CHECK(host.ReloadCount() == 1);
    CHECK(host.SystemNames() == std::vector<std::string>{"move", "spin", "expire", "regen"});
    CHECK(w.EntityCount() == 3000);                      // state untouched by the reload
    CHECK(w.Validate());

    for (int i = 0; i < 5; ++i) pipeline.Run(0.1f);
    for (auto [e, p, h] : w.Query<const Position, const Health>()) {
        (void)e;
        CHECK(p.x == doctest::Approx(1.5f));             // +0.5 at v1 speed, then +1.0 at v2 speed
        CHECK(h.current == doctest::Approx(12.5f));      // regen only exists in v2
    }
}

TEST_CASE("hot reload: a broken build rolls back to the running one")
{
    Scheduler sched(4);
    World w;
    Populate(w, 500);
    SystemPipeline pipeline(w, sched);
    ModuleHost host(w, sched, pipeline);
    LiveModule live;
    live.Install(GAMEPLAY_V2_PATH);
    REQUIRE(host.Load(live.path.string()));

    live.InstallGarbage();
    std::string err;
    CHECK_FALSE(PollUntilReloaded(host, &err));
    CHECK_FALSE(err.empty());
    CHECK(host.Loaded());
    CHECK(host.ModuleVersion() == 2);                    // still running v2
    CHECK(pipeline.Systems().size() == 4);
    pipeline.Run(0.1f);
    CHECK(w.Validate());

    live.Install(GAMEPLAY_V1_PATH);                      // a good build afterwards loads fine
    CHECK(PollUntilReloaded(host));
    CHECK(host.ModuleVersion() == 1);
}

TEST_CASE("hot reload during heavy job load waits for every in-flight job")
{
    Scheduler sched(std::max(4u, std::thread::hardware_concurrency()));
    World w;
    Populate(w, 20000);
    SystemPipeline pipeline(w, sched);
    ModuleHost host(w, sched, pipeline);
    LiveModule live;
    live.Install(GAMEPLAY_V1_PATH);
    REQUIRE(host.Load(live.path.string()));

    const auto chunks = w.Query<Position, const Velocity>().Chunks();
    REQUIRE(chunks.size() > 8);
    constexpr int kIterations = 200;
    for (int round = 0; round < 6; ++round) {
        // One long-running job per chunk executing module code...
        EngineSystemFn move = host.Entry("move");
        REQUIRE(move);
        const EngineHostApi* api = host.Api();
        {
            StructureLock lock(w);
            for (const ChunkView& v : chunks)
                sched.Submit([move, api, v] {
                    EngineChunk c{v.archetype, v.chunk};
                    for (int i = 0; i < kIterations; ++i) move(api, &c, 0.001f, nullptr);
                });
        }
        // ...and reload while they are still running. Quiesce must wait for
        // all of them before the library is unmapped.
        live.Install(round % 2 ? GAMEPLAY_V1_PATH : GAMEPLAY_V2_PATH);
        std::string err;
        REQUIRE_MESSAGE(host.Reload(&err), err);
        CHECK(sched.InFlight() == 0);
    }
    // Every job ran to completion: 3 rounds at v1 speed, 3 at v2.
    const float expect = 3 * kIterations * 0.001f * 1.0f + 3 * kIterations * 0.001f * 2.0f;
    for (auto [e, p] : w.Query<const Position>()) { (void)e; CHECK(p.x == doctest::Approx(expect).epsilon(1e-4)); }
    CHECK(host.ReloadCount() == 6);
}

TEST_CASE("modules may not reference components the host does not define")
{
    Scheduler sched(2);
    World w;                                   // nothing registered
    SystemPipeline pipeline(w, sched);
    ModuleHost host(w, sched, pipeline);
    LiveModule live;
    live.Install(GAMEPLAY_V1_PATH);
    std::string err;
    CHECK_FALSE(host.Load(live.path.string(), &err));
    CHECK(err.find("does not define") != std::string::npos);
    CHECK(pipeline.Systems().empty());
}
