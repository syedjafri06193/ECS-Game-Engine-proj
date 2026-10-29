# ECS Game Engine — Design & Build Guide

**Project:** Custom entity-component-system engine with a job-based scheduler, hot-reloadable gameplay modules, and a Vulkan forward+ renderer
**Language:** C++20
**Status of this document:** planning + reference

---

## Table of contents

1. [Executive summary and scope](#1-executive-summary-and-scope)
2. [Reality check](#2-reality-check)
3. [Hot reload](#3-hot-reload)
4. [ECS storage](#4-ecs-storage)
5. [The job scheduler](#5-the-job-scheduler)
6. [The renderer](#6-the-renderer)
7. [The render graph](#7-the-render-graph)
8. [Where the three pillars meet](#8-where-the-three-pillars-meet)
9. [Performance](#9-performance)
10. [Tech stack and setup](#10-tech-stack-and-setup)
11. [Repository layout](#11-repository-layout)
12. [Milestone ladder](#12-milestone-ladder)
13. [Reference implementations](#13-reference-implementations)
14. [Testing](#14-testing)
15. [Stretch goals](#15-stretch-goals)
16. [References](#16-references)

---

## 1. Executive summary and scope

### The original statement

> Custom entity-component-system engine with a job-based scheduler, hot-reloadable gameplay modules, and a Vulkan forward+ renderer.

Five findings reshape this:

1. **These are three separate multi-year projects, and one pairing is actively hostile.** ECS and a job scheduler are designed for each other. Vulkan is orthogonal. But **hot-reloadable native modules and an archetype ECS fight directly**: if a reloadable DLL defines component types, changing a struct's layout invalidates every instance already stored in your chunks. Most engines dodge this rather than solve it. See section 3.2.
2. **The goal of hot reload is iteration speed, and a script layer delivers that with none of the type-layout problem.** The honest question to answer in week one is why native DLLs rather than a scripting VM — and the answer is usually "performance," which only applies to the small fraction of gameplay code that's actually hot. See section 3.1.
3. **Archetype versus sparse-set storage is a genuine tradeoff, not a settled question.** Archetypes give fast linear iteration and expensive structural changes; sparse sets give cheap add/remove and slower multi-component queries. Tag components toggled frequently — an extremely common ECS idiom — are pathological for archetypes. See section 4.1.
4. **Bindless has made the hardest part of Vulkan much easier.** Descriptor indexing plus buffer device address lets shaders reach resources through 64-bit pointers without descriptors at all, and newer descriptor-heap extensions bring DX12-style heaps to Vulkan. Descriptor set management used to be the wall people hit; it largely isn't anymore. See section 6.2.
5. **Clustered forward is the better default than tiled Forward+.** A 3D froxel grid handles large depth ranges far better than 2D screen tiles, and it's what current from-scratch Vulkan engines actually ship. See section 6.4.

### The scope problem, stated plainly

A competent solo developer needs roughly a year per pillar to do it well. Three pillars is three-plus years before you have a game.

**If the goal is to ship a game, this project is the reason you won't.** That's not a criticism of engine work — it's the most commonly reported failure mode in the field, and it's worth deciding against deliberately rather than discovering in year two.

Three legitimate framings:

| Goal | What to do |
|---|---|
| **Learn systems programming deeply** | Build all three. Excellent choice. Accept the timeline and don't attach a game to it. |
| **Ship a specific game** | Use Bevy, Flecs, or EnTT plus an existing renderer. Build the one custom piece your game genuinely needs. |
| **Build a reusable engine** | Pick one pillar to be genuinely good at and use libraries for the rest |

### Revised project statement

> A C++20 ECS engine with a hybrid storage model, a work-stealing job scheduler with statically-declared system access and deferred structural changes, a Vulkan 1.3+ renderer using bindless resources and a render graph with clustered forward shading — and a hot-reload strategy that uses a script layer for gameplay logic, with native module reload scoped to stateless systems whose component types are owned by the host.

### Explicit non-goals

- **Not an editor.** Editors are their own multi-year project. Use a text-based scene format and a debug UI.
- **Not a physics engine.** Jolt or PhysX.
- **Not an asset pipeline in v1.** Load glTF directly; add a cooker later.
- **Not cross-platform in v1.** One OS, one GPU vendor, until things work.
- **Not networked.** Composes badly with hot reload and adds determinism requirements.

---

## 2. Reality check

### 2.1 What exists

| | Project | Notes |
|---|---|---|
| **ECS** | EnTT, Flecs, Bevy ECS | All mature, all fast, all free |
| **Job scheduler** | Taskflow, enkiTS, Marl | Work stealing, solved |
| **Vulkan abstraction** | VMA, vk-bootstrap, volk | Use all three; don't rewrite them |
| **Render graph** | Several open implementations | Worth reading before writing |
| **Hot reload** | Live++ (commercial), various | The commercial one works and is not expensive |

**Use VMA, vk-bootstrap, and volk unconditionally.** Writing your own Vulkan memory allocator or instance bootstrapper teaches you very little and costs weeks. Save the from-scratch energy for the parts that are actually interesting.

### 2.2 The failure modes

| Symptom | Cause |
|---|---|
| **Hot reload corrupts the world** | Component layout changed under stored data (§3.2) |
| Crash on reload | A job was in flight in the unloaded DLL (§3.4) |
| Reload works, then leaks | Vulkan objects created by the old module never released (§3.5) |
| Structural change crashes mid-iteration | No deferred command buffer (§5.3) |
| Parallelism never materializes | Too many sync points (§5.4) |
| Adding a tag component is slow | Archetype churn (§4.1) |
| Validation errors only on another GPU | Not running with validation layers on (§6.6) |
| Barriers wrong in subtle ways | Hand-written synchronization at scale (§7.1) |
| Frame time spikes every few seconds | Pipeline compilation on demand (§6.5) |
| **Three years in, no game** | Scope (§1) |

### 2.3 The order matters

The pillars have a dependency structure, and building them in the wrong order wastes work:

```
1. ECS core            everything depends on the data model
2. Job scheduler       needs the ECS access model to schedule against
3. Vulkan foundation   independent, but needs somewhere to get data from
4. Render graph        needs the Vulkan foundation
5. Clustered forward   needs the render graph
6. Hot reload          needs everything else stable to reload into
```

**Hot reload last.** It touches every other system, and building it against moving targets means rebuilding it repeatedly.

---

## 3. Hot reload ★

### 3.1 Ask why native, first ★

The value of hot reload is iteration speed — change gameplay logic, see it immediately, without losing state or restarting.

**A script VM gives you exactly that, for free, with none of the hard problems.** Lua, Wren, or AngelScript reload trivially: no type layouts to migrate, no vtables to dangle, no statics to lose, no jobs to quiesce.

The case for native DLLs is performance and C++ debugging. But that only matters for code that's actually hot — and in most engines, hot code is the systems that iterate over thousands of entities, not the gameplay logic that responds to an event.

**The hybrid is usually correct:**

| Code | Where | Reload |
|---|---|---|
| Transform, physics, animation, culling, render extraction | Compiled into the engine | Restart |
| Gameplay rules, AI behavior, UI logic, level scripting | Script | Instant |
| Hot gameplay systems that profiling proves need C++ | Reloadable module (§3.3) | Native reload |

That last row should be small, and it should be *earned* by a profile rather than assumed.

The rest of this section assumes you want native reload anyway — which is a reasonable choice for a learning project, and it's genuinely interesting engineering.

### 3.2 The component layout problem ★

This is the hard part, and it's the one that makes hot reload and ECS hostile to each other.

```cpp
// Version 1, compiled into gameplay.dll
struct Velocity { float x, y; };        // 8 bytes

// You edit and rebuild:
struct Velocity { float x, y, z; };     // 12 bytes
```

Every `Velocity` already stored in an archetype chunk is now misinterpreted. The chunk's stride is wrong, the field offsets are wrong, and the data is garbage. Nothing crashes immediately — entities just start behaving insanely.

Three responses:

| Approach | Cost |
|---|---|
| **Forbid it.** Layout changes require a restart. | Simple, honest, and limits the feature's usefulness |
| **Reflect and migrate.** Serialize the world before unload, deserialize after, mapping fields by name. | A hitch proportional to world size; handles added/removed/reordered fields |
| **Host owns all component types.** Modules contain only systems. | Severe constraint, but it makes reload actually safe |

**The third is the right architecture, with the second as a fallback for iterating on component definitions.**

```cpp
// engine/components.h — compiled into the HOST, never the module.
struct Velocity { float x, y, z; };

// gameplay.dll — systems only. No type definitions, no state.
extern "C" void MoveSystem(World* w, float dt) {
    for (auto [e, pos, vel] : w->Query<Position, Velocity>()) {
        pos.x += vel.x * dt;
        pos.y += vel.y * dt;
        pos.z += vel.z * dt;
    }
}
```

Now reload is safe: the layouts are stable because the host owns them, and the module is pure logic.

When you *do* need to change a component, the reflection path handles it:

```cpp
struct FieldDesc { const char* name; TypeId type; size_t offset, size; };
struct ComponentDesc {
    const char* name;
    uint32_t version;
    size_t size, align;
    std::span<const FieldDesc> fields;
};

// Migrate by field NAME, not by offset. Added fields get defaults;
// removed fields are dropped; reordered fields follow their names.
void MigrateComponent(const ComponentDesc& from, const ComponentDesc& to,
                      const void* src, void* dst);
```

### 3.3 What a reloadable module may not contain

The constraints are severe and non-negotiable:

| Forbidden | Why |
|---|---|
| **Any persistent state** (globals, statics, function-local statics) | Lost or duplicated on reload |
| **Virtual functions on objects that outlive the reload** | The vtable pointer points into unloaded memory |
| **Function pointers stored in host structures** | Dangle after unload |
| **Component type definitions** | §3.2 |
| **Resources not registered with the host** | Leak on unload (§3.5) |
| `std::function` capturing module code | Same as function pointers |

**A reloadable module is a stateless function library.** All state lives in the host's world; the module only reads and writes it. That constraint is what makes reload tractable at all.

Enforce it where you can: an export-table check on load that rejects a module exporting anything other than the expected system entry points, and a debug allocator that asserts on allocation from module code outside the host's arenas.

### 3.4 Quiescing the scheduler ★

Unloading a DLL while a worker thread is executing code inside it is an immediate crash.

```cpp
void Engine::ReloadModule(const char* path) {
    // 1. Stop scheduling new work and wait for everything in flight.
    scheduler.Quiesce();                    // blocks until all jobs complete

    // 2. Drop every reference into the module.
    systemRegistry.UnregisterFrom(module);
    renderer.ReleaseResourcesOwnedBy(module);   // §3.5

    // 3. Optional: snapshot the world if component layouts may have changed.
    auto snapshot = reflection.SerializeWorld(world);

    // 4. Unload, reload.
    platform::UnloadLibrary(module.handle);
    module.handle = platform::LoadLibrary(path);

    // 5. Re-register and migrate.
    auto init = platform::GetSymbol<ModuleInitFn>(module.handle, "ModuleInit");
    init(&hostApi);
    reflection.DeserializeWorld(world, snapshot);

    // 6. Resume.
    scheduler.Resume();
}
```

`Quiesce` is the critical step and the one people forget. It must wait for every in-flight job, not just stop dispatching new ones — a work-stealing scheduler has jobs sitting in worker deques that will execute after you stop submitting.

**Copy the DLL to a temp path before loading** on Windows, or the linker can't overwrite it on the next build. Watch the original with a file watcher and copy-then-load on change.

### 3.5 GPU resources owned by modules

If module code creates Vulkan objects — pipelines, descriptor sets, buffers — those must be released before the module unloads, or you leak and the driver holds references to shader code that's about to vanish.

```cpp
class ResourceRegistry {
    // Every GPU object is tagged with its creating module.
    std::unordered_multimap<ModuleId, GpuHandle> ownership;
public:
    template <typename T> T Create(ModuleId owner, const Desc& d);
    void ReleaseAll(ModuleId owner);      // called before unload
};
```

Better still: **don't let modules create GPU resources at all.** They describe what they want (a material, a draw), and the host owns the Vulkan objects. That removes the whole class of problem and fits the stateless-module rule in §3.3.

---

## 4. ECS storage

### 4.1 Archetype versus sparse set ★

| | **Archetype** (DOTS, Flecs, Bevy) | **Sparse set** (EnTT) |
|---|---|---|
| Layout | Entities with identical component sets share chunks | Per-component dense array + sparse index |
| Multi-component iteration | **Fast** — linear, contiguous, no checks | Slower — intersect sets, indirection |
| Add/remove a component | **Expensive** — copies all the entity's components to a new archetype | **Cheap** — one array push/swap |
| Random access by entity | Indirection through a lookup | Direct via the sparse index |
| Memory | Some waste from partially-filled chunks | Tighter |
| Pathological case | **Frequently toggled tag components** | Queries over many component types |

**The archetype pathology is worth understanding**, because tag toggling is a natural ECS idiom. Adding a `Stunned` tag to an entity moves every one of its components — transform, mesh, physics, health — into a different chunk. Do that for a hundred entities a frame and structural change dominates your frame time.

Mitigations: use a boolean field inside an existing component rather than a tag when it toggles often; batch structural changes (§5.3); or use a hybrid.

**A hybrid is defensible.** Archetype storage for stable, iteration-heavy components (transforms, meshes, physics bodies) and sparse-set storage for volatile, frequently-toggled ones. More implementation work, and it matches how the data actually behaves.

For a learning project, **build archetype first** — it's the more interesting structure, it's what the modern engines use, and the chunk layout is what makes the job scheduler's parallelism natural.

### 4.2 Chunk layout

```cpp
constexpr size_t CHUNK_SIZE = 16 * 1024;     // sized to fit comfortably in L2

struct Chunk {
    Archetype*  archetype;
    uint32_t    entityCount;
    uint32_t    capacity;
    alignas(64) std::byte data[CHUNK_SIZE];   // SoA: each component contiguous
};
```

Components are stored **structure-of-arrays within the chunk** — all the `Position`s, then all the `Velocity`s — so a system reading only `Position` touches only the cache lines it needs.

Chunk size is a real tuning parameter. 16 KB is a common choice: large enough to amortize per-chunk overhead, small enough that a chunk's working set fits in L2 alongside whatever else the system touches.

**Chunks are also the unit of parallelism** (§5.2), which is a meaningful design payoff.

### 4.3 Entity handles need generations

```cpp
struct Entity {
    uint32_t index;        // slot in the entity table
    uint32_t generation;   // bumped on destroy — invalidates stale handles
};
```

Without the generation counter, a destroyed entity's index gets reused and a stale handle silently refers to a different entity. That's a bug class that produces wrong behavior rather than a crash, which makes it much worse.

### 4.4 Queries and caching

A query is a component-set filter. Resolving which archetypes match should happen once, not per frame:

```cpp
class Query {
    ComponentMask include, exclude;
    std::vector<Archetype*> matched;    // cached
    uint32_t archetypeGeneration;       // invalidate when a new archetype appears
public:
    void Refresh(World& w) {
        if (archetypeGeneration == w.ArchetypeGeneration()) return;
        matched.clear();
        for (auto* a : w.Archetypes())
            if (a->mask.Matches(include, exclude)) matched.push_back(a);
        archetypeGeneration = w.ArchetypeGeneration();
    }
};
```

Archetypes are created only when a novel component combination first appears, so in a settled game the generation counter stops changing and query refresh costs nothing.

---

## 5. The job scheduler

### 5.1 Work stealing

The standard design: one worker per hardware thread, each with a local deque. Workers push and pop from their own deque's bottom; idle workers steal from the top of a random victim's.

```cpp
class WorkStealingDeque {
    std::atomic<int64_t> top{0}, bottom{0};
    Job* buffer[CAPACITY];
public:
    void Push(Job* j);              // owner only, bottom
    Job* Pop();                     // owner only, bottom — LIFO, cache-friendly
    Job* Steal();                   // thieves, top — FIFO, takes the oldest
};
```

LIFO for the owner is deliberate: the most recently pushed job is most likely to have its data in cache. FIFO for thieves is also deliberate: older jobs tend to be larger, so a steal moves more work per synchronization.

**Don't write this from scratch unless the lock-free deque is the point.** enkiTS and Taskflow are mature and this is subtle code where a bug manifests as a rare deadlock.

### 5.2 Chunks as job granularity

The chunk layout pays off here. A system over N chunks becomes N jobs, each operating on contiguous memory with no sharing:

```cpp
void ScheduleSystem(SystemFn fn, Query& q, Scheduler& s) {
    for (Archetype* a : q.matched)
        for (Chunk* c : a->chunks)
            s.Submit([fn, c] { fn(ChunkView{c}); });
}
```

**Granularity matters in both directions.** One job per entity is dominated by scheduling overhead; one job for everything is no parallelism. A chunk of a few hundred entities is usually about right, and batching several small chunks into one job is worth doing when chunks are sparsely filled.

### 5.3 Deferred structural changes ★

Creating or destroying entities, or adding or removing components, **moves memory that other jobs may be iterating.** Doing it inline is the classic ECS crash.

```cpp
class CommandBuffer {
    // Per-thread, lock-free recording. Applied at a sync point.
    std::vector<Command> commands;
public:
    Entity CreateEntity();                       // returns a placeholder ID
    void   DestroyEntity(Entity e);
    template <typename T> void AddComponent(Entity e, T v);
    template <typename T> void RemoveComponent(Entity e);
};

void World::ApplyCommandBuffers(std::span<CommandBuffer> buffers) {
    // Single-threaded, at a sync point. Deterministic order across buffers.
    for (auto& b : buffers)
        for (auto& c : b.commands) Apply(c);
}
```

The placeholder-ID mechanism matters: a job creating an entity needs something to reference immediately, so `CreateEntity` returns a provisional handle that the apply step resolves to a real one.

**Apply in a deterministic order across buffers** — by worker index, not completion order — or the same frame produces different results on different runs, which makes every bug non-reproducible.

### 5.4 Sync points are where parallelism dies ★

A sync point waits for every job, applies structural changes, then resumes. **Every sync point serializes the entire scheduler**, and the cost is the tail of the slowest job plus the apply step.

```
systems A,B,C in parallel → SYNC → systems D,E in parallel → SYNC → ...
```

With eight sync points per frame and workers idle for a few hundred microseconds at each, you've spent milliseconds doing nothing.

**Minimize them:**

- **Batch all structural changes to one or two sync points per frame**, not one per system
- **Order systems so independent groups run together** between syncs
- **Systems that don't make structural changes don't need one**

### 5.5 Static access declaration

Automatic parallelization requires knowing what each system touches before it runs:

```cpp
struct SystemAccess {
    ComponentMask reads;
    ComponentMask writes;
    bool structuralChanges;
};

// Two systems conflict if either writes something the other touches.
bool Conflicts(const SystemAccess& a, const SystemAccess& b) {
    return (a.writes & b.reads).Any()
        || (a.writes & b.writes).Any()
        || (b.writes & a.reads).Any();
}
```

The scheduler builds a dependency graph from these and runs non-conflicting systems concurrently.

**This constrains how systems are written** — you can't query arbitrarily at runtime, because the declaration wouldn't cover it. That's a real ergonomic cost and it's the price of automatic parallelism. Derive the declaration from the query types where you can, so it can't drift from reality.

---

## 6. The renderer

### 6.1 Target Vulkan 1.3 or later

Several things that used to be painful are now core or widely available:

| Feature | What it removes |
|---|---|
| **Dynamic rendering** (1.3) | Render pass and framebuffer objects, entirely |
| **`synchronization2`** (1.3) | The old barrier API's confusing stage/access pairs |
| **Descriptor indexing** (1.2) | Per-material descriptor sets (§6.2) |
| **Buffer device address** (1.2) | Descriptors for buffers, entirely |
| **Timeline semaphores** (1.2) | Fence-and-semaphore juggling |
| Descriptor buffer / descriptor heaps | Descriptor pool management |

Requiring 1.3 excludes some older hardware and removes a great deal of incidental complexity. For a from-scratch engine that's clearly the right trade.

### 6.2 Bindless is the modern answer ★

Descriptor set management used to be the wall people hit in Vulkan. It largely isn't anymore.

**Descriptor indexing** puts every texture in one unbounded array, bound once per frame:

```glsl
#extension GL_EXT_nonuniform_qualifier : require
layout(set = 0, binding = 0) uniform sampler2D textures[];

void main() {
    vec4 albedo = texture(textures[nonuniformEXT(material.albedoIndex)], uv);
}
```

**Buffer device address** removes descriptors for buffers entirely — shaders reach them through 64-bit pointers:

```glsl
#extension GL_EXT_buffer_reference : require
layout(buffer_reference, std430) readonly buffer VertexBuffer { Vertex v[]; };

layout(push_constant) uniform Push {
    VertexBuffer vertices;      // just an address
    uint materialIndex;
} pc;
```

Together these collapse the descriptor problem into: one large descriptor set for textures, bound once, and everything else addressed by index or pointer.

Newer **descriptor heap** extensions bring DX12-style heaps with `layout(descriptor_heap)` shader syntax — worth tracking, and a sign of where the API is going, though descriptor indexing plus buffer device address is sufficient today and more widely supported.

**Architectural consequence:** bindless enables GPU-driven rendering. Draw commands become data, culling becomes a compute shader, and the CPU stops submitting per-object draws. That's the direction to design toward even if v1 doesn't get there.

### 6.3 Frames in flight

Two or three frames in flight means per-frame resources need that many copies:

```cpp
constexpr uint32_t FRAMES_IN_FLIGHT = 2;

struct FrameResources {
    VkCommandPool     commandPool;
    VkCommandBuffer   commandBuffer;
    VkSemaphore       imageAvailable, renderFinished;
    VkFence           inFlight;
    LinearAllocator   uploadArena;      // per-frame scratch, reset each frame
};
FrameResources frames[FRAMES_IN_FLIGHT];
```

The per-frame linear arena is the pattern that saves the most pain: any transient upload — instance data, UI vertices, debug lines — bump-allocates from it and the whole thing resets when the frame completes. No per-allocation lifetime tracking.

### 6.4 Clustered forward over tiled Forward+ ★

The statement says "forward+," which usually means tiled forward: bin lights into 2D screen tiles, then shade reading the per-tile list.

**Clustered forward is the better default.** It divides the view frustum into a 3D grid of froxels — tiles subdivided along depth, usually with exponential slicing — so lights are bounded in depth as well as screen space.

| | **Tiled (Forward+)** | **Clustered** |
|---|---|---|
| Grid | 2D, e.g. 16×16 px | 3D, e.g. 16×9×24 froxels |
| Depth handling | Min/max per tile from a depth prepass | Explicit depth slices |
| Large depth range in a tile | **Poor** — a tile spanning near and far accumulates every light between | Good |
| Depth prepass | Effectively required | Optional |
| Transparency | Awkward (no depth) | **Works** — froxels don't need depth |

That transparency row is the decisive one. Tiled forward derives tile depth bounds from an opaque prepass, so transparent geometry has no valid tile assignment. Clustered froxels are defined by the frustum alone, so transparent objects look up their cluster the same way opaque ones do.

From-scratch Vulkan engines shipping today use clustered, and it's the right call.

```
1080p, 16×9×24 froxels = 3456 clusters
max 256 lights per cluster × 4 bytes = 3.5 MB light index buffer
```

Exponential depth slicing (the standard `slice = log(z/near) / log(far/near) × numSlices`) gives finer resolution near the camera where it matters.

### 6.5 Pipeline compilation

Creating a Vulkan pipeline is expensive, and doing it lazily on first use produces a visible hitch exactly when new content appears.

- **Pipeline cache**, serialized to disk between runs
- **Precompile at load** for everything you know you'll need
- **Pipeline libraries** or **shader objects** in newer Vulkan reduce the cost substantially by deferring or eliminating the link step

Never create a pipeline during a frame if you can avoid it.

### 6.6 Validation layers, always

Run with validation layers on for all development. Vulkan will happily let you do undefined things that work on your GPU and fail on someone else's, and the validation layer will tell you precisely what's wrong.

```cpp
#ifdef DEBUG
    instanceExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    layers.push_back("VK_LAYER_KHRONOS_validation");
    // Also enable synchronization validation — it catches the barrier
    // mistakes that are otherwise invisible until they aren't.
#endif
```

Synchronization validation in particular catches missing and incorrect barriers, which is the single hardest class of Vulkan bug to find by inspection.

---

## 7. The render graph ★

### 7.1 Hand-written barriers don't scale

For a handful of passes you can write barriers by hand. By a dozen passes with transient resources, several queues, and conditional passes, you can't — and the failure mode is a bug that manifests only on one vendor's driver.

A render graph takes declared pass inputs and outputs and derives barriers, transitions, and transient resource lifetimes automatically.

```cpp
graph.AddPass("depth-prepass", [&](PassBuilder& b) {
    b.WriteDepth(depthBuffer);
    return [=](CommandBuffer& cmd) { DrawDepthOnly(cmd); };
});

graph.AddPass("light-culling", [&](PassBuilder& b) {
    b.ReadTexture(depthBuffer, ShaderStage::Compute);
    b.WriteBuffer(clusterLightIndices);
    return [=](CommandBuffer& cmd) { cmd.Dispatch(clusterX, clusterY, clusterZ); };
});

graph.AddPass("forward", [&](PassBuilder& b) {
    b.ReadBuffer(clusterLightIndices, ShaderStage::Fragment);
    b.ReadDepth(depthBuffer);
    b.WriteColor(hdrTarget);
    return [=](CommandBuffer& cmd) { DrawOpaque(cmd); };
});
```

The graph derives: the barrier between light culling and forward, the depth layout transition from attachment to shader-read and back, that `hdrTarget` is transient and can alias other transient memory, and that any pass whose output nobody reads can be culled.

### 7.2 What it gives you

- **Automatic barriers**, derived from declared access rather than remembered
- **Transient resource aliasing** — passes with disjoint lifetimes share memory
- **Dead pass elimination**
- **Reordering** for better overlap
- **Visualization** — dump the graph and see your frame's structure

**Build it before the renderer gets complicated**, not after. Retrofitting a render graph means rewriting every pass.

---

## 8. Where the three pillars meet

### 8.1 Extraction decouples simulation from rendering

The renderer should not walk the ECS world during rendering — that couples GPU pacing to simulation and makes parallelism awkward.

```
simulate (jobs over chunks)
    ↓
extract: gather renderable data into a flat, POD frame packet
    ↓
render: consume the packet; never touches the World
```

```cpp
struct RenderPacket {
    Camera camera;
    std::vector<DrawItem> opaque, transparent;   // sorted, flat, POD
    std::vector<LightItem> lights;
};
```

Extraction is itself a parallel job over chunks. The packet is double-buffered, so the renderer consumes frame N while simulation produces N+1 — which is where a real chunk of your frame-time headroom comes from.

It also means hot reload only has to quiesce simulation jobs, not the renderer.

### 8.2 The frame

```
┌─ CPU ───────────────────────────────────────────────────┐
│ input                                                   │
│ simulation systems (parallel over chunks)               │
│ SYNC: apply command buffers                             │
│ extraction (parallel) → RenderPacket                    │
│ render graph compile (cached when unchanged)            │
│ command recording (parallel, secondary buffers)         │
│ submit                                                  │
└─────────────────────────────────────────────────────────┘
        ║ frame N+1 simulation overlaps frame N GPU work
┌─ GPU ───────────────────────────────────────────────────┐
│ depth prepass → cluster assign → light cull             │
│ → forward shading → post → present                      │
└─────────────────────────────────────────────────────────┘
```

### 8.3 Hot reload touches everything

Reloading must coordinate across all three pillars: quiesce the scheduler (§3.4), release module-owned GPU resources (§3.5), preserve world state (§3.2), and re-register systems with their access declarations (§5.5).

**That's why hot reload is the last milestone.** Every one of those touchpoints needs the system it touches to be stable first.

---

## 9. Performance

### 9.1 Where the time goes

| Stage | Budget at 60 Hz |
|---|---|
| Simulation systems | 3–5 ms |
| Structural change apply | < 0.5 ms |
| Extraction | 1–2 ms |
| Command recording | 1–2 ms |
| GPU | 8–12 ms |

### 9.2 ECS-specific

- **Chunk size** tuned to the L2 working set (§4.2)
- **Batch structural changes** — the apply step is single-threaded, so keep it small
- **Avoid tag churn** in archetype storage (§4.1)
- **Cache query results** and invalidate on archetype generation (§4.4)
- **Avoid random access by entity handle** in hot loops — it defeats the entire layout

### 9.3 Scheduler-specific

- **Fewer sync points** (§5.4) — the highest-leverage change
- **Right-sized jobs** — batch small chunks
- **Pad shared data to 64 bytes** — false sharing between workers is invisible in a profile and can halve throughput
- Pin workers to cores where the OS allows it

### 9.4 GPU-specific

- **Depth prepass** pays for itself when overdraw is high, and costs when it isn't. Measure.
- **Instancing and indirect draws** — with bindless (§6.2), draws become data
- **GPU culling** in compute rather than on the CPU
- **Watch the cluster light-list build** — it's a compute pass that scales with light count and can become the bottleneck with hundreds of lights

### 9.5 Profile properly

Tracy for CPU with per-job zones; RenderDoc for frame capture and inspection; Nsight or Radeon GPU Profiler for actual GPU timings.

**GPU timestamp queries in-engine** give you per-pass costs continuously, which is how you notice a regression rather than discovering one.

---

## 10. Tech stack and setup

| Layer | Choice | Why |
|---|---|---|
| **Language** | C++20 | Concepts and ranges genuinely help here; modules if your toolchain is ready |
| **Build** | CMake | |
| **Vulkan loader** | **volk** | Avoids the loader's dispatch overhead |
| **Instance/device setup** | **vk-bootstrap** | Saves several hundred lines of boilerplate |
| **Memory** | **VMA** | Don't write your own |
| **Shaders** | GLSL → SPIR-V via glslang, or Slang | Slang is worth evaluating — one source, multiple targets |
| **Jobs** | enkiTS or Taskflow, or your own | Your own only if the lock-free deque is the point |
| **Math** | glm, or your own | |
| **Assets** | cgltf, stb_image | |
| **Debug UI** | Dear ImGui | Non-negotiable |
| **Profiling** | Tracy | Non-negotiable |
| **Physics** | Jolt | When you need it |
| **Scripting** | Lua (sol2) or Wren | §3.1 |

Three setup notes:

**Dear ImGui and Tracy on day one.** Not "when things work." A debug UI and a profiler are how you find out whether things work, and adding them later means debugging blind until then.

**Validation layers on by default in debug**, with synchronization validation enabled. A Vulkan bug found by the validation layer costs minutes; the same bug found on another GPU costs days.

**One platform, one GPU vendor, until it works.** Cross-vendor differences will find every piece of undefined behavior you have, and you want to fix those with a working baseline rather than three broken ones.

---

## 11. Repository layout

```
ECS-Game-Engine/
├── README.md
├── docs/
│   ├── design.md                ← this document
│   ├── module-contract.md       ← ★ what a reloadable module may not do (§3.3)
│   └── render-graph.md
├── engine/
│   ├── core/
│   │   ├── memory/              ← arenas, pools; no general allocation in hot paths
│   │   ├── reflection/          ← ★ component descriptors for migration (§3.2)
│   │   └── platform/
│   ├── ecs/
│   │   ├── world.h/.cpp
│   │   ├── archetype.h/.cpp     ← ★ chunk layout (§4.2)
│   │   ├── query.h              ← cached, generation-invalidated
│   │   ├── command_buffer.h     ← ★ deferred structural change (§5.3)
│   │   └── components.h         ← ★ HOST-owned types (§3.2)
│   ├── jobs/
│   │   ├── scheduler.h/.cpp
│   │   ├── deque.h              ← work stealing
│   │   └── system_graph.cpp     ← ★ access declarations → dependencies (§5.5)
│   ├── render/
│   │   ├── vk/                  ← device, swapchain, bindless set
│   │   ├── graph/               ← ★ render graph (§7)
│   │   ├── passes/              ← depth, cluster, cull, forward, post
│   │   └── extract.cpp          ← ★ World → RenderPacket (§8.1)
│   └── modules/
│       ├── loader.cpp           ← ★ quiesce, unload, reload, migrate (§3.4)
│       └── host_api.h           ← the ABI modules see
├── gameplay/                    ← the reloadable module: systems only
├── scripts/                     ← Lua gameplay logic (§3.1)
├── shaders/
└── tools/
    └── module_verify.py         ← ★ reject modules violating §3.3
```

`engine/ecs/components.h` living in the host rather than the module is the structural decision that makes §3 work at all.

---

## 12. Milestone ladder

### M0 — Scope decision ★
**Est. 2–3 days**

Decide honestly: learning project, a specific game, or a reusable engine (§1). Decide the hot-reload strategy — script, native, or hybrid (§3.1). Write `docs/module-contract.md`.

**Done when:** you've written down what you're *not* building, and why.

---

### M1 — Core and ECS ★
**Est. 6–8 weeks**

Arenas and pools, entity handles with generations, archetype chunk storage, queries with caching, command buffers.

**Build this first — everything depends on the data model.** And build reflection alongside it, because retrofitting reflection onto existing components is much worse than including it from the start.

**Done when:** a million entities can be created, queried, and iterated with measured throughput.

---

### M2 — Job scheduler
**Est. 3–4 weeks**

Work-stealing deque (or enkiTS), chunk-granularity dispatch, system access declarations, the dependency graph, sync points.

**Done when:** independent systems demonstrably run in parallel, and structural changes never crash under a stress test with many threads.

---

### M3 — Vulkan foundation
**Est. 6–8 weeks**

volk, vk-bootstrap, VMA, swapchain, frames in flight, dynamic rendering, `synchronization2`, bindless descriptor set, buffer device address, pipeline cache. A triangle, then a textured mesh.

**This is the longest single milestone and it feels unproductive.** It is not — every shortcut here becomes a structural problem later.

---

### M4 — Render graph ★
**Est. 3–4 weeks**

Pass declaration, resource lifetime analysis, automatic barriers, transient aliasing, dead pass culling, visualization.

**Before the renderer gets complicated.** Retrofitting means rewriting every pass.

---

### M5 — Clustered forward
**Est. 4–6 weeks**

Depth prepass, cluster assignment, compute light culling, forward shading with PBR, shadows, tonemapping.

**Done when:** a Sponza-class scene renders with a few hundred lights at a measured frame time.

---

### M6 — ECS/renderer integration
**Est. 2–3 weeks**

Extraction to a flat render packet, double buffering, parallel command recording, GPU-driven culling.

---

### M7 — Scripting
**Est. 2–3 weeks**

Lua bindings over the world, hot-reloadable scripts, a script system type.

**This delivers most of the hot-reload value** (§3.1) for a fraction of M8's cost. Do it first and find out whether M8 is still needed.

---

### M8 — Native hot reload ★
**Est. 4–6 weeks**

Module loading, scheduler quiescing, resource ownership tracking, reflection-driven world migration, the module contract verifier.

**Last, deliberately.** It touches every other system.

---

### M9 — Tooling and polish
**Est. ongoing**

ImGui inspectors, Tracy zones everywhere, GPU timestamps per pass, scene serialization, asset loading.

---

**Total: roughly 30–40 weeks of focused full-time work for a competent engineer**, and that estimate has no game in it.

---

## 13. Reference implementations

### 13.1 The archetype chunk

```cpp
class Archetype {
    ComponentMask mask;
    std::vector<ComponentTypeInfo> types;    // sorted by type id — deterministic
    std::vector<size_t> offsets;             // into the chunk's data block
    std::vector<Chunk*> chunks;
    uint32_t entitiesPerChunk;

public:
    template <typename T>
    std::span<T> GetArray(Chunk* c) {
        const int i = IndexOf(TypeId<T>());
        return { reinterpret_cast<T*>(c->data + offsets[i]), c->entityCount };
    }

    void ComputeLayout() {
        // Sort by alignment descending to minimize padding, then lay out SoA.
        size_t stride = 0;
        for (const auto& t : types) stride += t.size;
        entitiesPerChunk = uint32_t(CHUNK_SIZE / stride);

        size_t off = 0;
        for (size_t i = 0; i < types.size(); ++i) {
            off = AlignUp(off, types[i].align);
            offsets[i] = off;
            off += types[i].size * entitiesPerChunk;
        }
    }
};
```

Sorting `types` by type id is not cosmetic — it makes the archetype's identity independent of the order components were added, so `{Position, Velocity}` and `{Velocity, Position}` are the same archetype rather than two.

### 13.2 The system dependency graph

```cpp
std::vector<std::vector<SystemId>> BuildExecutionStages(
    std::span<const SystemAccess> systems)
{
    std::vector<std::vector<SystemId>> stages;
    std::vector<bool> scheduled(systems.size(), false);
    size_t remaining = systems.size();

    while (remaining > 0) {
        std::vector<SystemId> stage;
        ComponentMask stageWrites, stageReads;

        for (size_t i = 0; i < systems.size(); ++i) {
            if (scheduled[i]) continue;
            const auto& s = systems[i];

            // A structural-change system gets a stage to itself.
            if (s.structuralChanges && !stage.empty()) continue;

            if ((s.writes & stageReads).Any() ||
                (s.writes & stageWrites).Any() ||
                (s.reads & stageWrites).Any()) continue;

            stage.push_back(SystemId(i));
            stageWrites |= s.writes;
            stageReads  |= s.reads;
            scheduled[i] = true;
            --remaining;

            if (s.structuralChanges) break;     // alone in its stage
        }
        stages.push_back(std::move(stage));
    }
    return stages;
}
```

Greedy stage packing is simple and good enough. A topological schedule with critical-path prioritization is better and rarely worth the complexity at this scale.

### 13.3 The clustered light assignment

```glsl
// Exponential depth slicing: fine near the camera, coarse far away.
uint ClusterIndex(vec2 screenUV, float viewZ) {
    uvec2 tile = uvec2(screenUV * vec2(CLUSTER_X, CLUSTER_Y));
    uint slice = uint(log(viewZ / zNear) / log(zFar / zNear) * float(CLUSTER_Z));
    slice = clamp(slice, 0u, CLUSTER_Z - 1u);
    return (slice * CLUSTER_Y + tile.y) * CLUSTER_X + tile.x;
}
```

Linear depth slicing wastes almost all the slices on the far plane, where the perspective divide means they cover very little screen area. The logarithmic distribution is what makes clustering work.

---

## 14. Testing

### 14.1 ECS invariants

```cpp
TEST_CASE("stale handles are rejected") {
    World w;
    Entity e = w.Create();
    w.Destroy(e);
    Entity e2 = w.Create();              // likely reuses the index
    REQUIRE_FALSE(w.IsAlive(e));         // generation differs
    REQUIRE(w.IsAlive(e2));
}

TEST_CASE("archetype identity is order-independent") {
    World w;
    Entity a = w.Create(); w.Add<Position>(a); w.Add<Velocity>(a);
    Entity b = w.Create(); w.Add<Velocity>(b); w.Add<Position>(b);
    REQUIRE(w.ArchetypeOf(a) == w.ArchetypeOf(b));
}

TEST_CASE("queries see every matching entity exactly once") {
    // Populate across many archetypes, then count.
}
```

### 14.2 Scheduler stress

```cpp
TEST_CASE("concurrent structural changes never corrupt the world") {
    World w;
    Scheduler s(std::thread::hardware_concurrency());

    for (int iter = 0; iter < 1000; ++iter) {
        auto buffers = s.ParallelFor(10000, [&](int i, CommandBuffer& cb) {
            if (i % 3 == 0) cb.AddComponent(entities[i], Tag{});
            if (i % 5 == 0) cb.RemoveComponent<Tag>(entities[i]);
            if (i % 7 == 0) cb.DestroyEntity(entities[i]);
        });
        w.ApplyCommandBuffers(buffers);
        REQUIRE(w.Validate());           // full internal consistency check
    }
}
```

Run under ThreadSanitizer. Data races in a work-stealing scheduler are rare, catastrophic, and invisible without a sanitizer.

### 14.3 Hot reload

```cpp
TEST_CASE("world survives a reload with a changed component layout") {
    World w = LoadTestWorld();
    auto before = Snapshot(w);

    LoadModule("gameplay_v1.dll");
    ReloadModule("gameplay_v2.dll");      // Velocity gains a z field

    REQUIRE(w.EntityCount() == before.entityCount);
    for (auto [e, v] : w.Query<Velocity>()) {
        REQUIRE(v.x == before.velocity[e].x);   // preserved by name
        REQUIRE(v.z == 0.0f);                    // new field defaulted
    }
}

TEST_CASE("reload during heavy job load does not crash") {
    // Submit thousands of jobs, reload mid-flight, assert quiesce worked.
}
```

### 14.4 Rendering

- **Golden image comparison** with a tolerance — GPU results differ across vendors and drivers, so hash equality won't work
- **Validation layers clean** on every test run, as a build gate
- **Render graph correctness**: assert the derived barriers match a hand-verified expectation for a set of fixture graphs
- **GPU timestamp regression**: track per-pass timings and fail on a significant increase

### 14.5 Memory

Run under ASan and UBSan. Assert zero leaks at shutdown, including Vulkan objects — the validation layer reports unreleased handles at device destruction, and that report should be empty.

---

## 15. Stretch goals

| Feature | Effort | Value |
|---|---|---|
| **GPU-driven rendering** | Large | Draw commands as data, compute culling. Bindless already points here. |
| **Mesh shaders** | Medium | Meshlet culling; substantial on modern hardware |
| Ray-traced shadows or GI | Large | `VK_KHR_ray_tracing_pipeline` |
| **Editor** | Very large | Its own project; resist until the engine is stable |
| Asset cooking pipeline | Medium | Offline conversion, texture compression, mesh optimization |
| Multi-queue (async compute) | Medium | Overlap compute with graphics; the render graph makes it tractable |
| **Scene serialization** | Medium | Reflection already exists for §3.2 — this is mostly free |
| Networking | Large | See the rollback netcode analysis; composes badly with hot reload |
| Cross-platform | Medium each | After one platform is solid |

Scene serialization is the best value on that list: the reflection system built for hot-reload migration is exactly what serialization needs, so it's largely a matter of choosing a format.

---

## 16. References

### ECS

- **Unity DOTS** documentation — the clearest public explanation of archetype chunk storage
- **Flecs** documentation and its author's blog series — the most thorough public writing on ECS design tradeoffs
- **EnTT** — the reference sparse-set implementation; read the source
- **Bevy ECS** — a modern archetype implementation with an unusually clean scheduler
- Sander Mertens, "Why Vanilla ECS Is Not Enough" and the archetype/sparse-set comparisons

### Jobs

- **Chase & Lev**, "Dynamic Circular Work-Stealing Deque" — the canonical algorithm
- **enkiTS**, **Taskflow** — mature implementations worth reading
- Naughty Dog's "Parallelizing the Naughty Dog Engine Using Fibers" (GDC) — fibers as an alternative model

### Vulkan

| Source | For |
|---|---|
| **Vulkan Guide** (vkguide.dev) | The best modern introduction; uses dynamic rendering and bindless |
| **Vulkan Specification** — Resource Descriptors chapter | Descriptor types, descriptor buffer, buffer device address |
| **Sascha Willems' samples** | Including recent descriptor-heap examples |
| **VMA** documentation | Memory allocation patterns |
| "Render graphs and Vulkan" (Themaister) | The render graph design in §7 |
| Vulkan Memory Model and synchronization examples | Barriers, the hardest part |

### Rendering

- **Ola Olsson et al.**, "Clustered Deferred and Forward Shading" — the origin of the clustered approach in §6.4
- **Emil Persson**, "Practical Clustered Shading"
- **Takahiro Harada**, "Forward+: Bringing Deferred Lighting to the Next Level" — the tiled technique the statement names
- **Real-Time Rendering**, 4th edition — the reference for everything else
- Open from-scratch Vulkan engines using clustered forward with bindless — worth reading for how the pieces fit together

---

## Appendix A — Decision record

| Decision | Rationale |
|---|---|
| **Decide the goal before the architecture** | Three pillars is 30–40 weeks with no game in it. "Building an engine for a game" is the most common way both fail. |
| **Ask why native hot reload rather than a script VM** | The value is iteration speed, which a script layer delivers with no type-layout, vtable, or static-state problems |
| Hybrid: hot systems compiled, cold logic scripted | Most gameplay code isn't hot; the reloadable native set should be earned by a profile |
| **Host owns all component type definitions** | A module-defined struct whose layout changes invalidates every stored instance — the core ECS/hot-reload conflict |
| Reflection-driven migration by field name | Handles added, removed, and reordered fields at the cost of a hitch proportional to world size |
| **Reloadable modules are stateless function libraries** | No globals, no statics, no vtables surviving reload, no function pointers held by the host |
| Modules describe GPU resources; the host owns them | Removes the entire class of leak-on-unload and dangling-shader problems |
| **Quiesce the scheduler before unload** | A work-stealing deque holds jobs that execute after you stop submitting; unloading under them is an immediate crash |
| Copy the DLL to a temp path before loading | Otherwise the linker can't overwrite it on the next build |
| **Archetype storage first, hybrid if needed** | Fast linear iteration, and chunks are the natural unit of job parallelism |
| Know the archetype pathology: toggled tags | Adding a tag moves *all* of an entity's components; use a bool field when it toggles often |
| Entity handles carry a generation | Without it, index reuse makes a stale handle silently refer to a different entity |
| Archetype types sorted by type id | Makes archetype identity independent of component insertion order |
| Chunk size ~16 KB, SoA within the chunk | Fits L2, and a system reading one component touches only its cache lines |
| **Deferred structural changes via command buffers** | Structural changes move memory other jobs are iterating — the classic ECS crash |
| Apply command buffers in deterministic order | Completion-order apply makes the same frame produce different results |
| **Minimize sync points** | Each one serializes the whole scheduler; eight per frame costs milliseconds of idle workers |
| Static access declarations, derived from query types | Automatic parallelism requires knowing access ahead of time; deriving it prevents drift |
| Pad shared scheduler data to 64 bytes | False sharing is invisible in a profile and can halve throughput |
| **Target Vulkan 1.3+** | Dynamic rendering, `synchronization2`, descriptor indexing, buffer device address, timeline semaphores — a large reduction in incidental complexity |
| **Bindless: descriptor indexing + buffer device address** | Collapses the hardest part of Vulkan into one set bound per frame plus 64-bit pointers, and points toward GPU-driven rendering |
| Use volk, vk-bootstrap, and VMA | Writing your own teaches little and costs weeks |
| **Clustered forward over tiled Forward+** | 3D froxels handle large depth ranges, and transparency works because froxels don't depend on an opaque depth prepass |
| Exponential depth slicing | Linear slicing wastes nearly all slices on the far plane |
| **Render graph before the renderer gets complicated** | Hand-written barriers don't survive a dozen passes, and retrofitting means rewriting every pass |
| Pipeline cache on disk; never create a pipeline mid-frame | Lazy compilation hitches exactly when new content appears |
| **Validation layers plus synchronization validation, always on in debug** | The barrier bugs are otherwise invisible until they appear on someone else's GPU |
| **Extraction to a flat POD render packet** | Decouples GPU pacing from simulation, enables double buffering, and means reload only quiesces simulation |
| ImGui and Tracy on day one | They're how you find out whether anything works |
| One platform, one vendor, until it works | Cross-vendor differences find every piece of undefined behavior at once |
| **Hot reload is the last milestone** | It touches every other system and building it against moving targets means building it repeatedly |
| Golden image tests use tolerance, not hashes | GPU results differ across vendors and drivers |
| Scheduler stress tests under ThreadSanitizer | Work-stealing races are rare, catastrophic, and invisible without it |

---

## Appendix B — Quick reference card

```
SCOPE — three pillars ≈ 30–40 weeks, and no game in that number
  learning project      → build all three, don't attach a game
  shipping a game       → use Bevy/Flecs/EnTT + an existing renderer
  reusable engine       → be excellent at ONE pillar

ORDER (dependencies are real)
  ECS → scheduler → Vulkan → render graph → clustered forward
  → scripting → NATIVE HOT RELOAD LAST (it touches everything)

HOT RELOAD
  ★ ask first: why not a script VM? it gives iteration speed for free
  if native, the module may NOT contain:
    state/globals/statics · vtables outliving reload · function pointers
    COMPONENT TYPE DEFINITIONS · unregistered GPU resources
  ★ host owns all component types — a changed layout invalidates
    every stored instance in archetype chunks
  reflection migration maps fields BY NAME (added → default, removed → drop)
  ★ QUIESCE the scheduler before unload — deques hold jobs you didn't submit
  copy the DLL to temp before loading, or the linker can't overwrite it

ECS STORAGE
                  archetype            sparse set
  iteration       FAST (linear SoA)    slower (set intersection)
  add/remove      EXPENSIVE (moves     cheap (array push/swap)
                  ALL components)
  pathology       toggled tag comps    many-component queries
  chunk ~16 KB, SoA inside · chunks = job granularity
  sort types by id ⇒ archetype identity is insertion-order independent
  entity = {index, GENERATION} or stale handles silently alias

SCHEDULER
  work stealing: owner pops LIFO (cache), thieves steal FIFO (bigger jobs)
  ★ structural changes NEVER inline → per-thread command buffers
    applied at a sync point in DETERMINISTIC order
  ★ sync points serialize everything — 1–2 per frame, not one per system
  static reads/writes masks → dependency graph → parallel stages
  pad shared data to 64 bytes (false sharing)

VULKAN — target 1.3+
  dynamic rendering · synchronization2 · timeline semaphores
  ★ BINDLESS: descriptor indexing (one unbounded texture array)
             + buffer device address (64-bit pointers, no descriptors)
    → removes the hardest part of Vulkan, enables GPU-driven rendering
  volk + vk-bootstrap + VMA — do not rewrite these
  pipeline cache on disk; never compile mid-frame
  ★ validation layers + SYNCHRONIZATION validation, always, in debug

CLUSTERED > TILED FORWARD+
  tiled:     2D 16×16 px, needs depth prepass, transparency awkward
  clustered: 3D froxels (16×9×24), depth-bounded, TRANSPARENCY WORKS
  slice = log(z/near)/log(far/near) × numSlices   ← linear wastes slices
  1080p 16×9×24 = 3456 clusters × 256 lights × 4B = 3.5 MB

RENDER GRAPH — build it BEFORE the renderer gets complicated
  declare pass reads/writes → derive barriers, transitions, aliasing
  hand-written barriers stop scaling around a dozen passes, and the
  failure mode is a bug that only appears on another vendor's driver

INTEGRATION
  simulate → SYNC → extract to a flat POD RenderPacket → render
  the renderer never walks the World
  double-buffer the packet: frame N+1 sim overlaps frame N GPU
```
