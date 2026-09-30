#include <doctest/doctest.h>
#include <cmath>
#include <random>
#include <thread>
#include "ecs/components.h"
#include "render/cluster/cluster.h"
#include "render/extract.h"
#include "render/graph/render_graph.h"

using namespace engine;
using namespace engine::rg;

namespace {

struct Frame {
    RenderGraph g;
    ResourceId depth, lights, hdr, swap, debug;
    uint32_t prepass, cull, forward, tonemap, overlay;
};

// The design doc's §7.1 example plus tonemap and a dead debug pass.
void Build(Frame& f, bool with_overlay = true)
{
    RenderGraph& g = f.g;
    f.depth  = g.CreateTexture("depth", {1920, 1080, Format::D32F});
    f.lights = g.CreateBuffer("cluster-light-indices", {3456ull * 256 * 4});
    f.hdr    = g.CreateTexture("hdr", {1920, 1080, Format::RGBA16F});
    f.debug  = g.CreateTexture("debug-overlay", {1920, 1080, Format::RGBA8_UNORM});
    f.swap   = g.ImportTexture("swapchain", {1920, 1080, Format::BGRA8_SRGB},
                               State{StageNone, AccessNone, Layout::Undefined},
                               State{StageBottomOfPipe, AccessNone, Layout::Present});

    f.prepass = g.AddPass("depth-prepass", QueueType::Graphics, [&](PassBuilder& b) {
        b.WriteDepth(f.depth);
        return ExecuteFn{};
    });
    f.cull = g.AddPass("light-culling", QueueType::Compute, [&](PassBuilder& b) {
        b.ReadTexture(f.depth, ShaderCompute);
        b.WriteBuffer(f.lights, ShaderCompute);
        return ExecuteFn{};
    });
    f.forward = g.AddPass("forward", QueueType::Graphics, [&](PassBuilder& b) {
        b.ReadBuffer(f.lights, ShaderFragment);
        b.ReadDepth(f.depth);
        b.WriteColor(f.hdr);
        return ExecuteFn{};
    });
    f.overlay = with_overlay ? g.AddPass("debug-overlay", QueueType::Graphics, [&](PassBuilder& b) {
        b.WriteColor(f.debug);                    // nobody reads it -> culled
        return ExecuteFn{};
    }) : ~0u;
    f.tonemap = g.AddPass("tonemap", QueueType::Graphics, [&](PassBuilder& b) {
        b.ReadTexture(f.hdr, ShaderFragment);
        b.WriteColor(f.swap);
        return ExecuteFn{};
    });
}

const Barrier* FindBarrier(const CompiledGraph& c, uint32_t pass, ResourceId r)
{
    for (const auto& p : c.passes)
        if (p.pass == pass)
            for (const auto& b : p.barriers)
                if (b.resource == r) return &b;
    return nullptr;
}

}  // namespace

