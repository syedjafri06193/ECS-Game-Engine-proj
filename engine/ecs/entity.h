#pragma once
#include <array>
#include <bit>
#include <cstdint>
#include <functional>
#include "core/base.h"

namespace engine {

// Generational handle (§4.3). Destroy bumps the slot's generation, so a stale
// handle to a reused slot is rejected instead of silently aliasing.
struct Entity {
    uint32_t index = 0;
    uint32_t generation = 0;   // 0 = null

    constexpr bool operator==(const Entity&) const = default;
    constexpr explicit operator bool() const { return generation != 0; }
    constexpr uint64_t Bits() const { return (uint64_t(generation) << 32) | index; }
    static constexpr Entity FromBits(uint64_t b) { return {uint32_t(b), uint32_t(b >> 32)}; }
};

constexpr Entity kNullEntity{};
// Provisional handles returned by CommandBuffer::CreateEntity (§5.3).
constexpr uint32_t kPlaceholderGeneration = 0xFFFFFFFFu;
constexpr bool IsPlaceholder(Entity e) { return e.generation == kPlaceholderGeneration; }

using ComponentId = uint16_t;
constexpr size_t kMaxComponents = 256;
constexpr ComponentId kInvalidComponent = 0xFFFF;

class ComponentMask {
public:
    static constexpr size_t kWords = kMaxComponents / 64;

    constexpr void Set(ComponentId id) { w_[id >> 6] |= (1ull << (id & 63)); }
    constexpr void Clear(ComponentId id) { w_[id >> 6] &= ~(1ull << (id & 63)); }
    constexpr bool Test(ComponentId id) const { return (w_[id >> 6] >> (id & 63)) & 1ull; }
    constexpr bool Any() const { for (auto w : w_) if (w) return true; return false; }
    constexpr bool None() const { return !Any(); }
    constexpr bool ContainsAll(const ComponentMask& o) const
    {
        for (size_t i = 0; i < kWords; ++i) if ((w_[i] & o.w_[i]) != o.w_[i]) return false;
        return true;
    }
    constexpr bool Intersects(const ComponentMask& o) const
    {
        for (size_t i = 0; i < kWords; ++i) if (w_[i] & o.w_[i]) return true;
        return false;
    }
    // Archetype matches a query: has every included, none excluded.
    constexpr bool Matches(const ComponentMask& include, const ComponentMask& exclude) const
    {
        return ContainsAll(include) && !Intersects(exclude);
    }
    size_t Count() const { size_t n = 0; for (auto w : w_) n += size_t(std::popcount(w)); return n; }

    constexpr ComponentMask operator&(const ComponentMask& o) const { ComponentMask r; for (size_t i = 0; i < kWords; ++i) r.w_[i] = w_[i] & o.w_[i]; return r; }
    constexpr ComponentMask operator|(const ComponentMask& o) const { ComponentMask r; for (size_t i = 0; i < kWords; ++i) r.w_[i] = w_[i] | o.w_[i]; return r; }
    constexpr ComponentMask& operator|=(const ComponentMask& o) { for (size_t i = 0; i < kWords; ++i) w_[i] |= o.w_[i]; return *this; }
    constexpr bool operator==(const ComponentMask&) const = default;

    template <typename Fn> void ForEach(Fn&& fn) const
    {
        for (size_t i = 0; i < kWords; ++i)
            for (uint64_t w = w_[i]; w; w &= w - 1) fn(ComponentId(i * 64 + size_t(std::countr_zero(w))));
    }
    size_t Hash() const
    {
        size_t h = 0;
        for (auto w : w_) h = h * 1099511628211ull ^ std::hash<uint64_t>{}(w);
        return h;
    }

private:
    std::array<uint64_t, kWords> w_{};
};

}  // namespace engine

template <> struct std::hash<engine::Entity> {
    size_t operator()(const engine::Entity& e) const noexcept { return std::hash<uint64_t>{}(e.Bits()); }
};
