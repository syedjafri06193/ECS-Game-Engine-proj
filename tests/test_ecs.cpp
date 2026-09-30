#include <doctest/doctest.h>
#include <algorithm>
#include <random>
#include <set>
#include "ecs/command_buffer.h"
#include "ecs/components.h"
#include "ecs/snapshot.h"
#include "ecs/world.h"

using namespace engine;
using namespace game;

TEST_CASE("stale handles are rejected")
{
    World w;
    Entity e = w.Create();
    w.Destroy(e);
    Entity e2 = w.Create();                 // reuses the index
    CHECK(e2.index == e.index);
    CHECK_FALSE(w.IsAlive(e));              // generation differs
    CHECK(w.IsAlive(e2));
    CHECK(w.Get<Position>(e) == nullptr);
    CHECK_FALSE(w.Destroy(e));
}

TEST_CASE("archetype identity is order-independent")
{
    World w;
    Entity a = w.Create(); w.Add<Position>(a); w.Add<Velocity>(a);
    Entity b = w.Create(); w.Add<Velocity>(b); w.Add<Position>(b);
    CHECK(w.ArchetypeOf(a) == w.ArchetypeOf(b));
    Entity c = w.Create(Velocity{}, Position{});
    CHECK(w.ArchetypeOf(c) == w.ArchetypeOf(a));
}

TEST_CASE("components survive archetype moves")
{
    World w;
    Entity e = w.Create(Position{1, 2, 3});
    w.Add(e, Velocity{4, 5, 6});
    w.Add(e, Health{50, 100});
    w.Remove<Velocity>(e);
    REQUIRE(w.Get<Position>(e));
    CHECK(w.Get<Position>(e)->y == 2);
    CHECK(w.Get<Health>(e)->current == 50);
    CHECK_FALSE(w.Has<Velocity>(e));
    CHECK(w.Validate());
}

TEST_CASE("queries see every matching entity exactly once")
{
    World w;
    std::mt19937 rng(7);
    std::set<uint64_t> expect;
    for (int i = 0; i < 5000; ++i) {
        Entity e = w.Create();
        const unsigned bits = rng() % 16;
        if (bits & 1) w.Add(e, Position{float(i), 0, 0});
        if (bits & 2) w.Add(e, Velocity{});
        if (bits & 4) w.Add(e, Health{});
        if (bits & 8) w.Add<Dead>(e);
        if ((bits & 3) == 3 && !(bits & 8)) expect.insert(e.Bits());
    }
    std::set<uint64_t> seen;
    size_t visits = 0;
    for (auto [e, p, v] : w.Query<Position, const Velocity>().Without<Dead>()) {
        (void)p; (void)v;
        seen.insert(e.Bits());
        ++visits;
    }
    CHECK(visits == expect.size());
    CHECK(seen == expect);
    CHECK(w.Query<Position, const Velocity>().Without<Dead>().Count() == expect.size());

    size_t via_foreach = 0;
    w.Query<Position>().With<Velocity>().Without<Dead>().ForEach([&](Entity, Position&) { ++via_foreach; });
    CHECK(via_foreach == expect.size());
}

TEST_CASE("chunks stay dense and span multiple blocks")
{
    World w;
    std::vector<Entity> es;
    for (int i = 0; i < 3000; ++i) es.push_back(w.Create(Position{float(i)}, Velocity{}));
    Archetype* a = w.ArchetypeOf(es[0]);
    CHECK(a->Chunks().size() > 1);
    for (int i = 0; i < 3000; i += 3) w.Destroy(es[size_t(i)]);
    std::string err;
    CHECK_MESSAGE(w.Validate(&err), err);
    float sum = 0;
    for (auto [e, p] : w.Query<const Position>()) { (void)e; sum += p.x; }
    float want = 0;
    for (int i = 0; i < 3000; ++i) if (i % 3) want += float(i);
    CHECK(sum == want);
}

TEST_CASE("query cache refreshes only when archetypes change")
{
    World w;
    w.Create(Position{});
    auto q = w.Query<Position>();
    const auto gen = w.ArchetypeGeneration();
    CHECK(q.Archetypes().size() == 1);
    w.Create(Position{});                      // same archetype: no new generation
    CHECK(w.ArchetypeGeneration() == gen);
    w.Create(Position{}, Health{});            // new archetype
    CHECK(w.ArchetypeGeneration() > gen);
    CHECK(q.Archetypes().size() == 2);
}

