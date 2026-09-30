#pragma once
// World snapshots with reflection-driven migration (design doc §3.2).
//
// A snapshot stores every entity's handle and, per component, the component's
// descriptor (name, version, fields) plus raw bytes. Restoring into a world
// whose registered layout differs maps fields BY NAME: added fields get their
// defaults, removed fields are dropped, reordered fields follow their names.
// This is the fallback path when a component definition changes, and it is
// also scene serialisation (§15) for free.
#include <cstdint>
#include <string>
#include <vector>
#include "ecs/world.h"

namespace engine {

struct Snapshot {
    struct Field { std::string name; FieldType type; uint16_t count; uint32_t offset, size; };
    struct Type {
        std::string name;
        uint32_t version = 0, size = 0, align = 1;
        bool sparse = false;
        std::vector<Field> fields;
    };
    struct Item { uint32_t type; uint32_t offset; };      // into types / blob
    struct EntityEntry { Entity handle; uint32_t first_item, item_count; };

    std::vector<Type> types;
    std::vector<EntityEntry> entities;
    std::vector<Item> items;
    std::vector<std::byte> blob;

    size_t Bytes() const { return blob.size() + items.size() * sizeof(Item) + entities.size() * sizeof(EntityEntry); }
};

struct RestoreReport {
    size_t entities = 0;
    size_t components = 0;
    size_t migrated = 0;             // components whose layout differed
    size_t dropped_unknown = 0;      // component types the target world doesn't know
};

Snapshot SerializeWorld(const World& w);
// Target world must be empty. Component types are looked up by name in the
// target's registry; unregistered ones are dropped (counted in the report).
RestoreReport DeserializeWorld(World& w, const Snapshot& s);

// Binary round-trip for saving to disk.
std::vector<std::byte> EncodeSnapshot(const Snapshot& s);
bool DecodeSnapshot(const std::vector<std::byte>& bytes, Snapshot& out);

}  // namespace engine
