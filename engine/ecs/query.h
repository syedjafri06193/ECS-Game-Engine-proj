#pragma once
// Queries (design doc §4.4). A query is an include/exclude mask; the list of
// matching archetypes is cached in the world and refreshed only when a new
// archetype appears, so in a settled game resolving a query costs a lookup.
//
//   for (auto [e, pos, vel] : world.Query<Position, const Velocity>()) ...
//   world.Query<Position, const Velocity>().Without<Dead>().ForEach(fn);
//
// `const T` means read-only; that is what the scheduler's access declaration
// is derived from (§5.5), so the declaration cannot drift from the code.
// Iterating holds a StructureLock: adding a component mid-loop asserts
// instead of corrupting the chunk being walked.
#include <array>
#include <tuple>
#include <vector>
#include "ecs/world.h"

namespace engine {

template <Component... Ts>
class QueryView {
public:
    static constexpr size_t N = sizeof...(Ts);

    explicit QueryView(World& w) : w_(&w), ids_{w.Id<std::remove_const_t<Ts>>()...}
    {
        for (ComponentId id : ids_) {
            ENGINE_ASSERT(!w.Registry().Info(id).IsTag(), "tags have no data; use .With<Tag>()");
            ENGINE_ASSERT(!w.Registry().Info(id).sparse, "sparse components are not archetype-queryable");
            include_.Set(id);
        }
    }

    template <Component T> QueryView With() const { QueryView q(*this); q.include_.Set(w_->Id<T>()); return q; }
    template <Component T> QueryView Without() const { QueryView q(*this); q.exclude_.Set(w_->Id<T>()); return q; }

    const std::vector<Archetype*>& Archetypes() const { return w_->Cached(include_, exclude_).matched; }
    const std::array<ComponentId, N>& Ids() const { return ids_; }
    const ComponentMask& Include() const { return include_; }
    const ComponentMask& Exclude() const { return exclude_; }

    // Access declaration derived from the types (§5.5).
    ComponentMask Reads() const
    {
        ComponentMask m = include_;            // every queried type and With<> filter is read
        return m;
    }
    ComponentMask Writes() const
    {
        ComponentMask m;
        constexpr bool is_const[] = {std::is_const_v<Ts>...};
        for (size_t i = 0; i < N; ++i)
            if (!is_const[i]) m.Set(ids_[i]);
        return m;
    }

    size_t Count() const
    {
        size_t n = 0;
        for (Archetype* a : Archetypes()) n += a->EntityCount();
        return n;
    }

    // Fastest path: tight per-chunk loop over raw column pointers.
    template <typename Fn> void ForEach(Fn&& fn) const
    {
        StructureLock lock(*w_);
        for (Archetype* a : Archetypes())
            for (Chunk* c : a->Chunks()) RunChunk(a, c, fn);
    }

    template <typename Fn> void ForEachChunk(Fn&& fn) const
    {
        StructureLock lock(*w_);
        for (Archetype* a : Archetypes())
            for (Chunk* c : a->Chunks()) fn(ChunkView{a, c});
    }

    // Per-chunk body usable from a job: fn(Entity, Ts&...)
    template <typename Fn> void RunChunk(Archetype* a, Chunk* c, Fn&& fn) const
    {
        const Entity* ents = a->Entities(c);
        auto cols = Columns(a, c, std::index_sequence_for<Ts...>{});
        const uint32_t n = c->count;
        for (uint32_t row = 0; row < n; ++row)
            std::apply([&](auto*... p) { fn(ents[row], p[row]...); }, cols);
    }

    std::vector<ChunkView> Chunks() const
    {
        std::vector<ChunkView> out;
        for (Archetype* a : Archetypes())
            for (Chunk* c : a->Chunks()) out.push_back({a, c});
        return out;
    }

    // ------------------------------------------------ range-for support
    // The iterator returned by begin() owns the structure lock, so it is held
    // for exactly the duration of a range-for loop (the loop destroys its
    // begin iterator on exit). Copies never own the lock.
    class Iterator {
    public:
        Iterator(const QueryView* q, const std::vector<Archetype*>* archs, size_t ai, World* lock_owner = nullptr)
            : q_(q), archs_(archs), ai_(ai), lock_(lock_owner)
        {
            if (lock_) lock_->LockStructure();
            Settle();
        }
        Iterator(const Iterator& o)
            : q_(o.q_), archs_(o.archs_), ai_(o.ai_), ci_(o.ci_), row_(o.row_), count_(o.count_), ents_(o.ents_),
              cols_(o.cols_) {}
        Iterator& operator=(const Iterator&) = delete;
        ~Iterator() { if (lock_) lock_->UnlockStructure(); }
        std::tuple<Entity, Ts&...> operator*() const
        {
            return std::apply([&](auto*... p) { return std::tuple<Entity, Ts&...>(ents_[row_], p[row_]...); }, cols_);
        }
        Iterator& operator++()
        {
            if (++row_ >= count_) { row_ = 0; ++ci_; Settle(); }
            return *this;
        }
        bool operator!=(const Iterator& o) const { return ai_ != o.ai_ || ci_ != o.ci_ || row_ != o.row_; }

    private:
        void Settle()
        {
            for (; ai_ < archs_->size(); ++ai_, ci_ = 0) {
                Archetype* a = (*archs_)[ai_];
                for (; ci_ < a->Chunks().size(); ++ci_) {
                    Chunk* c = a->Chunks()[ci_];
                    if (c->count == 0) continue;
                    count_ = c->count;
                    ents_ = a->Entities(c);
                    cols_ = q_->Columns(a, c, std::index_sequence_for<Ts...>{});
                    return;
                }
            }
            ci_ = 0; row_ = 0;
        }
        const QueryView* q_;
        const std::vector<Archetype*>* archs_;
        size_t ai_, ci_ = 0;
        uint32_t row_ = 0, count_ = 0;
        const Entity* ents_ = nullptr;
        std::tuple<Ts*...> cols_{};
        World* lock_ = nullptr;
    };

    Iterator begin() const { return Iterator(this, &Archetypes(), 0, w_); }
    Iterator end() const
    {
        const auto& a = Archetypes();
        return Iterator(this, &a, a.size());
    }

private:
    template <size_t... I>
    std::tuple<Ts*...> Columns(Archetype* a, Chunk* c, std::index_sequence<I...>) const
    {
        return {reinterpret_cast<Ts*>(a->Column(c, a->ColumnIndex(ids_[I])))...};
    }

    World* w_;
    std::array<ComponentId, N> ids_;
    ComponentMask include_, exclude_;
};

template <Component... Ts>
QueryView<Ts...> World::Query() { return QueryView<Ts...>(*this); }

}  // namespace engine
