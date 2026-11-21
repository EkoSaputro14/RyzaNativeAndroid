// Built-in pure-Vulkan-compute 2x DIS Bidirectional Optical-Flow Frame Generation
// Direct C11 port of mhrise-recomp's runtime/src/nnsdk/nvn/lsfg/motion.cpp + fg_*.comp shaders.
// Runs 100% on Vulkan compute (no external Lossless.dll), tuned for Adreno 618 / mobile tilers.
#include "framegen.h"
#include "shaders.h"
#include "../../settings.h"
#include "../../platform.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define FG_MAX_LEVELS 3
#define FG_MAX_W 1280u
#define FG_MAX_H 720u

enum {
    FG_PASS_LUMA = 0,
    FG_PASS_DOWN = 1,
    FG_PASS_PATCH = 2,
    FG_PASS_DENSIFY = 3,
    FG_PASS_WARP = 4,
    FG_PASS_COUNT = 5,
};

typedef struct {
    VkImage image;
    VmaAllocation alloc;
    VkImageView view;
    VkExtent2D extent;
    VkFormat format;
} FgImg;

typedef struct {
    VkDescriptorSetLayout set_layout;
    VkPipelineLayout layout;
    VkPipeline pipeline;
} FgPipe;

typedef struct {
    int32_t size[2];
    int32_t src_size[2];
    float inv_src[2];
    int32_t flags;
    int32_t extra;
    float t;
} FgPush;

static struct {
    bool pipes_ready;
    bool pipes_failed;
    FgPipe pipes[FG_PASS_COUNT];
    VkSampler sampler;

    bool res_ready;
    uint32_t w, h;
    uint32_t finest_scale;  // 4 = Light, 2 = Balanced
    int levels;
    bool images_initialized;

    FgImg frames[2];
    FgImg output;
    FgImg lum[2][FG_MAX_LEVELS];
    FgImg flows[2][FG_MAX_LEVELS];
    FgImg patches[2][FG_MAX_LEVELS];

    VkDescriptorPool pool;
    VkDescriptorSet luma_set[2];
    VkDescriptorSet warp_set[2];
    VkDescriptorSet down_set[2][FG_MAX_LEVELS];
    VkDescriptorSet patch_set[2][2][FG_MAX_LEVELS];
    VkDescriptorSet dense_set[2][2][FG_MAX_LEVELS];

    uint64_t seq_n;
    uint64_t last_ns;
    _Atomic uint64_t generated_total;
    _Atomic bool is_active;
} g_fg;

static VkExtent2D fg_grid(VkExtent2D lv) {
    uint32_t gw = (lv.width > 8u ? lv.width - 8u : 0u);
    uint32_t gh = (lv.height > 8u ? lv.height - 8u : 0u);
    VkExtent2D r = {(gw + 3u) / 4u + 1u, (gh + 3u) / 4u + 1u};
    return r;
}

static bool fg_create_image(FgImg *img, uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage) {
    memset(img, 0, sizeof *img);
    img->extent.width = w;
    img->extent.height = h;
    img->format = fmt;
    VkImageCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = fmt,
        .extent = {w, h, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_GPU_ONLY};
    if (vmaCreateImage(g_vk.vma, &ci, &ai, &img->image, &img->alloc, NULL) != VK_SUCCESS)
        return false;
    VkImageViewCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = img->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = fmt,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    if (vkCreateImageView(g_vk.dev, &vi, NULL, &img->view) != VK_SUCCESS) {
        vmaDestroyImage(g_vk.vma, img->image, img->alloc);
        memset(img, 0, sizeof *img);
        return false;
    }
    return true;
}

static void fg_destroy_image(FgImg *img) {
    if (img->view) vkDestroyImageView(g_vk.dev, img->view, NULL);
    if (img->image) vmaDestroyImage(g_vk.vma, img->image, img->alloc);
    memset(img, 0, sizeof *img);
}

static void fg_destroy_resources(void) {
    if (!g_fg.res_ready) return;
    vkDeviceWaitIdle(g_vk.dev);
    if (g_fg.pool) {
        vkDestroyDescriptorPool(g_vk.dev, g_fg.pool, NULL);
        g_fg.pool = VK_NULL_HANDLE;
    }
    for (int i = 0; i < 2; i++) fg_destroy_image(&g_fg.frames[i]);
    fg_destroy_image(&g_fg.output);
    for (int i = 0; i < 2; i++) {
        for (int l = 0; l < FG_MAX_LEVELS; l++) {
            fg_destroy_image(&g_fg.lum[i][l]);
            fg_destroy_image(&g_fg.flows[i][l]);
            fg_destroy_image(&g_fg.patches[i][l]);
        }
    }
    g_fg.res_ready = false;
    g_fg.images_initialized = false;
    g_fg.seq_n = 0;
}

