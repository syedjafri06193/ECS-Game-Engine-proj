#pragma once
// Engine-wide basics: assertions and profiler zones.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#if defined(ENGINE_TRACY)
#include <tracy/Tracy.hpp>
#define ENGINE_ZONE(name) ZoneScopedN(name)
#define ENGINE_FRAME_MARK FrameMark
#else
#define ENGINE_ZONE(name) ((void)0)
#define ENGINE_FRAME_MARK ((void)0)
#endif

namespace engine {

[[noreturn]] inline void AssertFail(const char* expr, const char* file, int line, const char* msg)
{
    std::fprintf(stderr, "ASSERT FAILED: %s\n  at %s:%d\n  %s\n", expr, file, line, msg ? msg : "");
    std::fflush(stderr);
    std::abort();
}

}  // namespace engine

// Always on: an ECS that silently corrupts is worse than one that stops.
#define ENGINE_ASSERT(cond, msg) \
    do { if (!(cond)) ::engine::AssertFail(#cond, __FILE__, __LINE__, msg); } while (0)

#ifndef NDEBUG
#define ENGINE_DEBUG_ASSERT(cond, msg) ENGINE_ASSERT(cond, msg)
#else
#define ENGINE_DEBUG_ASSERT(cond, msg) ((void)0)
#endif

namespace engine {

constexpr size_t kCacheLine = 64;

constexpr size_t AlignUp(size_t v, size_t a) { return (v + a - 1) & ~(a - 1); }

// FNV-1a, constexpr: component identity is the hash of its name, so a module
// and the host agree on ids without sharing any static state (§3.3).
constexpr uint64_t HashName(const char* s)
{
    uint64_t h = 0xcbf29ce484222325ull;
    while (*s) { h ^= static_cast<uint8_t>(*s++); h *= 0x100000001b3ull; }
    return h;
}

}  // namespace engine
