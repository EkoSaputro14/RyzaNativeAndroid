#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RYZA_MAX_CAPTURED_DRAWS 32
#define RYZA_MAX_ATTRS 32
#define RYZA_MAX_SAMPLES_PER_DRAW 6

typedef enum {
    RYZA_DUMP_STATUS_IDLE = 0,
    RYZA_DUMP_STATUS_RUNNING,
    RYZA_DUMP_STATUS_SUCCESS,
    RYZA_DUMP_STATUS_FAILED
} RyzaDumpStatus;

typedef struct {
    RyzaDumpStatus status;
    int error_code;
    char error_msg[128];
    char target_path[256];
    size_t bytes_written;
    char timestamp[32];
} RyzaDumpResult;

typedef struct {
    bool ran;
    bool passed;
    int error_code;
    char error_msg[128];
    char target_path[256];
    size_t bytes_written;
    size_t bytes_read;
} RyzaTestResult;

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
    bool is_character;
    bool is_deformed;
    bool is_locked;

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
        uint8_t was_converted;
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
    char vs_glsl[8192];

    // Bone matrices / constant buffer samples
    uint8_t cbuf1_bank;
    uint8_t cbuf3_bank;
    uint64_t cbuf1_addr;
    uint32_t cbuf1_size;
    float cbuf1_sample[16]; // First VS UBO (hardware bank c3+ / compiler c1)
    uint64_t cbuf3_addr;
    uint32_t cbuf3_size;
    float cbuf3_sample[32]; // Secondary distinct VS UBO sample

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
extern uint64_t g_ryza_character_draw_count;
extern bool g_ryza_capture_freeze;
extern RyzaDumpResult g_ryza_dump_result;
extern RyzaTestResult g_ryza_test_result;
extern char g_ryza_session_id[32];

#if defined(__ANDROID__)
int android_open_log_named(const char *name, bool first);
int android_open_log_for_read(const char *name);
void android_share_log_file(const char *name);
void android_view_log_file(const char *name);
bool android_delete_log_file(const char *name);
void android_export_log_file(const char *name);
char *android_list_log_files(void);
#endif

const char *ryza_get_build_fingerprint(void);
void ryza_forensics_init(void);
RyzaDrawForensic *ryza_allocate_capture_slot(uint32_t icount, uint32_t attr_mask, bool has_bones, bool is_character, bool is_deformed);
void ryza_capture_draw_state(uint64_t frame, uint32_t draw_idx, uint64_t vs_code, uint64_t fs_code,
                             uint64_t fs_hash, uint32_t attr_mask, uint32_t nattrib, uint32_t nstream,
                             bool indexed, uint32_t icount, uint32_t itype, uint64_t iaddr,
                             int32_t base_vertex);
void ryza_export_diagnostic_log(void);
void ryza_request_auto_dump(uint32_t draw_idx);
void ryza_process_pending_auto_dump(void);
void ryza_capture_lock(void);
void ryza_capture_unlock(void);
void ryza_run_file_write_test(void);
void ryza_rearm_capture(void);

#ifdef __cplusplus
}
#endif
