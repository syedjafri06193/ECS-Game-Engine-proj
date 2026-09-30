#include "ecs/registry.h"
#include <cstring>

namespace engine {

static bool SameLayout(const ComponentDesc& a, const ComponentDesc& b)
{
    if (a.size != b.size || a.align != b.align || a.fields.size() != b.fields.size()) return false;
    for (size_t i = 0; i < a.fields.size(); ++i) {
        const auto &x = a.fields[i], &y = b.fields[i];
        if (std::strcmp(x.name, y.name) != 0 || x.type != y.type || x.count != y.count || x.offset != y.offset)
            return false;
    }
    return true;
}

ComponentId ComponentRegistry::Register(const ComponentDesc& desc, bool sparse)
{
    const uint64_t h = HashName(desc.name);
    if (auto it = by_hash_.find(h); it != by_hash_.end()) {
        ENGINE_ASSERT(SameLayout(infos_[it->second].desc, desc),
                      "component re-registered with a different layout; use snapshot migration");
        return it->second;
    }
    ENGINE_ASSERT(infos_.size() < kMaxComponents, "too many component types");
    ENGINE_ASSERT(desc.align > 0 && (desc.align & (desc.align - 1)) == 0, "alignment must be a power of two");

    Owned& o = owned_.emplace_back();
    o.name = desc.name;
    o.field_names.reserve(desc.fields.size());
    for (const auto& f : desc.fields) o.field_names.emplace_back(f.name);
    for (size_t i = 0; i < desc.fields.size(); ++i) {
        FieldDesc f = desc.fields[i];
        f.name = o.field_names[i].c_str();
        o.fields.push_back(f);
    }
    if (desc.defaults && desc.size) {
        o.defaults.resize(desc.size);
        std::memcpy(o.defaults.data(), desc.defaults, desc.size);
    }

    ComponentInfo info;
    info.id = ComponentId(infos_.size());
    info.desc = desc;
    info.desc.name = o.name.c_str();
    info.desc.fields = o.fields;
    info.desc.defaults = o.defaults.empty() ? nullptr : o.defaults.data();
    info.sparse = sparse;
    infos_.push_back(info);
    by_hash_.emplace(h, info.id);
    return info.id;
}

}  // namespace engine