static bool fg_build_pipelines(void) {
    if (g_fg.pipes_ready) return true;
    if (g_fg.pipes_failed) return false;

    static const char *const names[FG_PASS_COUNT] = {
        "fg_luma.comp", "fg_down.comp", "fg_patch.comp", "fg_densify.comp", "fg_warp.comp"
    };
    // 0 = STORAGE_IMAGE, 1 = COMBINED_IMAGE_SAMPLER
    static const uint8_t nbind[FG_PASS_COUNT] = {2, 2, 4, 4, 5};
    static const uint8_t btype[FG_PASS_COUNT][5] = {
        {1, 0, 0, 0, 0},  // luma: sampler src, storage dst
        {0, 0, 0, 0, 0},  // down: storage src, storage dst
        {0, 0, 0, 0, 0},  // patch: storage i0, i1, coarse, patches
        {0, 0, 0, 0, 0},  // densify: storage patches, i0, i1, flow
        {1, 1, 1, 1, 0},  // warp: sampler frame_a, frame_b, flow_ab, flow_ba, storage dst
    };

    for (int p = 0; p < FG_PASS_COUNT; p++) {
        VkDescriptorSetLayoutBinding b[5];
        memset(b, 0, sizeof b);
        for (uint8_t i = 0; i < nbind[p]; i++) {
            b[i].binding = i;
            b[i].descriptorType = btype[p][i] ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            b[i].descriptorCount = 1;
            b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo dli = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = nbind[p],
            .pBindings = b,
        };
        if (vkCreateDescriptorSetLayout(g_vk.dev, &dli, NULL, &g_fg.pipes[p].set_layout) != VK_SUCCESS) {
            g_fg.pipes_failed = true;
            return false;
        }
        VkPushConstantRange pcr = {.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .offset = 0, .size = sizeof(FgPush)};
        VkPipelineLayoutCreateInfo pli = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1,
            .pSetLayouts = &g_fg.pipes[p].set_layout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &pcr,
        };
        if (vkCreatePipelineLayout(g_vk.dev, &pli, NULL, &g_fg.pipes[p].layout) != VK_SUCCESS) {
            g_fg.pipes_failed = true;
            return false;
        }
        VkShaderModule mod = vk_builtin_shader(names[p]);
        if (!mod) {
            g_fg.pipes_failed = true;
            return false;
        }
        VkComputePipelineCreateInfo cp = {
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                .module = mod,
                .pName = "main",
            },
            .layout = g_fg.pipes[p].layout,
        };
        if (vkCreateComputePipelines(g_vk.dev, g_vk.pcache, 1, &cp, NULL, &g_fg.pipes[p].pipeline) != VK_SUCCESS) {
            g_fg.pipes_failed = true;
            return false;
        }
    }

    VkSamplerCreateInfo si = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .maxLod = 0.0f,
    };
    if (vkCreateSampler(g_vk.dev, &si, NULL, &g_fg.sampler) != VK_SUCCESS) {
        g_fg.pipes_failed = true;
        return false;
    }
    g_fg.pipes_ready = true;
    LOG("[fg] mhrise-arch 2x optical-flow compute pipelines ready (5 DIS shaders)\n");
    return true;
}

static bool fg_alloc_set(int pass, VkDescriptorSet *out) {
    VkDescriptorSetAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = g_fg.pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &g_fg.pipes[pass].set_layout,
    };
    return vkAllocateDescriptorSets(g_vk.dev, &ai, out) == VK_SUCCESS;
}