TEST_CASE("render graph: derived barriers match the hand-verified expectation")
{
    Frame f;
    Build(f);
    const CompiledGraph& c = f.g.Compile();
    REQUIRE(c.Ok());

    // Culling: the overlay writes a transient nobody reads.
    REQUIRE(c.culled.size() == 1);
    CHECK(c.culled[0] == f.overlay);
    CHECK(c.passes.size() == 4);

    // depth-prepass: Undefined -> DepthAttachment
    auto* b = FindBarrier(c, f.prepass, f.depth);
    REQUIRE(b);
    CHECK(b->before.layout == Layout::Undefined);
    CHECK(b->after.layout == Layout::DepthAttachment);
    CHECK((b->after.access & AccessDepthWrite));

    // light-culling: depth write -> compute sampled read; fresh buffer needs none
    b = FindBarrier(c, f.cull, f.depth);
    REQUIRE(b);
    CHECK(b->before.layout == Layout::DepthAttachment);
    CHECK((b->before.stages & StageLateFragTests));
    CHECK((b->before.access & AccessDepthWrite));
    CHECK(b->after.layout == Layout::ShaderReadOnly);
    CHECK(b->after.stages == StageComputeShader);
    CHECK(FindBarrier(c, f.cull, f.lights) == nullptr);

    // forward: compute write -> fragment read on the light list; depth -> read-only attachment; hdr init
    b = FindBarrier(c, f.forward, f.lights);
    REQUIRE(b);
    CHECK(b->before.stages == StageComputeShader);
    CHECK(b->before.access == AccessShaderStorageWrite);
    CHECK(b->after.stages == StageFragmentShader);
    CHECK(b->after.access == AccessShaderStorageRead);
    b = FindBarrier(c, f.forward, f.depth);
    REQUIRE(b);
    CHECK(b->before.layout == Layout::ShaderReadOnly);
    CHECK(b->after.layout == Layout::DepthReadOnly);
    b = FindBarrier(c, f.forward, f.hdr);
    REQUIRE(b);
    CHECK(b->before.layout == Layout::Undefined);
    CHECK(b->after.layout == Layout::ColorAttachment);

    // tonemap: hdr color write -> fragment sampled read; swapchain -> attachment
    b = FindBarrier(c, f.tonemap, f.hdr);
    REQUIRE(b);
    CHECK(b->before.access == AccessColorWrite);
    CHECK(b->after.layout == Layout::ShaderReadOnly);
    b = FindBarrier(c, f.tonemap, f.swap);
    REQUIRE(b);
    CHECK(b->after.layout == Layout::ColorAttachment);

    // Final: swapchain -> Present
    REQUIRE(c.final_barriers.size() == 1);
    CHECK(c.final_barriers[0].resource == f.swap);
    CHECK(c.final_barriers[0].after.layout == Layout::Present);
    CHECK(c.BarrierCount() == 8);          // 1 + 1 + 3 + 2 + final 1
}

TEST_CASE("render graph: read-after-read merges, the next write waits for all readers")
{
    RenderGraph g;
    auto t = g.CreateTexture("t", {64, 64, Format::RGBA8_UNORM});
    auto out = g.ImportTexture("out", {64, 64}, {}, State{StageBottomOfPipe, AccessNone, Layout::ShaderReadOnly});
    uint32_t w = g.AddPass("write", QueueType::Graphics, [&](PassBuilder& b) { b.WriteColor(t); return ExecuteFn{}; });
    uint32_t r1 = g.AddPass("read-vs", QueueType::Graphics, [&](PassBuilder& b) { b.ReadTexture(t, ShaderVertex); b.WriteColor(out); return ExecuteFn{}; });
    uint32_t r2 = g.AddPass("read-cs", QueueType::Compute, [&](PassBuilder& b) { b.ReadTexture(t, ShaderCompute); b.SideEffect(); return ExecuteFn{}; });
    uint32_t w2 = g.AddPass("rewrite", QueueType::Graphics, [&](PassBuilder& b) { b.WriteColor(t); b.SideEffect(); return ExecuteFn{}; });
    const auto& c = g.Compile();
    REQUIRE(c.Ok());
    (void)w;
    CHECK(FindBarrier(c, r1, t) != nullptr);          // write -> read
    CHECK(FindBarrier(c, r2, t) == nullptr);          // read -> read, same layout: merged
    auto* b = FindBarrier(c, w2, t);
    REQUIRE(b);
    CHECK((b->before.stages & StageVertexShader));    // waits for BOTH readers
    CHECK((b->before.stages & StageComputeShader));
}

