#pragma once
// Arenas and pools: no general-purpose allocation in hot paths.
#include <cstring>
#include <memory>
#include <new>
#include <vector>
#include "core/base.h"

namespace engine {

// Bump allocator. Reset frees everything at once - the per-frame upload arena
// pattern (§6.3): no per-allocation lifetime tracking.
class LinearArena {
public:
    explicit LinearArena(size_t capacity)
        : buffer_(static_cast<std::byte*>(::operator new(capacity, std::align_val_t{kCacheLine}))),
          capacity_(capacity) {}
    ~LinearArena() { ::operator delete(buffer_, std::align_val_t{kCacheLine}); }
    LinearArena(const LinearArena&) = delete;
    LinearArena& operator=(const LinearArena&) = delete;

    void* Allocate(size_t size, size_t align = alignof(std::max_align_t))
    {
        const size_t start = AlignUp(offset_, align);
        if (start + size > capacity_) return nullptr;
        offset_ = start + size;
        high_water_ = offset_ > high_water_ ? offset_ : high_water_;
        return buffer_ + start;
    }

    template <typename T, typename... Args>
    T* New(Args&&... args)
    {
        static_assert(std::is_trivially_destructible_v<T>, "arena objects are never destroyed");
        void* p = Allocate(sizeof(T), alignof(T));
        return p ? new (p) T(std::forward<Args>(args)...) : nullptr;
    }

    void   Reset() { offset_ = 0; }
    size_t Used() const { return offset_; }
    size_t Capacity() const { return capacity_; }
    size_t HighWater() const { return high_water_; }
    bool   Owns(const void* p) const
    {
        auto* b = static_cast<const std::byte*>(p);
        return b >= buffer_ && b < buffer_ + capacity_;
    }

private:
    std::byte* buffer_;
    size_t     capacity_;
    size_t     offset_ = 0;
    size_t     high_water_ = 0;
};

// Fixed-size block pool with an intrusive free list. Blocks come from large
// pages that are never returned until the pool dies - stable addresses.
class BlockPool {
public:
    BlockPool(size_t block_size, size_t block_align, size_t blocks_per_page = 64)
        : block_size_(AlignUp(block_size < sizeof(void*) ? sizeof(void*) : block_size, block_align)),
          block_align_(block_align < alignof(void*) ? alignof(void*) : block_align),
          per_page_(blocks_per_page) {}
    ~BlockPool()
    {
        for (void* p : pages_) ::operator delete(p, std::align_val_t{block_align_});
    }
    BlockPool(const BlockPool&) = delete;
    BlockPool& operator=(const BlockPool&) = delete;

    void* Allocate()
    {
        if (!free_) Grow();
        void* b = free_;
        std::memcpy(&free_, b, sizeof(void*));
        ++live_;
        return b;
    }
    void Free(void* b)
    {
        std::memcpy(b, &free_, sizeof(void*));
        free_ = b;
        --live_;
    }
    size_t Live() const { return live_; }
    size_t BlockSize() const { return block_size_; }

private:
    void Grow()
    {
        auto* page = static_cast<std::byte*>(::operator new(block_size_ * per_page_, std::align_val_t{block_align_}));
        pages_.push_back(page);
        for (size_t i = per_page_; i-- > 0;) {
            void* b = page + i * block_size_;
            std::memcpy(b, &free_, sizeof(void*));
            free_ = b;
        }
    }
    size_t block_size_, block_align_, per_page_;
    void* free_ = nullptr;
    size_t live_ = 0;
    std::vector<void*> pages_;
};

}  // namespace engine
