#pragma once
// Native module loading and hot reload (design doc §3.4, M8).
//
// Reload sequence:
//   1. Quiesce the scheduler - wait for EVERY in-flight job, including ones
//      queued in worker deques.
//   2. Drop every reference into the module: its systems leave the pipeline.
//      (Modules create no GPU resources, so there is nothing to release - §3.5.)
//   3. Unload the old library.
//   4. Load the new one from a fresh temp copy (Windows can't overwrite a
//      loaded DLL; Linux/macOS would otherwise hand back the cached image).
//   5. Validate the manifest and re-register systems with their access
//      declarations.
//   6. Resume.
// World state is untouched: the host owns every component type (§3.2).
// If the new build fails to load, the previous build is reloaded, so a bad
// compile never leaves the game without its systems.
#include <cstdint>
#include <string>
#include <vector>
#include "core/platform/platform.h"
#include "jobs/scheduler.h"
#include "jobs/system_graph.h"
#include "modules/host_api.h"

namespace engine {

class ModuleHost {
public:
    ModuleHost(World& world, Scheduler& scheduler, SystemPipeline& pipeline);
    ~ModuleHost();
    ModuleHost(const ModuleHost&) = delete;
    ModuleHost& operator=(const ModuleHost&) = delete;

    // Loads the library at `path` and registers its systems. `path` is the
    // build output being watched.
    bool Load(const std::string& path, std::string* error = nullptr);
    // Unconditional reload of the watched path.
    bool Reload(std::string* error = nullptr);
    // Reloads if the watched file changed and has been stable for `settle_ms`
    // (so a linker still writing it isn't loaded half-written).
    bool PollAndReload(int settle_ms = 100, std::string* error = nullptr);
    void Unload();

    bool Loaded() const { return lib_ != nullptr; }
    const std::string& ModuleName() const { return name_; }
    uint32_t ModuleVersion() const { return version_; }
    uint32_t Owner() const { return owner_; }
    uint32_t ReloadCount() const { return reloads_; }
    double   LastReloadMs() const { return last_reload_ms_; }
    std::vector<std::string> SystemNames() const;

    // For callers that dispatch module code themselves (tests, tools): look up
    // an entry point of the CURRENT build. Never cache it across a reload.
    EngineSystemFn Entry(const std::string& system_name) const;
    const EngineHostApi* Api() const { return &api_; }

private:
    bool LoadInternal(const std::string& source, std::string* error);
    void UnloadInternal();

    World& world_;
    Scheduler& scheduler_;
    SystemPipeline& pipeline_;
    EngineHostApi api_{};

    std::string watched_path_;
    std::string loaded_copy_;
    int64_t watched_mtime_ = 0;
    int64_t pending_mtime_ = 0;
    int64_t pending_since_ns_ = 0;

    platform::LibraryHandle lib_ = nullptr;
    std::string name_;
    uint32_t version_ = 0;
    uint32_t owner_ = 0;
    uint32_t next_owner_ = 1000;
    uint32_t copy_counter_ = 0;
    uint32_t reloads_ = 0;
    double last_reload_ms_ = 0;
    struct Entry_ { std::string name; EngineSystemFn fn; };
    std::vector<Entry_> entries_;
};

}  // namespace engine