TEST_CASE("render graph: transient aliasing shares memory across disjoint lifetimes")
{
    RenderGraph g;
    auto out = g.ImportTexture("out", {256, 256}, {}, State{StageBottomOfPipe, AccessNone, Layout::Present});
    auto a = g.CreateTexture("bloom-a", {256, 256, Format::RGBA16F});
    auto b = g.CreateTexture("bloom-b", {256, 256, Format::RGBA16F});
    auto c = g.CreateTexture("bloom-c", {256, 256, Format::RGBA16F});
    g.AddPass("p0", QueueType::Graphics, [&](PassBuilder& p) { p.WriteColor(a); return ExecuteFn{}; });
    g.AddPass("p1", QueueType::Graphics, [&](PassBuilder& p) { p.ReadTexture(a, ShaderFragment); p.WriteColor(b); return ExecuteFn{}; });
    g.AddPass("p2", QueueType::Graphics, [&](PassBuilder& p) { p.ReadTexture(b, ShaderFragment); p.WriteColor(c); return ExecuteFn{}; });
    const uint32_t p3 = g.AddPass("p3", QueueType::Graphics, [&](PassBuilder& p) { p.ReadTexture(c, ShaderFragment); p.WriteColor(out); return ExecuteFn{}; });
    const auto& cg = g.Compile();
    REQUIRE(cg.Ok());
    CHECK(cg.memory_slot[a] == cg.memory_slot[c]);    // a dies at p1, c born at p2
    CHECK(cg.memory_slot[a] != cg.memory_slot[b]);
    CHECK(cg.transient_bytes_aliased * 3 == cg.transient_bytes_unaliased * 2);
    const Barrier* alias = nullptr;
    for (const auto& p : cg.passes)
        for (const auto& bar : p.barriers)
            if (bar.resource == c && !alias) alias = &bar;     // first use of c
    REQUIRE(alias);
    CHECK(alias->aliasing);
    CHECK(alias->aliased_from == a);
    CHECK(alias->before.layout == Layout::Undefined);          // contents discarded
    CHECK((alias->before.stages & StageFragmentShader));       // waits for a's last reader
    (void)p3;
}

TEST_CASE("render graph: reading a transient nobody wrote is an error")
{
    RenderGraph g;
    auto t = g.CreateTexture("never-written", {8, 8});
    auto out = g.ImportTexture("out", {8, 8}, {}, State{StageBottomOfPipe, AccessNone, Layout::Present});
    g.AddPass("reader", QueueType::Graphics, [&](PassBuilder& b) { b.ReadTexture(t, ShaderFragment); b.WriteColor(out); return ExecuteFn{}; });
    const auto& c = g.Compile();
    REQUIRE(c.errors.size() == 1);
    CHECK(c.errors[0].find("never-written") != std::string::npos);
}

TEST_CASE("render graph: compile is cached across identical frames; executor order")
{
    struct Recorder : Backend {
        std::vector<std::string> log;
        const RenderGraph* g = nullptr;
        void Barriers(std::span<const Barrier> bs, PassContext&) override { log.push_back("barriers:" + std::to_string(bs.size())); }
        void BeginPass(uint32_t p, PassContext&) override { log.push_back("begin:" + g->Passes()[p].name); }
    } rec;

    Frame f;
    Build(f);
    f.g.Compile();
    CHECK(f.g.CompileCount() == 1);
    f.g.Reset();
    Build(f);
    f.g.Compile();
    CHECK(f.g.CompileCount() == 1);            // unchanged declarations: cached
    f.g.Reset();
    Build(f, /*with_overlay=*/false);
    f.g.Compile();
    CHECK(f.g.CompileCount() == 2);            // structure changed: recompiled

    rec.g = &f.g;
    PassContext ctx;
    f.g.Execute(rec, ctx);
    const std::vector<std::string> want = {
        "barriers:1", "begin:depth-prepass", "barriers:1", "begin:light-culling",
        "barriers:3", "begin:forward", "barriers:2", "begin:tonemap", "barriers:1"};
    CHECK(rec.log == want);
    const std::string dot = f.g.ToDot();
    CHECK(dot.find("digraph") != std::string::npos);
    CHECK(dot.find("light-culling") != std::string::npos);
}

// ---------------------------------------------------------------- clustering

