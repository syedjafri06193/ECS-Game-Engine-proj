#pragma once
// Chase-Lev work-stealing deque (design doc §5.1), after Lê, Pop, Cohen and
// Zappa Nardelli, "Correct and Efficient Work-Stealing for Weak Memory
// Models" (PPoPP 2013). The owner pushes and pops at the bottom (LIFO: the
// newest job's data is most likely still in cache); thieves steal from the
// top (FIFO: older jobs tend to be bigger, so a steal moves more work).
//
// Fixed capacity: Push returns false when full and the caller runs the job
// inline, which is always safe. seq_cst operations are used instead of
// standalone fences so ThreadSanitizer can reason about them.
#include <atomic>
#include <cstdint>
#include <memory>
#include "core/base.h"

namespace engine {

template <typename T>
class WorkStealingDeque {
public:
    explicit WorkStealingDeque(size_t capacity_pow2 = 8192)
        : mask_(int64_t(capacity_pow2) - 1), buffer_(new std::atomic<T*>[capacity_pow2])
    {
        ENGINE_ASSERT((capacity_pow2 & (capacity_pow2 - 1)) == 0, "capacity must be a power of two");
        for (size_t i = 0; i < capacity_pow2; ++i) buffer_[i].store(nullptr, std::memory_order_relaxed);
    }

    // Owner only.
    bool Push(T* item)
    {
        const int64_t b = bottom_.load(std::memory_order_relaxed);
        const int64_t t = top_.load(std::memory_order_acquire);
        if (b - t > mask_) return false;                                   // full
        buffer_[b & mask_].store(item, std::memory_order_relaxed);
        bottom_.store(b + 1, std::memory_order_release);
        return true;
    }

    // Owner only.
    T* Pop()
    {
        const int64_t b = bottom_.load(std::memory_order_relaxed) - 1;
        bottom_.store(b, std::memory_order_seq_cst);
        int64_t t = top_.load(std::memory_order_seq_cst);
        if (t > b) {                                                        // empty
            bottom_.store(b + 1, std::memory_order_relaxed);
            return nullptr;
        }
        T* item = buffer_[b & mask_].load(std::memory_order_relaxed);
        if (t == b) {                                                       // last item: race thieves
            if (!top_.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst, std::memory_order_relaxed))
                item = nullptr;
            bottom_.store(b + 1, std::memory_order_relaxed);
        }
        return item;
    }

    // Any thread.
    T* Steal()
    {
        int64_t t = top_.load(std::memory_order_seq_cst);
        const int64_t b = bottom_.load(std::memory_order_seq_cst);
        if (t >= b) return nullptr;
        T* item = buffer_[t & mask_].load(std::memory_order_relaxed);
        if (!top_.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst, std::memory_order_relaxed))
            return nullptr;                                                 // lost the race; caller retries elsewhere
        return item;
    }

    int64_t SizeApprox() const
    {
        return bottom_.load(std::memory_order_relaxed) - top_.load(std::memory_order_relaxed);
    }

private:
    // Separate cache lines: thieves hammer top, the owner hammers bottom.
    alignas(kCacheLine) std::atomic<int64_t> top_{0};
    alignas(kCacheLine) std::atomic<int64_t> bottom_{0};
    alignas(kCacheLine) int64_t mask_;
    std::unique_ptr<std::atomic<T*>[]> buffer_;
};

}  // namespace engine
