#include "scripting/script_system.h"
#include <cstring>
#include <string_view>
extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}
#include "core/platform/platform.h"

namespace engine {

namespace {

constexpr const char* kProxyMeta = "engine.component";

struct Proxy {
    void*        ptr;
    ComponentId  id;
    bool         writable;
    uint64_t     epoch;
    ScriptSystem* sys;
};

// Epoch per ScriptSystem, bumped after each update: proxies from an earlier
// update (stashed in `state`, say) become invalid instead of dangling.
struct Epochs { uint64_t value = 1; };
Epochs& EpochOf(lua_State* L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, "engine.epoch");
    auto* e = static_cast<Epochs*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return *e;
}

ScriptSystem* Self(lua_State* L) { return static_cast<ScriptSystem*>(lua_touserdata(L, lua_upvalueindex(1))); }

ComponentId CheckComponent(lua_State* L, ScriptSystem* s, int idx)
{
    const char* name = luaL_checkstring(L, idx);
    const ComponentId id = s->ResolveComponent(name);
    if (id == kInvalidComponent) luaL_error(L, "unknown component '%s'", name);
    return id;
}

void PushField(lua_State* L, const FieldDesc& f, const std::byte* base)
{
    auto one = [&](const std::byte* p) {
        switch (f.type) {
        case FieldType::F32: { float v; std::memcpy(&v, p, 4); lua_pushnumber(L, v); break; }
        case FieldType::F64: { double v; std::memcpy(&v, p, 8); lua_pushnumber(L, v); break; }
        case FieldType::I32: { int32_t v; std::memcpy(&v, p, 4); lua_pushinteger(L, v); break; }
        case FieldType::U32: { uint32_t v; std::memcpy(&v, p, 4); lua_pushinteger(L, v); break; }
        case FieldType::I64: { int64_t v; std::memcpy(&v, p, 8); lua_pushinteger(L, v); break; }
        case FieldType::U64: case FieldType::Entity: { int64_t v; std::memcpy(&v, p, 8); lua_pushinteger(L, v); break; }
        case FieldType::U8: lua_pushinteger(L, lua_Integer(static_cast<uint8_t>(*p))); break;
        case FieldType::Bool: lua_pushboolean(L, *p != std::byte{0}); break;
        }
    };
    const size_t es = FieldElementSize(f.type);
    if (f.count == 1) { one(base + f.offset); return; }
    lua_createtable(L, f.count, 0);
    for (uint16_t i = 0; i < f.count; ++i) {
        one(base + f.offset + i * es);
        lua_rawseti(L, -2, i + 1);
    }
}

void StoreField(lua_State* L, const FieldDesc& f, std::byte* base, int idx)
{
    auto one = [&](std::byte* p, int vi) {
        switch (f.type) {
        case FieldType::F32: { float v = float(luaL_checknumber(L, vi)); std::memcpy(p, &v, 4); break; }
        case FieldType::F64: { double v = luaL_checknumber(L, vi); std::memcpy(p, &v, 8); break; }
        case FieldType::I32: { int32_t v = int32_t(luaL_checkinteger(L, vi)); std::memcpy(p, &v, 4); break; }
        case FieldType::U32: { uint32_t v = uint32_t(luaL_checkinteger(L, vi)); std::memcpy(p, &v, 4); break; }
        case FieldType::I64: case FieldType::U64: case FieldType::Entity: { int64_t v = luaL_checkinteger(L, vi); std::memcpy(p, &v, 8); break; }
        case FieldType::U8: *p = static_cast<std::byte>(uint8_t(luaL_checkinteger(L, vi))); break;
        case FieldType::Bool: *p = static_cast<std::byte>(lua_toboolean(L, vi) ? 1 : 0); break;
        }
    };
    const size_t es = FieldElementSize(f.type);
    if (f.count == 1) { one(base + f.offset, idx); return; }
    luaL_checktype(L, idx, LUA_TTABLE);
    for (uint16_t i = 0; i < f.count; ++i) {
        lua_rawgeti(L, idx, i + 1);
        if (!lua_isnil(L, -1)) one(base + f.offset + i * es, lua_gettop(L));
        lua_pop(L, 1);
    }
}

Proxy* NewProxy(lua_State* L, ScriptSystem* s, void* ptr, ComponentId id)
{
    auto* p = static_cast<Proxy*>(lua_newuserdatauv(L, sizeof(Proxy), 0));
    *p = Proxy{ptr, id, s->Writes().Test(id), EpochOf(L).value, s};
    luaL_setmetatable(L, kProxyMeta);
    return p;
}

Proxy* CheckLiveProxy(lua_State* L)
{
    auto* p = static_cast<Proxy*>(luaL_checkudata(L, 1, kProxyMeta));
    if (!p->ptr || p->epoch != EpochOf(L).value) luaL_error(L, "stale component proxy (used outside the update/each that produced it)");
    return p;
}

int ProxyIndex(lua_State* L)
{
    Proxy* p = CheckLiveProxy(L);
    const ComponentDesc& d = p->sys->GetWorld().Registry().Info(p->id).desc;
    const FieldDesc* f = FindField(d, luaL_checkstring(L, 2));
    if (!f) return luaL_error(L, "component '%s' has no field '%s'", d.name, lua_tostring(L, 2));
    PushField(L, *f, static_cast<std::byte*>(p->ptr));
    return 1;
}

int ProxyNewIndex(lua_State* L)
{
    Proxy* p = CheckLiveProxy(L);
    const ComponentDesc& d = p->sys->GetWorld().Registry().Info(p->id).desc;
    if (!p->writable) return luaL_error(L, "component '%s' is not declared in this script's writes", d.name);
    const FieldDesc* f = FindField(d, luaL_checkstring(L, 2));
    if (!f) return luaL_error(L, "component '%s' has no field '%s'", d.name, lua_tostring(L, 2));
    StoreField(L, *f, static_cast<std::byte*>(p->ptr), 3);
    return 0;
}

// Builds component bytes from defaults plus a Lua table of field values.
// The scratch buffer is a Lua userdata left on the stack: if a field has the
// wrong type, luaL_error longjmps and the GC reclaims it - a std::vector here
// would leak (C Lua unwinds with longjmp, skipping C++ destructors).
std::byte* BytesFromTable(lua_State* L, const ComponentDesc& d, int idx)
{
    auto* b = static_cast<std::byte*>(lua_newuserdatauv(L, d.size ? d.size : 1, 0));
    if (d.defaults) std::memcpy(b, d.defaults, d.size);
    else std::memset(b, 0, d.size);
    if (lua_istable(L, idx)) {
        for (const auto& f : d.fields) {
            lua_getfield(L, idx, f.name);
            if (!lua_isnil(L, -1)) StoreField(L, f, b, lua_gettop(L));
            lua_pop(L, 1);
        }
    }
    return b;
}

void CheckDeclared(lua_State* L, ScriptSystem* s, ComponentId id)
{
    if (!s->Reads().Test(id))
        luaL_error(L, "component '%s' is not declared in this script's reads or writes",
                   s->GetWorld().Registry().Info(id).desc.name);
}

// world:each({"A", "B"}, fn(e, a, b))
//
// Everything on the C++ stack here is trivially destructible, so a Lua
// error (longjmp) anywhere in this function leaks nothing. The structure
// lock is released by hand around the protected callback calls.
int WorldEach(lua_State* L)
{
    constexpr int kMaxComponents = 16;
    ScriptSystem* s = Self(L);
    luaL_checktype(L, 2, LUA_TTABLE);
    luaL_checktype(L, 3, LUA_TFUNCTION);
    World& w = s->GetWorld();
    const lua_Integer n = luaL_len(L, 2);
    if (n < 1 || n > kMaxComponents) return luaL_error(L, "each() takes 1..%d component names", kMaxComponents);

    ComponentId ids[kMaxComponents];
    ComponentMask include;
    for (lua_Integer i = 1; i <= n; ++i) {
        lua_rawgeti(L, 2, i);
        const ComponentId id = CheckComponent(L, s, lua_gettop(L));
        lua_pop(L, 1);
        CheckDeclared(L, s, id);
        if (w.Registry().Info(id).IsTag() || w.Registry().Info(id).sparse)
            return luaL_error(L, "each() takes data components; '%s' is a tag or sparse", w.Registry().Info(id).desc.name);
        ids[i - 1] = id;
        include.Set(id);
    }
    // One proxy per component, re-pointed per row (no per-row allocation).
    const int base = lua_gettop(L);
    Proxy* proxies[kMaxComponents];
    for (lua_Integer k = 0; k < n; ++k) proxies[k] = NewProxy(L, s, nullptr, ids[k]);

    const std::vector<Archetype*>& archs = w.Cached(include, ComponentMask{}).matched;  // stable while locked
    w.LockStructure();
    int status = LUA_OK;
    for (size_t ai = 0; ai < archs.size() && status == LUA_OK; ++ai) {
        Archetype* a = archs[ai];
        for (size_t ci = 0; ci < a->Chunks().size() && status == LUA_OK; ++ci) {
            Chunk* c = a->Chunks()[ci];
            for (uint32_t row = 0; row < c->count && status == LUA_OK; ++row) {
                lua_pushvalue(L, 3);
                lua_pushinteger(L, lua_Integer(a->Entities(c)[row].Bits()));
                for (lua_Integer k = 0; k < n; ++k) {
                    proxies[k]->ptr = a->Element(c, a->ColumnIndex(ids[k]), row);
                    lua_pushvalue(L, base + int(k) + 1);
                }
                status = lua_pcall(L, int(n) + 1, 0, 0);
            }
        }
    }
    w.UnlockStructure();
    for (lua_Integer k = 0; k < n; ++k) proxies[k]->ptr = nullptr;
    if (status != LUA_OK) return lua_error(L);                    // rethrow with the message on top
    return 0;
}

int WorldCount(lua_State* L)
{
    ScriptSystem* s = Self(L);
    luaL_checktype(L, 2, LUA_TTABLE);
    ComponentMask include;
    const lua_Integer n = luaL_len(L, 2);
    for (lua_Integer i = 1; i <= n; ++i) {
        lua_rawgeti(L, 2, i);
        include.Set(CheckComponent(L, s, lua_gettop(L)));
        lua_pop(L, 1);
    }
    size_t count = 0;
    for (Archetype* a : s->GetWorld().Cached(include, ComponentMask{}).matched) count += a->EntityCount();
    lua_pushinteger(L, lua_Integer(count));
    return 1;
}

int WorldGet(lua_State* L)
{
    ScriptSystem* s = Self(L);
    const Entity e = Entity::FromBits(uint64_t(luaL_checkinteger(L, 2)));
    const ComponentId id = CheckComponent(L, s, 3);
    CheckDeclared(L, s, id);
    void* p = s->GetWorld().GetRaw(e, id);
    if (!p || s->GetWorld().Registry().Info(id).IsTag()) { lua_pushnil(L); return 1; }
    NewProxy(L, s, p, id);
    return 1;
}

int WorldHas(lua_State* L)
{
    ScriptSystem* s = Self(L);
    const Entity e = Entity::FromBits(uint64_t(luaL_checkinteger(L, 2)));
    const ComponentId id = CheckComponent(L, s, 3);
    World& w = s->GetWorld();
    lua_pushboolean(L, w.Registry().Info(id).sparse ? w.HasSparseRaw(e, id) : w.HasRaw(e, id));
    return 1;
}

int WorldAlive(lua_State* L)
{
    ScriptSystem* s = Self(L);
    lua_pushboolean(L, s->GetWorld().IsAlive(Entity::FromBits(uint64_t(luaL_checkinteger(L, 2)))));
    return 1;
}

// world:spawn({ Position = {x=1}, Velocity = {} }) -> entity (placeholder until the update ends)
int WorldSpawn(lua_State* L)
{
    ScriptSystem* s = Self(L);
    luaL_checktype(L, 2, LUA_TTABLE);
    CommandBuffer& cb = s->Deferred();
    const Entity e = cb.CreateEntity();
    lua_pushnil(L);
    while (lua_next(L, 2)) {
        const int value = lua_gettop(L);
        lua_pushvalue(L, -2);
        const ComponentId id = CheckComponent(L, s, lua_gettop(L));
        lua_pop(L, 1);
        const ComponentDesc& d = s->GetWorld().Registry().Info(id).desc;
        const std::byte* bytes = BytesFromTable(L, d, value);
        if (s->GetWorld().Registry().Info(id).sparse) cb.AddSparseRaw(e, id, bytes, d.size);
        else cb.AddRaw(e, id, bytes, d.size);
        lua_pop(L, 2);                                   // scratch + value
    }
    lua_pushinteger(L, lua_Integer(e.Bits()));
    return 1;
}

int WorldDestroy(lua_State* L)
{
    ScriptSystem* s = Self(L);
    s->Deferred().DestroyEntity(Entity::FromBits(uint64_t(luaL_checkinteger(L, 2))));
    return 0;
}

int WorldAdd(lua_State* L)
{
    ScriptSystem* s = Self(L);
    const Entity e = Entity::FromBits(uint64_t(luaL_checkinteger(L, 2)));
    const ComponentId id = CheckComponent(L, s, 3);
    const ComponentDesc& d = s->GetWorld().Registry().Info(id).desc;
    const std::byte* bytes = BytesFromTable(L, d, 4);
    if (s->GetWorld().Registry().Info(id).sparse) s->Deferred().AddSparseRaw(e, id, bytes, d.size);
    else s->Deferred().AddRaw(e, id, bytes, d.size);
    return 0;
}

int WorldRemove(lua_State* L)
{
    ScriptSystem* s = Self(L);
    const Entity e = Entity::FromBits(uint64_t(luaL_checkinteger(L, 2)));
    const ComponentId id = CheckComponent(L, s, 3);
    if (s->GetWorld().Registry().Info(id).sparse) s->Deferred().RemoveSparseRaw(e, id);
    else s->Deferred().RemoveRaw(e, id);
    return 0;
}

int Log(lua_State* L)
{
    // luaL_Buffer, not std::string: __tostring may raise (longjmp).
    const int n = lua_gettop(L);
    luaL_Buffer buf;
    luaL_buffinit(L, &buf);
    for (int i = 1; i <= n; ++i) {
        if (i > 1) luaL_addchar(&buf, '\t');
        luaL_tolstring(L, i, nullptr);
        luaL_addvalue(&buf);
    }
    luaL_pushresult(&buf);
    std::fprintf(stdout, "[lua:%s] %s\n", Self(L)->Name().c_str(), lua_tostring(L, -1));
    return 0;
}

void RegisterBindings(lua_State* L, ScriptSystem* s)
{
    luaL_newmetatable(L, kProxyMeta);
    lua_pushcfunction(L, ProxyIndex);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, ProxyNewIndex);
    lua_setfield(L, -2, "__newindex");
    lua_pop(L, 1);

