#include "render/vk/vk_context.h"
#include <cstring>
#include <fstream>
#include <iterator>
#include <VkBootstrap.h>
#ifndef GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_NONE
#endif
#include <GLFW/glfw3.h>

// glfwCreateWindowSurface is declared only when Vulkan headers are included
// before GLFW; declare it directly against volk's types instead.
extern "C" VkResult glfwCreateWindowSurface(VkInstance, GLFWwindow*, const VkAllocationCallbacks*, VkSurfaceKHR*);

namespace engine::vk {

void* FrameArena::Allocate(VkDeviceSize size, VkDeviceAddress* address, VkDeviceSize align)
{
    const VkDeviceSize start = (offset + align - 1) & ~(align - 1);
    if (start + size > buffer.size) return nullptr;
    offset = start + size;
    high_water = offset > high_water ? offset : high_water;
    if (address) *address = buffer.address + start;
    return static_cast<std::byte*>(buffer.mapped) + start;
}

bool Context::Init(GLFWwindow* window, const ContextDesc& desc)
{
    window_ = window;
    desc_ = desc;
    if (volkInitialize() != VK_SUCCESS) {
        std::fprintf(stderr, "No Vulkan loader found\n");
        return false;
    }

    vkb::InstanceBuilder ib(vkGetInstanceProcAddr);
    ib.set_app_name(desc.app_name.c_str()).require_api_version(1, 3, 0);
    if (desc.validation) {
        // §6.6: validation on, and synchronization validation too - it
        // catches the barrier mistakes that are otherwise invisible.
        ib.request_validation_layers(true)
            .use_default_debug_messenger()
            .add_validation_feature_enable(VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT);
    }
    auto inst = ib.build();
    if (!inst) {
        std::fprintf(stderr, "Vulkan instance: %s\n", inst.error().message().c_str());
        return false;
    }
    instance_ = inst->instance;
    messenger_ = inst->debug_messenger;
    volkLoadInstance(instance_);

    VK_CHECK(glfwCreateWindowSurface(instance_, window_, nullptr, &surface_));

    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.bufferDeviceAddress = VK_TRUE;
    f12.descriptorIndexing = VK_TRUE;
    f12.runtimeDescriptorArray = VK_TRUE;
    f12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    f12.descriptorBindingPartiallyBound = VK_TRUE;
    f12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    f12.descriptorBindingVariableDescriptorCount = VK_TRUE;
    f12.timelineSemaphore = VK_TRUE;
    f12.scalarBlockLayout = VK_TRUE;
    f12.hostQueryReset = VK_TRUE;

    vkb::PhysicalDeviceSelector sel(*inst);
    auto phys = sel.set_surface(surface_)
                    .set_minimum_version(1, 3)
                    .set_required_features_13(f13)
                    .set_required_features_12(f12)
                    .prefer_gpu_device_type(vkb::PreferredDeviceType::discrete)
                    .select();
    if (!phys) {
        std::fprintf(stderr, "No suitable GPU (needs Vulkan 1.3 + bindless): %s\n", phys.error().message().c_str());
        return false;
    }
    physical_ = phys->physical_device;
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physical_, &props);
    timestamp_period_ = props.limits.timestampPeriod;
    std::printf("GPU: %s\n", props.deviceName);

    vkb::DeviceBuilder db(*phys);
    auto dev = db.build();
    if (!dev) {
        std::fprintf(stderr, "Vulkan device: %s\n", dev.error().message().c_str());
        return false;
    }
    device_ = dev->device;
    volkLoadDevice(device_);
    queue_ = dev->get_queue(vkb::QueueType::graphics).value();
    queue_family_ = dev->get_queue_index(vkb::QueueType::graphics).value();

    VmaVulkanFunctions funcs{};
    funcs.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    funcs.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    VmaAllocatorCreateInfo aci{};
    aci.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    aci.vulkanApiVersion = VK_API_VERSION_1_3;
    aci.physicalDevice = physical_;
    aci.device = device_;
    aci.instance = instance_;
    aci.pVulkanFunctions = &funcs;
    VK_CHECK(vmaCreateAllocator(&aci, &allocator_));

