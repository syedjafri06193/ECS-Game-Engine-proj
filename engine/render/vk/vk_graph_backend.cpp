#include "render/vk/vk_graph_backend.h"
#include <algorithm>

namespace engine::vk {

using namespace engine::rg;

VkFormat ToVk(Format f)
{
    switch (f) {
    case Format::RGBA8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
    case Format::RGBA8_SRGB:  return VK_FORMAT_R8G8B8A8_SRGB;
    case Format::BGRA8_SRGB:  return VK_FORMAT_B8G8R8A8_SRGB;
    case Format::RGBA16F:     return VK_FORMAT_R16G16B16A16_SFLOAT;
    case Format::RG16F:       return VK_FORMAT_R16G16_SFLOAT;
    case Format::R32F:        return VK_FORMAT_R32_SFLOAT;
    case Format::R32_UINT:    return VK_FORMAT_R32_UINT;
    case Format::D32F:        return VK_FORMAT_D32_SFLOAT;
    }
    return VK_FORMAT_UNDEFINED;
}

VkPipelineStageFlags2 ToVkStages(uint32_t s)
{
    VkPipelineStageFlags2 r = VK_PIPELINE_STAGE_2_NONE;
    if (s & StageDrawIndirect)   r |= VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
    if (s & StageVertexShader)   r |= VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
    if (s & StageFragmentShader) r |= VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    if (s & StageEarlyFragTests) r |= VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT;
    if (s & StageLateFragTests)  r |= VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    if (s & StageColorOutput)    r |= VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    if (s & StageComputeShader)  r |= VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    if (s & StageTransfer)       r |= VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    // BottomOfPipe as a destination (present) means "nothing waits": NONE.
    return r;
}

VkAccessFlags2 ToVkAccess(uint32_t a)
{
    VkAccessFlags2 r = VK_ACCESS_2_NONE;
    if (a & AccessIndirectRead)       r |= VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
    if (a & AccessUniformRead)        r |= VK_ACCESS_2_UNIFORM_READ_BIT;
    if (a & AccessShaderSampledRead)  r |= VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    if (a & AccessShaderStorageRead)  r |= VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    if (a & AccessShaderStorageWrite) r |= VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    if (a & AccessColorRead)          r |= VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT;
    if (a & AccessColorWrite)         r |= VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    if (a & AccessDepthRead)          r |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    if (a & AccessDepthWrite)         r |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    if (a & AccessTransferRead)       r |= VK_ACCESS_2_TRANSFER_READ_BIT;
    if (a & AccessTransferWrite)      r |= VK_ACCESS_2_TRANSFER_WRITE_BIT;
    return r;
}

VkImageLayout ToVk(Layout l)
{
    switch (l) {
    case Layout::Undefined:       return VK_IMAGE_LAYOUT_UNDEFINED;
    case Layout::General:         return VK_IMAGE_LAYOUT_GENERAL;
    case Layout::ColorAttachment: return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    case Layout::DepthAttachment: return VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    case Layout::DepthReadOnly:   return VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
    case Layout::ShaderReadOnly:  return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    case Layout::TransferSrc:     return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    case Layout::TransferDst:     return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    case Layout::Present:         return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    }
    return VK_IMAGE_LAYOUT_UNDEFINED;
}

void GraphBackend::SetImportedImage(ResourceId id, VkImage image, VkImageView view, VkImageAspectFlags aspect)
{
    if (resources_.size() <= id) resources_.resize(size_t(id) + 1);
    resources_[id].image = image;
    resources_[id].view = view;
    resources_[id].aspect = aspect;
}

void GraphBackend::SetImportedBuffer(ResourceId id, VkBuffer buffer, VkDeviceAddress address)
{
    if (resources_.size() <= id) resources_.resize(size_t(id) + 1);
    resources_[id].buffer = buffer;
    resources_[id].address = address;
}

void GraphBackend::Destroy()
{
    VkDevice dev = ctx_.Device();
    if (!dev) return;
    for (VkImageView v : owned_views_) vkDestroyImageView(dev, v, nullptr);
    for (VkImage i : owned_images_) vkDestroyImage(dev, i, nullptr);
    for (VkBuffer b : owned_buffers_) vkDestroyBuffer(dev, b, nullptr);
    for (VmaAllocation a : slot_memory_) vmaFreeMemory(ctx_.Allocator(), a);
    owned_views_.clear();
    owned_images_.clear();
    owned_buffers_.clear();
    slot_memory_.clear();
    built_hash_ = 0;
}

void GraphBackend::Prepare(const RenderGraph& graph, VkCommandBuffer cmd)
{
    graph_ = &graph;
    const CompiledGraph& cg = graph.Compiled();
    VkDevice dev = ctx_.Device();
    const auto& decl = graph.Resources();
    if (resources_.size() < decl.size()) resources_.resize(decl.size());

    // ---- timestamps from the last time this frame slot ran (its fence has signalled)
    FrameResources& fr = ctx_.Frame();
    auto& names = recorded_[ctx_.FrameIndex()];
    if (!names.empty()) {
        std::vector<uint64_t> ts(names.size() * 2);
        if (vkGetQueryPoolResults(dev, fr.timestamps, 0, uint32_t(ts.size()), ts.size() * sizeof(uint64_t), ts.data(),
                                  sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            timings_.clear();
            for (size_t i = 0; i < names.size(); ++i)
                timings_.emplace_back(names[i], double(ts[2 * i + 1] - ts[2 * i]) * ctx_.TimestampPeriodNs() * 1e-6);
        }
    }
    names.clear();
    query_ = 0;
    vkCmdResetQueryPool(cmd, fr.timestamps, 0, 64);

    // ---- transient resources: rebuild only when the graph shape or size changed
    if (cg.hash == built_hash_ && built_extent_.width == ctx_.Extent().width && built_extent_.height == ctx_.Extent().height)
        return;
    vkDeviceWaitIdle(dev);                        // old transients may still be in use
    Destroy();

    // Usage flags from every declared access.
    std::vector<VkImageUsageFlags> usage(decl.size(), 0);
    std::vector<VkBufferUsageFlags> busage(decl.size(), 0);
    for (const auto& p : graph.Passes())
        for (const auto& a : p.accesses) {
            const uint32_t acc = a.state.access;
            if (acc & (AccessColorRead | AccessColorWrite)) usage[a.resource] |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            if (acc & (AccessDepthRead | AccessDepthWrite)) usage[a.resource] |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
            if (acc & AccessShaderSampledRead) usage[a.resource] |= VK_IMAGE_USAGE_SAMPLED_BIT;
            if (acc & (AccessShaderStorageRead | AccessShaderStorageWrite)) {
                usage[a.resource] |= VK_IMAGE_USAGE_STORAGE_BIT;
                busage[a.resource] |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            }
            if (acc & AccessTransferRead) { usage[a.resource] |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT; busage[a.resource] |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT; }
            if (acc & AccessTransferWrite) { usage[a.resource] |= VK_IMAGE_USAGE_TRANSFER_DST_BIT; busage[a.resource] |= VK_BUFFER_USAGE_TRANSFER_DST_BIT; }
            if (acc & AccessIndirectRead) busage[a.resource] |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
            if (acc & AccessUniformRead) busage[a.resource] |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        }

    // Create unbound objects, gather requirements per slot, allocate one block
    // per slot, then bind every resource of the slot to it (aliasing, §7.2).
    const size_t nslots = cg.slot_sizes.size();
    std::vector<VkMemoryRequirements> slot_req(nslots, VkMemoryRequirements{0, 1, ~0u});
    for (ResourceId r = 0; r < decl.size(); ++r) {
        if (decl[r].imported || cg.memory_slot[r] < 0) continue;
        GraphResource& gr = resources_[r];
        VkMemoryRequirements req;
        if (decl[r].is_texture) {
            const auto& t = decl[r].texture;
            VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            ii.imageType = VK_IMAGE_TYPE_2D;
            ii.format = ToVk(t.format);
            // Width/height of 0 means "swapchain size".
            ii.extent = {t.width ? t.width : ctx_.Extent().width, t.height ? t.height : ctx_.Extent().height, 1};
            ii.mipLevels = 1;
            ii.arrayLayers = 1;
            ii.samples = VK_SAMPLE_COUNT_1_BIT;
            ii.tiling = VK_IMAGE_TILING_OPTIMAL;
            ii.usage = usage[r];
            VK_CHECK(vkCreateImage(dev, &ii, nullptr, &gr.image));
            owned_images_.push_back(gr.image);
            gr.aspect = IsDepth(t.format) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
            vkGetImageMemoryRequirements(dev, gr.image, &req);
        } else {
            VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bi.size = decl[r].buffer.size;
            bi.usage = busage[r] | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
            VK_CHECK(vkCreateBuffer(dev, &bi, nullptr, &gr.buffer));
            owned_buffers_.push_back(gr.buffer);
            vkGetBufferMemoryRequirements(dev, gr.buffer, &req);
        }
        auto& s = slot_req[size_t(cg.memory_slot[r])];
        s.size = std::max(s.size, req.size);
        s.alignment = std::max(s.alignment, req.alignment);
        s.memoryTypeBits &= req.memoryTypeBits;
    }
    aliased_bytes_ = unaliased_bytes_ = 0;
    for (size_t s = 0; s < nslots; ++s) {
        VmaAllocationCreateInfo ai{};
        ai.preferredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        VmaAllocation alloc = nullptr;
        VK_CHECK(vmaAllocateMemory(ctx_.Allocator(), &slot_req[s], &ai, &alloc, nullptr));
        slot_memory_.push_back(alloc);
        aliased_bytes_ += slot_req[s].size;
    }
    for (ResourceId r = 0; r < decl.size(); ++r) {
        if (decl[r].imported || cg.memory_slot[r] < 0) continue;
        GraphResource& gr = resources_[r];
        VmaAllocation alloc = slot_memory_[size_t(cg.memory_slot[r])];
        if (decl[r].is_texture) {
            VK_CHECK(vmaBindImageMemory(ctx_.Allocator(), alloc, gr.image));
            VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vi.image = gr.image;
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = ToVk(decl[r].texture.format);
            vi.subresourceRange = {gr.aspect, 0, 1, 0, 1};
            VK_CHECK(vkCreateImageView(dev, &vi, nullptr, &gr.view));
            owned_views_.push_back(gr.view);
            if (usage[r] & VK_IMAGE_USAGE_SAMPLED_BIT) {
                auto it = bindless_by_name_.find(decl[r].name);
                if (it == bindless_by_name_.end()) it = bindless_by_name_.emplace(decl[r].name, ctx_.RegisterTexture(gr.view)).first;
                else ctx_.UpdateTexture(it->second, gr.view);
                gr.bindless = it->second;
            }
        } else {
            VK_CHECK(vmaBindBufferMemory(ctx_.Allocator(), alloc, gr.buffer));
            VkBufferDeviceAddressInfo da{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
            da.buffer = gr.buffer;
            gr.address = vkGetBufferDeviceAddress(dev, &da);
        }
        unaliased_bytes_ += decl[r].Bytes();
    }
    built_hash_ = cg.hash;
    built_extent_ = ctx_.Extent();
}

void GraphBackend::Barriers(std::span<const Barrier> barriers, PassContext& ctx)
{
    std::vector<VkImageMemoryBarrier2> images;
    std::vector<VkBufferMemoryBarrier2> buffers;
    const auto& decl = graph_->Resources();
    for (const Barrier& b : barriers) {
        const GraphResource& r = resources_[b.resource];
        if (decl[b.resource].is_texture) {
            VkImageMemoryBarrier2 ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            ib.srcStageMask = ToVkStages(b.before.stages);
            ib.srcAccessMask = ToVkAccess(b.before.access);
            ib.dstStageMask = ToVkStages(b.after.stages);
            ib.dstAccessMask = ToVkAccess(b.after.access);
            ib.oldLayout = ToVk(b.before.layout);
            ib.newLayout = ToVk(b.after.layout);
            ib.image = r.image;
            ib.subresourceRange = {r.aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
            images.push_back(ib);
        } else {
            VkBufferMemoryBarrier2 bb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
            bb.srcStageMask = ToVkStages(b.before.stages);
            bb.srcAccessMask = ToVkAccess(b.before.access);
            bb.dstStageMask = ToVkStages(b.after.stages);
            bb.dstAccessMask = ToVkAccess(b.after.access);
            bb.buffer = r.buffer;
            bb.size = VK_WHOLE_SIZE;
            buffers.push_back(bb);
        }
    }
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = uint32_t(images.size());
    dep.pImageMemoryBarriers = images.data();
    dep.bufferMemoryBarrierCount = uint32_t(buffers.size());
    dep.pBufferMemoryBarriers = buffers.data();
    vkCmdPipelineBarrier2(static_cast<VkCommandBuffer>(ctx.native_command_buffer), &dep);
}

void GraphBackend::BeginPass(uint32_t pass, PassContext& ctx)
{
    auto cmd = static_cast<VkCommandBuffer>(ctx.native_command_buffer);
    const std::string& name = graph_->Passes()[pass].name;
    if (vkCmdBeginDebugUtilsLabelEXT) {           // RenderDoc / validation show pass names
        VkDebugUtilsLabelEXT label{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
        label.pLabelName = name.c_str();
        vkCmdBeginDebugUtilsLabelEXT(cmd, &label);
    }
    if (query_ + 2 <= 64) {
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, ctx_.Frame().timestamps, query_);
        recorded_[ctx_.FrameIndex()].push_back(name);
    }
}

void GraphBackend::EndPass(uint32_t, PassContext& ctx)
{
    auto cmd = static_cast<VkCommandBuffer>(ctx.native_command_buffer);
    if (query_ + 2 <= 64) {
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, ctx_.Frame().timestamps, query_ + 1);
        query_ += 2;
    }
    if (vkCmdEndDebugUtilsLabelEXT) vkCmdEndDebugUtilsLabelEXT(cmd);
}

}  // namespace engine::vk
