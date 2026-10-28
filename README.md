# ECS-Game-Engine-proj

Custom entity-component-system engine with a job-based scheduler, hot-reloadable
gameplay modules, and a Vulkan forward+ renderer.

Built to the design in [`1—Documentation/design-guide.md`](1—Documentation/design-guide.md) — or
more precisely to its revised statement: a C++20 ECS with **hybrid storage**, a
**work-stealing scheduler** with statically declared system access and deferred
structural changes, a **Vulkan 1.3** renderer with bindless resources and a
**render graph** doing **clustered** forward shading, and **hot reload** through
Lua scripts plus native modules that own no state and no types.

```
            ┌────────────── SystemPipeline (stages, sync points) ──────────────┐
 World  ──▶ │ host systems │ gameplay module (hot reload) │ Lua scripts (reload) │
 (archetype └───────────────────────── Scheduler (work stealing) ──────────────┘
  chunks +                          │
  sparse sets)                      ▼ Extract (parallel, double-buffered RenderPacket)
                                    │
                       RenderGraph: depth prepass → light culling (compute)
                                    → clustered forward → tonemap → ui
```

## Status

| Milestone | State |
|---|---|
| M0 scope decision | [`1—Documentation/scope.md`](1—Documentation/scope.md) |
| M1 core + ECS | Done, tested, benchmarked at 1 M entities |
| M2 job scheduler | Done, tested under ThreadSanitizer |
| M3 Vulkan foundation | Written and **compiles**; needs a GPU to run (see below) |
| M4 render graph | Done — barrier derivation, culling, aliasing, caching unit-tested without a GPU |
| M5 clustered forward | CPU reference tested; GPU shaders compile to SPIR-V; needs a GPU to validate |
| M6 ECS/renderer integration | Extraction + double-buffered packets tested; parallel command recording and GPU-driven culling not done |
| M7 scripting | Done — Lua with live reload |
| M8 native hot reload | Done — quiesce, reload, rollback, contract verifier |
| M9 tooling | ImGui overlay, GPU timestamps per pass, Tracy hooks, snapshot serialisation; asset loading not done |

**What's verified here and what isn't.** Everything on the CPU side — ECS,
scheduler, render graph, clustering math, extraction, scripting, hot reload —
is covered by 39 test cases (75,000+ assertions) that pass under GCC and
Clang with `-Werror`, AddressSanitizer + UBSan (zero leaks), and
ThreadSanitizer. The Vulkan renderer and demo compile cleanly and the shaders
compile to SPIR-V, but they were built without a GPU, so they have **not
been run**. Expect to spend M3's first session with the validation layers on,
fixing what they report.

## Build

Requirements: CMake ≥ 3.25, a C++20 compiler (GCC 12+, Clang 15+, AppleClang 15+,
MSVC 2022), Python 3 (for the module verifier on Linux). Dependencies (Lua,
doctest, and the Vulkan stack when enabled) are fetched and pinned by CMake.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
./build/apps/bench/engine_bench            # 1M entities
```

Sanitizer builds: `-DENGINE_SANITIZE=address` (ASan + UBSan) or `=thread`.

### The Vulkan demo

```sh
cmake -S . -B build-vk -G Ninja -DCMAKE_BUILD_TYPE=Release -DENGINE_BUILD_VULKAN=ON
cmake --build build-vk
./build-vk/apps/demo/engine_demo
```

Needs a Vulkan 1.3 GPU with descriptor indexing and buffer device address
(any desktop GPU from the last several years). If `glslangValidator` isn't
installed, glslang is built from source. On Linux, GLFW needs the X11
development packages (`libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev`).
**macOS**: the CPU side builds and all tests run; the renderer needs a
MoltenVK that exposes Vulkan 1.3, which depends on your Vulkan SDK version —
untested.

In the demo: 1,600 cubes, 400 orbiting point lights, an ImGui overlay with
simulation stats, reload counts and per-pass GPU timings. **R** forces a
module reload, **G** prints the render graph as Graphviz.

## Hot reload, in practice

**Lua** (instant): edit [`scripts/lights.lua`](scripts/lights.lua) while the
demo runs and save. A syntax error or a typo in a component name keeps the
previous version running and shows the error in the overlay.

**Native module**: edit [`gameplay/gameplay.cpp`](gameplay/gameplay.cpp) and
run `cmake --build build-vk --target gameplay`. The demo notices the new
build, quiesces the scheduler, swaps the library and re-registers its
systems; the world is untouched. A broken build rolls back to the running one.
Modules contain **systems only** — see [`1—Documentation/module-contract.md`](1—Documentation/module-contract.md);
the build rejects modules that hold state, export extra symbols or use vtables.

**Component layout changes** need a restart; world state survives through
reflection-driven snapshot migration (`ecs/snapshot.h`), which maps fields
by name.

## Using the ECS

```cpp
World w;
Entity e = w.Create(Position{0, 0, 0}, Velocity{1, 0, 0});
w.Add(e, Health{100, 100});