TEST_CASE("clusters: exponential slicing is geometric; linear wastes slices far away")
{
    cluster::Grid g;                               // 16x9x24, 0.1..200
    CHECK(g.Count() == 3456);
    CHECK(cluster::IndexBufferBytes(g) == 3538944);   // the doc's "3.5 MB"
    CHECK(cluster::Slice(g, g.z_near) == 0);
    CHECK(cluster::Slice(g, g.z_far * 0.999f) == g.z - 1);
    const float ratio = cluster::SliceNear(g, 1) / cluster::SliceNear(g, 0);
    for (uint32_t k = 1; k + 1 < g.z; ++k)
        CHECK(cluster::SliceNear(g, k + 1) / cluster::SliceNear(g, k) == doctest::Approx(ratio).epsilon(1e-4));

    // Everything within 10 m of the camera: exponential gives it >half the
    // slices, linear crams it into the first one.
    CHECK(cluster::Slice(g, 10.0f) > g.z / 2);
    CHECK(cluster::SliceLinear(g, 8.0f) == 0);
}

TEST_CASE("clusters: a projected point lies inside its cluster's bounds")
{
    cluster::Grid g;
    std::mt19937 rng(3);
    std::uniform_real_distribution<float> U(0, 1);
    int checked = 0;
    for (int i = 0; i < 5000; ++i) {
        const float d = g.z_near + (g.z_far - g.z_near) * U(rng) * U(rng);
        const float tan_y = std::tan(g.fov_y / 2), tan_x = tan_y * g.aspect;
        const Vec3 p{(U(rng) * 2 - 1) * d * tan_x * 0.999f, (U(rng) * 2 - 1) * d * tan_y * 0.999f, -d};
        float u, v, dist;
        REQUIRE(cluster::Project(g, p, u, v, dist));
        uint32_t cx, cy, cz;
        cluster::ClusterCoords(g, cluster::ClusterIndex(g, u, v, dist), cx, cy, cz);
        const auto box = cluster::ClusterBounds(g, cx, cy, cz);
        const float eps = 1e-3f * d;
        CHECK(p.x >= box.min.x - eps); CHECK(p.x <= box.max.x + eps);
        CHECK(p.y >= box.min.y - eps); CHECK(p.y <= box.max.y + eps);
        CHECK(p.z >= box.min.z - eps); CHECK(p.z <= box.max.z + eps);
        ++checked;
    }
    CHECK(checked == 5000);
}

TEST_CASE("clusters: assignment is conservative and actually culls")
{
    cluster::Grid g;
    std::mt19937 rng(11);
    std::uniform_real_distribution<float> U(0, 1);
    std::vector<cluster::LightSphere> lights;
    for (int i = 0; i < 300; ++i)
        lights.push_back({{(U(rng) * 2 - 1) * 40, (U(rng) * 2 - 1) * 20, -(1 + U(rng) * 80)}, 2 + U(rng) * 6});
    const auto a = cluster::Assign(g, lights);
    CHECK(a.overflowed == 0);

    // Conservative: any point lit by light L is in a cluster that lists L.
    int misses = 0, samples = 0;
    for (int i = 0; i < 3000; ++i) {
        const auto& l = lights[size_t(rng() % lights.size())];
        const float r = l.radius * 0.99f * U(rng);
        const Vec3 dir = Normalize({U(rng) * 2 - 1, U(rng) * 2 - 1, U(rng) * 2 - 1});
        const Vec3 p = l.view_pos + dir * r;
        float u, v, d;
        if (!cluster::Project(g, p, u, v, d) || u < 0 || u > 1 || v < 0 || v > 1 || d < g.z_near || d > g.z_far) continue;
        const uint32_t ci = cluster::ClusterIndex(g, u, v, d);
        const uint32_t li = uint32_t(&l - lights.data());
        bool found = false;
        for (uint32_t k = 0; k < a.counts[ci]; ++k) found |= a.indices[a.offsets[ci] + k] == li;
        misses += !found;
        ++samples;
    }
    CHECK(samples > 1000);
    CHECK(misses == 0);

    // Culling effectiveness: the average cluster sees a small fraction of lights.
    const double avg = double(a.indices.size()) / g.Count();
    CHECK(avg < 0.05 * double(lights.size()));
}

// ---------------------------------------------------------------- extraction