TEST_CASE("structural change during iteration asserts instead of corrupting")
{
    World w;
    w.Create(Position{});
    auto q = w.Query<Position>();
    q.ForEach([&](Entity, Position&) { CHECK(w.StructureLocked()); });
    CHECK_FALSE(w.StructureLocked());
    auto q2 = w.Query<Position>();
    for (auto [e, p] : q2) { (void)e; (void)p; CHECK(w.StructureLocked()); }
    CHECK_FALSE(w.StructureLocked());          // released when the loop ends, not when q2 dies
    w.Create(Position{});                      // so this is legal
    CHECK(w.Validate());
}

TEST_CASE("command buffers: placeholders, deterministic order, benign double destroy")
{
    World w;
    w.Register<Position, Velocity, Health, Dead>();
    Entity target = w.Create(Position{});
    CommandBuffer a(&w), b(&w);

    Entity p = a.CreateEntity();
    a.AddComponent(p, Position{7, 0, 0});
    a.AddComponent(p, Velocity{1, 0, 0});
    a.DestroyEntity(target);
    b.DestroyEntity(target);                   // second destroy of the same entity is skipped
    b.AddComponent(target, Health{});          // on a dead entity: skipped

    CommandBuffer* bufs[] = {&a, &b};
    const size_t applied = w.ApplyCommandBuffers(std::span<CommandBuffer* const>(bufs));
    CHECK(applied == 4);
    CHECK_FALSE(w.IsAlive(target));
    CHECK(w.EntityCount() == 1);
    int found = 0;
    for (auto [e, pos, vel] : w.Query<const Position, const Velocity>()) {
        (void)e; (void)vel;
        CHECK(pos.x == 7);
        ++found;
    }
    CHECK(found == 1);
    CHECK(a.Empty());
    CHECK(w.Validate());
}

TEST_CASE("hybrid storage: sparse tags never move entities")
{
    World w;
    w.RegisterSparse<Stunned>();
    std::vector<Entity> es;
    for (int i = 0; i < 1000; ++i) es.push_back(w.Create(Position{}, Velocity{}, Health{}));
    const auto moves_before = w.GetStats().archetype_moves;
    for (int round = 0; round < 10; ++round)
        for (Entity e : es) {
            if (round % 2 == 0) w.AddSparse<Stunned>(e);
            else w.RemoveSparse<Stunned>(e);
        }
    CHECK(w.GetStats().archetype_moves == moves_before);
    CHECK(w.Sparse(w.Registry().Find("game::Stunned"))->Size() == 0);

    w.AddSparse<Stunned>(es[3]);
    CHECK(w.HasSparse<Stunned>(es[3]));
    w.Destroy(es[3]);                         // destroy clears sparse membership
    CHECK(w.Validate());

    // The same toggling through archetype storage moves every component.
    World a;
    std::vector<Entity> as;
    for (int i = 0; i < 1000; ++i) as.push_back(a.Create(Position{}, Velocity{}, Health{}));
    for (int round = 0; round < 10; ++round)
        for (Entity e : as) {
            if (round % 2 == 0) a.Add<Stunned>(e);
            else a.Remove<Stunned>(e);
        }
    CHECK(a.GetStats().archetype_moves >= 10000);
}

TEST_CASE("random structural churn keeps the world consistent")
{
    World w;
    std::mt19937 rng(1234);
    std::vector<Entity> live;
    for (int step = 0; step < 20000; ++step) {
        const unsigned r = rng() % 100;
        if (r < 30 || live.empty()) {
            live.push_back(w.Create(Position{float(step)}));
        } else {
            Entity e = live[rng() % live.size()];
            if (r < 45) { w.Destroy(e); live.erase(std::find(live.begin(), live.end(), e)); }
            else if (r < 65) w.Add(e, Velocity{1, 2, 3});
            else if (r < 80) w.Remove<Velocity>(e);
            else if (r < 90) w.Add<Dead>(e);
            else w.Remove<Dead>(e);
        }
        if (step % 2000 == 0) {
            std::string err;
            REQUIRE_MESSAGE(w.Validate(&err), err);
        }
    }
    CHECK(w.EntityCount() == live.size());
    for (Entity e : live) CHECK(w.IsAlive(e));
    CHECK(w.Validate());
}