    // Pipeline cache, persisted between runs (§6.5). Drivers validate the
    // header themselves and ignore data from another GPU or driver version.
    std::vector<char> cache_data;
    if (std::ifstream f{desc.pipeline_cache_path, std::ios::binary}) cache_data.assign(std::istreambuf_iterator<char>(f), {});
    VkPipelineCacheCreateInfo pci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    pci.initialDataSize = cache_data.size();
    pci.pInitialData = cache_data.empty() ? nullptr : cache_data.data();
    VK_CHECK(vkCreatePipelineCache(device_, &pci, nullptr, &pipeline_cache_));

    // Frames in flight (§6.3).
    for (auto& fr : frames_) {
        VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        cp.queueFamilyIndex = queue_family_;
        VK_CHECK(vkCreateCommandPool(device_, &cp, nullptr, &fr.pool));
        VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ca.commandPool = fr.pool;
        ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ca.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(device_, &ca, &fr.cmd));
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK(vkCreateSemaphore(device_, &si, nullptr, &fr.image_available));
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VK_CHECK(vkCreateFence(device_, &fi, nullptr, &fr.in_flight));
        fr.arena.buffer = CreateBuffer(desc.frame_arena_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = 64;
        VK_CHECK(vkCreateQueryPool(device_, &qi, nullptr, &fr.timestamps));
        vkResetQueryPool(device_, fr.timestamps, 0, 64);
    }
    VkCommandPoolCreateInfo up{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    up.queueFamilyIndex = queue_family_;
    up.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    VK_CHECK(vkCreateCommandPool(device_, &up, nullptr, &upload_pool_));

    // Bindless texture set (§6.2): one unbounded array, partially bound,
    // updatable after bind, bound once per frame.
    const VkDescriptorBindingFlags bflags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                                            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                                            VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT;
    VkDescriptorSetLayoutBindingFlagsCreateInfo bfi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    bfi.bindingCount = 1;
    bfi.pBindingFlags = &bflags;
    VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxBindlessTextures, VK_SHADER_STAGE_ALL, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.pNext = &bfi;
    li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    li.bindingCount = 1;
    li.pBindings = &binding;
    VK_CHECK(vkCreateDescriptorSetLayout(device_, &li, nullptr, &bindless_layout_));
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxBindlessTextures};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    pi.maxSets = 1;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &ps;
    VK_CHECK(vkCreateDescriptorPool(device_, &pi, nullptr, &bindless_pool_));
    const uint32_t count = kMaxBindlessTextures;
    VkDescriptorSetVariableDescriptorCountAllocateInfo vc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO};
    vc.descriptorSetCount = 1;
    vc.pDescriptorCounts = &count;
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.pNext = &vc;
    ai.descriptorPool = bindless_pool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &bindless_layout_;
    VK_CHECK(vkAllocateDescriptorSets(device_, &ai, &bindless_set_));
    VkSamplerCreateInfo smp{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    smp.magFilter = smp.minFilter = VK_FILTER_LINEAR;
    smp.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    smp.addressModeU = smp.addressModeV = smp.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    smp.maxLod = VK_LOD_CLAMP_NONE;
    VK_CHECK(vkCreateSampler(device_, &smp, nullptr, &sampler_));

    CreateSwapchain();
    return true;
}

void Context::CreateSwapchain()
{
    int w = 0, h = 0;
    glfwGetFramebufferSize(window_, &w, &h);
    vkb::SwapchainBuilder sb(physical_, device_, surface_, queue_family_, queue_family_);
    auto sc = sb.set_desired_format({VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR})
                  .set_desired_present_mode(desc_.vsync ? VK_PRESENT_MODE_FIFO_KHR : VK_PRESENT_MODE_MAILBOX_KHR)
                  .set_desired_extent(uint32_t(w), uint32_t(h))
                  .add_image_usage_flags(VK_IMAGE_USAGE_TRANSFER_DST_BIT)
                  .set_old_swapchain(swapchain_)
                  .build();
    if (!sc) {
        std::fprintf(stderr, "swapchain: %s\n", sc.error().message().c_str());
        std::abort();
    }
    if (swapchain_) {
        for (auto v : swapchain_views_) vkDestroyImageView(device_, v, nullptr);
        vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    }
    swapchain_ = sc->swapchain;
    swapchain_format_ = sc->image_format;
    extent_ = sc->extent;
    swapchain_images_ = sc->get_images().value();
    swapchain_views_ = sc->get_image_views().value();
    for (auto s : render_finished_) vkDestroySemaphore(device_, s, nullptr);
    render_finished_.resize(swapchain_images_.size());
    // One present semaphore per swapchain IMAGE: reusing a per-frame one
    // races the presentation engine.
    for (auto& s : render_finished_) {
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK(vkCreateSemaphore(device_, &si, nullptr, &s));
    }
}

