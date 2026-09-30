#pragma once
// Vulkan 1.3 baseline (design doc §6.1): dynamic rendering, synchronization2,
// descriptor indexing, buffer device address, timeline semaphores.
// volk loads function pointers; VMA owns memory; vk-bootstrap builds the
// instance/device/swapchain. None of these are rewritten here (§2.1).
#include <volk.h>
#include <cstdio>
#include <cstdlib>

#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include <vk_mem_alloc.h>

#define VK_CHECK(expr)                                                                         \
    do {                                                                                       \
        VkResult r_ = (expr);                                                                  \
        if (r_ != VK_SUCCESS) {                                                                \
            std::fprintf(stderr, "Vulkan error %d at %s:%d: %s\n", int(r_), __FILE__, __LINE__, #expr); \
            std::abort();                                                                      \
        }                                                                                      \
    } while (0)

namespace engine::vk {

constexpr uint32_t kFramesInFlight = 2;          // §6.3
constexpr uint32_t kMaxBindlessTextures = 4096;

struct Buffer {
    VkBuffer        buffer = VK_NULL_HANDLE;
    VmaAllocation   allocation = nullptr;
    VkDeviceAddress address = 0;
    void*           mapped = nullptr;
    VkDeviceSize    size = 0;
};

struct Image {
    VkImage       image = VK_NULL_HANDLE;
    VkImageView   view = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    VkFormat      format = VK_FORMAT_UNDEFINED;
    VkExtent2D    extent{};
};

}  // namespace engine::vk