// ---------------------------------------------------------------- migration (§3.2, §14.3)

namespace v1 { struct Velocity { float x, y; }; }
namespace v2 { struct Velocity { float z, x, y; int32_t flags; }; }   // reordered + added fields

TEST_CASE("world survives a component layout change via snapshot migration")
{
    static constexpr FieldDesc f1[] = {
        {"x", FieldType::F32, 1, offsetof(v1::Velocity, x), 4},
        {"y", FieldType::F32, 1, offsetof(v1::Velocity, y), 4},
    };
    static constexpr v2::Velocity v2_default{0.0f, 0.0f, 0.0f, 42};
    static constexpr FieldDesc f2[] = {
        {"z", FieldType::F32, 1, offsetof(v2::Velocity, z), 4},
        {"x", FieldType::F32, 1, offsetof(v2::Velocity, x), 4},
        {"y", FieldType::F32, 1, offsetof(v2::Velocity, y), 4},
        {"flags", FieldType::I32, 1, offsetof(v2::Velocity, flags), 4},
    };
    const ComponentDesc d1{"Velocity", 1, sizeof(v1::Velocity), alignof(v1::Velocity), f1, nullptr};
    const ComponentDesc d2{"Velocity", 2, sizeof(v2::Velocity), alignof(v2::Velocity), f2, &v2_default};

    World before;
    const ComponentId vid = before.Registry().Register(d1);
    std::vector<Entity> es;
    for (int i = 0; i < 500; ++i) {
        Entity e = before.Create(Position{float(i)});
        v1::Velocity v{float(i), float(-i)};
        before.AddRaw(e, vid, &v);
        es.push_back(e);
    }
    before.Destroy(es[10]);                    // holes in the index space survive too

    const Snapshot snap = SerializeWorld(before);
    Snapshot decoded;
    REQUIRE(DecodeSnapshot(EncodeSnapshot(snap), decoded));

    World after;
    after.Register<Position>();
    const ComponentId vid2 = after.Registry().Register(d2);
    const RestoreReport rep = DeserializeWorld(after, decoded);

    CHECK(rep.entities == 499);
    CHECK(rep.migrated == 499);
    CHECK(after.EntityCount() == before.EntityCount());
    for (int i = 0; i < 500; ++i) {
        if (i == 10) { CHECK_FALSE(after.IsAlive(es[10])); continue; }
        auto* v = static_cast<v2::Velocity*>(after.GetRaw(es[size_t(i)], vid2));
        REQUIRE(v);
        CHECK(v->x == float(i));               // preserved by name
        CHECK(v->y == float(-i));
        CHECK(v->z == 0.0f);                   // new field defaulted
        CHECK(v->flags == 42);                 // default comes from the descriptor
        CHECK(after.Get<Position>(es[size_t(i)])->x == float(i));
    }
    CHECK(after.Validate());
    Entity fresh = after.Create();             // free list rebuilt: no collision with restored handles
    CHECK(fresh.index == 10);
    CHECK(after.Validate());
}

TEST_CASE("MigrateComponent converts numeric types and drops removed fields")
{
    struct A { int32_t hp; float speed; float gone; };
    struct B { float hp; double speed; };
    static constexpr FieldDesc fa[] = {{"hp", FieldType::I32, 1, 0, 4}, {"speed", FieldType::F32, 1, 4, 4}, {"gone", FieldType::F32, 1, 8, 4}};
    static constexpr FieldDesc fb[] = {{"hp", FieldType::F32, 1, 0, 4}, {"speed", FieldType::F64, 1, 8, 8}};
    ComponentDesc da{"X", 1, sizeof(A), alignof(A), fa, nullptr}, db{"X", 2, sizeof(B), alignof(B), fb, nullptr};
    A a{12, 2.5f, 9.0f};
    B b{};
    CHECK(MigrateComponent(da, db, &a, &b) == 2);
    CHECK(b.hp == 12.0f);
    CHECK(b.speed == 2.5);
}
