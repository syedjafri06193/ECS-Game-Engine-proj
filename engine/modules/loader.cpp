#include "modules/loader.h"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include "ecs/command_buffer.h"

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace engine {

namespace {

static_assert(sizeof(Entity) == sizeof(uint64_t), "entity column is read as uint64 by modules");

uint16_t ApiComponentId(void* world, const char* name)
{
    return static_cast<World*>(world)->Registry().Find(name);
}
uint32_t ApiChunkCount(const EngineChunk* c) { return static_cast<Chunk*>(c->chunk)->count; }
void* ApiChunkColumn(const EngineChunk* c, uint16_t id)
{
    auto* a = static_cast<Archetype*>(c->archetype);
    if (id == kInvalidComponent || !a->HasColumn(id)) return nullptr;
    return a->Column(static_cast<Chunk*>(c->chunk), a->ColumnIndex(id));
}
const uint64_t* ApiChunkEntities(const EngineChunk* c)
{
    auto* a = static_cast<Archetype*>(c->archetype);
    return reinterpret_cast<const uint64_t*>(a->Entities(static_cast<Chunk*>(c->chunk)));
}
void ApiDestroy(void* cmds, uint64_t e) { static_cast<CommandBuffer*>(cmds)->DestroyEntity(Entity::FromBits(e)); }
void ApiAdd(void* cmds, uint64_t e, uint16_t id, const void* data, uint32_t size)
{
    static_cast<CommandBuffer*>(cmds)->AddRaw(Entity::FromBits(e), id, data, size);
}
void ApiRemove(void* cmds, uint64_t e, uint16_t id) { static_cast<CommandBuffer*>(cmds)->RemoveRaw(Entity::FromBits(e), id); }
void ApiLog(const char* msg) { std::fprintf(stdout, "[module] %s\n", msg); }

int64_t NowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void RemoveFile(const std::string& p)
{
    if (p.empty()) return;
    std::error_code ec;
    std::filesystem::remove(p, ec);
}

int ProcessId()
{
#if defined(_WIN32)
    return 0;
#else
    return int(::getpid());
#endif
}

}  // namespace

ModuleHost::ModuleHost(World& world, Scheduler& scheduler, SystemPipeline& pipeline)
    : world_(world), scheduler_(scheduler), pipeline_(pipeline)
{
    api_.abi_version = ENGINE_MODULE_ABI_VERSION;
    api_.world = &world_;
    api_.component_id = ApiComponentId;
    api_.chunk_count = ApiChunkCount;
    api_.chunk_column = ApiChunkColumn;
    api_.chunk_entities = ApiChunkEntities;
    api_.cmd_destroy = ApiDestroy;
    api_.cmd_add = ApiAdd;
    api_.cmd_remove = ApiRemove;
    api_.log = ApiLog;
}

ModuleHost::~ModuleHost()
{
    if (lib_) Unload();
}

bool ModuleHost::Load(const std::string& path, std::string* error)
{
    ENGINE_ASSERT(!lib_, "a module is already loaded; use Reload");
    watched_path_ = path;
    watched_mtime_ = platform::FileModifiedNs(path);
    return LoadInternal(path, error);
}

bool ModuleHost::LoadInternal(const std::string& source, std::string* error)
{
    auto fail = [&](const std::string& msg) {
        if (error) *error = msg;
        std::fprintf(stderr, "[module] load failed: %s\n", msg.c_str());
        if (lib_) { platform::UnloadLibrary(lib_); lib_ = nullptr; }
        return false;
    };

    // Load from a private copy (see header).
    const std::string copy = platform::TempDirectory() + "/engine_module_" + std::to_string(ProcessId()) + "_" +
                             std::to_string(++copy_counter_) + platform::SharedLibraryExtension();
    if (!platform::CopyFile(source, copy)) return fail("cannot copy " + source);

    std::string dl_error;
    lib_ = platform::LoadLibrary(copy, &dl_error);
    if (!lib_) { RemoveFile(copy); return fail(dl_error); }

    auto manifest_fn = platform::GetSymbol<EngineManifestFn>(lib_, "EngineModuleManifest");
    if (!manifest_fn) { RemoveFile(copy); return fail("no EngineModuleManifest export"); }
    const EngineManifest* m = manifest_fn();
    if (!m || m->abi_version != ENGINE_MODULE_ABI_VERSION) {
        RemoveFile(copy);
        return fail("ABI mismatch: module " + std::to_string(m ? m->abi_version : 0) + ", host " +
                    std::to_string(ENGINE_MODULE_ABI_VERSION));
    }

    // Validate everything before registering anything.
    const uint32_t owner = ++next_owner_;
    std::vector<System> systems;
    std::vector<Entry_> entries;
    for (uint32_t i = 0; i < m->system_count; ++i) {
        const EngineSystemDesc& d = m->systems[i];
        auto fn = platform::GetSymbol<EngineSystemFn>(lib_, d.entry);
        if (!fn) { RemoveFile(copy); return fail(std::string("missing entry point ") + d.entry); }
        System s;
        s.name = std::string(m->name) + "::" + d.name;
        s.owner = owner;
        s.access.structural = d.structural != 0;
        if (d.min_batch_entities) s.min_batch_entities = d.min_batch_entities;
        for (int pass = 0; pass < 2; ++pass) {
            const char* const* names = pass == 0 ? d.reads : d.writes;
            for (; names && *names; ++names) {
                const ComponentId id = world_.Registry().Find(*names);
                if (id == kInvalidComponent) {
                    RemoveFile(copy);
                    return fail("system " + s.name + " uses component '" + *names +
                                "' the host does not define (modules may not define components)");
                }
                s.access.reads.Set(id);
                if (pass == 1) s.access.writes.Set(id);
                s.include.Set(id);
            }
        }
        const EngineHostApi* api = &api_;
        s.chunk_fn = [fn, api](const ChunkView& v, SystemContext& ctx) {
            EngineChunk c{v.archetype, v.chunk};
            fn(api, &c, ctx.dt, &ctx.commands);
        };
        entries.push_back({d.name, fn});
        systems.push_back(std::move(s));
    }

    for (auto& s : systems) pipeline_.Add(std::move(s));
    entries_ = std::move(entries);
    owner_ = owner;
    name_ = m->name;
    version_ = m->version;
    loaded_copy_ = copy;
    return true;
}

void ModuleHost::UnloadInternal()
{
    pipeline_.RemoveOwnedBy(owner_);       // drop every reference into the module first
    entries_.clear();
    if (lib_) platform::UnloadLibrary(lib_);
    lib_ = nullptr;
}

bool ModuleHost::Reload(std::string* error)
{
    ENGINE_ZONE("ModuleHost::Reload");
    const auto t0 = std::chrono::steady_clock::now();

    scheduler_.Quiesce();                  // 1. nothing in flight can be inside the module
    const std::string previous = loaded_copy_;
    UnloadInternal();                      // 2-3. unregister, unload

    std::string err;
    bool ok = LoadInternal(watched_path_, &err);   // 4-5. fresh copy, validate, register
    if (!ok) {
        // Roll back to the build that was running (its private copy is still on disk).
        std::string rollback_err;
        if (previous.empty() || !LoadInternal(previous, &rollback_err))
            std::fprintf(stderr, "[module] rollback failed: %s\n", rollback_err.c_str());
        if (error) *error = err;
    } else {
        ++reloads_;
    }
    RemoveFile(previous);                  // the running build now has its own copy
    scheduler_.Resume();                   // 6.

    last_reload_ms_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return ok;
}

bool ModuleHost::PollAndReload(int settle_ms, std::string* error)
{
    const int64_t m = platform::FileModifiedNs(watched_path_);
    if (m == 0 || m == watched_mtime_) return false;
    if (m != pending_mtime_) {             // still being written? wait for it to settle
        pending_mtime_ = m;
        pending_since_ns_ = NowNs();
        return false;
    }
    if (NowNs() - pending_since_ns_ < int64_t(settle_ms) * 1000000) return false;
    watched_mtime_ = m;
    return Reload(error);
}

void ModuleHost::Unload()
{
    if (!lib_) return;
    scheduler_.Quiesce();
    UnloadInternal();
    RemoveFile(loaded_copy_);
    loaded_copy_.clear();
    scheduler_.Resume();
}

std::vector<std::string> ModuleHost::SystemNames() const
{
    std::vector<std::string> out;
    for (const auto& e : entries_) out.push_back(e.name);
    return out;
}

EngineSystemFn ModuleHost::Entry(const std::string& system_name) const
{
    for (const auto& e : entries_)
        if (e.name == system_name) return e.fn;
    return nullptr;
}

}  // namespace engine