for (auto [entity, pos, vel] : w.Query<Position, const Velocity>().Without<Dead>())
    pos.x += vel.x * dt;                       // `const` = read-only access

Scheduler scheduler;                            // one worker per hardware thread
SystemPipeline pipeline(w, scheduler);
pipeline.Each<Position, const Velocity>("move", [](SystemContext& ctx, Entity, Position& p, const Velocity& v) {
    p.x += v.x * ctx.dt;                        // runs in parallel over chunks
});
pipeline.Each<Lifetime>("expire", [](SystemContext& ctx, Entity e, Lifetime& l) {
    if ((l.seconds -= ctx.dt) <= 0) ctx.commands.DestroyEntity(e);   // deferred to the sync point
}, /*structural=*/true);
pipeline.Run(dt);
```

Access declarations are derived from the query's types, so they can't drift
from the code. Structural changes during a parallel stage — or while
iterating — assert instead of corrupting memory.

## Benchmarks

`engine_bench`, 1,000,000 entities, 2-core cloud VM (numbers scale with cores):

| | |
|---|---|
| Create (Position, Velocity) | 166 ns/entity |
| Iterate, single thread | 3.3 ns/entity (306 M/s) |
| Iterate, parallel pipeline | 1.5 ns/entity per frame |
| Apply structural commands | 150 ns/command |
| Tag toggle, archetype storage (6 components) | 121 ns |
| Tag toggle, sparse set (hybrid) | **26 ns** |
| Empty job round trip | 320 ns |

## Findings along the way

- **The design doc's stage packer (§13.2) can reorder dependent systems.** A
  system that reads Y may be packed into a stage *before* an earlier system
  that writes Y. The implementation also checks earlier waiting systems;
  `tests/test_jobs.cpp` reproduces the doc's version failing.
- **"Apply command buffers in worker order" isn't deterministic** under work
  stealing. Buffers are per job instead; the world ends up bit-identical with
  1 or N workers.
- **Lua and C++ RAII don't mix.** C Lua reports errors with `longjmp`, which
  skips destructors; ASan caught leaked vectors in the bindings. They now
  keep only trivially destructible state across anything that can raise.
- **Range-for over a named query held the iteration lock until the query
  object died** (caught by the lock's own assertion). The lock now lives in
  the loop's iterator.

## Layout

```
engine/core       arenas, reflection, platform, math
engine/ecs        World, archetypes, queries, command buffers, sparse sets, snapshots, components.h (host-owned types)
engine/jobs       work-stealing deque, scheduler, system dependency graph
engine/render     render graph, clustering (CPU reference), extraction, vk/ (Vulkan backend)
engine/scripting  Lua script systems
engine/modules    host ABI, module SDK, loader
gameplay/         the reloadable module (systems only) + fixtures
scripts/          Lua gameplay
shaders/          GLSL (bindless, buffer device address)
apps/             demo, bench
tests/            doctest suites
tools/            module_verify.py
docs/             scope, module contract, render graph
```

## Next steps

1. Run the demo on a GPU with validation + synchronization validation on;
   fix what they report; enable back-face culling once winding is confirmed.
2. Parallel command recording (secondary command buffers) and GPU-driven
   culling (M6).
3. Shadows and a glTF loader (M5, M9).
4. A debug allocator that asserts on allocations from module code (§3.3).
