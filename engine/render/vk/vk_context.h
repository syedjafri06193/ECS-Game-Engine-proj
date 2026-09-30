#pragma once
#include <string>
#include <vector>
#include "render/vk/vk_common.h"

struct GLFWwindow;

namespace engine::vk {

// Per-frame scratch for transient uploads (instances, lights, UI): bump
// allocate, reset when the frame's fence signals. No per-allocation lifetime
// tracking (§6.3).
struct FrameArena {
    Buffer   buffer;
    VkDeviceSize offset = 0;
    VkDeviceSize high_water = 0;

    // Returns host pointer + device address, or null if full.
    void* Allocate(VkDeviceSize size, VkDeviceAddress* address, VkDeviceSize align = 16);
    void  Reset() { offset = 0; }
};

struct FrameResources {
    VkCommandPool   pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkSemaphore     image_available = VK_NULL_HANDLE;
    VkFence         in_flight = VK_NULL_HANDLE;
    FrameArena      arena;
    VkQueryPool     timestamps = VK_NULL_HANDLE;
    uint32_t        timestamp_count = 0;
};

struct ContextDesc {
    std::string app_name = "ECS Engine";
    bool validation = true;          // §6.6: always on in development
    bool vsync = true;
    VkDeviceSize frame_arena_bytes = 32u << 20;
    std::string pipeline_cache_path = "pipeline_cache.bin";
};

class Context {
public:
    bool Init(GLFWwindow* window, const ContextDesc& desc);
    void Shutdown();

    // Frame loop. BeginFrame waits for the frame's fence, resets its arena and
    // acquires a swapchain image; returns false if the swapchain was recreated.
    bool BeginFrame();
    void EndFrame();                  // submit + present
    void WaitIdle() { if (device_) vkDeviceWaitIdle(device_); }
    void RecreateSwapchain();

    // Resources
    Buffer CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host_visible);
    void   DestroyBuffer(Buffer& b);
    Image  CreateImage(VkExtent2D extent, VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect);
    void   DestroyImage(Image& i);
    // Uploads through a staging buffer and waits (load time only).
    void   UploadBuffer(Buffer& dst, const void* data, VkDeviceSize size);
    Image  UploadTexture(uint32_t w, uint32_t h, const uint32_t* rgba8);

    // Bindless: one descriptor set for every sampled texture (§6.2).
    uint32_t RegisterTexture(VkImageView view);             // returns the array index
    void     UpdateTexture(uint32_t index, VkImageView view);
    VkDescriptorSet BindlessSet() const { return bindless_set_; }
    VkDescriptorSetLayout BindlessLayout() const { return bindless_layout_; }

    VkShaderModule LoadShader(const std::string& spv_path);
    void SavePipelineCache();

    // Accessors
    VkDevice Device() const { return device_; }
    VkPhysicalDevice Physical() const { return physical_; }
    VkInstance Instance() const { return instance_; }
    VkQueue Queue() const { return queue_; }
    uint32_t QueueFamily() const { return queue_family_; }
    VmaAllocator Allocator() const { return allocator_; }
    VkPipelineCache PipelineCache() const { return pipeline_cache_; }
    FrameResources& Frame() { return frames_[frame_index_]; }
    uint32_t FrameIndex() const { return frame_index_; }
    uint64_t FrameNumber() const { return frame_number_; }
    VkImage SwapchainImage() const { return swapchain_images_[image_index_]; }
    VkImageView SwapchainView() const { return swapchain_views_[image_index_]; }
    VkFormat SwapchainFormat() const { return swapchain_format_; }
    VkExtent2D Extent() const { return extent_; }
    uint32_t ImageCount() const { return uint32_t(swapchain_images_.size()); }
    float TimestampPeriodNs() const { return timestamp_period_; }

private:
    void CreateSwapchain();
    void DestroySwapchain();

    GLFWwindow* window_ = nullptr;
    ContextDesc desc_;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queue_family_ = 0;
    VmaAllocator allocator_ = nullptr;
    VkPipelineCache pipeline_cache_ = VK_NULL_HANDLE;
    float timestamp_period_ = 1.0f;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat swapchain_format_ = VK_FORMAT_B8G8R8A8_SRGB;
    VkExtent2D extent_{};
    std::vector<VkImage> swapchain_images_;
    std::vector<VkImageView> swapchain_views_;
    std::vector<VkSemaphore> render_finished_;       // per swapchain image
    uint32_t image_index_ = 0;

    FrameResources frames_[kFramesInFlight];
    uint32_t frame_index_ = 0;
    uint64_t frame_number_ = 0;

    VkDescriptorPool bindless_pool_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout bindless_layout_ = VK_NULL_HANDLE;
    VkDescriptorSet bindless_set_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    uint32_t next_texture_ = 0;

    VkCommandPool upload_pool_ = VK_NULL_HANDLE;
};

}  // namespace engine::vk
