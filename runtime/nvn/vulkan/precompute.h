// Precomputed / Pre-recorded Draw Pipeline for Vulkan Backend (Adreno 640 / Turnip Mesa)
// Architectural extension for moving reusable rendering preparation to startup/loading.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>

#ifdef __cplusplus
extern "C" {
#endif

// Real-time telemetry & benchmark metrics for Precomputed Render Pipeline
typedef struct {
    // Shaders
    _Atomic uint32_t shaders_registered;
    _Atomic uint32_t shaders_translated;
    _Atomic uint32_t shaders_compiled;
    _Atomic uint32_t shaders_cached;
    _Atomic uint32_t gameplay_shaders_compiled; // Compilation that leaked into gameplay

    // Pipelines (PSOs)
    _Atomic uint32_t pipelines_manifest_total;
    _Atomic uint32_t pipelines_prewarmed;
    _Atomic uint32_t pipelines_cached;
    _Atomic uint32_t gameplay_pipelines_created; // Pipeline creation that leaked into gameplay

    // Cache & Hit Rates
    _Atomic uint64_t cache_hits;
    _Atomic uint64_t cache_misses;

    // Draw & Command Buffer Stats
    _Atomic uint64_t draw_calls_total;
    _Atomic uint64_t draw_calls_skipped;
    _Atomic uint64_t static_passes_recorded;
    _Atomic uint64_t static_passes_replayed;
    _Atomic uint64_t invalidations_triggered;

    // Timing (nanoseconds / microseconds)
    uint64_t warmup_start_ns;
    uint64_t warmup_end_ns;
    uint32_t warmup_duration_ms;
    _Atomic bool is_warmup_active;

    // Rolling Frame Time Metrics (Microseconds)
    uint32_t frame_time_p50_us;
    uint32_t frame_time_p95_us;
    uint32_t frame_time_p99_us;
    uint32_t cpu_render_time_us;
    uint32_t gpu_time_us;
    uint64_t total_frames;
} PrecomputeTelemetry;

extern PrecomputeTelemetry g_precompute_stats;

// Lifecycle & Control
void precompute_init(void);
void precompute_shutdown(void);
void precompute_warmup_start(void);
void precompute_warmup_tick(void);
void precompute_warmup_end(void);

// Program Registration & Early Compilation
void precompute_register_program(void *nvn_program);

// Pipeline Manifest Recording & Pre-warming
void precompute_record_pipeline_hash(uint64_t hash);
bool precompute_has_pipeline_cached(uint64_t hash);

// Static Secondary / Template Command Buffer Management
typedef struct {
    uint64_t key;
    uint32_t generation;
    void *vk_cmd_buffer;
    bool valid;
} StaticPassRecord;

StaticPassRecord *precompute_get_static_pass(uint64_t pass_key);
void precompute_register_static_pass(uint64_t pass_key, void *vk_cmd_buffer);
void precompute_invalidate_static_pass(uint64_t pass_key);
void precompute_invalidate_all_static_passes(void);
uint32_t precompute_get_generation(void);
void precompute_bump_generation(void);

// Frame Metrics & Telemetry
void precompute_record_frame_latency(uint64_t cpu_render_ns, uint64_t gpu_ns);
void precompute_get_loading_progress(float *out_percent, char *out_status, size_t status_len);

#ifdef __cplusplus
}
#endif