static void fg_write_set(VkDescriptorSet set, int count, const FgImg *const *imgs, const bool *sampled) {
    VkDescriptorImageInfo ii[5];
    VkWriteDescriptorSet w[5];
    memset(ii, 0, sizeof ii);
    memset(w, 0, sizeof w);
    for (int i = 0; i < count; i++) {
        ii[i].sampler = sampled[i] ? g_fg.sampler : VK_NULL_HANDLE;
        ii[i].imageView = imgs[i]->view;
        ii[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet = set;
        w[i].dstBinding = (uint32_t)i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = sampled[i] ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[i].pImageInfo = &ii[i];
    }
    vkUpdateDescriptorSets(g_vk.dev, (uint32_t)count, w, 0, NULL);
}

static bool fg_build_resources(uint32_t w, uint32_t h, uint32_t scale) {
    fg_destroy_resources();
    g_fg.w = w;
    g_fg.h = h;
    g_fg.finest_scale = scale;

    uint32_t lw = (w + scale - 1u) / scale;
    uint32_t lh = (h + scale - 1u) / scale;
    if (lw < 16u) lw = 16u;
    if (lh < 16u) lh = 16u;

    int levels = 0;
    uint32_t tw = lw, th = lh;
    while (levels < FG_MAX_LEVELS && tw >= 16u && th >= 16u) {
        levels++;
        tw = (tw + 1u) / 2u;
        th = (th + 1u) / 2u;
    }
    if (levels < 1) levels = 1;
    g_fg.levels = levels;

    for (int i = 0; i < 2; i++) {
        if (!fg_create_image(&g_fg.frames[i], w, h, VK_FORMAT_R8G8B8A8_UNORM,
                             VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT))
            goto fail;
    }
    if (!fg_create_image(&g_fg.output, w, h, VK_FORMAT_R8G8B8A8_UNORM,
                         VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT))
        goto fail;

    tw = lw;
    th = lh;
    for (int l = 0; l < levels; l++) {
        VkExtent2D le = {tw, th};
        VkExtent2D ge = fg_grid(le);
        for (int i = 0; i < 2; i++) {
            if (!fg_create_image(&g_fg.lum[i][l], tw, th, VK_FORMAT_R32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT))
                goto fail;
            if (!fg_create_image(&g_fg.flows[i][l], tw, th, VK_FORMAT_R16G16B16A16_SFLOAT,
                                 VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT))
                goto fail;
            if (!fg_create_image(&g_fg.patches[i][l], ge.width, ge.height, VK_FORMAT_R32G32B32A32_SFLOAT,
                                 VK_IMAGE_USAGE_STORAGE_BIT))
                goto fail;
        }
        tw = (tw + 1u) / 2u;
        th = (th + 1u) / 2u;
    }

    uint32_t max_sets = 2u * (2u + (uint32_t)levels * 5u);
    VkDescriptorPoolSize ps[2] = {
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, max_sets * 4u},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, max_sets * 4u},
    };
    VkDescriptorPoolCreateInfo pi = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = max_sets,
        .poolSizeCount = 2,
        .pPoolSizes = ps,
    };
    if (vkCreateDescriptorPool(g_vk.dev, &pi, NULL, &g_fg.pool) != VK_SUCCESS)
        goto fail;

    for (int p = 0; p < 2; p++) {
        int a = 1 - p;
        if (!fg_alloc_set(FG_PASS_LUMA, &g_fg.luma_set[p]) || !fg_alloc_set(FG_PASS_WARP, &g_fg.warp_set[p]))
            goto fail;
        {
            const FgImg *imgs[2] = {&g_fg.frames[p], &g_fg.lum[p][0]};
            const bool samp[2] = {true, false};
            fg_write_set(g_fg.luma_set[p], 2, imgs, samp);
        }
        {
            const FgImg *imgs[5] = {&g_fg.frames[a], &g_fg.frames[p], &g_fg.flows[0][0], &g_fg.flows[1][0], &g_fg.output};
            const bool samp[5] = {true, true, true, true, false};
            fg_write_set(g_fg.warp_set[p], 5, imgs, samp);
        }
        for (int l = 0; l < levels; l++) {
            if (l > 0) {
                if (!fg_alloc_set(FG_PASS_DOWN, &g_fg.down_set[p][l])) goto fail;
                const FgImg *imgs[2] = {&g_fg.lum[p][l - 1], &g_fg.lum[p][l]};
                const bool samp[2] = {false, false};
                fg_write_set(g_fg.down_set[p][l], 2, imgs, samp);
            }
            for (int wdir = 0; wdir < 2; wdir++) {
                const FgImg *i0 = &g_fg.lum[wdir == 0 ? a : p][l];
                const FgImg *i1 = &g_fg.lum[wdir == 0 ? p : a][l];
                const FgImg *coarse = &g_fg.flows[wdir][l + 1 < levels ? l + 1 : l];
                if (!fg_alloc_set(FG_PASS_PATCH, &g_fg.patch_set[p][wdir][l]) ||
                    !fg_alloc_set(FG_PASS_DENSIFY, &g_fg.dense_set[p][wdir][l]))
                    goto fail;
                {
                    const FgImg *imgs[4] = {i0, i1, coarse, &g_fg.patches[wdir][l]};
                    const bool samp[4] = {false, false, false, false};
                    fg_write_set(g_fg.patch_set[p][wdir][l], 4, imgs, samp);
                }
                {
                    const FgImg *imgs[4] = {&g_fg.patches[wdir][l], i0, i1, &g_fg.flows[wdir][l]};
                    const bool samp[4] = {false, false, false, false};
                    fg_write_set(g_fg.dense_set[p][wdir][l], 4, imgs, samp);
                }
            }
        }
    }

    g_fg.res_ready = true;
    g_fg.images_initialized = false;
    g_fg.seq_n = 0;
    LOG("[fg] mhrise-arch optical-flow pyramid ready: %ux%u scale=%u levels=%d (%s)\n",
        w, h, scale, levels, scale >= 4u ? "2x Light" : "2x Balanced");
    return true;

