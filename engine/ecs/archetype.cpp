#include "ecs/archetype.h"
#include <algorithm>

namespace engine {

Archetype::Archetype(uint32_t id, const ComponentMask& mask, const ComponentRegistry& reg)
    : id_(id), mask_(mask)
{
    column_of_.fill(-1);
    mask.ForEach([&](ComponentId c) { types_.push_back(c); });   // ascending id order
    std::sort(types_.begin(), types_.end());

    // Only non-tag components get a column; tags live in the mask alone.
    std::vector<ComponentId> stored;
    for (ComponentId c : types_)
        if (!reg.Info(c).IsTag()) stored.push_back(c);

    size_t stride = sizeof(Entity), worst_padding = 0;
    for (ComponentId c : stored) {
        stride += reg.Info(c).desc.size;
        worst_padding += reg.Info(c).desc.align;
    }
    capacity_ = uint32_t((kChunkDataSize - worst_padding) / stride);
    ENGINE_ASSERT(capacity_ >= 1, "component set too large for one chunk");

    // SoA: entity column first, then each component column, aligned.
    size_t off = AlignUp(size_t(capacity_) * sizeof(Entity), kCacheLine);
    for (ComponentId c : stored) {
        const auto& d = reg.Info(c).desc;
        off = AlignUp(off, std::max<size_t>(d.align, 16));
        column_of_[c] = int16_t(sizes_.size());
        sizes_.push_back(d.size);
        offsets_.push_back(uint32_t(off));
        off += size_t(d.size) * capacity_;
    }
    // Shrink capacity if generous alignment pushed us over (rare).
    while (off > kChunkDataSize) {
        --capacity_;
        off = AlignUp(size_t(capacity_) * sizeof(Entity), kCacheLine);
        for (size_t i = 0; i < stored.size(); ++i) {
            const auto& d = reg.Info(stored[i]).desc;
            off = AlignUp(off, std::max<size_t>(d.align, 16));
            offsets_[i] = uint32_t(off);
            off += size_t(d.size) * capacity_;
        }
    }
    // Tags: mark present with a sentinel column that has no storage.
    for (ComponentId c : types_)
        if (reg.Info(c).IsTag()) column_of_[c] = int16_t(-2);
}

}  // namespace engine
