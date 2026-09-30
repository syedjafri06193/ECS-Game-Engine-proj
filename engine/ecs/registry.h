#pragma once
// Component registry: maps a component's NAME to a small dense id. The
// host owns every registration; modules resolve ids through the world, so
// no module code holds type identity in a static (§3.3).
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "core/reflection/reflection.h"
#include "ecs/entity.h"

namespace engine {

struct ComponentInfo {
    ComponentId   id;
    ComponentDesc desc;        // points into registry-owned storage
    bool          sparse;      // stored in a sparse set, not in archetypes (§4.1 hybrid)
    bool IsTag() const { return desc.size == 0; }
};

class ComponentRegistry {
public:
    // Registers a descriptor (copied). Re-registering the same name with the
    // same layout returns the existing id; a different layout is an error -
    // changing a layout goes through snapshot migration (ecs/snapshot.h).
    ComponentId Register(const ComponentDesc& desc, bool sparse = false);

    ComponentId Find(uint64_t name_hash) const
    {
        auto it = by_hash_.find(name_hash);
        return it == by_hash_.end() ? kInvalidComponent : it->second;
    }
    ComponentId Find(const char* name) const { return Find(HashName(name)); }

    template <Component T>
    ComponentId Id()
    {
        using U = std::remove_const_t<T>;
        ComponentId id = Find(ComponentTraits<U>::kHash);
        return id != kInvalidComponent ? id : Register(ComponentTraits<U>::Desc());
    }

    const ComponentInfo& Info(ComponentId id) const { return infos_[id]; }
    size_t Count() const { return infos_.size(); }

private:
    struct Owned {
        std::string name;
        std::vector<std::string> field_names;
        std::vector<FieldDesc> fields;
        std::vector<std::byte> defaults;
    };
    std::vector<ComponentInfo> infos_;
    std::deque<Owned> owned_;
    std::unordered_map<uint64_t, ComponentId> by_hash_;
};

}  // namespace engine