fail:
    fg_destroy_resources();
    LOG("[fg] failed to build optical-flow resources for %ux%u\n", w, h);
    return false;
}

static void fg_init_layouts(VkCommandBuffer cb) {
    VkImageMemoryBarrier b[3 + 2 * 3 * FG_MAX_LEVELS];
    uint32_t nb = 0;
    FgImg *base[3] = {&g_fg.frames[0], &g_fg.frames[1], &g_fg.output};
    for (int i = 0; i < 3; i++) {
        VkImageMemoryBarrier m = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                             VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = base[i]->image,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        b[nb++] = m;
    }
    for (int i = 0; i < 2; i++) {
        for (int l = 0; l < g_fg.levels; l++) {
            FgImg *arr[3] = {&g_fg.lum[i][l], &g_fg.flows[i][l], &g_fg.patches[i][l]};
            for (int k = 0; k < 3; k++) {
                VkImageMemoryBarrier m = {
                    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                    .srcAccessMask = 0,
                    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                    .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = arr[k]->image,
                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
                };
                b[nb++] = m;
            }
        }
    }
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, nb, b);
    g_fg.images_initialized = true;
}

static void fg_compute_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier mb = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
    };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 1, &mb, 0, NULL, 0, NULL);
}

static void fg_dispatch(VkCommandBuffer cb, int pass, VkDescriptorSet set, VkExtent2D size, VkExtent2D src,
                        int flags, int extra, float t, bool groups) {
    const FgPipe *p = &g_fg.pipes[pass];
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p->pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p->layout, 0, 1, &set, 0, NULL);
    FgPush push = {
        .size = {(int32_t)size.width, (int32_t)size.height},
        .src_size = {(int32_t)src.width, (int32_t)src.height},
        .inv_src = {1.0f / (float)g_fg.w, 1.0f / (float)g_fg.h},
        .flags = flags,
        .extra = extra,
        .t = t,
    };
    vkCmdPushConstants(cb, p->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, &push);
    if (groups)
        vkCmdDispatch(cb, size.width, size.height, 1);
    else
        vkCmdDispatch(cb, (size.width + 7u) / 8u, (size.height + 7u) / 8u, 1);
}

void vk_framegen_reset(void) {
    g_fg.seq_n = 0;
    g_fg.last_ns = 0;
    atomic_store_explicit(&g_fg.is_active, false, memory_order_relaxed);
}

uint64_t vk_framegen_generated_count(void) {
    return atomic_load_explicit(&g_fg.generated_total, memory_order_relaxed);
}

bool vk_framegen_active(void) {
    return atomic_load_explicit(&g_fg.is_active, memory_order_relaxed);
}

