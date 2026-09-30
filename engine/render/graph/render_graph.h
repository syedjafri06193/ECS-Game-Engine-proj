#pragma once
// Render graph (design doc §7). Passes declare what they read and write; the
// graph derives barriers and layout transitions, culls passes whose output
// nobody reads, computes transient lifetimes and aliases transient memory.
//
// This layer is backend-agnostic: stages, access bits and layouts mirror
// Vulkan synchronization2 one-to-one (render/vk maps them), so everything
// here - the part where barrier bugs live - is unit-tested without a GPU.
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace engine::rg {

// ---------------------------------------------------------------- sync vocabulary (mirrors VK sync2)
enum Stage : uint32_t {
    StageNone          = 0,
    StageDrawIndirect  = 1u << 0,
    StageVertexShader  = 1u << 1,
    StageFragmentShader= 1u << 2,
    StageEarlyFragTests= 1u << 3,
    StageLateFragTests = 1u << 4,
    StageColorOutput   = 1u << 5,
    StageComputeShader = 1u << 6,
    StageTransfer      = 1u << 7,
    StageBottomOfPipe  = 1u << 8,
};
enum AccessBits : uint32_t {
    AccessNone              = 0,
    AccessIndirectRead      = 1u << 0,
    AccessUniformRead       = 1u << 1,
    AccessShaderSampledRead = 1u << 2,
    AccessShaderStorageRead = 1u << 3,
    AccessShaderStorageWrite= 1u << 4,
    AccessColorRead         = 1u << 5,
    AccessColorWrite        = 1u << 6,
    AccessDepthRead         = 1u << 7,
    AccessDepthWrite        = 1u << 8,
    AccessTransferRead      = 1u << 9,
    AccessTransferWrite     = 1u << 10,
};
constexpr uint32_t kWriteAccess = AccessShaderStorageWrite | AccessColorWrite | AccessDepthWrite | AccessTransferWrite;

enum class Layout : uint8_t {
    Undefined, General, ColorAttachment, DepthAttachment, DepthReadOnly, ShaderReadOnly,
    TransferSrc, TransferDst, Present,
};
const char* LayoutName(Layout l);

enum class Format : uint8_t { RGBA8_UNORM, RGBA8_SRGB, BGRA8_SRGB, RGBA16F, RG16F, R32F, R32_UINT, D32F };
uint32_t BytesPerPixel(Format f);
bool IsDepth(Format f);

struct TextureDesc { uint32_t width = 1, height = 1; Format format = Format::RGBA8_UNORM; uint32_t mips = 1; };
struct BufferDesc  { uint64_t size = 0; };

struct State {
    uint32_t stages = StageNone;
    uint32_t access = AccessNone;
    Layout   layout = Layout::Undefined;
    bool operator==(const State&) const = default;
};

using ResourceId = uint32_t;
constexpr ResourceId kInvalidResource = ~0u;

struct Barrier {
    ResourceId resource;
    State      before, after;
    bool       aliasing = false;     // first use of memory previously owned by another transient
    ResourceId aliased_from = kInvalidResource;
};

// ---------------------------------------------------------------- passes
enum class QueueType : uint8_t { Graphics, Compute };
enum ShaderStageMask : uint32_t { ShaderVertex = 1, ShaderFragment = 2, ShaderCompute = 4 };

class RenderGraph;
struct PassContext;
using ExecuteFn = std::function<void(PassContext&)>;

class PassBuilder {
public:
    // Textures
    // load_existing: the pass draws over the current contents (UI overlay),
    // so the previous writer is a dependency and is never culled.
    void WriteColor(ResourceId t, bool load_existing = false);
    void WriteDepth(ResourceId t);                 // depth test + write
    void ReadDepth(ResourceId t);                  // depth test, read-only attachment
    void ReadTexture(ResourceId t, uint32_t shader_stages);   // sampled
    void WriteStorageTexture(ResourceId t, uint32_t shader_stages);
    // Buffers
    void ReadBuffer(ResourceId b, uint32_t shader_stages);    // storage read
    void ReadUniform(ResourceId b, uint32_t shader_stages);
    void ReadIndirect(ResourceId b);
    void WriteBuffer(ResourceId b, uint32_t shader_stages);   // storage write
    void CopySrc(ResourceId r);
    void CopyDst(ResourceId r);
    // Never cull this pass (e.g. it writes to the CPU or a debug readback).
    void SideEffect();

private:
    friend class RenderGraph;
    struct Access { ResourceId resource; State state; bool write; };
    void Add(ResourceId r, State s);
    std::vector<Access> accesses_;
    bool side_effect_ = false;
    const RenderGraph* graph_ = nullptr;
};

struct PassContext {
    void*       native_command_buffer = nullptr;   // VkCommandBuffer in the Vulkan backend
    const RenderGraph* graph = nullptr;
    uint32_t    pass = 0;
    void*       user = nullptr;                    // backend resource resolver
};

// ---------------------------------------------------------------- compiled output
struct CompiledPass {
    uint32_t pass;                        // index into declared passes
    std::vector<Barrier> barriers;        // issue before the pass
};

struct Lifetime { uint32_t first = ~0u, last = 0; };   // indices into CompiledGraph::passes

struct CompiledGraph {
    std::vector<CompiledPass> passes;     // surviving passes, in order
    std::vector<Barrier> final_barriers;  // imported resources -> their final state
    std::vector<uint32_t> culled;         // declared pass indices that were culled
    std::vector<Lifetime> lifetimes;      // per resource (transients only meaningful)
    std::vector<int32_t> memory_slot;     // per resource: aliasing slot, -1 imported/unused
    std::vector<uint64_t> slot_sizes;
    uint64_t transient_bytes_unaliased = 0;
    uint64_t transient_bytes_aliased = 0;
    std::vector<std::string> errors;
    uint64_t hash = 0;
    bool Ok() const { return errors.empty(); }
    size_t BarrierCount() const;
};

// Callbacks the executor needs from a backend.
class Backend {
public:
    virtual ~Backend() = default;
    virtual void Barriers(std::span<const Barrier> barriers, PassContext& ctx) = 0;
    virtual void BeginPass(uint32_t pass, PassContext& ctx) { (void)pass; (void)ctx; }
    virtual void EndPass(uint32_t pass, PassContext& ctx) { (void)pass; (void)ctx; }
};

// ---------------------------------------------------------------- the graph
class RenderGraph {
public:
    struct Resource {
        std::string name;
        bool        is_texture = true;
        bool        imported = false;
        TextureDesc texture;
        BufferDesc  buffer;
        State       initial, final_state;    // imported only
        uint64_t    external = 0;            // backend handle for imported resources
        uint64_t Bytes() const;
    };
    struct Pass {
        std::string name;
        QueueType   queue = QueueType::Graphics;
        std::vector<PassBuilder::Access> accesses;
        bool        side_effect = false;
        ExecuteFn   execute;
    };

    ResourceId CreateTexture(std::string name, const TextureDesc& d);
    ResourceId CreateBuffer(std::string name, const BufferDesc& d);
    // Imported resources live outside the graph (swapchain image, persistent
    // buffers). Writing one is an observable output, so it keeps its writer alive.
    ResourceId ImportTexture(std::string name, const TextureDesc& d, State initial, State final_state, uint64_t external = 0);
    ResourceId ImportBuffer(std::string name, const BufferDesc& d, State initial, State final_state, uint64_t external = 0);

    // setup(builder) declares accesses and returns the execute callback.
    uint32_t AddPass(std::string name, QueueType q, const std::function<ExecuteFn(PassBuilder&)>& setup);

    // Derives everything. Cached: if the declarations hash equals the last
    // compile's, the previous result is reused (§8.2).
    const CompiledGraph& Compile();
    void Execute(Backend& backend, PassContext& ctx);

    const std::vector<Resource>& Resources() const { return resources_; }
    const std::vector<Pass>& Passes() const { return passes_; }
    const CompiledGraph& Compiled() const { return compiled_; }
    uint32_t CompileCount() const { return compile_count_; }

    // Clears declarations for the next frame; keeps the compile cache.
    void Reset();

    // Graphviz dump: passes, resources, read/write edges, culled passes dashed.
    std::string ToDot() const;

private:
    uint64_t HashDeclarations() const;
    std::vector<Resource> resources_;
    std::vector<Pass> passes_;
    CompiledGraph compiled_;
    uint32_t compile_count_ = 0;
};

}  // namespace engine::rg
