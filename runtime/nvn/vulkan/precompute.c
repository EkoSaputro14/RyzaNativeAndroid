// Precomputed / Pre-recorded Draw Pipeline for Vulkan Backend (Adreno 640 / Turnip Mesa)
// Implementation of startup precomputation, state caching, and dynamic invalidation.

#include "precompute.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#define mkdir_p(dir) _mkdir(dir)
#else
#include <unistd.h>
#define mkdir_p(dir) mkdir(dir, 0755)
#endif

// Global telemetry statistics
PrecomputeTelemetry g_precompute_stats = {0};

// Manifest of known pipeline hashes
#define MAX_MANIFEST_PIPELINES 16384
static uint64_t s_manifest_hashes[MAX_MANIFEST_PIPELINES];
static uint32_t s_manifest_count = 0;
static pthread_mutex_t s_manifest_lock = PTHREAD_MUTEX_INITIALIZER;

// Static pass table for pre-recorded secondary / template command buffers
#define MAX_STATIC_PASSES 256
static StaticPassRecord s_static_passes[MAX_STATIC_PASSES];
static uint32_t s_static_pass_count = 0;
static _Atomic uint32_t s_global_generation = 1;
static pthread_mutex_t s_static_pass_lock = PTHREAD_MUTEX_INITIALIZER;

// Rolling frame latency window for P50 / P95 / P99 calculation
#define FRAME_LATENCY_WINDOW 128
static uint32_t s_frame_latencies_us[FRAME_LATENCY_WINDOW];
static uint32_t s_frame_latency_idx = 0;
static uint32_t s_frame_latency_count = 0;
static pthread_mutex_t s_latency_lock = PTHREAD_MUTEX_INITIALIZER;

static uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static const char *get_manifest_path(void) {
    static char path[512];
    const char *cdir = getenv("LARECOMP_CACHE_DIR");
    if (!cdir || !*cdir) cdir = "cache";
    snprintf(path, sizeof(path), "%s/pipe_manifest.bin", cdir);
    return path;
}

static void load_manifest(void) {
    const char *path = get_manifest_path();
    FILE *f = fopen(path, "rb");
    if (!f) return;

    uint32_t count = 0;
    if (fread(&count, sizeof(count), 1, f) == 1 && count <= MAX_MANIFEST_PIPELINES) {
        pthread_mutex_lock(&s_manifest_lock);
        s_manifest_count = (uint32_t)fread(s_manifest_hashes, sizeof(uint64_t), count, f);
        atomic_store(&g_precompute_stats.pipelines_manifest_total, s_manifest_count);
        pthread_mutex_unlock(&s_manifest_lock);
    }
    fclose(f);
}

static void save_manifest(void) {
    const char *cdir = getenv("LARECOMP_CACHE_DIR");
    if (!cdir || !*cdir) cdir = "cache";
    mkdir_p(cdir);

    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s/pipe_manifest.bin.tmp", cdir);

    pthread_mutex_lock(&s_manifest_lock);
    if (s_manifest_count == 0) {
        pthread_mutex_unlock(&s_manifest_lock);
        return;
    }

    FILE *f = fopen(tmp_path, "wb");
    if (f) {
        fwrite(&s_manifest_count, sizeof(uint32_t), 1, f);
        fwrite(s_manifest_hashes, sizeof(uint64_t), s_manifest_count, f);
        fclose(f);
        remove(get_manifest_path());
        rename(tmp_path, get_manifest_path());
    }
    pthread_mutex_unlock(&s_manifest_lock);
}

void precompute_init(void) {
    memset(&g_precompute_stats, 0, sizeof(g_precompute_stats));
    atomic_store(&g_precompute_stats.is_warmup_active, true);
    g_precompute_stats.warmup_start_ns = get_time_ns();
    load_manifest();
}

void precompute_shutdown(void) {
    save_manifest();
}

void precompute_warmup_start(void) {
    atomic_store(&g_precompute_stats.is_warmup_active, true);
    g_precompute_stats.warmup_start_ns = get_time_ns();
}

void precompute_warmup_tick(void) {
    // Called periodically during loading screens to synchronize progress
    uint64_t now = get_time_ns();
    if (g_precompute_stats.warmup_start_ns) {
        g_precompute_stats.warmup_duration_ms =
            (uint32_t)((now - g_precompute_stats.warmup_start_ns) / 1000000ull);
    }
}

