#pragma once
/* The ABI a reloadable module sees (design doc §3.3, §11).
 *
 * Plain C: stable across compilers and immune to C++ ABI drift between the
 * host and a module built later. A module is a STATELESS FUNCTION LIBRARY:
 *
 *   - it exports EngineModuleManifest() and its system entry points, nothing else
 *   - it defines no component types (they live in engine/ecs/components.h)
 *   - it has no globals, statics or function-local statics
 *   - it never hands the host a pointer into itself that outlives the call
 *   - it creates no GPU resources
 *
 * tools/module_verify.py rejects modules that break these rules at build
 * time; docs/module-contract.md explains each one.
 */
#include <stdint.h>

#define ENGINE_MODULE_ABI_VERSION 1u

#if defined(_WIN32)
#define ENGINE_MODULE_EXPORT extern "C" __declspec(dllexport)
#else
#define ENGINE_MODULE_EXPORT extern "C" __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque view of one archetype chunk. */
typedef struct EngineChunk { void* archetype; void* chunk; } EngineChunk;

typedef struct EngineHostApi {
    uint32_t abi_version;
    void*    world;
    /* Resolve a component id by registered name ("game::Position"). */
    uint16_t (*component_id)(void* world, const char* name);
    /* Chunk access. */
    uint32_t        (*chunk_count)(const EngineChunk* c);
    void*           (*chunk_column)(const EngineChunk* c, uint16_t component);   /* null if absent */
    const uint64_t* (*chunk_entities)(const EngineChunk* c);
    /* Deferred structural changes into the job's command buffer. */
    void (*cmd_destroy)(void* commands, uint64_t entity);
    void (*cmd_add)(void* commands, uint64_t entity, uint16_t component, const void* data, uint32_t size);
    void (*cmd_remove)(void* commands, uint64_t entity, uint16_t component);
    void (*log)(const char* message);
} EngineHostApi;

/* One system entry point: called per chunk batch, possibly in parallel. */
typedef void (*EngineSystemFn)(const EngineHostApi* api, const EngineChunk* chunk, float dt, void* commands);

typedef struct EngineSystemDesc {
    const char*        name;
    const char* const* reads;        /* null-terminated component names */
    const char* const* writes;       /* null-terminated; written components are also read */
    uint32_t           structural;   /* records commands -> sync point after its stage */
    const char*        entry;        /* exported symbol name of an EngineSystemFn */
    uint32_t           min_batch_entities;
} EngineSystemDesc;

typedef struct EngineManifest {
    uint32_t                abi_version;
    const char*             name;
    uint32_t                version;
    uint32_t                system_count;
    const EngineSystemDesc* systems;
} EngineManifest;

typedef const EngineManifest* (*EngineManifestFn)(void);

#ifdef __cplusplus
}
#endif