void Context::DestroySwapchain()
{
    for (auto v : swapchain_views_) vkDestroyImageView(device_, v, nullptr);
    for (auto s : render_finished_) vkDestroySemaphore(device_, s, nullptr);
    swapchain_views_.clear();
    render_finished_.clear();
    if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
}

void Context::RecreateSwapchain()
{
    int w = 0, h = 0;
    glfwGetFramebufferSize(window_, &w, &h);
    while (w == 0 || h == 0) {                       // minimised
        glfwWaitEvents();
        glfwGetFramebufferSize(window_, &w, &h);
    }
    vkDeviceWaitIdle(device_);
    CreateSwapchain();
}

bool Context::BeginFrame()
{
    FrameResources& fr = frames_[frame_index_];
    VK_CHECK(vkWaitForFences(device_, 1, &fr.in_flight, VK_TRUE, UINT64_MAX));
    const VkResult r = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, fr.image_available, VK_NULL_HANDLE, &image_index_);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        RecreateSwapchain();
        return false;
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) VK_CHECK(r);
    VK_CHECK(vkResetFences(device_, 1, &fr.in_flight));      // only after a successful acquire
    fr.arena.Reset();
    VK_CHECK(vkResetCommandPool(device_, fr.pool, 0));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(fr.cmd, &bi));
    ++frame_number_;
    return true;
}

void Context::EndFrame()
{
    FrameResources& fr = frames_[frame_index_];
    VK_CHECK(vkEndCommandBuffer(fr.cmd));
    VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    wait.semaphore = fr.image_available;
    wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signal.semaphore = render_finished_[image_index_];
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkCommandBufferSubmitInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cb.commandBuffer = fr.cmd;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.waitSemaphoreInfoCount = 1;
    si.pWaitSemaphoreInfos = &wait;
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cb;
    si.signalSemaphoreInfoCount = 1;
    si.pSignalSemaphoreInfos = &signal;
    VK_CHECK(vkQueueSubmit2(queue_, 1, &si, fr.in_flight));

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &render_finished_[image_index_];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &image_index_;
    const VkResult r = vkQueuePresentKHR(queue_, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) RecreateSwapchain();
    else VK_CHECK(r);
    frame_index_ = (frame_index_ + 1) % kFramesInFlight;
}

// ---------------------------------------------------------------- resources

