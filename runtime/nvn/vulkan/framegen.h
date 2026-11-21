#pragma once
#include "vk.h"

#ifdef __cplusplus
extern "C" {
#endif

// Built-in pure-Vulkan-compute 2x DIS optical-flow frame generation (ported from mhrise-recomp/lsfg/motion.cpp).
// Records blit + 3-level luma pyramid + bidirectional DIS optical flow + warp (t=0.5) into `cb`.
// Returns a valid VkImageView (VK_IMAGE_LAYOUT_GENERAL) of the generated intermediate frame when ready,
// or VK_NULL_HANDLE on the first frame / when disabled / on scene cut.
VkImageView vk_framegen_prepare(VkCommandBuffer cb, TexBackend *src_tex);

// Reset temporal history (e.g. swapchain recreate or toggle off).
void vk_framegen_reset(void);

// Telemetry for performance overlay.
uint64_t vk_framegen_generated_count(void);
bool vk_framegen_active(void);

#ifdef __cplusplus
}
#endif
