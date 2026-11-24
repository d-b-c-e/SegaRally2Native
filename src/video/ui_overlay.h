// Full-window menu layer: a BGRA picture with premultiplied alpha, drawn on
// the CPU (ui/frontend.cpp) and blended over the composed frame.
#pragma once
#include <cstdint>
#include <vector>

#include "vk_presenter.h"

class UiOverlay {
public:
    bool init(VkPresenter& vk);
    void shutdown();

    // New picture (w x h BGRA, premultiplied); uploaded with the next record
    void set_image(const uint32_t* bgra, uint32_t w, uint32_t h);
    void hide() { visible_ = false; }
    // Wheelkit (a modified version's addition): the picture between x and the window's width less x (on three screens
    // the centre one), not stretched over the whole window
    void set_left(uint32_t x) { left_ = x; }
    bool visible() const { return visible_; }

    // After the main renderer: blends the picture over the swapchain image
    void record(VkCommandBuffer cmd, VkImageView target, VkExtent2D extent);

private:
    void destroy_image();

    VkPresenter* vk_ = nullptr;
    VkDevice dev_ = VK_NULL_HANDLE;
    VkImage image_ = VK_NULL_HANDLE;
    VkDeviceMemory image_memory_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
    uint32_t w_ = 0, h_ = 0;
    VkBuffer staging_ = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory_ = VK_NULL_HANDLE;
    void* mapped_ = nullptr;
    size_t staging_size_ = 0;
    std::vector<uint32_t> pending_;
    uint32_t pending_w_ = 0, pending_h_ = 0;
    bool dirty_ = false, visible_ = false;
    uint32_t left_ = 0;   // Wheelkit: set_left

    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
};