Buffer Context::CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host_visible)
{
    Buffer b;
    b.size = size;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    if (host_visible) ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info{};
    VK_CHECK(vmaCreateBuffer(allocator_, &bi, &ai, &b.buffer, &b.allocation, &info));
    b.mapped = info.pMappedData;
    VkBufferDeviceAddressInfo da{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    da.buffer = b.buffer;
    b.address = vkGetBufferDeviceAddress(device_, &da);
    return b;
}

void Context::DestroyBuffer(Buffer& b)
{
    if (b.buffer) vmaDestroyBuffer(allocator_, b.buffer, b.allocation);
    b = {};
}

Image Context::CreateImage(VkExtent2D extent, VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect)
{
    Image img;
    img.format = format;
    img.extent = extent;
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = format;
    ii.extent = {extent.width, extent.height, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    VK_CHECK(vmaCreateImage(allocator_, &ii, &ai, &img.image, &img.allocation, nullptr));
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = {aspect, 0, 1, 0, 1};
    VK_CHECK(vkCreateImageView(device_, &vi, nullptr, &img.view));
    return img;
}

void Context::DestroyImage(Image& i)
{
    if (i.view) vkDestroyImageView(device_, i.view, nullptr);
    if (i.image) vmaDestroyImage(allocator_, i.image, i.allocation);
    i = {};
}

namespace {
VkCommandBuffer BeginOneShot(VkDevice dev, VkCommandPool pool)
{
    VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ca.commandPool = pool;
    ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ca.commandBufferCount = 1;
    VkCommandBuffer cmd;
    VK_CHECK(vkAllocateCommandBuffers(dev, &ca, &cmd));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
    return cmd;
}
void EndOneShot(VkDevice dev, VkQueue q, VkCommandPool pool, VkCommandBuffer cmd)
{
    VK_CHECK(vkEndCommandBuffer(cmd));
    VkCommandBufferSubmitInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cb.commandBuffer = cmd;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cb;
    VK_CHECK(vkQueueSubmit2(q, 1, &si, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(q));
    vkFreeCommandBuffers(dev, pool, 1, &cmd);
}
}  // namespace

void Context::UploadBuffer(Buffer& dst, const void* data, VkDeviceSize size)
{
    Buffer staging = CreateBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    std::memcpy(staging.mapped, data, size);
    VkCommandBuffer cmd = BeginOneShot(device_, upload_pool_);
    VkBufferCopy copy{0, 0, size};
    vkCmdCopyBuffer(cmd, staging.buffer, dst.buffer, 1, &copy);
    EndOneShot(device_, queue_, upload_pool_, cmd);
    DestroyBuffer(staging);
}

Image Context::UploadTexture(uint32_t w, uint32_t h, const uint32_t* rgba8)
{
    Image img = CreateImage({w, h}, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            VK_IMAGE_ASPECT_COLOR_BIT);
    const VkDeviceSize size = VkDeviceSize(w) * h * 4;
    Buffer staging = CreateBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    std::memcpy(staging.mapped, rgba8, size);
    VkCommandBuffer cmd = BeginOneShot(device_, upload_pool_);

    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
    b.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.image = img.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {w, h, 1};
    vkCmdCopyBufferToImage(cmd, staging.buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier2(cmd, &dep);
    EndOneShot(device_, queue_, upload_pool_, cmd);
    DestroyBuffer(staging);
    return img;
}

uint32_t Context::RegisterTexture(VkImageView view)
{
    const uint32_t index = next_texture_++;
    UpdateTexture(index, view);
    return index;
}

void Context::UpdateTexture(uint32_t index, VkImageView view)
{
    VkDescriptorImageInfo ii{sampler_, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = bindless_set_;
    w.dstBinding = 0;
    w.dstArrayElement = index;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
}

VkShaderModule Context::LoadShader(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "missing shader %s\n", path.c_str());
        std::abort();
    }
    std::vector<char> code((std::istreambuf_iterator<char>(f)), {});
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = code.size();
    ci.pCode = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule m;
    VK_CHECK(vkCreateShaderModule(device_, &ci, nullptr, &m));
    return m;
}

void Context::SavePipelineCache()
{
    size_t size = 0;
    if (vkGetPipelineCacheData(device_, pipeline_cache_, &size, nullptr) != VK_SUCCESS || size == 0) return;
    std::vector<char> data(size);
    if (vkGetPipelineCacheData(device_, pipeline_cache_, &size, data.data()) != VK_SUCCESS) return;
    std::ofstream(desc_.pipeline_cache_path, std::ios::binary).write(data.data(), std::streamsize(size));
}

void Context::Shutdown()
{
    if (!device_) return;
    vkDeviceWaitIdle(device_);
    SavePipelineCache();
    for (auto& fr : frames_) {
        DestroyBuffer(fr.arena.buffer);
        vkDestroyQueryPool(device_, fr.timestamps, nullptr);
        vkDestroyFence(device_, fr.in_flight, nullptr);
        vkDestroySemaphore(device_, fr.image_available, nullptr);
        vkDestroyCommandPool(device_, fr.pool, nullptr);
    }
    vkDestroyCommandPool(device_, upload_pool_, nullptr);
    vkDestroySampler(device_, sampler_, nullptr);
    vkDestroyDescriptorPool(device_, bindless_pool_, nullptr);
    vkDestroyDescriptorSetLayout(device_, bindless_layout_, nullptr);
    vkDestroyPipelineCache(device_, pipeline_cache_, nullptr);
    DestroySwapchain();
    vmaDestroyAllocator(allocator_);
    vkDestroyDevice(device_, nullptr);
    vkDestroySurfaceKHR(instance_, surface_, nullptr);
    if (messenger_) vkb::destroy_debug_utils_messenger(instance_, messenger_);
    vkDestroyInstance(instance_, nullptr);
    device_ = VK_NULL_HANDLE;
}

}  // namespace engine::vk
