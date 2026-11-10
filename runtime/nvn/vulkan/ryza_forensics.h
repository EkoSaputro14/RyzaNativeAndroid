#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RYZA_MAX_CAPTURED_DRAWS 32
#define RYZA_MAX_ATTRS 32
#define RYZA_MAX_SAMPLES_PER_DRAW 6

typedef struct {
    uint32_t vertex_idx;
    float pos[4];
    float norm[4];
    uint32_t bone_indices[4];
    float bone_weights[4];
    float clip_pos[4];
    float attr_raw[8][4];
    bool w_valid;
    bool is_corrupted;
} RyzaVertexSample;

typedef struct {
    uint64_t frame;
    uint32_t draw_idx;
    char body_part[32];
    uint64_t vs_code;
    uint64_t fs_code;
    uint64_t vs_hash;
    uint64_t fs_hash;
    uint32_t attr_mask;
    uint32_t nattrib;
    uint32_t nstream;

    struct {
        uint8_t stream;
        uint8_t nvn_fmt;
        uint8_t vk_fmt;
        uint8_t kind; // 0: float, 1: sint, 2: uint
        uint32_t off;
        uint32_t stride;
        uint64_t buf_addr;
        uint64_t buf_size;
        uint32_t raw_data[4];
        float float_data[4];
    } attrs[RYZA_MAX_ATTRS];

    uint32_t itype; // 2: uint16, 4: uint32
    uint32_t icount;
    uint64_t iaddr;
    int32_t base_vertex;
    uint32_t min_idx;
    uint32_t max_idx;
    uint32_t raw_indices[16];

    // Sampled vertices
    uint32_t num_samples;
    RyzaVertexSample samples[RYZA_MAX_SAMPLES_PER_DRAW];

    // Vertex shader source snippet
    char vs_glsl[2048];

    // Bone matrices / constant buffer samples
    uint64_t cbuf1_addr;
    uint32_t cbuf1_size;
    float cbuf1_sample[16]; // View/Proj matrix
    uint64_t cbuf3_addr;
    uint32_t cbuf3_size;
    float cbuf3_sample[32]; // Bone matrices sample

    // Rasterization / Depth state
    uint32_t cull_mode;
    uint32_t front_face;
    uint32_t depth_test;
    uint32_t depth_write;
    uint32_t depth_compare_op;

    bool is_valid;
} RyzaDrawForensic;

extern RyzaDrawForensic g_ryza_captured_draws[RYZA_MAX_CAPTURED_DRAWS];
extern uint32_t g_ryza_captured_draw_count;
extern uint64_t g_ryza_3d_draw_count;

const char *ryza_get_build_fingerprint(void);
void ryza_forensics_init(void);
RyzaDrawForensic *ryza_allocate_capture_slot(uint32_t icount, uint32_t attr_mask, bool has_bones);
void ryza_capture_draw_state(uint64_t frame, uint32_t draw_idx, uint64_t vs_code, uint64_t fs_code,
                             uint64_t fs_hash, uint32_t attr_mask, uint32_t nattrib, uint32_t nstream,
                             bool indexed, uint32_t icount, uint32_t itype, uint64_t iaddr,
                             int32_t base_vertex);
void ryza_export_diagnostic_log(void);
void ryza_render_forensic_hud(void);

#ifdef __cplusplus
}
#endif