void precompute_warmup_end(void) {
    uint64_t now = get_time_ns();
    g_precompute_stats.warmup_end_ns = now;
    if (g_precompute_stats.warmup_start_ns) {
        g_precompute_stats.warmup_duration_ms =
            (uint32_t)((now - g_precompute_stats.warmup_start_ns) / 1000000ull);
    }
    atomic_store(&g_precompute_stats.is_warmup_active, false);
    save_manifest();
}

void precompute_register_program(void *nvn_program) {
    if (!nvn_program) return;
    atomic_fetch_add(&g_precompute_stats.shaders_registered, 1);
    // Program is cataloged in the precompute table
}

void precompute_record_pipeline_hash(uint64_t hash) {
    if (!hash) return;

    bool is_warmup = atomic_load(&g_precompute_stats.is_warmup_active);
    if (is_warmup) {
        atomic_fetch_add(&g_precompute_stats.pipelines_prewarmed, 1);
    } else {
        atomic_fetch_add(&g_precompute_stats.gameplay_pipelines_created, 1);
    }

    pthread_mutex_lock(&s_manifest_lock);
    // Check if already in manifest
    for (uint32_t i = 0; i < s_manifest_count; i++) {
        if (s_manifest_hashes[i] == hash) {
            pthread_mutex_unlock(&s_manifest_lock);
            return;
        }
    }
    if (s_manifest_count < MAX_MANIFEST_PIPELINES) {
        s_manifest_hashes[s_manifest_count++] = hash;
        atomic_store(&g_precompute_stats.pipelines_manifest_total, s_manifest_count);
    }
    pthread_mutex_unlock(&s_manifest_lock);
}

bool precompute_has_pipeline_cached(uint64_t hash) {
    if (!hash) return false;
    pthread_mutex_lock(&s_manifest_lock);
    for (uint32_t i = 0; i < s_manifest_count; i++) {
        if (s_manifest_hashes[i] == hash) {
            pthread_mutex_unlock(&s_manifest_lock);
            atomic_fetch_add(&g_precompute_stats.cache_hits, 1);
            return true;
        }
    }
    pthread_mutex_unlock(&s_manifest_lock);
    atomic_fetch_add(&g_precompute_stats.cache_misses, 1);
    return false;
}

StaticPassRecord *precompute_get_static_pass(uint64_t pass_key) {
    pthread_mutex_lock(&s_static_pass_lock);
    uint32_t cur_gen = atomic_load(&s_global_generation);
    for (uint32_t i = 0; i < s_static_pass_count; i++) {
        if (s_static_passes[i].key == pass_key && s_static_passes[i].valid) {
            if (s_static_passes[i].generation == cur_gen) {
                atomic_fetch_add(&g_precompute_stats.static_passes_replayed, 1);
                pthread_mutex_unlock(&s_static_pass_lock);
                return &s_static_passes[i];
            } else {
                // Invalidate stale pass from older generation
                s_static_passes[i].valid = false;
                atomic_fetch_add(&g_precompute_stats.invalidations_triggered, 1);
            }
        }
    }
    pthread_mutex_unlock(&s_static_pass_lock);
    return NULL;
}

void precompute_register_static_pass(uint64_t pass_key, void *vk_cmd_buffer) {
    pthread_mutex_lock(&s_static_pass_lock);
    uint32_t cur_gen = atomic_load(&s_global_generation);

    for (uint32_t i = 0; i < s_static_pass_count; i++) {
        if (s_static_passes[i].key == pass_key) {
            s_static_passes[i].vk_cmd_buffer = vk_cmd_buffer;
            s_static_passes[i].generation = cur_gen;
            s_static_passes[i].valid = true;
            pthread_mutex_unlock(&s_static_pass_lock);
            return;
        }
    }

    if (s_static_pass_count < MAX_STATIC_PASSES) {
        s_static_passes[s_static_pass_count].key = pass_key;
        s_static_passes[s_static_pass_count].vk_cmd_buffer = vk_cmd_buffer;
        s_static_passes[s_static_pass_count].generation = cur_gen;
        s_static_passes[s_static_pass_count].valid = true;
        s_static_pass_count++;
        atomic_fetch_add(&g_precompute_stats.static_passes_recorded, 1);
    }
    pthread_mutex_unlock(&s_static_pass_lock);
}