    new (lua_newuserdatauv(L, sizeof(Epochs), 0)) Epochs{};
    lua_setfield(L, LUA_REGISTRYINDEX, "engine.epoch");

    const luaL_Reg world_fns[] = {
        {"each", WorldEach}, {"count", WorldCount}, {"get", WorldGet}, {"has", WorldHas},
        {"alive", WorldAlive}, {"spawn", WorldSpawn}, {"destroy", WorldDestroy},
        {"add", WorldAdd}, {"remove", WorldRemove}, {nullptr, nullptr}};
    lua_newtable(L);
    lua_pushlightuserdata(L, s);
    luaL_setfuncs(L, world_fns, 1);
    lua_setfield(L, LUA_REGISTRYINDEX, "engine.world");

    lua_pushlightuserdata(L, s);
    lua_pushcclosure(L, Log, 1);
    lua_setglobal(L, "log");
}

}  // namespace

// ---------------------------------------------------------------- ScriptSystem

ScriptSystem::ScriptSystem(World& world, std::string path)
    : world_(world), path_(std::move(path)), deferred_(&world)
{
    L_ = luaL_newstate();
    luaL_openlibs(L_);
    RegisterBindings(L_, this);
    lua_newtable(L_);
    state_ref_ = luaL_ref(L_, LUA_REGISTRYINDEX);
}

