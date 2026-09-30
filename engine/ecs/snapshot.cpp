#include "ecs/snapshot.h"
#include <algorithm>
#include <cstring>
#include <unordered_map>

namespace engine {

namespace {

Snapshot::Type TypeFromInfo(const ComponentInfo& info)
{
    Snapshot::Type t;
    t.name = info.desc.name;
    t.version = info.desc.version;
    t.size = info.desc.size;
    t.align = info.desc.align;
    t.sparse = info.sparse;
    for (const auto& f : info.desc.fields) t.fields.push_back({f.name, f.type, f.count, f.offset, f.size});
    return t;
}

bool SameLayout(const Snapshot::Type& t, const ComponentDesc& d)
{
    if (t.size != d.size || t.fields.size() != d.fields.size()) return false;
    for (size_t i = 0; i < t.fields.size(); ++i) {
        const auto& a = t.fields[i];
        const auto& b = d.fields[i];
        if (a.name != b.name || a.type != b.type || a.count != b.count || a.offset != b.offset) return false;
    }
    return true;
}

}  // namespace

Snapshot SerializeWorld(const World& w)
{
    Snapshot s;
    const ComponentRegistry& reg = w.Registry();
    std::vector<int32_t> type_index(reg.Count(), -1);
    auto type_of = [&](ComponentId id) {
        if (type_index[id] < 0) {
            type_index[id] = int32_t(s.types.size());
            s.types.push_back(TypeFromInfo(reg.Info(id)));
        }
        return uint32_t(type_index[id]);
    };

    struct Pending { Entity e; std::vector<Snapshot::Item> items; };
    std::vector<Pending> pending;
    std::unordered_map<uint32_t, size_t> by_index;

    auto add_bytes = [&](const void* p, uint32_t size) {
        const uint32_t off = uint32_t(s.blob.size());
        s.blob.resize(s.blob.size() + AlignUp(size, 8));
        if (size) std::memcpy(s.blob.data() + off, p, size);
        return off;
    };

    for (const auto& a : w.Archetypes()) {
        for (Chunk* c : a->Chunks()) {
            for (uint32_t row = 0; row < c->count; ++row) {
                const Entity e = a->Entities(c)[row];
                by_index[e.index] = pending.size();
                Pending& p = pending.emplace_back();
                p.e = e;
                for (ComponentId id : a->Types()) {
                    const uint32_t size = reg.Info(id).desc.size;
                    const void* src = a->HasColumn(id) ? a->Element(c, a->ColumnIndex(id), row) : nullptr;
                    p.items.push_back({type_of(id), add_bytes(src, size)});
                }
            }
        }
    }
    for (ComponentId id = 0; id < reg.Count(); ++id) {
        if (!reg.Info(id).sparse) continue;
        auto* set = const_cast<World&>(w).Sparse(id);
        if (!set) continue;
        for (Entity e : set->Dense()) {
            auto it = by_index.find(e.index);
            if (it == by_index.end()) continue;
            const void* src = const_cast<World&>(w).GetSparseRaw(e, id);
            pending[it->second].items.push_back({type_of(id), add_bytes(src, set->ElementSize())});
        }
    }

    std::sort(pending.begin(), pending.end(), [](const Pending& a, const Pending& b) { return a.e.index < b.e.index; });
    for (auto& p : pending) {
        s.entities.push_back({p.e, uint32_t(s.items.size()), uint32_t(p.items.size())});
        s.items.insert(s.items.end(), p.items.begin(), p.items.end());
    }
    return s;
}

RestoreReport DeserializeWorld(World& w, const Snapshot& s)
{
    ENGINE_ASSERT(w.EntityCount() == 0, "DeserializeWorld needs an empty world");
    RestoreReport rep;
    ComponentRegistry& reg = w.Registry();

    // Resolve each snapshot type against the target registry once.
    struct Resolved {
        ComponentId id = kInvalidComponent;
        bool migrate = false;
        std::vector<FieldDesc> from_fields;
        ComponentDesc from{};
    };
    std::vector<Resolved> res(s.types.size());
    for (size_t i = 0; i < s.types.size(); ++i) {
        const auto& t = s.types[i];
        Resolved& r = res[i];
        r.id = reg.Find(t.name.c_str());
        if (r.id == kInvalidComponent) continue;
        const ComponentDesc& to = reg.Info(r.id).desc;
        r.migrate = !SameLayout(t, to);
        for (const auto& f : t.fields) r.from_fields.push_back({f.name.c_str(), f.type, f.count, f.offset, f.size});
        r.from = ComponentDesc{t.name.c_str(), t.version, t.size, t.align, r.from_fields, nullptr};
    }

    std::vector<std::byte> tmp;
    for (const auto& ent : s.entities) {
        w.CreateAt(ent.handle);
        ++rep.entities;
        for (uint32_t k = 0; k < ent.item_count; ++k) {
            const auto& item = s.items[ent.first_item + k];
            const Resolved& r = res[item.type];
            if (r.id == kInvalidComponent) { ++rep.dropped_unknown; continue; }
            const ComponentInfo& info = reg.Info(r.id);
            const void* src = s.blob.data() + item.offset;
            if (r.migrate) {
                tmp.assign(info.desc.size ? info.desc.size : 1, std::byte{0});
                MigrateComponent(r.from, info.desc, src, tmp.data());
                src = tmp.data();
                ++rep.migrated;
            }
            if (info.sparse) {
                SparseSet* set = w.Sparse(r.id);
                ENGINE_ASSERT(set, "sparse component not registered with the world");
                set->Insert(ent.handle, src);
            } else {
                w.AddRaw(ent.handle, r.id, info.desc.size ? src : nullptr);
            }
            ++rep.components;
        }
    }
    return rep;
}

// ---------------------------------------------------------------- binary encoding

namespace {
struct Writer {
    std::vector<std::byte>& out;
    template <typename T> void Pod(const T& v)
    {
        const size_t o = out.size();
        out.resize(o + sizeof(T));
        std::memcpy(out.data() + o, &v, sizeof(T));
    }
    void Str(const std::string& s)
    {
        Pod(uint32_t(s.size()));
        const size_t o = out.size();
        out.resize(o + s.size());
        std::memcpy(out.data() + o, s.data(), s.size());
    }
};
struct Reader {
    const std::vector<std::byte>& in;
    size_t pos = 0;
    bool ok = true;
    template <typename T> T Pod()
    {
        T v{};
        if (pos + sizeof(T) > in.size()) { ok = false; return v; }
        std::memcpy(&v, in.data() + pos, sizeof(T));
        pos += sizeof(T);
        return v;
    }
    std::string Str()
    {
        const uint32_t n = Pod<uint32_t>();
        if (!ok || pos + n > in.size()) { ok = false; return {}; }
        std::string s(reinterpret_cast<const char*>(in.data() + pos), n);
        pos += n;
        return s;
    }
};
constexpr uint32_t kMagic = 0x53534345;   // "ECSS"
constexpr uint32_t kFormat = 1;
}  // namespace

std::vector<std::byte> EncodeSnapshot(const Snapshot& s)
{
    std::vector<std::byte> out;
    Writer w{out};
    w.Pod(kMagic); w.Pod(kFormat);
    w.Pod(uint32_t(s.types.size()));
    for (const auto& t : s.types) {
        w.Str(t.name); w.Pod(t.version); w.Pod(t.size); w.Pod(t.align); w.Pod(uint8_t(t.sparse));
        w.Pod(uint32_t(t.fields.size()));
        for (const auto& f : t.fields) { w.Str(f.name); w.Pod(f.type); w.Pod(f.count); w.Pod(f.offset); w.Pod(f.size); }
    }
    w.Pod(uint32_t(s.entities.size()));
    for (const auto& e : s.entities) { w.Pod(e.handle.Bits()); w.Pod(e.first_item); w.Pod(e.item_count); }
    w.Pod(uint32_t(s.items.size()));
    for (const auto& i : s.items) { w.Pod(i.type); w.Pod(i.offset); }
    w.Pod(uint64_t(s.blob.size()));
    out.insert(out.end(), s.blob.begin(), s.blob.end());
    return out;
}

bool DecodeSnapshot(const std::vector<std::byte>& bytes, Snapshot& s)
{
    Reader r{bytes};
    if (r.Pod<uint32_t>() != kMagic || r.Pod<uint32_t>() != kFormat) return false;
    s = Snapshot{};
    const uint32_t nt = r.Pod<uint32_t>();
    for (uint32_t i = 0; i < nt && r.ok; ++i) {
        Snapshot::Type t;
        t.name = r.Str(); t.version = r.Pod<uint32_t>(); t.size = r.Pod<uint32_t>(); t.align = r.Pod<uint32_t>();
        t.sparse = r.Pod<uint8_t>() != 0;
        const uint32_t nf = r.Pod<uint32_t>();
        for (uint32_t j = 0; j < nf && r.ok; ++j) {
            Snapshot::Field f;
            f.name = r.Str(); f.type = r.Pod<FieldType>(); f.count = r.Pod<uint16_t>();
            f.offset = r.Pod<uint32_t>(); f.size = r.Pod<uint32_t>();
            t.fields.push_back(std::move(f));
        }
        s.types.push_back(std::move(t));
    }
    const uint32_t ne = r.Pod<uint32_t>();
    for (uint32_t i = 0; i < ne && r.ok; ++i) {
        Snapshot::EntityEntry e;
        e.handle = Entity::FromBits(r.Pod<uint64_t>()); e.first_item = r.Pod<uint32_t>(); e.item_count = r.Pod<uint32_t>();
        s.entities.push_back(e);
    }
    const uint32_t ni = r.Pod<uint32_t>();
    for (uint32_t i = 0; i < ni && r.ok; ++i) {
        Snapshot::Item it;
        it.type = r.Pod<uint32_t>(); it.offset = r.Pod<uint32_t>();
        s.items.push_back(it);
    }
    const uint64_t nb = r.Pod<uint64_t>();
    if (!r.ok || r.pos + nb != bytes.size()) return false;
    s.blob.assign(bytes.begin() + std::ptrdiff_t(r.pos), bytes.end());
    // Bounds-check references so a corrupt file can't index out of range.
    for (const auto& e : s.entities)
        if (size_t(e.first_item) + e.item_count > s.items.size()) return false;
    for (const auto& it : s.items)
        if (it.type >= s.types.size() || size_t(it.offset) + s.types[it.type].size > s.blob.size()) return false;
    return true;
}

}  // namespace engine
