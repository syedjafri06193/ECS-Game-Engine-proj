#pragma once
// Component reflection (design doc §3.2). Descriptors describe a component's
// fields by NAME so data can survive a layout change: added fields take their
// defaults, removed fields are dropped, reordered fields follow their names.
// The same descriptors drive the Lua bindings and world snapshots.
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>
#include "core/base.h"

namespace engine {

enum class FieldType : uint8_t { F32, F64, I32, U32, I64, U64, U8, Bool, Entity };

struct FieldDesc {
    const char* name;
    FieldType   type;
    uint16_t    count;     // array length (Vec3 = F32 x 3)
    uint32_t    offset;
    uint32_t    size;      // total bytes = element size x count
};

struct ComponentDesc {
    const char* name;
    uint32_t    version;
    uint32_t    size;
    uint32_t    align;
    std::span<const FieldDesc> fields;
    const void* defaults;  // bytes of a default-constructed value, may be null (zeros)
};

size_t FieldElementSize(FieldType t);
const FieldDesc* FindField(const ComponentDesc& d, std::string_view name);

// Converts `src` (laid out per `from`) into `dst` (laid out per `to`).
// Returns the number of fields carried over.
size_t MigrateComponent(const ComponentDesc& from, const ComponentDesc& to, const void* src, void* dst);

// ---------------------------------------------------------------- traits
template <typename T> struct ComponentTraits;   // specialised by ENGINE_COMPONENT

template <typename T>
concept Component = requires {
    { ComponentTraits<std::remove_const_t<T>>::Desc() } -> std::same_as<const ComponentDesc&>;
} && std::is_trivially_copyable_v<std::remove_const_t<T>>
  && std::is_default_constructible_v<std::remove_const_t<T>>;

template <typename T> struct FieldTypeOf;
template <> struct FieldTypeOf<float>    { static constexpr FieldType type = FieldType::F32; static constexpr uint16_t count = 1; };
template <> struct FieldTypeOf<double>   { static constexpr FieldType type = FieldType::F64; static constexpr uint16_t count = 1; };
template <> struct FieldTypeOf<int32_t>  { static constexpr FieldType type = FieldType::I32; static constexpr uint16_t count = 1; };
template <> struct FieldTypeOf<uint32_t> { static constexpr FieldType type = FieldType::U32; static constexpr uint16_t count = 1; };
template <> struct FieldTypeOf<int64_t>  { static constexpr FieldType type = FieldType::I64; static constexpr uint16_t count = 1; };
template <> struct FieldTypeOf<uint64_t> { static constexpr FieldType type = FieldType::U64; static constexpr uint16_t count = 1; };
template <> struct FieldTypeOf<uint8_t>  { static constexpr FieldType type = FieldType::U8;  static constexpr uint16_t count = 1; };
template <> struct FieldTypeOf<bool>     { static constexpr FieldType type = FieldType::Bool; static constexpr uint16_t count = 1; };
template <typename T, size_t N> struct FieldTypeOf<T[N]> {
    static constexpr FieldType type = FieldTypeOf<T>::type;
    static constexpr uint16_t count = static_cast<uint16_t>(N * FieldTypeOf<T>::count);
};

}  // namespace engine

#define ENGINE_FIELD(Type, member)                                                              \
    ::engine::FieldDesc {                                                                       \
        #member, ::engine::FieldTypeOf<decltype(Type::member)>::type,                           \
        ::engine::FieldTypeOf<decltype(Type::member)>::count,                                   \
        static_cast<uint32_t>(offsetof(Type, member)), static_cast<uint32_t>(sizeof(Type::member)) \
    }

// Declares a component: name, layout version and its reflected fields. Only
// constant data is emitted, so this is safe to include from a reloadable
// module (it creates no mutable statics - module contract §3.3).
#define ENGINE_COMPONENT(Type, Version, ...)                                                     \
    template <> struct engine::ComponentTraits<Type> {                                           \
        static constexpr const char* kName = #Type;                                              \
        static constexpr uint64_t kHash = ::engine::HashName(#Type);                             \
        static constexpr Type kDefault{};                                                        \
        static constexpr ::engine::FieldDesc kFields[] = {__VA_ARGS__};                          \
        static const ::engine::ComponentDesc& Desc() {                                           \
            static constexpr ::engine::ComponentDesc d{#Type, Version, sizeof(Type), alignof(Type), \
                                                       kFields, &kDefault};                      \
            return d;                                                                            \
        }                                                                                        \
    }

// Tags: empty components. Stored in chunks with zero bytes per entity.
#define ENGINE_TAG(Type)                                                                          \
    template <> struct engine::ComponentTraits<Type> {                                           \
        static constexpr const char* kName = #Type;                                              \
        static constexpr uint64_t kHash = ::engine::HashName(#Type);                             \
        static const ::engine::ComponentDesc& Desc() {                                           \
            static constexpr ::engine::ComponentDesc d{#Type, 1, 0, 1, {}, nullptr};             \
            return d;                                                                            \
        }                                                                                        \
    }
