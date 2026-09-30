#pragma once
// Sparse-set storage for volatile components (design doc §4.1 hybrid).
// Adding or removing a sparse component is one push or one swap-remove; it
// never moves the entity between archetypes, so tag churn costs nothing.
#include <cstring>
#include <vector>
#include "ecs/entity.h"

namespace engine {

class SparseSet {
public:
    SparseSet(ComponentId id, uint32_t elem_size) : id_(id), elem_size_(elem_size) {}

    bool Contains(Entity e) const
    {
        return e.index < sparse_.size() && sparse_[e.index] != 0 && dense_[sparse_[e.index] - 1] == e;
    }
    void* Get(Entity e)
    {
        if (!Contains(e)) return nullptr;
        return elem_size_ ? data_.data() + size_t(sparse_[e.index] - 1) * elem_size_ : static_cast<void*>(this);
    }
    void Insert(Entity e, const void* value)
    {
        if (e.index >= sparse_.size()) sparse_.resize(size_t(e.index) + 1, 0);
        if (Contains(e)) {
            if (elem_size_) std::memcpy(Get(e), value, elem_size_);
            return;
        }
        if (sparse_[e.index] != 0) Remove(dense_[sparse_[e.index] - 1]);   // stale generation
        dense_.push_back(e);
        sparse_[e.index] = uint32_t(dense_.size());
        if (elem_size_) {
            data_.resize(dense_.size() * elem_size_);
            std::memcpy(data_.data() + (dense_.size() - 1) * elem_size_, value, elem_size_);
        }
    }
    bool Remove(Entity e)
    {
        if (e.index >= sparse_.size() || sparse_[e.index] == 0) return false;
        const uint32_t slot = sparse_[e.index] - 1;
        if (dense_[slot].generation != e.generation) return false;
        const uint32_t last = uint32_t(dense_.size() - 1);
        if (slot != last) {
            dense_[slot] = dense_[last];
            sparse_[dense_[slot].index] = slot + 1;
            if (elem_size_)
                std::memcpy(data_.data() + size_t(slot) * elem_size_, data_.data() + size_t(last) * elem_size_, elem_size_);
        }
        dense_.pop_back();
        if (elem_size_) data_.resize(dense_.size() * elem_size_);
        sparse_[e.index] = 0;
        return true;
    }

    ComponentId Id() const { return id_; }
    size_t Size() const { return dense_.size(); }
    const std::vector<Entity>& Dense() const { return dense_; }
    std::byte* Data() { return data_.data(); }
    uint32_t ElementSize() const { return elem_size_; }

private:
    ComponentId id_;
    uint32_t elem_size_;
    std::vector<uint32_t> sparse_;    // entity index -> dense slot + 1
    std::vector<Entity> dense_;
    std::vector<std::byte> data_;
};

}  // namespace engine
