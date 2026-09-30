#pragma once
// Archetype chunk storage (design doc §4.2, §13.1). Entities with identical
// component sets share 16 KB chunks laid out structure-of-arrays: the entity
// column, then every Position, then every Velocity... A system that reads
// only Position touches only Position's cache lines.
#include <array>
#include <span>
#include <vector>
#include "ecs/entity.h"
#include "ecs/registry.h"

namespace engine {

constexpr size_t kChunkSize = 16 * 1024;     // total block, header included
class Archetype;

struct alignas(kCacheLine) Chunk {
    Archetype* archetype = nullptr;
    uint32_t   count = 0;
    uint32_t   capacity = 0;
    uint32_t   index = 0;                    // position in archetype->chunks
    alignas(kCacheLine) std::byte data[kChunkSize - kCacheLine];
};
static_assert(sizeof(Chunk) == kChunkSize, "chunk must be exactly one block");
constexpr size_t kChunkDataSize = sizeof(Chunk::data);

class Archetype {
public:
    Archetype(uint32_t id, const ComponentMask& mask, const ComponentRegistry& reg);

    uint32_t Id() const { return id_; }
    const ComponentMask& Mask() const { return mask_; }
    std::span<const ComponentId> Types() const { return types_; }
    uint32_t ChunkCapacity() const { return capacity_; }
    uint32_t EntityCount() const { return entity_count_; }
    std::span<Chunk* const> Chunks() const { return chunks_; }

    bool Has(ComponentId c) const { return column_of_[c] != -1; }        // tags are -2: present, no column
    bool HasColumn(ComponentId c) const { return column_of_[c] >= 0; }
    int  ColumnIndex(ComponentId c) const { return column_of_[c]; }
    uint32_t ColumnSize(int col) const { return sizes_[size_t(col)]; }

    Entity* Entities(Chunk* c) const { return reinterpret_cast<Entity*>(c->data); }
    std::byte* Column(Chunk* c, int col) const { return c->data + offsets_[size_t(col)]; }
    std::byte* Element(Chunk* c, int col, uint32_t row) const { return Column(c, col) + size_t(row) * sizes_[size_t(col)]; }

    template <typename T> T* Array(Chunk* c, ComponentId id) const
    {
        return reinterpret_cast<T*>(Column(c, column_of_[id]));
    }

    // Graph edges: cached target archetype for adding/removing a component.
    Archetype* add_edge[kMaxComponents] = {};
    Archetype* remove_edge[kMaxComponents] = {};

private:
    friend class World;
    uint32_t id_;
    ComponentMask mask_;
    std::vector<ComponentId> types_;            // sorted by id: order-independent identity (§13.1)
    std::vector<uint32_t> sizes_, offsets_;
    std::array<int16_t, kMaxComponents> column_of_;
    uint32_t capacity_ = 0;
    uint32_t entity_count_ = 0;
    std::vector<Chunk*> chunks_;
};

// Read-only window onto one chunk, handed to per-chunk jobs.
struct ChunkView {
    Archetype* archetype;
    Chunk*     chunk;

    uint32_t Count() const { return chunk->count; }
    std::span<const Entity> Entities() const { return {archetype->Entities(chunk), chunk->count}; }
    template <typename T> std::span<T> Column(ComponentId id) const
    {
        return {archetype->Array<T>(chunk, id), chunk->count};
    }
};

}  // namespace engine