ScriptSystem::~ScriptSystem()
{
    if (L_) lua_close(L_);
}

ComponentId ScriptSystem::ResolveComponent(const char* name) const
{
    ComponentId id = world_.Registry().Find(name);
    if (id == kInvalidComponent) id = world_.Registry().Find(("game::" + std::string(name)).c_str());
    return id;
}

bool ScriptSystem::Load()
{
    mtime_ = platform::FileModifiedNs(path_);
    const int top = lua_gettop(L_);
    auto fail = [&](std::string msg) {
        last_error_ = std::move(msg);
        std::fprintf(stderr, "[script] %s: %s (keeping previous version)\n", path_.c_str(), last_error_.c_str());
        lua_settop(L_, top);
        return false;
    };

    if (luaL_loadfile(L_, path_.c_str()) != LUA_OK || lua_pcall(L_, 0, 1, 0) != LUA_OK)
        return fail(lua_tostring(L_, -1) ? lua_tostring(L_, -1) : "load error");
    if (!lua_istable(L_, -1)) return fail("script must return a table");
    lua_getfield(L_, -1, "update");
    if (!lua_isfunction(L_, -1)) return fail("script table needs an update(world, dt, state) function");
    lua_pop(L_, 1);

    // Access declaration: validated now so a typo fails the reload, not a frame.
    ComponentMask reads, writes;
    for (const char* key : {"reads", "writes"}) {
        lua_getfield(L_, -1, key);
        if (lua_istable(L_, -1)) {
            const lua_Integer n = luaL_len(L_, -1);
            for (lua_Integer i = 1; i <= n; ++i) {
                lua_rawgeti(L_, -1, i);
                const char* cname = lua_tostring(L_, -1);
                const ComponentId id = cname ? ResolveComponent(cname) : kInvalidComponent;
                if (id == kInvalidComponent) return fail(std::string("unknown component in ") + key + ": " + (cname ? cname : "?"));
                reads.Set(id);
                if (std::strcmp(key, "writes") == 0) writes.Set(id);
                lua_pop(L_, 1);
            }
        }
        lua_pop(L_, 1);
    }
    lua_getfield(L_, -1, "name");
    std::string name = lua_isstring(L_, -1) ? lua_tostring(L_, -1) : path_;
    lua_pop(L_, 1);

    // Commit: swap in the new module table.
    if (module_ref_ != LUA_NOREF && module_ref_ != -2) luaL_unref(L_, LUA_REGISTRYINDEX, module_ref_);
    module_ref_ = luaL_ref(L_, LUA_REGISTRYINDEX);
    lua_settop(L_, top);
    reads_ = reads;
    writes_ = writes;
    name_ = std::move(name);
    healthy_ = true;
    last_error_.clear();
    ++reloads_;
    return true;
}

