#include "render/graph/render_graph.h"
#include <algorithm>
#include <sstream>

namespace engine::rg {

const char* LayoutName(Layout l)
{
    switch (l) {
    case Layout::Undefined: return "Undefined";
    case Layout::General: return "General";
    case Layout::ColorAttachment: return "ColorAttachment";
    case Layout::DepthAttachment: return "DepthAttachment";
    case Layout::DepthReadOnly: return "DepthReadOnly";
    case Layout::ShaderReadOnly: return "ShaderReadOnly";
    case Layout::TransferSrc: return "TransferSrc";
    case Layout::TransferDst: return "TransferDst";
    case Layout::Present: return "Present";
    }
    return "?";
}

uint32_t BytesPerPixel(Format f)
{
    switch (f) {
    case Format::RGBA8_UNORM: case Format::RGBA8_SRGB: case Format::BGRA8_SRGB:
    case Format::RG16F: case Format::R32F: case Format::R32_UINT: case Format::D32F: return 4;
    case Format::RGBA16F: return 8;
    }
    return 4;
}

bool IsDepth(Format f) { return f == Format::D32F; }

uint64_t RenderGraph::Resource::Bytes() const
{
    if (!is_texture) return buffer.size;
    uint64_t b = uint64_t(texture.width) * texture.height * BytesPerPixel(texture.format);
    return texture.mips > 1 ? b * 4 / 3 : b;
}

size_t CompiledGraph::BarrierCount() const
{
    size_t n = final_barriers.size();
    for (const auto& p : passes) n += p.barriers.size();
    return n;
}

// ---------------------------------------------------------------- builder

static uint32_t ShaderStages(uint32_t s)
{
    uint32_t r = 0;
    if (s & ShaderVertex) r |= StageVertexShader;
    if (s & ShaderFragment) r |= StageFragmentShader;
    if (s & ShaderCompute) r |= StageComputeShader;
    return r;
}

void PassBuilder::Add(ResourceId r, State s)
{
    const bool write = (s.access & kWriteAccess) != 0;
    if (!graph_->Resources()[r].is_texture) s.layout = Layout::Undefined;   // buffers have no layout
    for (auto& a : accesses_) {
        if (a.resource != r) continue;
        a.state.stages |= s.stages;
        a.state.access |= s.access;
        a.write |= write;
        if (a.state.layout != s.layout) a.state.layout = Layout::General;  // mixed use: General
        return;
    }
    accesses_.push_back({r, s, write});
}

void PassBuilder::WriteColor(ResourceId t, bool load_existing)
{
    Add(t, {StageColorOutput, AccessColorWrite | (load_existing ? uint32_t(AccessColorRead) : 0u), Layout::ColorAttachment});
}
void PassBuilder::WriteDepth(ResourceId t)
{
    Add(t, {StageEarlyFragTests | StageLateFragTests, AccessDepthRead | AccessDepthWrite, Layout::DepthAttachment});
}
void PassBuilder::ReadDepth(ResourceId t)
{
    Add(t, {StageEarlyFragTests | StageLateFragTests, AccessDepthRead, Layout::DepthReadOnly});
}
void PassBuilder::ReadTexture(ResourceId t, uint32_t s) { Add(t, {ShaderStages(s), AccessShaderSampledRead, Layout::ShaderReadOnly}); }
void PassBuilder::WriteStorageTexture(ResourceId t, uint32_t s)
{
    Add(t, {ShaderStages(s), AccessShaderStorageWrite | AccessShaderStorageRead, Layout::General});
}
void PassBuilder::ReadBuffer(ResourceId b, uint32_t s) { Add(b, {ShaderStages(s), AccessShaderStorageRead, Layout::Undefined}); }
void PassBuilder::ReadUniform(ResourceId b, uint32_t s) { Add(b, {ShaderStages(s), AccessUniformRead, Layout::Undefined}); }
void PassBuilder::ReadIndirect(ResourceId b) { Add(b, {StageDrawIndirect, AccessIndirectRead, Layout::Undefined}); }
void PassBuilder::WriteBuffer(ResourceId b, uint32_t s) { Add(b, {ShaderStages(s), AccessShaderStorageWrite, Layout::Undefined}); }
void PassBuilder::CopySrc(ResourceId r) { Add(r, {StageTransfer, AccessTransferRead, Layout::TransferSrc}); }
void PassBuilder::CopyDst(ResourceId r) { Add(r, {StageTransfer, AccessTransferWrite, Layout::TransferDst}); }
void PassBuilder::SideEffect() { side_effect_ = true; }

// ---------------------------------------------------------------- declarations

ResourceId RenderGraph::CreateTexture(std::string name, const TextureDesc& d)
{
    Resource r;
    r.name = std::move(name);
    r.texture = d;
    resources_.push_back(std::move(r));
    return ResourceId(resources_.size() - 1);
}

ResourceId RenderGraph::CreateBuffer(std::string name, const BufferDesc& d)
{
    Resource r;
    r.name = std::move(name);
    r.is_texture = false;
    r.buffer = d;
    resources_.push_back(std::move(r));
    return ResourceId(resources_.size() - 1);
}

ResourceId RenderGraph::ImportTexture(std::string name, const TextureDesc& d, State initial, State final_state, uint64_t external)
{
    ResourceId id = CreateTexture(std::move(name), d);
    Resource& r = resources_[id];
    r.imported = true;
    r.initial = initial;
    r.final_state = final_state;
    r.external = external;
    return id;
}

ResourceId RenderGraph::ImportBuffer(std::string name, const BufferDesc& d, State initial, State final_state, uint64_t external)
{
    ResourceId id = CreateBuffer(std::move(name), d);
    Resource& r = resources_[id];
    r.imported = true;
    r.initial = initial;
    r.final_state = final_state;
    r.external = external;
    return id;
}

uint32_t RenderGraph::AddPass(std::string name, QueueType q, const std::function<ExecuteFn(PassBuilder&)>& setup)
{
    PassBuilder b;
    b.graph_ = this;
    ExecuteFn fn = setup(b);
    Pass p;
    p.name = std::move(name);
    p.queue = q;
    p.accesses = std::move(b.accesses_);
    p.side_effect = b.side_effect_;
    p.execute = std::move(fn);
    passes_.push_back(std::move(p));
    return uint32_t(passes_.size() - 1);
}

void RenderGraph::Reset()
{
    resources_.clear();
    passes_.clear();
}

uint64_t RenderGraph::HashDeclarations() const
{
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
    auto mix_str = [&](const std::string& s) { for (char c : s) mix(uint8_t(c)); mix(0xFF); };
    for (const auto& r : resources_) {
        mix_str(r.name); mix(r.is_texture); mix(r.imported);
        mix(r.texture.width); mix(r.texture.height); mix(uint64_t(r.texture.format)); mix(r.texture.mips);
        mix(r.buffer.size);
        mix(r.initial.stages); mix(r.initial.access); mix(uint64_t(r.initial.layout));
        mix(r.final_state.stages); mix(r.final_state.access); mix(uint64_t(r.final_state.layout));
    }
    for (const auto& p : passes_) {
        mix_str(p.name); mix(uint64_t(p.queue)); mix(p.side_effect);
        for (const auto& a : p.accesses) { mix(a.resource); mix(a.state.stages); mix(a.state.access); mix(uint64_t(a.state.layout)); }
    }
    return h;
}

// ---------------------------------------------------------------- compile

const CompiledGraph& RenderGraph::Compile()
{
    const uint64_t hash = HashDeclarations();
    if (compile_count_ > 0 && hash == compiled_.hash) return compiled_;
    ++compile_count_;

    CompiledGraph out;
    out.hash = hash;
    const size_t np = passes_.size(), nr = resources_.size();
    auto reads = [](const PassBuilder::Access& a) { return (a.state.access & ~kWriteAccess) != 0 || !a.write; };

    // 1. Dead pass culling, walking backwards from observable outputs.
    std::vector<bool> needed(nr, false), alive(np, false);
    for (size_t r = 0; r < nr; ++r) needed[r] = resources_[r].imported;
    for (size_t p = np; p-- > 0;) {
        const Pass& pass = passes_[p];
        bool keep = pass.side_effect;
        for (const auto& a : pass.accesses)
            if (a.write && needed[a.resource]) keep = true;
        if (!keep) continue;
        alive[p] = true;
        for (const auto& a : pass.accesses)
            if (a.write && !resources_[a.resource].imported) needed[a.resource] = false;
        for (const auto& a : pass.accesses)
            if (reads(a)) needed[a.resource] = true;
    }
    std::vector<uint32_t> order;
    for (uint32_t p = 0; p < np; ++p) {
        if (alive[p]) order.push_back(p);
        else out.culled.push_back(p);
    }

    // 2. Validation: no transient is read before something writes it.
    std::vector<bool> written(nr, false);
    for (size_t r = 0; r < nr; ++r) written[r] = resources_[r].imported;
    for (uint32_t p : order) {
        for (const auto& a : passes_[p].accesses) {
            // Attachment / storage writes that also read (depth test, RMW) are
            // allowed as a first use: they start from cleared contents.
            const bool write_first = a.write && !(a.state.access & AccessColorRead) &&
                                     (a.state.access & (AccessDepthWrite | AccessColorWrite | AccessShaderStorageWrite));
            if (reads(a) && !written[a.resource] && !write_first)
                out.errors.push_back("pass '" + passes_[p].name + "' reads '" + resources_[a.resource].name +
                                     "' before any pass writes it");
        }
        for (const auto& a : passes_[p].accesses)
            if (a.write) written[a.resource] = true;
    }

    // 3. Lifetimes over the surviving order.
    out.lifetimes.assign(nr, Lifetime{});
    for (uint32_t k = 0; k < order.size(); ++k)
        for (const auto& a : passes_[order[k]].accesses) {
            Lifetime& l = out.lifetimes[a.resource];
            l.first = std::min(l.first, k);
            l.last = std::max(l.last, k);
        }

    // 4. Transient aliasing: greedy interval packing, textures with textures
    // and buffers with buffers. Resources whose lifetimes don't overlap share
    // a memory slot (§7.2).
    struct Slot { uint64_t size; uint32_t last; bool texture; ResourceId occupant; };
    std::vector<Slot> slots;
    out.memory_slot.assign(nr, -1);
    std::vector<ResourceId> aliased_from(nr, kInvalidResource);
    std::vector<ResourceId> transients;
    for (ResourceId r = 0; r < nr; ++r)
        if (!resources_[r].imported && out.lifetimes[r].first != ~0u) transients.push_back(r);
    std::sort(transients.begin(), transients.end(),
              [&](ResourceId a, ResourceId b) { return out.lifetimes[a].first < out.lifetimes[b].first; });
    for (ResourceId r : transients) {
        const uint64_t bytes = resources_[r].Bytes();
        out.transient_bytes_unaliased += bytes;
        int best = -1;
        for (size_t s = 0; s < slots.size(); ++s) {
            const Slot& sl = slots[s];
            if (sl.texture != resources_[r].is_texture || sl.last >= out.lifetimes[r].first || sl.size < bytes) continue;
            if (best < 0 || sl.size < slots[size_t(best)].size) best = int(s);
        }
        if (best < 0) {
            slots.push_back({bytes, out.lifetimes[r].last, resources_[r].is_texture, r});
            best = int(slots.size() - 1);
        } else {
            aliased_from[r] = slots[size_t(best)].occupant;
            slots[size_t(best)].last = out.lifetimes[r].last;
            slots[size_t(best)].occupant = r;
        }
        out.memory_slot[r] = best;
    }
    for (const auto& s : slots) {
        out.slot_sizes.push_back(s.size);
        out.transient_bytes_aliased += s.size;
    }

    // 5. Barriers. Track each resource's current state; read-after-read in
    // the same layout merges instead of emitting a barrier, so a later write
    // waits for every reader.
    std::vector<State> cur(nr);
    for (size_t r = 0; r < nr; ++r) cur[r] = resources_[r].imported ? resources_[r].initial : State{};
    std::vector<bool> touched(nr, false);
    for (uint32_t p : order) {
        CompiledPass cp;
        cp.pass = p;
        for (const auto& a : passes_[p].accesses) {
            const ResourceId r = a.resource;
            State& s = cur[r];
            const State want = a.state;
            const bool first_use = !touched[r];
            touched[r] = true;

            if (first_use && !resources_[r].imported) {
                if (aliased_from[r] != kInvalidResource) {
                    // Memory previously held another resource: wait for its last
                    // users, discard contents (old layout Undefined).
                    State before = cur[aliased_from[r]];
                    before.layout = Layout::Undefined;
                    cp.barriers.push_back({r, before, want, true, aliased_from[r]});
                } else if (resources_[r].is_texture) {
                    cp.barriers.push_back({r, State{StageNone, AccessNone, Layout::Undefined}, want});
                }                                   // fresh buffer: nothing to wait for
                s = want;
                continue;
            }
            const bool prev_write = (s.access & kWriteAccess) != 0;
            if (!a.write && !prev_write && s.layout == want.layout && s.stages != StageNone) {
                s.stages |= want.stages;           // read after read: merge
                s.access |= want.access;
                continue;
            }
            if (!a.write && !prev_write && s.layout == want.layout && s.stages == StageNone) {
                s = want;                           // imported, never accessed on GPU yet, right layout
                continue;
            }
            cp.barriers.push_back({r, s, want});
            s = want;
        }
        out.passes.push_back(std::move(cp));
    }
    for (ResourceId r = 0; r < nr; ++r) {
        if (!resources_[r].imported || !touched[r]) continue;
        const State& f = resources_[r].final_state;
        if (cur[r].layout != f.layout || (cur[r].access & kWriteAccess))
            out.final_barriers.push_back({r, cur[r], f});
    }

    compiled_ = std::move(out);
    return compiled_;
}

void RenderGraph::Execute(Backend& backend, PassContext& ctx)
{
    (void)Compile();
    ctx.graph = this;
    for (const auto& cp : compiled_.passes) {
        ctx.pass = cp.pass;
        if (!cp.barriers.empty()) backend.Barriers(cp.barriers, ctx);
        backend.BeginPass(cp.pass, ctx);
        if (passes_[cp.pass].execute) passes_[cp.pass].execute(ctx);
        backend.EndPass(cp.pass, ctx);
    }
    if (!compiled_.final_barriers.empty()) backend.Barriers(compiled_.final_barriers, ctx);
}

// ---------------------------------------------------------------- visualisation

std::string RenderGraph::ToDot() const
{
    std::ostringstream o;
    o << "digraph RenderGraph {\n  rankdir=LR;\n  node [fontname=\"Helvetica\"];\n";
    std::vector<bool> culled(passes_.size(), false);
    for (uint32_t p : compiled_.culled) if (p < culled.size()) culled[p] = true;
    for (size_t p = 0; p < passes_.size(); ++p)
        o << "  p" << p << " [shape=box,style=\"" << (culled[p] ? "dashed" : "filled")
          << "\",fillcolor=\"#dbe8ff\",label=\"" << passes_[p].name << (culled[p] ? "\\n(culled)" : "") << "\"];\n";
    for (size_t r = 0; r < resources_.size(); ++r) {
        const auto& res = resources_[r];
        o << "  r" << r << " [shape=" << (res.is_texture ? "ellipse" : "cylinder") << ",label=\"" << res.name;
        if (r < compiled_.memory_slot.size() && compiled_.memory_slot[r] >= 0) o << "\\nslot " << compiled_.memory_slot[r];
        if (res.imported) o << "\\n(imported)";
        o << "\"];\n";
    }
    for (size_t p = 0; p < passes_.size(); ++p)
        for (const auto& a : passes_[p].accesses) {
            if (a.write) o << "  p" << p << " -> r" << a.resource << " [color=\"#c0392b\"];\n";
            if ((a.state.access & ~kWriteAccess) || !a.write) o << "  r" << a.resource << " -> p" << p << ";\n";
        }
    o << "}\n";
    return o.str();
}

}  // namespace engine::rg
