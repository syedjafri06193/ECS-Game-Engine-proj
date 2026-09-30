# M0 — Scope decision

The design doc (§1) asks for this to be written down before anything else:
what is being built, what isn't, and why.

## Framing

**Learning project: build all three pillars.** The goal is to understand
data-oriented ECS storage, work-stealing scheduling, explicit-API rendering
and hot reload deeply, not to ship a particular game. That framing accepts
the doc's 30–40 week estimate and deliberately attaches no game to it.

Where a library would do, a library is used: volk, vk-bootstrap and VMA for
Vulkan plumbing (§2.1), Lua for scripting, Dear ImGui for debug UI. The
work-stealing deque is written from scratch because it *is* the point of M2.

## Hot-reload strategy: hybrid (§3.1)

| Code | Where | Reload |
|---|---|---|
| Transform, culling, extraction, rendering | Compiled into the host | Restart |
| Gameplay rules, AI, level logic | Lua (`scripts/`) | Instant, on save |
| Hot gameplay systems profiling proves need C++ | Native module (`gameplay/`) | Hot, via quiesce + reload |

Native modules are stateless function libraries that never define component
types — see [`module-contract.md`](module-contract.md). Component layout
changes go through reflection-driven snapshot migration and a restart.

## Storage: hybrid (§4.1)

Archetype chunks for stable, iteration-heavy components; sparse sets for
volatile ones toggled often (`World::RegisterSparse<T>()`). Measured on the
benchmark: toggling a tag on 6-component entities costs 121 ns in archetype
storage and 26 ns in a sparse set.

## Explicit non-goals (§1)

- **No editor.** Scenes are code; the debug UI is ImGui.
- **No physics engine.** Jolt when it's needed.
- **No asset pipeline in v1.** Procedural meshes/textures; glTF later.
- **Not cross-platform in v1.** Linux first; the CPU side is portable
  (Linux/macOS/Windows), the renderer targets one Vulkan 1.3 desktop GPU.
- **No networking.** It composes badly with hot reload.

## Deliberate departures from the design doc

1. **Stage packing preserves registration order** (§13.2). The doc's
   greedy packer lets a later system that reads Y run before an earlier
   system that writes Y. `BuildExecutionStages` also checks earlier
   *waiting* systems; `tests/test_jobs.cpp` demonstrates the difference.
2. **Command buffers are per job, not per worker** (§5.3). With work
   stealing, which items land on which worker varies run to run, so
   "apply in worker order" is still nondeterministic. One buffer per job,
   applied in job order, gives bit-identical results across thread counts
   (tested with 1 vs N workers).
3. **Structural systems may share a stage with earlier non-conflicting
   work**, but always end it (the doc gives them a stage alone). Commands
   still apply before anything registered after them runs.
4. **Iteration holds a structure lock.** Adding a component while iterating
   asserts immediately instead of corrupting the chunk being walked.
5. **Light culling doesn't read depth.** Froxels are defined by the frustum
   alone (§6.4), so the cull pass depends only on lights; the doc's example
   graph reads depth there, and the unit-test fixture keeps that shape to
   check the barrier derivation.
