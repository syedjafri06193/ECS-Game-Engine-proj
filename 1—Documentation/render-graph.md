# Render graph

[`engine/render/graph/render_graph.h`](../engine/render/graph/render_graph.h)
— design doc §7.

Passes declare what they read and write. The graph derives barriers and
layout transitions, culls dead passes, computes transient lifetimes and
aliases transient memory. The core is **backend-agnostic** — stages, access
bits and layouts mirror Vulkan synchronization2 one-to-one — so the part
where barrier bugs live is unit-tested without a GPU.
[`render/vk/vk_graph_backend.cpp`](../engine/render/vk/vk_graph_backend.cpp)
maps it to `vkCmdPipelineBarrier2`.

## Declaring a frame

```cpp
RenderGraph g;
auto depth  = g.CreateTexture("depth", {w, h, Format::D32F});
auto lights = g.CreateBuffer("cluster-lights", {3456ull * 257 * 4});
auto hdr    = g.CreateTexture("hdr", {w, h, Format::RGBA16F});
auto swap   = g.ImportTexture("swapchain", {w, h, Format::BGRA8_SRGB},
                              State{},                                   // initial
                              State{StageBottomOfPipe, AccessNone, Layout::Present});

g.AddPass("depth-prepass", QueueType::Graphics, [&](PassBuilder& b) {
    b.WriteDepth(depth);
    return [=](PassContext& ctx) { /* record */ };
});
g.AddPass("light-culling", QueueType::Compute, [&](PassBuilder& b) {
    b.WriteBuffer(lights, ShaderCompute);
    return ...;
});
g.AddPass("forward", QueueType::Graphics, [&](PassBuilder& b) {
    b.ReadBuffer(lights, ShaderFragment);
    b.ReadDepth(depth);
    b.WriteColor(hdr);
    return ...;
});
g.AddPass("tonemap", ...);                         // ReadTexture(hdr), WriteColor(swap)
g.AddPass("ui", ...);                              // WriteColor(swap, /*load_existing=*/true)

g.Compile();
g.Execute(backend, ctx);
```

## What `Compile` does

1. **Dead pass culling.** Walk backwards from observable outputs (imported
   resources, passes marked `SideEffect()`). A pass survives if it writes
   something a surviving pass needs. A pass that draws into a transient
   nobody reads is dropped.
2. **Validation.** Reading a transient before any surviving pass wrote it
   is an error (attachment writes that read — depth test, blending — count
   as starting from cleared contents; `load_existing` does not).
3. **Lifetimes** over the surviving order.
4. **Aliasing.** Greedy interval packing: transients whose lifetimes don't
   overlap share a memory slot (textures with textures, buffers with
   buffers). The backend allocates one block per slot and binds each resource
   to it.
5. **Barriers.** Each resource's state is tracked through the passes:
   - read after read in the same layout **merges** — no barrier — so the
     next write waits for *every* reader;
   - anything involving a write, or a layout change, emits a barrier;
   - the first use of aliased memory waits for the previous occupant's last
     users and discards contents (old layout `Undefined`);
   - imported resources get a final barrier to their declared final state
     (e.g. `Present`).
6. **Caching.** The declarations are hashed; an unchanged frame reuses the
   previous compile, so rebuilding the graph every frame is cheap (§8.2).

## Verified expectations

`tests/test_render.cpp` checks the derived barriers for the doc's example
frame against a hand-verified list:

| Pass | Resource | Transition |
|---|---|---|
| depth-prepass | depth | Undefined → DepthAttachment |
| light-culling | depth | DepthAttachment (late-frag write) → ShaderReadOnly (compute) |
| light-culling | cluster lights | none (fresh buffer) |
| forward | cluster lights | compute storage write → fragment storage read |
| forward | depth | ShaderReadOnly → DepthReadOnly |
| forward | hdr | Undefined → ColorAttachment |
| tonemap | hdr | color write → fragment sampled read |
| tonemap | swapchain | Undefined → ColorAttachment |
| (final) | swapchain | ColorAttachment → Present |

plus: read-read merging, aliasing of a three-pass bloom chain into two slots,
the uninitialised-read error, compile caching, executor ordering, and the
overlay-keeps-its-underlay rule. `RenderGraph::ToDot()` dumps the graph for
Graphviz (press **G** in the demo).
