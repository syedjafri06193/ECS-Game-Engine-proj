#include "ecs/world.h"
#include <algorithm>
#include "ecs/command_buffer.h"

namespace engine {

namespace {
// Returned by Add/Get for tag components, which have no storage.
alignas(16) std::byte g_tag_storage[16];
}

World::World()
{
    empty_archetype_ = FindOrCreateArchetype(ComponentMask{});
}

World::~World()
{
    for (auto& a : archetypes_)
        for (Chunk* c : a->chunks_) chunk_pool_.Free(c);
}

// ---------------------------------------------------------------- entities

World::Record* World::Live(Entity e)
{
    if (e.index >= records_.size()) return nullptr;
    Record& r = records_[e.index];
    return (r.alive && r.generation == e.generation) ? &r : nullptr;
}
const World::Record* World::Live(Entity e) const { return const_cast<World*>(this)->Live(e); }

bool World::IsAlive(Entity e) const { return Live(e) != nullptr; }

Entity World::AllocateHandle()
{
    uint32_t index;
    for (;;) {
        if (free_indices_.empty()) {
            index = uint32_t(records_.size());
            records_.emplace_back();
            break;
        }
        index = free_indices_.back();
        free_indices_.pop_back();
        if (!records_[index].alive) break;          // skip indices taken by CreateAt
    }
    Record& r = records_[index];
    r.alive = true;
    ++alive_;
    ++stats_.entities_created;
    return {index, r.generation};
}

Entity World::Create()
{
    AssertMutable();
    Entity e = AllocateHandle();
    PlaceInArchetype(e, records_[e.index], empty_archetype_);
    return e;
}

Entity World::CreateAt(Entity handle)
{
    AssertMutable();
    ENGINE_ASSERT(handle.generation != 0 && !IsPlaceholder(handle), "invalid handle");
    if (handle.index >= records_.size()) {
        const uint32_t old = uint32_t(records_.size());
        records_.resize(size_t(handle.index) + 1);
        for (uint32_t i = handle.index; i-- > old;) free_indices_.push_back(i);
    }
    Record& r = records_[handle.index];
    ENGINE_ASSERT(!r.alive, "CreateAt: slot already alive");
    r.alive = true;
    r.generation = handle.generation;
    ++alive_;
    ++stats_.entities_created;
    PlaceInArchetype(handle, r, empty_archetype_);
    return handle;
}

bool World::Destroy(Entity e)
{
    AssertMutable();
    Record* r = Live(e);
    if (!r) return false;
    RemoveRow(r->archetype, r->chunk, r->row);
    for (auto& s : sparse_)
        if (s) s->Remove(e);
    r->alive = false;
    r->archetype = nullptr;
    r->chunk = nullptr;
    if (++r->generation == kPlaceholderGeneration || r->generation == 0) r->generation = 1;
    free_indices_.push_back(e.index);
    --alive_;
    ++stats_.entities_destroyed;
    return true;
}

// ---------------------------------------------------------------- archetypes

Archetype* World::FindOrCreateArchetype(const ComponentMask& mask)
{
    auto& bucket = archetype_by_hash_[mask.Hash()];
    for (Archetype* a : bucket)
        if (a->Mask() == mask) return a;
    archetypes_.push_back(std::make_unique<Archetype>(uint32_t(archetypes_.size()), mask, registry_));
    Archetype* a = archetypes_.back().get();
    bucket.push_back(a);
    ++archetype_generation_;          // invalidates cached queries (§4.4)
    return a;
}

Archetype* World::WithComponent(Archetype* from, ComponentId id)
{
    if (Archetype* a = from->add_edge[id]) return a;
    ComponentMask m = from->Mask();
    m.Set(id);
    Archetype* a = FindOrCreateArchetype(m);
    from->add_edge[id] = a;
    a->remove_edge[id] = from;
    return a;
}

Archetype* World::WithoutComponent(Archetype* from, ComponentId id)
{
    if (Archetype* a = from->remove_edge[id]) return a;
    ComponentMask m = from->Mask();
    m.Clear(id);
    Archetype* a = FindOrCreateArchetype(m);
    from->remove_edge[id] = a;
    a->add_edge[id] = from;
    return a;
}

Chunk* World::ChunkWithSpace(Archetype* a)
{
    if (!a->chunks_.empty() && a->chunks_.back()->count < a->capacity_) return a->chunks_.back();
    Chunk* c = new (chunk_pool_.Allocate()) Chunk;
    c->archetype = a;
    c->capacity = a->capacity_;
    c->index = uint32_t(a->chunks_.size());
    a->chunks_.push_back(c);
    return c;
}

void World::PlaceInArchetype(Entity e, Record& r, Archetype* a)
{
    Chunk* c = ChunkWithSpace(a);
    const uint32_t row = c->count++;
    ++a->entity_count_;
    a->Entities(c)[row] = e;
    r.archetype = a;
    r.chunk = c;
    r.row = row;
}

// Swap-remove against the archetype's LAST row so every chunk but the last
// stays full: iteration never sees holes.
void World::RemoveRow(Archetype* a, Chunk* c, uint32_t row)
{
    Chunk* last = a->chunks_.back();
    const uint32_t last_row = last->count - 1;
    if (c != last || row != last_row) {
        const Entity moved = a->Entities(last)[last_row];
        a->Entities(c)[row] = moved;
        for (size_t col = 0; col < a->sizes_.size(); ++col)
            std::memcpy(a->Element(c, int(col), row), a->Element(last, int(col), last_row), a->sizes_[col]);
        Record& mr = records_[moved.index];
        mr.chunk = c;
        mr.row = row;
    }
    --last->count;
    --a->entity_count_;
    if (last->count == 0) {
        a->chunks_.pop_back();
        chunk_pool_.Free(last);
    }
}

void World::MoveToArchetype(Entity e, Record& r, Archetype* to)
{
    Archetype* from = r.archetype;
    Chunk* old_chunk = r.chunk;
    const uint32_t old_row = r.row;

    PlaceInArchetype(e, r, to);
    for (ComponentId id : to->Types()) {
        if (!to->HasColumn(id) || !from->HasColumn(id)) continue;
        const int tc = to->ColumnIndex(id), fc = from->ColumnIndex(id);
        std::memcpy(to->Element(r.chunk, tc, r.row), from->Element(old_chunk, fc, old_row), to->ColumnSize(tc));
    }
    RemoveRow(from, old_chunk, old_row);
    ++stats_.archetype_moves;
}

Archetype* World::ArchetypeOf(Entity e) const
{
    const Record* r = Live(e);
    return r ? r->archetype : nullptr;
}

// ---------------------------------------------------------------- components

void* World::AddRaw(Entity e, ComponentId id, const void* value)
{
    AssertMutable();
    Record* r = Live(e);
    if (!r) return nullptr;
    const ComponentInfo& info = registry_.Info(id);
    ENGINE_ASSERT(!info.sparse, "sparse component added through the archetype path");
    if (!r->archetype->Has(id)) MoveToArchetype(e, *r, WithComponent(r->archetype, id));
    if (info.IsTag()) return g_tag_storage;
    void* dst = r->archetype->Element(r->chunk, r->archetype->ColumnIndex(id), r->row);
    if (value) std::memcpy(dst, value, info.desc.size);
    else if (info.desc.defaults) std::memcpy(dst, info.desc.defaults, info.desc.size);
    else std::memset(dst, 0, info.desc.size);
    return dst;
}

bool World::RemoveRaw(Entity e, ComponentId id)
{
    AssertMutable();
    Record* r = Live(e);
    if (!r || id == kInvalidComponent || !r->archetype->Has(id)) return false;
    MoveToArchetype(e, *r, WithoutComponent(r->archetype, id));
    return true;
}

void* World::GetRaw(Entity e, ComponentId id)
{
    Record* r = Live(e);
    if (!r || id == kInvalidComponent || !r->archetype->Has(id)) return nullptr;
    if (!r->archetype->HasColumn(id)) return g_tag_storage;
    return r->archetype->Element(r->chunk, r->archetype->ColumnIndex(id), r->row);
}

bool World::HasRaw(Entity e, ComponentId id) const
{
    const Record* r = Live(e);
    return r && id != kInvalidComponent && r->archetype->Has(id);
}

// ---------------------------------------------------------------- sparse

ComponentId World::RegisterSparseDesc(const ComponentDesc& d)
{
    const ComponentId existing = registry_.Find(d.name);
    ENGINE_ASSERT(existing == kInvalidComponent || registry_.Info(existing).sparse,
                  "component already registered as archetype storage");
    const ComponentId id = registry_.Register(d, true);
    if (sparse_.size() <= id) sparse_.resize(size_t(id) + 1);
    if (!sparse_[id]) sparse_[id] = std::make_unique<SparseSet>(id, d.size);
    return id;
}

SparseSet* World::Sparse(ComponentId id)
{
    return id < sparse_.size() ? sparse_[id].get() : nullptr;
}

bool World::RemoveSparseRaw(Entity e, ComponentId id)
{
    AssertMutable();
    SparseSet* s = Sparse(id);
    return s && s->Remove(e);
}

bool World::HasSparseRaw(Entity e, ComponentId id) const
{
    return id < sparse_.size() && sparse_[id] && sparse_[id]->Contains(e);
}

void* World::GetSparseRaw(Entity e, ComponentId id)
{
    SparseSet* s = Sparse(id);
    return s ? s->Get(e) : nullptr;
}

// ---------------------------------------------------------------- queries

CachedQuery& World::Cached(const ComponentMask& include, const ComponentMask& exclude)
{
    std::lock_guard lock(query_mutex_);
    auto& slot = queries_[QueryKey{include, exclude}];
    if (!slot) {
        slot = std::make_unique<CachedQuery>();
        slot->include = include;
        slot->exclude = exclude;
    }
    if (slot->archetype_generation != archetype_generation_) {
        slot->matched.clear();
        for (auto& a : archetypes_)
            if (a->Mask().Matches(include, exclude)) slot->matched.push_back(a.get());
        slot->archetype_generation = archetype_generation_;
    }
    return *slot;
}

// ---------------------------------------------------------------- command buffers

size_t World::ApplyCommandBuffers(std::span<CommandBuffer> buffers)
{
    std::vector<CommandBuffer*> ptrs;
    ptrs.reserve(buffers.size());
    for (auto& b : buffers) ptrs.push_back(&b);
    return ApplyCommandBuffers(std::span<CommandBuffer* const>(ptrs));
}

size_t World::ApplyCommandBuffers(std::span<CommandBuffer* const> buffers)
{
    AssertMutable();
    size_t applied = 0;
    std::vector<Entity> resolved;
    for (CommandBuffer* buf : buffers) {
        resolved.assign(buf->PlaceholderCount(), kNullEntity);
        buf->Visit([&](const CommandBuffer::Command& c) {
            Entity e = c.entity;
            if (IsPlaceholder(e)) {
                if (c.op != CommandBuffer::Op::Create) {
                    ENGINE_ASSERT(e.index < resolved.size(), "placeholder from another command buffer");
                    e = resolved[e.index];
                }
            }
            switch (c.op) {
            case CommandBuffer::Op::Create:
                resolved[e.index] = Create();
                ++applied;
                break;
            case CommandBuffer::Op::Destroy:
                applied += Destroy(e) ? 1 : 0;             // double-destroy across jobs is benign
                break;
            case CommandBuffer::Op::Add:
                applied += AddRaw(e, c.component, c.size ? c.payload : nullptr) ? 1 : 0;
                break;
            case CommandBuffer::Op::Remove:
                applied += RemoveRaw(e, c.component) ? 1 : 0;
                break;
            case CommandBuffer::Op::AddSparse:
                if (IsAlive(e)) {
                    SparseSet* s = Sparse(c.component);
                    ENGINE_ASSERT(s, "AddSparse on a non-sparse component");
                    s->Insert(e, c.payload);
                    ++applied;
                }
                break;
            case CommandBuffer::Op::RemoveSparse:
                applied += RemoveSparseRaw(e, c.component) ? 1 : 0;
                break;
            }
        });
        buf->Clear();
    }
    return applied;
}

// ---------------------------------------------------------------- validation

bool World::Validate(std::string* error) const
{
    auto fail = [&](const std::string& msg) {
        if (error) *error = msg;
        return false;
    };

    size_t alive_records = 0;
    for (uint32_t i = 0; i < records_.size(); ++i) {
        const Record& r = records_[i];
        if (!r.alive) continue;
        ++alive_records;
        if (!r.archetype || !r.chunk) return fail("alive entity " + std::to_string(i) + " has no storage");
        if (r.chunk->archetype != r.archetype) return fail("record chunk belongs to another archetype");
        if (r.row >= r.chunk->count) return fail("record row beyond chunk count");
        const Entity stored = r.archetype->Entities(r.chunk)[r.row];
        if (stored.index != i || stored.generation != r.generation)
            return fail("entity column disagrees with record for index " + std::to_string(i));
    }
    if (alive_records != alive_) return fail("alive count mismatch");

    size_t total = 0;
    for (const auto& a : archetypes_) {
        size_t sum = 0;
        for (size_t ci = 0; ci < a->chunks_.size(); ++ci) {
            const Chunk* c = a->chunks_[ci];
            if (c->archetype != a.get() || c->index != ci) return fail("chunk header corrupt");
            if (c->count == 0) return fail("empty chunk retained");
            if (ci + 1 < a->chunks_.size() && c->count != c->capacity) return fail("hole in non-final chunk");
            for (uint32_t row = 0; row < c->count; ++row) {
                const Entity e = a->Entities(const_cast<Chunk*>(c))[row];
                const Record* r = Live(e);
                if (!r || r->archetype != a.get() || r->chunk != c || r->row != row)
                    return fail("stale or duplicated entity in chunk");
            }
            sum += c->count;
        }
        if (sum != a->entity_count_) return fail("archetype entity count mismatch");
        total += sum;
    }
    if (total != alive_) return fail("entities in chunks != alive entities");

    for (const auto& s : sparse_) {
        if (!s) continue;
        for (Entity e : s->Dense())
            if (!IsAlive(e)) return fail("sparse set holds a dead entity");
    }
    return true;
}

}  // namespace engine
