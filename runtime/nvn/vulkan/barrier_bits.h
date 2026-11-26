// NVN command buffer barrier bit classification (adapted from mhrise-recomp commits 79efe23 & 62907c0).
// Shared between runtime/nvn/vulkan/exec.c and host unit tests so both exercise identical logic.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// NVNbarrierBits:
//   0x01: ORDER_PRIMITIVES
//   0x02: ORDER_FRAGMENTS
//   0x04: ORDER_FRAGMENTS_TILED
//   0x08: ORDER_INDIRECT_DATA
//   0x10..0x80: invalidate caches (textures, shader code, texture descriptors, ZCull)
#define NVN_BARRIER_ORDER_PRIMITIVES       0x01u
#define NVN_BARRIER_ORDER_FRAGMENTS        0x02u
#define NVN_BARRIER_ORDER_FRAGMENTS_TILED  0x04u
#define NVN_BARRIER_ORDER_INDIRECT_DATA    0x08u
#define NVN_BARRIER_ORDER_MASK             0x0Fu
#define NVN_BARRIER_FRAG_ONLY_MASK         (NVN_BARRIER_ORDER_FRAGMENTS | NVN_BARRIER_ORDER_FRAGMENTS_TILED)

typedef enum {
    RYZA_BARRIER_SKIP = 0,
    RYZA_BARRIER_FRAG_ONLY = 1,
    RYZA_BARRIER_FULL = 2
} RyzaBarrierAction;

static inline RyzaBarrierAction ryza_classify_nvn_barrier(uint32_t bits,
                                                          bool barrier_bits_on,
                                                          bool is_mali,
                                                          bool has_other_dirty) {
    // nvnCommandBufferBeginRecording writes a CMD_BARRIER list-start sentinel with bits == 0.
    if (bits == 0u) return RYZA_BARRIER_SKIP;
    if (!barrier_bits_on || is_mali) return RYZA_BARRIER_FULL;
    uint32_t order = bits & NVN_BARRIER_ORDER_MASK;
    if (order == 0u) return RYZA_BARRIER_SKIP;
    if ((order & ~NVN_BARRIER_FRAG_ONLY_MASK) == 0u && !has_other_dirty) return RYZA_BARRIER_FRAG_ONLY;
    return RYZA_BARRIER_FULL;
}

#ifdef __cplusplus
}
#endif
