#pragma once
// Executes a compiled render graph on Vulkan: creates transient images and
// buffers (aliased per memory slot through VMA), turns graph barriers into
// vkCmdPipelineBarrier2, and brackets every pass with GPU timestamps so
// per-pass cost is visible continuously (§9.5).
#include <string>
#include <unordered_map>
#include <vector>
#include "render/graph/render_graph.h"
#include "render/vk/vk_context.h"

namespace engine::vk {

VkFormat ToVk(rg::Format f);
VkPipelineStageFlags2 ToVkStages(uint32_t stages);
VkAccessFlags2 ToVkAccess(uint32_t access);
VkImageLayout ToVk(rg::Layout l);

struct GraphResource {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceAddress address = 0;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    uint32_t bindless = ~0u;           // for sampled transient textures
};

class GraphBackend final : public rg::Backend {
public:
    explicit GraphBackend(Context& ctx) : ctx_(ctx) {}
    ~GraphBackend() override { Destroy(); }

    void SetImportedImage(rg::ResourceId id, VkImage image, VkImageView view, VkImageAspectFlags aspect);
    void SetImportedBuffer(rg::ResourceId id, VkBuffer buffer, VkDeviceAddress address);

    // Creates/recreates transients when the compiled graph changed, reads the
    // previous use of this frame slot's timestamps, resets the query pool.
    void Prepare(const rg::RenderGraph& graph, VkCommandBuffer cmd);

    const GraphResource& Resource(rg::ResourceId id) const { return resources_[id]; }

    void Barriers(std::span<const rg::Barrier> barriers, rg::PassContext& ctx) override;
    void BeginPass(uint32_t pass, rg::PassContext& ctx) override;
    void EndPass(uint32_t pass, rg::PassContext& ctx) override;

    // Most recent GPU time per pass name, in milliseconds.
    const std::vector<std::pair<std::string, double>>& PassTimings() const { return timings_; }
    uint64_t AliasedBytes() const { return aliased_bytes_; }
    uint64_t UnaliasedBytes() const { return unaliased_bytes_; }
    void Destroy();

private:
    Context& ctx_;
    const rg::RenderGraph* graph_ = nullptr;
    uint64_t built_hash_ = 0;
    VkExtent2D built_extent_{};
    std::vector<GraphResource> resources_;
    std::vector<VmaAllocation> slot_memory_;
    std::vector<VkImage> owned_images_;
    std::vector<VkImageView> owned_views_;
    std::vector<VkBuffer> owned_buffers_;
    std::unordered_map<std::string, uint32_t> bindless_by_name_;   // stable slots across rebuilds
    uint64_t aliased_bytes_ = 0, unaliased_bytes_ = 0;

    // timestamps: per frame slot, the pass names recorded
    std::vector<std::string> recorded_[kFramesInFlight];
    std::vector<std::pair<std::string, double>> timings_;
    uint32_t query_ = 0;
};

}  // namespace engine::vk
