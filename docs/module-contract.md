# The module contract

A reloadable gameplay module is a **stateless function library** (design doc
§3.3). All state lives in the host's World; a module only reads and writes it
through the C ABI in [`engine/modules/host_api.h`](../engine/modules/host_api.h).

## The rules

| A module may not… | Because… | Enforced by |
|---|---|---|
| **Define component types** | Reloading would change the layout under data already stored in chunks (§3.2) | Host resolves every component a system names; unknown names fail the load |
| **Hold persistent state** — globals, statics, function-local statics | Lost or duplicated on reload | `module_verify.py`: no symbols in `.data`/`.bss`/TLS |
| **Use virtual functions on objects that outlive a call** | The vtable pointer points into unloaded memory | `module_verify.py`: no vtables |
| **Store function pointers or `std::function`s in host structures** | They dangle after unload | By construction: the host looks entry points up itself and drops them before unloading |
| **Create GPU resources** | The driver would hold references to shader code about to vanish (§3.5) | By construction: the host API exposes no Vulkan |
| **Export anything but `EngineModuleManifest` and `System_*`** | An unexpected export is almost always hidden state or a leaked API | `module_verify.py` |

The verifier runs as a **post-build step** on every module (Linux), so a
violation fails the build, and CTest checks it rejects
[`gameplay/gameplay_bad.cpp`](../gameplay/gameplay_bad.cpp), which breaks
four rules on purpose.

## What a module looks like

```cpp
#include "modules/module_sdk.h"

ENGINE_MODULE_EXPORT void System_Move(const EngineHostApi* api, const EngineChunk* c, float dt, void*)
{
    auto* pos = engine::sdk::Column<game::Position>(api, c);
    const auto* vel = engine::sdk::Column<const game::Velocity>(api, c);
    for (uint32_t i = 0, n = engine::sdk::Count(api, c); i < n; ++i) pos[i].x += vel[i].x * dt;
}

// Manifest: constant data only.
constexpr const char* kReads[]  = {"game::Velocity", nullptr};
constexpr const char* kWrites[] = {"game::Position", nullptr};
constexpr EngineSystemDesc kSystems[] = {{"move", kReads, kWrites, 0, "System_Move", 256}};
constexpr EngineManifest kManifest = {ENGINE_MODULE_ABI_VERSION, "gameplay", 1, 1, kSystems};
ENGINE_MODULE_EXPORT const EngineManifest* EngineModuleManifest() { return &kManifest; }
```

Each system declares what it reads and writes; the scheduler builds its
dependency graph from those declarations. Structural systems (`structural = 1`)
destroy/add/remove through the command buffer passed as the last argument.

## The reload sequence

`ModuleHost::Reload` (see [`loader.h`](../engine/modules/loader.h)):

1. **Quiesce** the scheduler: wait for every in-flight job, including jobs
   queued in worker deques that haven't started.
2. **Unregister** the module's systems from the pipeline.
3. **Unload** the old library.
4. **Load** the new build from a fresh temp copy (Windows can't overwrite a
   loaded DLL; Linux and macOS would return the cached image).
5. **Validate** the manifest (ABI version, entry points, component names) and
   re-register systems.
6. **Resume.**

If step 4 or 5 fails — a half-written file, a broken build — the previous
build is reloaded from its private copy and the error is reported. The game
never runs without its systems.

`PollAndReload` waits for the file's modification time to be stable for a
short settle period before reloading, so a linker still writing the file is
never loaded.

## What the verifier can't see

- Heap allocations made by module code and kept (e.g. a `new` whose pointer is
  stashed in a component). Don't.
- Pointers into module `.rodata` handed to the host (string literals in
  components). The manifest's strings are the one exception: the host copies
  what it needs before unloading.
- Thread-local storage created lazily by the C++ runtime.

A debug allocator that asserts on allocations from module code outside the
host's arenas (§3.3) would close the first gap; it's listed in the README's
next steps.