VkImageView vk_framegen_prepare(VkCommandBuffer cb, TexBackend *src_tex) {
#ifdef __ANDROID__
    int mode = g_settings.frame_gen;
#else
    int mode = 0;
#endif
    if (mode <= 0 || !src_tex || !src_tex->image || src_tex->width < 64u || src_tex->height < 64u ||
        (src_tex->aspect & ~VK_IMAGE_ASPECT_COLOR_BIT)) {
        vk_framegen_reset();
        return VK_NULL_HANDLE;
    }
    if (!fg_build_pipelines()) {
        vk_framegen_reset();
        return VK_NULL_HANDLE;
    }

    // Clamp working optical-flow resolution to 1280x720 max so mobile GPU compute stays ~1.5-2.5ms
    uint32_t fw = src_tex->width;
    uint32_t fh = src_tex->height;
    if (fw > FG_MAX_W || fh > FG_MAX_H) {
        double sx = (double)FG_MAX_W / (double)fw;
        double sy = (double)FG_MAX_H / (double)fh;
        double s = sx < sy ? sx : sy;
        fw = (uint32_t)(fw * s);
        fh = (uint32_t)(fh * s);
    }
    fw = (fw + 3u) & ~3u;
    fh = (fh + 3u) & ~3u;
    if (fw < 64u) fw = 64u;
    if (fh < 64u) fh = 64u;

    uint32_t scale = (mode == 2) ? 2u : 4u;  // 1 = Light (scale=4), 2 = Balanced (scale=2)
    bool rebuilt = false;
    if (!g_fg.res_ready || g_fg.w != fw || g_fg.h != fh || g_fg.finest_scale != scale) {
        if (!fg_build_resources(fw, fh, scale)) {
            vk_framegen_reset();
            return VK_NULL_HANDLE;
        }
        rebuilt = true;
    }

    if (!g_fg.images_initialized) fg_init_layouts(cb);

    int p = (int)(g_fg.seq_n & 1ull);

    // Ensure previous scene writes to src_tex are visible to transfer read, and prior compute reads of frames[p] completed
    {
        VkMemoryBarrier mb = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                             VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
        };
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 1, &mb, 0, NULL, 0, NULL);
    }

    VkImageBlit blit = {
        .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .srcOffsets = {{0, 0, 0}, {(int32_t)src_tex->width, (int32_t)src_tex->height, 1}},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .dstOffsets = {{0, 0, 0}, {(int32_t)fw, (int32_t)fh, 1}},
    };
    vkCmdBlitImage(cb, src_tex->image, VK_IMAGE_LAYOUT_GENERAL,
                   g_fg.frames[p].image, VK_IMAGE_LAYOUT_GENERAL, 1, &blit, VK_FILTER_LINEAR);

    // Transfer write -> compute shader read
    {
        VkMemoryBarrier mb = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        };
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 1, &mb, 0, NULL, 0, NULL);
    }

    // Build luma pyramid for current frame
    VkExtent2D full_ext = {fw, fh};
    fg_dispatch(cb, FG_PASS_LUMA, g_fg.luma_set[p], g_fg.lum[p][0].extent, full_ext, 0, (int)scale, 0.5f, false);
    for (int l = 1; l < g_fg.levels; l++) {
        fg_compute_barrier(cb);
        fg_dispatch(cb, FG_PASS_DOWN, g_fg.down_set[p][l], g_fg.lum[p][l].extent, g_fg.lum[p][l - 1].extent, 0, 0, 0.5f, false);
    }

    uint64_t now = plat_now_ns();
    bool can_gen = !rebuilt && g_fg.seq_n > 0 && g_fg.last_ns > 0 && (now - g_fg.last_ns) < 250000000ull;

    if (can_gen) {
        fg_compute_barrier(cb);
        bool light = (scale >= 4u);
        for (int l = g_fg.levels - 1; l >= 0; l--) {
            bool coarsest = (l == g_fg.levels - 1);
            VkExtent2D le = g_fg.lum[p][l].extent;
            VkExtent2D ge = fg_grid(le);
            int pflags = (coarsest ? 2 : 1) | (light ? 4 : 0);
            int extra = coarsest ? 0 : (int)(g_fg.flows[0][l + 1].extent.width | (g_fg.flows[0][l + 1].extent.height << 16));
            for (int wdir = 0; wdir < 2; wdir++)
                fg_dispatch(cb, FG_PASS_PATCH, g_fg.patch_set[p][wdir][l], ge, le, pflags, extra, 0.5f, true);
            fg_compute_barrier(cb);
            for (int wdir = 0; wdir < 2; wdir++)
                fg_dispatch(cb, FG_PASS_DENSIFY, g_fg.dense_set[p][wdir][l], le, ge, 0, 0, 0.5f, false);
            fg_compute_barrier(cb);
        }
        fg_dispatch(cb, FG_PASS_WARP, g_fg.warp_set[p], full_ext, g_fg.flows[0][0].extent, light ? 0 : 1, (int)scale, 0.5f, false);

        // Compute write -> fragment shader read for present blit
        VkMemoryBarrier mb = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        };
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 1, &mb, 0, NULL, 0, NULL);
        atomic_fetch_add_explicit(&g_fg.generated_total, 1, memory_order_relaxed);
        atomic_store_explicit(&g_fg.is_active, true, memory_order_relaxed);
    } else {
        atomic_store_explicit(&g_fg.is_active, false, memory_order_relaxed);
    }

    vk_mark_cb_dirty(cb);
    g_fg.seq_n++;
    g_fg.last_ns = now;
    return can_gen ? g_fg.output.view : VK_NULL_HANDLE;
}
