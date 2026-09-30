#include "core/reflection/reflection.h"
#include <cstring>

namespace engine {

size_t FieldElementSize(FieldType t)
{
    switch (t) {
    case FieldType::F32: case FieldType::I32: case FieldType::U32: return 4;
    case FieldType::F64: case FieldType::I64: case FieldType::U64: case FieldType::Entity: return 8;
    case FieldType::U8: case FieldType::Bool: return 1;
    }
    return 0;
}

const FieldDesc* FindField(const ComponentDesc& d, std::string_view name)
{
    for (const auto& f : d.fields)
        if (name == f.name) return &f;
    return nullptr;
}

namespace {

double LoadAsDouble(FieldType t, const std::byte* p)
{
    switch (t) {
    case FieldType::F32:  { float v;    std::memcpy(&v, p, 4); return v; }
    case FieldType::F64:  { double v;   std::memcpy(&v, p, 8); return v; }
    case FieldType::I32:  { int32_t v;  std::memcpy(&v, p, 4); return v; }
    case FieldType::U32:  { uint32_t v; std::memcpy(&v, p, 4); return v; }
    case FieldType::I64:  { int64_t v;  std::memcpy(&v, p, 8); return static_cast<double>(v); }
    case FieldType::U64:  { uint64_t v; std::memcpy(&v, p, 8); return static_cast<double>(v); }
    case FieldType::U8:   return static_cast<double>(static_cast<uint8_t>(*p));
    case FieldType::Bool: return *p != std::byte{0} ? 1.0 : 0.0;
    case FieldType::Entity: return 0.0;
    }
    return 0.0;
}

void StoreFromDouble(FieldType t, std::byte* p, double v)
{
    switch (t) {
    case FieldType::F32:  { float x = static_cast<float>(v); std::memcpy(p, &x, 4); break; }
    case FieldType::F64:  { std::memcpy(p, &v, 8); break; }
    case FieldType::I32:  { int32_t x = static_cast<int32_t>(v); std::memcpy(p, &x, 4); break; }
    case FieldType::U32:  { uint32_t x = static_cast<uint32_t>(v); std::memcpy(p, &x, 4); break; }
    case FieldType::I64:  { int64_t x = static_cast<int64_t>(v); std::memcpy(p, &x, 8); break; }
    case FieldType::U64:  { uint64_t x = static_cast<uint64_t>(v); std::memcpy(p, &x, 8); break; }
    case FieldType::U8:   { *p = static_cast<std::byte>(static_cast<uint8_t>(v)); break; }
    case FieldType::Bool: { *p = static_cast<std::byte>(v != 0.0 ? 1 : 0); break; }
    case FieldType::Entity: break;
    }
}

}  // namespace

size_t MigrateComponent(const ComponentDesc& from, const ComponentDesc& to, const void* src, void* dst)
{
    auto* out = static_cast<std::byte*>(dst);
    auto* in = static_cast<const std::byte*>(src);
    if (to.defaults) std::memcpy(out, to.defaults, to.size);
    else std::memset(out, 0, to.size);

    size_t carried = 0;
    for (const auto& tf : to.fields) {
        const FieldDesc* ff = FindField(from, tf.name);
        if (!ff) continue;                                       // added field: keep default
        const uint16_t n = ff->count < tf.count ? ff->count : tf.count;
        if (ff->type == tf.type) {
            std::memcpy(out + tf.offset, in + ff->offset, n * FieldElementSize(tf.type));
        } else if (ff->type != FieldType::Entity && tf.type != FieldType::Entity) {
            const size_t fe = FieldElementSize(ff->type), te = FieldElementSize(tf.type);
            for (uint16_t i = 0; i < n; ++i)
                StoreFromDouble(tf.type, out + tf.offset + i * te, LoadAsDouble(ff->type, in + ff->offset + i * fe));
        } else {
            continue;                                            // entity <-> number: not meaningful
        }
        ++carried;
    }
    return carried;
}

}  // namespace engine
