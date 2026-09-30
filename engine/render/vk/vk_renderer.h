#pragma once
// Clustered forward renderer (design doc §6, §7, §8). Consumes a RenderPacket
// - it never touches the World - and builds the frame as a render graph:
//
//   depth-prepass -> light-culling (compute) -> forward -> tonemap -> ui
//
// Draws are data: instances, lights and materials live in buffers reached by
// device address; textures through the bindless array.
#include <functional>
#include <string>
#include "render/extract.h"
#include "render/graph/render_graph.h"
#include "render/vk/vk_context.h"
#include "render/vk/vk_graph_backend.h"

namespace engine::vk {

struct RendererStats {
    uint32_t draw_calls = 0, instances = 0, lights = 0, passes = 0, culled_passes = 0, barriers = 0;
    uint32_t graph_compiles = 0;
};

class Renderer {
public:
    bool Init(Context& ctx, const std::string& shader_dir);
    void Shutdown();

    // Records the frame into ctx.Frame().cmd. `ui` draws into the swapchain.
    void Render(const RenderPacket& packet, const std::function<void(VkCommandBuffer)>& ui);

    const RendererStats& Stats() const { return stats_; }
    const GraphBackend& Backend() const { return *backend_; }
    std::string GraphDot() const { return graph_.ToDot(); }

    static constexpr uint32_t kMeshCube = 0, kMeshPlane = 1;
    static constexpr uint32_t kMaterialCount = 4;

private:
    struct MeshRange { uint32_t first_index, index_count; int32_t vertex_offset; };

    VkPipeline MakeGraphics(VkShaderModule vs, VkShaderModule fs, VkFormat color, VkFormat depth,
                            VkCompareOp depth_op, bool depth_write, bool blend);
    void DrawRuns(VkCommandBuffer cmd, const std::vector<DrawItem>& items, uint32_t first_instance);

    Context* ctx_ = nullptr;
    std::unique_ptr<GraphBackend> backend_;
    rg::RenderGraph graph_;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline depth_ = VK_NULL_HANDLE, forward_ = VK_NULL_HANDLE, transparent_ = VK_NULL_HANDLE;
    VkPipeline tonemap_ = VK_NULL_HANDLE, cull_ = VK_NULL_HANDLE;
    VkFormat tonemap_format_ = VK_FORMAT_UNDEFINED;
    std::string shader_dir_;

    Buffer vertices_, indices_, materials_;
    Buffer clusters_[kFramesInFlight];              // one per frame in flight: no cross-frame race
    std::vector<MeshRange> meshes_;
    std::vector<Image> textures_;
    RendererStats stats_;
};

}  // namespace engine::vk
