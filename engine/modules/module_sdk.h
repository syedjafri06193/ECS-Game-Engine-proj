#pragma once
// Header-only helpers for writing module systems in C++ against the C ABI.
// Everything here is inline and stateless, so including it adds nothing a
// module is forbidden to contain.
#include "core/reflection/reflection.h"
#include "ecs/components.h"
#include "modules/host_api.h"

namespace engine::sdk {

template <typename T>
inline T* Column(const EngineHostApi* api, const EngineChunk* c)
{
    const uint16_t id = api->component_id(api->world, ComponentTraits<std::remove_const_t<T>>::kName);
    return static_cast<T*>(api->chunk_column(c, id));
}

inline uint32_t Count(const EngineHostApi* api, const EngineChunk* c) { return api->chunk_count(c); }

template <typename T>
inline void Add(const EngineHostApi* api, void* cmds, uint64_t e, const T& v)
{
    api->cmd_add(cmds, e, api->component_id(api->world, ComponentTraits<T>::kName), &v,
                 ComponentTraits<T>::Desc().size);
}

template <typename T>
inline void Remove(const EngineHostApi* api, void* cmds, uint64_t e)
{
    api->cmd_remove(cmds, e, api->component_id(api->world, ComponentTraits<T>::kName));
}

}  // namespace engine::sdk
