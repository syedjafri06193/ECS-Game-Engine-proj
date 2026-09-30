#include <doctest/doctest.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include "ecs/components.h"
#include "jobs/system_graph.h"
#include "scripting/script_system.h"

using namespace engine;
using namespace game;
namespace fs = std::filesystem;

namespace {
struct TempScript {
    fs::path path;
    explicit TempScript(const std::string& name) : path(fs::temp_directory_path() / ("ecs_test_" + name + ".lua")) {}
    ~TempScript() { std::error_code ec; fs::remove(path, ec); }
    void Write(const std::string& src)
    {
        const bool existed = fs::exists(path);
        const auto before = existed ? fs::last_write_time(path) : fs::file_time_type{};
        std::ofstream(path) << src;
        // Guarantee a visible mtime change even on coarse-grained filesystems.
        if (existed) fs::last_write_time(path, before + std::chrono::seconds(1));
    }
};

const char* kMover = R"(
return {
  name = "mover",
  reads = { "Velocity" },
  writes = { "Position" },
  update = function(world, dt, state)
    state.frames = (state.frames or 0) + 1
    world:each({ "Position", "Velocity" }, function(e, pos, vel)
      pos.x = pos.x + vel.x * dt
    end)
  end,
}
)";
}  // namespace

TEST_CASE("scripts: read/write through reflection, state persists across reloads")
{
    World w;
    w.Register<Position, Velocity, Health>();
    Entity e = w.Create(Position{}, Velocity{2, 0, 0});
    TempScript f("mover");
    f.Write(kMover);
    ScriptSystem s(w, f.path.string());
    REQUIRE(s.Load());
    for (int i = 0; i < 10; ++i) REQUIRE(s.Update(0.5f));
    CHECK(w.Get<Position>(e)->x == doctest::Approx(10.0));

    // Edit: now also writes y. Hot reload, world and `state` survive.
    std::string v2 = kMover;
    v2.replace(v2.find("pos.x = pos.x + vel.x * dt"), 26, "pos.x = pos.x + vel.x * dt; pos.y = state.frames");
    f.Write(v2);
    CHECK(s.ReloadIfChanged());
    CHECK_FALSE(s.ReloadIfChanged());                 // no change, no reload
    REQUIRE(s.Update(0.5f));
    CHECK(w.Get<Position>(e)->x == doctest::Approx(11.0));
    CHECK(w.Get<Position>(e)->y == doctest::Approx(11.0));   // frames counter kept counting: 11
    CHECK(s.ReloadCount() == 2);
}

TEST_CASE("scripts: a broken edit keeps the last good version running")
{
    World w;
    w.Register<Position, Velocity>();
    Entity e = w.Create(Position{}, Velocity{1, 0, 0});
    TempScript f("broken");
    f.Write(kMover);
    ScriptSystem s(w, f.path.string());
    REQUIRE(s.Load());
    f.Write("return { update = function( ");           // syntax error
    CHECK_FALSE(s.ReloadIfChanged());
    CHECK(s.LastError().find("expected") != std::string::npos);
    CHECK(s.Update(1.0f));                               // old code still runs
    CHECK(w.Get<Position>(e)->x == doctest::Approx(1.0));

    f.Write("return { reads = { \"NoSuchComponent\" }, update = function() end }");
    CHECK_FALSE(s.ReloadIfChanged());                    // typo in a declaration fails the reload
    CHECK(s.LastError().find("NoSuchComponent") != std::string::npos);
    CHECK(s.Update(1.0f));
}

TEST_CASE("scripts: writes are checked against the declaration")
{
    World w;
    w.Register<Position, Velocity>();
    Entity e = w.Create(Position{}, Velocity{1, 0, 0});
    TempScript f("undeclared");
    f.Write(R"(return { reads = {"Position", "Velocity"}, writes = {"Position"},
      update = function(world, dt)
        world:each({"Position", "Velocity"}, function(e, p, v) v.x = 99 end)
      end })");
    ScriptSystem s(w, f.path.string());
    REQUIRE(s.Load());
    CHECK_FALSE(s.Update(0.1f));
    CHECK(s.LastError().find("not declared in this script's writes") != std::string::npos);
    CHECK(w.Get<Velocity>(e)->x == 1.0f);
    CHECK_FALSE(w.StructureLocked());                    // the error path released the lock
    CHECK_FALSE(s.Healthy());
}

TEST_CASE("scripts: structural changes are deferred; stale proxies are caught")
{
    World w;
    w.Register<Position, Velocity, Health, Lifetime>();
    for (int i = 0; i < 10; ++i) w.Create(Position{float(i)}, Health{float(i * 10), 100});
    TempScript f("spawner");
    f.Write(R"(return { reads = {"Position"}, writes = {"Health"},
      update = function(world, dt, state)
        world:each({"Position", "Health"}, function(e, p, h)
          if h.current < 30 then world:destroy(e) end        -- deferred: safe mid-iteration
          state.kept = h                                      -- stash a proxy (bad idea)
        end)
        local s = world:spawn({ Position = { x = 42 }, Lifetime = { seconds = 3 } })
        world:add(s, "Velocity", { x = 1 })
        state.count = world:count({"Position"})
      end })");
    ScriptSystem s(w, f.path.string());
    REQUIRE(s.Load());
    REQUIRE(s.Update(0.1f));
    CHECK(w.EntityCount() == 8);                         // 3 destroyed, 1 spawned
    size_t spawned = 0;
    for (auto [e, p, v, l] : w.Query<const Position, const Velocity, const Lifetime>()) {
        (void)e; (void)v;
        CHECK(p.x == 42.0f);
        CHECK(l.seconds == 3.0f);
        ++spawned;
    }
    CHECK(spawned == 1);
    CHECK(w.Validate());

    f.Write(R"(return { reads = {"Health"}, update = function(world, dt, state) local x = state.kept.current end })");
    REQUIRE(s.ReloadIfChanged());
    CHECK_FALSE(s.Update(0.1f));
    CHECK(s.LastError().find("stale component proxy") != std::string::npos);
}

TEST_CASE("scripts: run as an exclusive stage inside the system pipeline")
{
    Scheduler sched(4);
    World w;
    w.Register<Position, Velocity, Health>();
    for (int i = 0; i < 1000; ++i) w.Create(Position{}, Velocity{1, 0, 0}, Health{50, 100});
    SystemPipeline p(w, sched);
    p.Each<Health>("regen", [](SystemContext& ctx, Entity, Health& h) { h.current += ctx.dt; });
    TempScript f("pipeline");
    f.Write(kMover);
    ScriptSystem s(w, f.path.string());
    REQUIRE(s.Load());
    p.Add(s.MakeSystem());
    p.Each<const Position>("reader", [](SystemContext&, Entity, const Position&) {});
    const auto& stages = p.Stages();
    CHECK(stages.size() == 3);                           // regen | script (alone) | reader
    for (int i = 0; i < 4; ++i) p.Run(0.25f);
    for (auto [e, pos, hp] : w.Query<const Position, const Health>()) {
        (void)e;
        CHECK(pos.x == doctest::Approx(1.0));
        CHECK(hp.current == doctest::Approx(51.0));
    }
}
