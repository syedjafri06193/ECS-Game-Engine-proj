#pragma once
// The World: entity table, archetypes, sparse sets and cached queries.
//
// Structural changes (create/destroy/add/remove) move memory other jobs may
// be iterating, so they are forbidden while a system stage runs (the
// scheduler holds a StructureLock) and go through CommandBuffer instead
// (design doc §5.3). Violations assert rather than corrupt.
#include <atomic>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>
#include "core/memory/arena.h"
#include "ecs/archetype.h"
#include "ecs/registry.h"
#include "ecs/sparse_set.h"

namespace engine {

class CommandBuffer;
template <Component... Ts> class QueryView;

struct CachedQuery {
    ComponentMask include, exclude;
    std::vector<Archetype*> matched;
    uint64_t archetype_generation = ~0ull;
};

class World {
public:
    World();
    ~World();
    World(const World&) = delete;
    World& operator=(const World&) = delete;

    // ---------------------------------------------------------- entities
    Entity Create();
    template <Component... Ts> Entity Create(const Ts&... values);
    // Recreates an entity with a specific handle (snapshot restore).
    Entity CreateAt(Entity handle);
    bool   Destroy(Entity e);
    bool   IsAlive(Entity e) const;
    size_t EntityCount() const { return alive_; }

    // ---------------------------------------------------------- archetype components
    template <Component T> T*   Add(Entity e, const T& value = T{});
    template <Component T> bool Remove(Entity e);
    template <Component T> bool Has(Entity e) const;
    template <Component T> T*   Get(Entity e);

    void* AddRaw(Entity e, ComponentId id, const void* value);
    bool  RemoveRaw(Entity e, ComponentId id);
    void* GetRaw(Entity e, ComponentId id);
    bool  HasRaw(Entity e, ComponentId id) const;

    // ---------------------------------------------------------- sparse components (hybrid storage)
    template <Component T> ComponentId RegisterSparse() { return RegisterSparseDesc(ComponentTraits<T>::Desc()); }
    ComponentId RegisterSparseDesc(const ComponentDesc& d);
    template <Component T> T*   AddSparse(Entity e, const T& value = T{});
    template <Component T> bool RemoveSparse(Entity e) { return RemoveSparseRaw(e, registry_.Find(ComponentTraits<T>::kHash)); }
    template <Component T> bool HasSparse(Entity e) const { return HasSparseRaw(e, registry_.Find(ComponentTraits<T>::kHash)); }
    template <Component T> T*   GetSparse(Entity e) { return static_cast<T*>(GetSparseRaw(e, registry_.Find(ComponentTraits<T>::kHash))); }
    bool  RemoveSparseRaw(Entity e, ComponentId id);
    bool  HasSparseRaw(Entity e, ComponentId id) const;
    void* GetSparseRaw(Entity e, ComponentId id);
    SparseSet* Sparse(ComponentId id);

    // ---------------------------------------------------------- queries
    template <Component... Ts> QueryView<Ts...> Query();
    CachedQuery& Cached(const ComponentMask& include, const ComponentMask& exclude);

    // ---------------------------------------------------------- deferred structural change
    // Applies buffers in the order given - callers pass them in a
    // deterministic order (by job index), never completion order.
    size_t ApplyCommandBuffers(std::span<CommandBuffer> buffers);
    size_t ApplyCommandBuffers(std::span<CommandBuffer* const> buffers);

    // ---------------------------------------------------------- introspection
    template <Component T> ComponentId Id()
    {
        using U = std::remove_const_t<T>;
        ComponentId id = registry_.Find(ComponentTraits<U>::kHash);
        if (id != kInvalidComponent) return id;
        // Registration mutates the registry; it must not race with jobs.
        ENGINE_ASSERT(!StructureLocked(), "component first used inside a parallel stage - register it at startup");
        return registry_.Register(ComponentTraits<U>::Desc());
    }
    template <Component... Ts> void Register() { (Id<Ts>(), ...); }
    ComponentRegistry& Registry() { return registry_; }
    const ComponentRegistry& Registry() const { return registry_; }
    Archetype* ArchetypeOf(Entity e) const;
    const std::vector<std::unique_ptr<Archetype>>& Archetypes() const { return archetypes_; }
    uint64_t ArchetypeGeneration() const { return archetype_generation_; }
    size_t ChunkCount() const { return chunk_pool_.Live(); }

    // Full internal consistency check (design doc §14.2). O(entities).
    bool Validate(std::string* error = nullptr) const;