bool ScriptSystem::ReloadIfChanged()
{
    const int64_t m = platform::FileModifiedNs(path_);
    if (m == 0 || m == mtime_) return false;
    return Load();
}

bool ScriptSystem::Update(float dt)
{
    if (!healthy_ || module_ref_ < 0) return false;
    const int top = lua_gettop(L_);
    lua_rawgeti(L_, LUA_REGISTRYINDEX, module_ref_);
    lua_getfield(L_, -1, "update");
    lua_getfield(L_, LUA_REGISTRYINDEX, "engine.world");
    lua_pushnumber(L_, dt);
    lua_rawgeti(L_, LUA_REGISTRYINDEX, state_ref_);
    const bool ok = lua_pcall(L_, 3, 0, 0) == LUA_OK;
    if (!ok) {
        last_error_ = lua_tostring(L_, -1) ? lua_tostring(L_, -1) : "runtime error";
        std::fprintf(stderr, "[script] %s: %s (disabled until the file changes)\n", name_.c_str(), last_error_.c_str());
        healthy_ = false;
        deferred_.Clear();                       // don't apply half a frame
    }
    lua_settop(L_, top);
    ++EpochOf(L_).value;                         // invalidate proxies from this update
    if (ok) world_.ApplyCommandBuffers(std::span<CommandBuffer>(&deferred_, 1));
    return ok;
}

System ScriptSystem::MakeSystem()
{
    System s;
    s.name = "script:" + name_;
    s.access.reads = reads_;
    s.access.writes = writes_;
    s.access.exclusive = true;                   // one Lua state: runs alone
    s.single_fn = [this](SystemContext& ctx) { Update(ctx.dt); };
    return s;
}

}  // namespace engine