TEST_CASE("extraction: flat sorted packet, deterministic, double-buffered")
{
    using namespace game;
    World w;
    Scheduler s(4);
    w.Register<Position, Orientation, Scale, MeshRenderer, PointLight, Camera>();
    w.Create(Camera{}, Position{0, 0, 10});
    for (int i = 0; i < 3000; ++i) {
        Entity e = w.Create(Position{float(i % 50) - 25, 0, -float(i / 50)}, MeshRenderer{uint32_t(i % 3), uint32_t(i % 4), i % 10 == 0});
        if (i % 2) w.Add(e, Scale{2});
    }
    for (int i = 0; i < 64; ++i) w.Create(PointLight{}, Position{float(i), 1, 0});

    RenderPacket a, b;
    Extract(w, s, 16.0f / 9.0f, a);
    Extract(w, s, 16.0f / 9.0f, b);
    CHECK(a.camera.valid);
    CHECK(a.opaque.size() == 2700);
    CHECK(a.transparent.size() == 300);
    CHECK(a.lights.size() == 64);
    bool same = a.opaque.size() == b.opaque.size();
    for (size_t i = 0; same && i < a.opaque.size(); ++i) same = a.opaque[i].entity_index == b.opaque[i].entity_index;
    CHECK(same);
    for (size_t i = 1; i < a.opaque.size(); ++i) {
        const auto &p = a.opaque[i - 1], &q = a.opaque[i];
        CHECK((p.material < q.material || (p.material == q.material && (p.mesh < q.mesh || (p.mesh == q.mesh && p.view_depth <= q.view_depth)))));
    }
    for (size_t i = 1; i < a.transparent.size(); ++i) CHECK(a.transparent[i - 1].view_depth >= a.transparent[i].view_depth);

    // Double buffering: the renderer holds N while the simulation writes N+1.
    PacketQueue q;
    RenderPacket& w1 = q.BeginWrite(); w1.lights.resize(1); q.Publish();
    const RenderPacket* r1 = q.AcquireLatest();
    REQUIRE(r1);
    RenderPacket& w2 = q.BeginWrite();
    CHECK(&w2 != r1);                         // never writes the slot being read
    w2.lights.resize(2); q.Publish();
    CHECK(r1->lights.size() == 1);            // reader's view unchanged
    q.Release();
    const RenderPacket* r2 = q.AcquireLatest();
    REQUIRE(r2);
    CHECK(r2->lights.size() == 2);
    CHECK(q.AcquireLatest() == nullptr);      // nothing newer
    q.Release();
}

TEST_CASE("render graph: a loading overlay keeps the pass under it alive")
{
    RenderGraph g;
    auto swap = g.ImportTexture("swapchain", {64, 64, Format::BGRA8_SRGB}, {}, State{StageBottomOfPipe, AccessNone, Layout::Present});
    const uint32_t scene = g.AddPass("scene", QueueType::Graphics, [&](PassBuilder& b) { b.WriteColor(swap); return ExecuteFn{}; });
    const uint32_t ui = g.AddPass("ui", QueueType::Graphics, [&](PassBuilder& b) { b.WriteColor(swap, true); return ExecuteFn{}; });
    const auto& c = g.Compile();
    REQUIRE(c.Ok());
    CHECK(c.culled.empty());                           // without load_existing, "scene" would be dead
    auto* b = FindBarrier(c, ui, swap);
    REQUIRE(b);                                        // write -> read/write on the same attachment
    CHECK(b->before.access == AccessColorWrite);
    (void)scene;

    RenderGraph g2;
    auto t = g2.CreateTexture("t", {8, 8});
    auto out = g2.ImportTexture("out", {8, 8}, {}, State{StageBottomOfPipe, AccessNone, Layout::Present});
    g2.AddPass("blend-onto-nothing", QueueType::Graphics, [&](PassBuilder& p) { p.WriteColor(t, true); return ExecuteFn{}; });
    g2.AddPass("copy", QueueType::Graphics, [&](PassBuilder& p) { p.ReadTexture(t, ShaderFragment); p.WriteColor(out); return ExecuteFn{}; });
    CHECK(g2.Compile().errors.size() == 1);            // loading an unwritten transient is an error
}