    // While locked, structural changes assert. Held by the scheduler around
    // parallel stages.
    void LockStructure() { structure_locks_.fetch_add(1, std::memory_order_acq_rel); }
    void UnlockStructure() { structure_locks_.fetch_sub(1, std::memory_order_acq_rel); }
    bool StructureLocked() const { return structure_locks_.load(std::memory_order_acquire) != 0; }

    struct Stats { uint64_t archetype_moves = 0, entities_created = 0, entities_destroyed = 0; };
    const Stats& GetStats() const { return stats_; }

private:
    struct Record {
        uint32_t   generation = 1;
        bool       alive = false;
        Archetype* archetype = nullptr;
        Chunk*     chunk = nullptr;
        uint32_t   row = 0;
    };

    Entity     AllocateHandle();
    Archetype* FindOrCreateArchetype(const ComponentMask& mask);
    Archetype* WithComponent(Archetype* from, ComponentId id);
    Archetype* WithoutComponent(Archetype* from, ComponentId id);
    void       PlaceInArchetype(Entity e, Record& r, Archetype* a);
    void       MoveToArchetype(Entity e, Record& r, Archetype* to);
    void       RemoveRow(Archetype* a, Chunk* c, uint32_t row);
    Chunk*     ChunkWithSpace(Archetype* a);
    void       AssertMutable() const
    {
        ENGINE_ASSERT(!StructureLocked(),
                      "structural change during a parallel stage - record it in a CommandBuffer (§5.3)");
    }
    Record*    Live(Entity e);
    const Record* Live(Entity e) const;

    ComponentRegistry registry_;
    BlockPool chunk_pool_{sizeof(Chunk), alignof(Chunk), 32};
    std::vector<Record> records_;
    std::vector<uint32_t> free_indices_;
    size_t alive_ = 0;

    std::vector<std::unique_ptr<Archetype>> archetypes_;
    std::unordered_map<size_t, std::vector<Archetype*>> archetype_by_hash_;
    Archetype* empty_archetype_ = nullptr;
    uint64_t archetype_generation_ = 0;

    std::vector<std::unique_ptr<SparseSet>> sparse_;   // indexed by ComponentId, null if not sparse

    struct QueryKey {
        ComponentMask include, exclude;
        bool operator==(const QueryKey&) const = default;
    };
    struct QueryKeyHash { size_t operator()(const QueryKey& k) const { return k.include.Hash() * 31 ^ k.exclude.Hash(); } };
    std::mutex query_mutex_;
    std::unordered_map<QueryKey, std::unique_ptr<CachedQuery>, QueryKeyHash> queries_;

    std::atomic<int> structure_locks_{0};
    Stats stats_;
};

// RAII helper used by the scheduler.
class StructureLock {
public:
    explicit StructureLock(World& w) : w_(w) { w_.LockStructure(); }
    ~StructureLock() { w_.UnlockStructure(); }
    StructureLock(const StructureLock&) = delete;
    StructureLock& operator=(const StructureLock&) = delete;
private:
    World& w_;
};

// ====================================================================== templates

template <Component... Ts>
Entity World::Create(const Ts&... values)
{
    AssertMutable();
    ComponentMask mask;
    (mask.Set(Id<Ts>()), ...);
    Entity e = AllocateHandle();
    Record& r = records_[e.index];
    PlaceInArchetype(e, r, FindOrCreateArchetype(mask));
    auto put = [&](ComponentId id, const void* v, size_t n) {
        if (n) std::memcpy(GetRaw(e, id), v, n);
    };
    (put(Id<Ts>(), &values, ComponentTraits<Ts>::Desc().size), ...);
    return e;
}

template <Component T>
T* World::Add(Entity e, const T& value)
{
    ENGINE_ASSERT(!registry_.Info(Id<T>()).sparse, "sparse component: use AddSparse");
    return static_cast<T*>(AddRaw(e, Id<T>(), &value));
}

template <Component T>
bool World::Remove(Entity e) { return RemoveRaw(e, Id<T>()); }

template <Component T>
bool World::Has(Entity e) const
{
    ComponentId id = registry_.Find(ComponentTraits<T>::kHash);
    return id != kInvalidComponent && HasRaw(e, id);
}

template <Component T>
T* World::Get(Entity e) { return static_cast<T*>(GetRaw(e, Id<T>())); }

template <Component T>
T* World::AddSparse(Entity e, const T& value)
{
    AssertMutable();
    ComponentId id = registry_.Find(ComponentTraits<T>::kHash);
    if (id == kInvalidComponent || id >= sparse_.size() || !sparse_[id]) id = RegisterSparse<T>();
    if (!IsAlive(e)) return nullptr;
    sparse_[id]->Insert(e, &value);
    return static_cast<T*>(sparse_[id]->Get(e));
}

}  // namespace engine

#include "ecs/query.h"
