#pragma once
#include "vk.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FG_STATUS_OFF = 0,
    FG_STATUS_WARMUP = 1,
    FG_STATUS_ACTIVE = 2,
    FG_STATUS_PAUSED_HITCH = 3,
    FG_STATUS_PAUSED_LOAD = 4,
    FG_STATUS_TOO_FAST = 5,
} FgStatus;

// Built-in pure-Vulkan-compute 2x DIS optical-flow frame generation (ported from mhrise-recomp/lsfg/motion.cpp
// & framegen.cpp). Includes warmup (3 frames), hitch detection (>100ms -> 1s pause), too-fast skip (<22ms),
// and exponential overload auto-backoff (5s -> 15s -> 30s -> 60s when real frame EMA > 38ms for 15 frames).
// Returns a valid VkImageView (VK_IMAGE_LAYOUT_GENERAL) of the generated intermediate frame when active,
// or VK_NULL_HANDLE when disabled / warming up / paused.
VkImageView vk_framegen_prepare(VkCommandBuffer cb, TexBackend *src_tex);

// Reset temporal history (e.g. swapchain recreate). Preserves active overload cooldown unless disabled.
void vk_framegen_reset(void);

// Telemetry for performance overlay & Auto-FIFO swapchain coordination.
uint64_t vk_framegen_generated_count(void);
bool vk_framegen_active(void);
int vk_framegen_status(void);
uint32_t vk_framegen_backoff_sec(void);
double vk_framegen_ema_ms(void);

// True while device.c is rendering the intermediate generated frame's overlay pass (prevents FPS double-counting).
bool vk_is_fg_present_pass(void);

#ifdef __cplusplus
}
#endif
