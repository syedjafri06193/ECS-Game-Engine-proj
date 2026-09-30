#pragma once
// Deferred structural changes (design doc §5.3). Jobs record into their own
// buffer without locks; the world applies them single-threaded at a sync
// point, in a deterministic buffer order.
//
// CreateEntity returns a placeholder handle usable immediately in the same
// buffer (Add/Remove/Destroy on it); Apply resolves it to a real entity.
#include <cstring>
#include <vector>
#include "ecs/entity.h"
#include "ecs/registry.h"

namespace engine {

class World;

class CommandBuffer {
public:
    enum class Op : uint8_t { Create, Destroy, Add, Remove, AddSparse, RemoveSparse };

    explicit CommandBuffer(World* world = nullptr) : world_(world) {}

    void Bind(World* w) { world_ = w; }

    Entity CreateEntity()
    {
        Entity p{placeholders_++, kPlaceholderGeneration};
        Push(Op::Create, p, kInvalidComponent, nullptr, 0);
        return p;
    }
    void DestroyEntity(Entity e) { Push(Op::Destroy, e, kInvalidComponent, nullptr, 0); }

    template <Component T> void AddComponent(Entity e, const T& v = T{})
    {
        Push(Op::Add, e, ResolveId<T>(), &v, ComponentTraits<T>::Desc().size);
    }
    template <Component T> void RemoveComponent(Entity e) { Push(Op::Remove, e, ResolveId<T>(), nullptr, 0); }
    template <Component T> void AddSparse(Entity e, const T& v = T{})
    {
        Push(Op::AddSparse, e, ResolveId<T>(), &v, ComponentTraits<T>::Desc().size);
    }
    template <Component T> void RemoveSparse(Entity e) { Push(Op::RemoveSparse, e, ResolveId<T>(), nullptr, 0); }

    void AddRaw(Entity e, ComponentId id, const void* v, uint32_t size) { Push(Op::Add, e, id, v, size); }
    void RemoveRaw(Entity e, ComponentId id) { Push(Op::Remove, e, id, nullptr, 0); }
    void AddSparseRaw(Entity e, ComponentId id, const void* v, uint32_t size) { Push(Op::AddSparse, e, id, v, size); }
    void RemoveSparseRaw(Entity e, ComponentId id) { Push(Op::RemoveSparse, e, id, nullptr, 0); }

    bool   Empty() const { return count_ == 0; }
    size_t CommandCount() const { return count_; }
    void   Clear() { bytes_.clear(); count_ = 0; placeholders_ = 0; }

    // Iteration for World::ApplyCommandBuffers.
    struct Command {
        Op op;
        ComponentId component;
        Entity entity;
        const std::byte* payload;
        uint32_t size;
    };
    template <typename Fn> void Visit(Fn&& fn) const
    {
        size_t off = 0;
        while (off < bytes_.size()) {
            Header h;
            std::memcpy(&h, bytes_.data() + off, sizeof h);
            off += sizeof h;
            fn(Command{h.op, h.component, Entity::FromBits(h.entity), bytes_.data() + off, h.size});
            off += AlignUp(h.size, alignof(Header));
        }
    }
    uint32_t PlaceholderCount() const { return placeholders_; }

private:
    struct Header {
        uint64_t entity;
        uint32_t size;
        ComponentId component;
        Op op;
    };

    template <Component T> ComponentId ResolveId();   // defined after World (command_buffer_impl.h)

    void Push(Op op, Entity e, ComponentId c, const void* payload, uint32_t size)
    {
        Header h{e.Bits(), size, c, op};
        const size_t off = bytes_.size();
        bytes_.resize(off + sizeof h + AlignUp(size, alignof(Header)));
        std::memcpy(bytes_.data() + off, &h, sizeof h);
        if (size) std::memcpy(bytes_.data() + off + sizeof h, payload, size);
        ++count_;
    }

    World* world_;
    std::vector<std::byte> bytes_;
    size_t count_ = 0;
    uint32_t placeholders_ = 0;
};

}  // namespace engine

#include "ecs/world.h"

namespace engine {
template <Component T> ComponentId CommandBuffer::ResolveId()
{
    ENGINE_ASSERT(world_ != nullptr, "CommandBuffer not bound to a world");
    // Registry lookups are read-only once components are registered; the
    // scheduler pre-registers every component a system declares.
    ComponentId id = world_->Registry().Find(ComponentTraits<T>::kHash);
    ENGINE_ASSERT(id != kInvalidComponent, "component not registered before recording (register at startup)");
    return id;
}
}  // namespace engine