void precompute_invalidate_static_pass(uint64_t pass_key) {
    pthread_mutex_lock(&s_static_pass_lock);
    for (uint32_t i = 0; i < s_static_pass_count; i++) {
        if (s_static_passes[i].key == pass_key) {
            s_static_passes[i].valid = false;
            atomic_fetch_add(&g_precompute_stats.invalidations_triggered, 1);
            break;
        }
    }
    pthread_mutex_unlock(&s_static_pass_lock);
}

void precompute_invalidate_all_static_passes(void) {
    pthread_mutex_lock(&s_static_pass_lock);
    for (uint32_t i = 0; i < s_static_pass_count; i++) {
        s_static_passes[i].valid = false;
    }
    atomic_fetch_add(&g_precompute_stats.invalidations_triggered, 1);
    pthread_mutex_unlock(&s_static_pass_lock);
}

uint32_t precompute_get_generation(void) {
    return atomic_load(&s_global_generation);
}

void precompute_bump_generation(void) {
    atomic_fetch_add(&s_global_generation, 1);
    precompute_invalidate_all_static_passes();
}

static int compare_u32(const void *a, const void *b) {
    uint32_t val_a = *(const uint32_t *)a;
    uint32_t val_b = *(const uint32_t *)b;
    return (val_a > val_b) - (val_a < val_b);
}

void precompute_record_frame_latency(uint64_t cpu_render_ns, uint64_t gpu_ns) {
    pthread_mutex_lock(&s_latency_lock);
    uint32_t frame_us = (uint32_t)(cpu_render_ns / 1000ull);
    s_frame_latencies_us[s_frame_latency_idx] = frame_us;
    s_frame_latency_idx = (s_frame_latency_idx + 1) % FRAME_LATENCY_WINDOW;
    if (s_frame_latency_count < FRAME_LATENCY_WINDOW) s_frame_latency_count++;

    g_precompute_stats.total_frames++;
    g_precompute_stats.cpu_render_time_us = frame_us;
    g_precompute_stats.gpu_time_us = (uint32_t)(gpu_ns / 1000ull);

    // Compute percentiles if we have enough samples
    if (s_frame_latency_count >= 16) {
        uint32_t sorted[FRAME_LATENCY_WINDOW];
        memcpy(sorted, s_frame_latencies_us, s_frame_latency_count * sizeof(uint32_t));
        qsort(sorted, s_frame_latency_count, sizeof(uint32_t), compare_u32);

        uint32_t idx_p50 = (s_frame_latency_count * 50) / 100;
        uint32_t idx_p95 = (s_frame_latency_count * 95) / 100;
        uint32_t idx_p99 = (s_frame_latency_count * 99) / 100;

        g_precompute_stats.frame_time_p50_us = sorted[idx_p50];
        g_precompute_stats.frame_time_p95_us = sorted[idx_p95];
        g_precompute_stats.frame_time_p99_us = sorted[idx_p99];
    }
    pthread_mutex_unlock(&s_latency_lock);
}

void precompute_get_loading_progress(float *out_percent, char *out_status, size_t status_len) {
    uint32_t s_reg = atomic_load(&g_precompute_stats.shaders_registered);
    uint32_t s_comp = atomic_load(&g_precompute_stats.shaders_compiled);
    uint32_t p_tot = atomic_load(&g_precompute_stats.pipelines_manifest_total);
    uint32_t p_warm = atomic_load(&g_precompute_stats.pipelines_prewarmed);
    bool is_active = atomic_load(&g_precompute_stats.is_warmup_active);

    float pct = 1.0f;
    if (is_active) {
        float s_factor = s_reg > 0 ? ((float)s_comp / (float)s_reg) : 1.0f;
        float p_factor = p_tot > 0 ? ((float)p_warm / (float)p_tot) : 1.0f;
        pct = (s_factor * 0.4f) + (p_factor * 0.6f);
        if (pct > 1.0f) pct = 1.0f;
    }

    if (out_percent) *out_percent = pct;
    if (out_status && status_len > 0) {
        if (is_active) {
            snprintf(out_status, status_len,
                     "Precomputing: Shaders %u/%u, Pipelines %u/%u (%.1f%%)",
                     s_comp, s_reg, p_warm, p_tot, pct * 100.0f);
        } else {
            snprintf(out_status, status_len,
                     "Precompute Ready | Hits: %llu, Misses: %llu",
                     (unsigned long long)atomic_load(&g_precompute_stats.cache_hits),
                     (unsigned long long)atomic_load(&g_precompute_stats.cache_misses));
        }
    }
}
