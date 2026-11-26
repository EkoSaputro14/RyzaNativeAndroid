// Precomputed / Pre-recorded Draw Pipeline for Vulkan Backend (Adreno 640 / Turnip Mesa)
// Implementation of startup precomputation, state caching, and dynamic invalidation.
#define _POSIX_C_SOURCE 200809L

#include "precompute.h"
#ifdef __ANDROID__
#include "../../platform.h"
#endif
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
static _Atomic bool s_manifest_dirty = false;
static _Atomic uint64_t s_last_manifest_save_ns = 0;

// Persistent POD PipeKey payloads for session-to-session VkPipeline pre-warming
#define MAX_PIPE_STATES 4096
#define MAX_PIPE_STATE_PAYLOAD 1024
#define PIPE_STATE_MAGIC 0x52595A50u
#define PIPE_STATE_VERSION 1u

typedef struct {
    uint64_t pipe_hash;
    uint64_t prog_hash;
    uint8_t payload[MAX_PIPE_STATE_PAYLOAD];
} PipeStateEntry;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t driver_id;
    uint32_t eds_flags;
    uint32_t payload_size;
    uint32_t count;
} PipeStateHeader;

static PipeStateEntry *s_pipe_states = NULL;
static uint32_t s_pipe_state_count = 0;
static uint32_t s_env_driver_id = 0;
static uint32_t s_env_eds_flags = 0;
static uint32_t s_env_payload_size = 0;
static bool s_pipe_state_loaded = false;
static pthread_mutex_t s_pipe_state_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic bool s_pipe_state_dirty = false;
static _Atomic uint64_t s_last_pipe_state_save_ns = 0;

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

#ifdef __ANDROID__
extern const char *rt_data_root(void);
#endif

static const char *get_cache_dir(void) {
    const char *pdir = getenv("LARECOMP_PRECOMPUTE_DIR");
    if (pdir && *pdir) return pdir;
    const char *cdir = getenv("LARECOMP_CACHE_DIR");
    if (cdir && *cdir) return cdir;
#ifdef __ANDROID__
    static char dir[512];
    const char *root = rt_data_root();
    if (root && *root) {
        snprintf(dir, sizeof(dir), "%s/cache", root);
        return dir;
    }
#endif
    return "cache";
}

static const char *get_manifest_path(void) {
    static char path[512];
    snprintf(path, sizeof(path), "%s/pipe_manifest.bin", get_cache_dir());
    return path;
}

static const char *get_pipe_state_path(void) {
    static char path[512];
    snprintf(path, sizeof(path), "%s/pipe_state_v1.bin", get_cache_dir());
    return path;
}

#define MANIFEST_MAGIC 0x52595A32u

static void load_manifest(void) {
    const char *path = get_manifest_path();
    FILE *f = fopen(path, "rb");
    if (!f) return;

    uint32_t hdr[2] = {0, 0};
    if (fread(hdr, sizeof(uint32_t), 2, f) == 2 && hdr[0] == MANIFEST_MAGIC && hdr[1] <= MAX_MANIFEST_PIPELINES) {
        pthread_mutex_lock(&s_manifest_lock);
        s_manifest_count = (uint32_t)fread(s_manifest_hashes, sizeof(uint64_t), hdr[1], f);
        atomic_store(&g_precompute_stats.pipelines_manifest_total, s_manifest_count);
        pthread_mutex_unlock(&s_manifest_lock);
    }
    fclose(f);
}

static bool ensure_pipe_state_buf_locked(void) {
    if (!s_pipe_states) {
        s_pipe_states = (PipeStateEntry *)calloc(MAX_PIPE_STATES, sizeof(PipeStateEntry));
    }
    return s_pipe_states != NULL;
}

static void load_pipe_states_locked(void) {
    s_pipe_state_loaded = true;
    s_pipe_state_count = 0;
    if (s_env_payload_size == 0 || s_env_payload_size > MAX_PIPE_STATE_PAYLOAD) return;

    const char *path = get_pipe_state_path();
    FILE *f = fopen(path, "rb");
    if (!f) return;

    PipeStateHeader hdr = {0};
    if (fread(&hdr, sizeof(hdr), 1, f) != 1 ||
        hdr.magic != PIPE_STATE_MAGIC ||
        hdr.version != PIPE_STATE_VERSION ||
        hdr.driver_id != s_env_driver_id ||
        hdr.eds_flags != s_env_eds_flags ||
        hdr.payload_size != s_env_payload_size ||
        hdr.count > MAX_PIPE_STATES) {
        fclose(f);
        return;
    }
    if (hdr.count == 0 || !ensure_pipe_state_buf_locked()) {
        fclose(f);
        return;
    }

    for (uint32_t i = 0; i < hdr.count; i++) {
        uint64_t hashes[2] = {0, 0};
        if (fread(hashes, sizeof(uint64_t), 2, f) != 2) break;
        if (fread(s_pipe_states[s_pipe_state_count].payload, 1, s_env_payload_size, f) != s_env_payload_size) break;
        s_pipe_states[s_pipe_state_count].pipe_hash = hashes[0];
        s_pipe_states[s_pipe_state_count].prog_hash = hashes[1];
        s_pipe_state_count++;
    }
    atomic_store(&g_precompute_stats.pipelines_cached, s_pipe_state_count);
    fclose(f);
}

static pthread_mutex_t s_manifest_io_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_manifest_io_cond = PTHREAD_COND_INITIALIZER;
static _Atomic bool s_manifest_writer_started = false;
static bool s_manifest_io_pending = false;

static void write_manifest_snapshot(uint32_t count, const uint64_t *hashes) {
    if (count == 0 || !hashes) return;
    const char *cdir = get_cache_dir();
    mkdir_p(cdir);

    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s/pipe_manifest.bin.tmp", cdir);

    pthread_mutex_lock(&s_manifest_io_lock);
    FILE *f = fopen(tmp_path, "wb");
    if (f) {
        uint32_t hdr[2] = {MANIFEST_MAGIC, count};
        bool ok = fwrite(hdr, sizeof(uint32_t), 2, f) == 2 &&
                  fwrite(hashes, sizeof(uint64_t), count, f) == count;
        if (fclose(f) != 0) ok = false;
        if (ok) {
            remove(get_manifest_path());
            rename(tmp_path, get_manifest_path());
        } else {
            remove(tmp_path);
        }
    }
    pthread_mutex_unlock(&s_manifest_io_lock);
}

static void save_manifest(void) {
    uint32_t count = 0;
    uint64_t *snap = NULL;
    pthread_mutex_lock(&s_manifest_lock);
    if (s_manifest_count > 0) {
        count = s_manifest_count;
        snap = malloc((size_t)count * sizeof(uint64_t));
        if (snap) memcpy(snap, s_manifest_hashes, (size_t)count * sizeof(uint64_t));
        atomic_store(&s_manifest_dirty, false);
        atomic_store(&s_last_manifest_save_ns, get_time_ns());
    }
    pthread_mutex_unlock(&s_manifest_lock);
    if (snap) {
        write_manifest_snapshot(count, snap);
        free(snap);
    }
}

static void write_pipe_state_snapshot(uint32_t count, uint32_t driver_id, uint32_t eds_flags,
                                      uint32_t payload_size, const PipeStateEntry *entries) {
    if (count == 0 || !entries || payload_size == 0 || payload_size > MAX_PIPE_STATE_PAYLOAD) return;
    const char *cdir = get_cache_dir();
    mkdir_p(cdir);

    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s/pipe_state_v1.bin.tmp", cdir);

    pthread_mutex_lock(&s_manifest_io_lock);
    FILE *f = fopen(tmp_path, "wb");
    if (f) {
        PipeStateHeader hdr = {
            .magic = PIPE_STATE_MAGIC,
            .version = PIPE_STATE_VERSION,
            .driver_id = driver_id,
            .eds_flags = eds_flags,
            .payload_size = payload_size,
            .count = count
        };
        bool ok = fwrite(&hdr, sizeof(hdr), 1, f) == 1;
        for (uint32_t i = 0; ok && i < count; i++) {
            uint64_t hashes[2] = {entries[i].pipe_hash, entries[i].prog_hash};
            if (fwrite(hashes, sizeof(uint64_t), 2, f) != 2 ||
                fwrite(entries[i].payload, 1, payload_size, f) != payload_size) {
                ok = false;
            }
        }
        if (fclose(f) != 0) ok = false;
        if (ok) {
            remove(get_pipe_state_path());
            rename(tmp_path, get_pipe_state_path());
        } else {
            remove(tmp_path);
        }
    }
    pthread_mutex_unlock(&s_manifest_io_lock);
}

static void save_pipe_states(void) {
    uint32_t count = 0;
    uint32_t driver_id = 0, eds_flags = 0, payload_size = 0;
    PipeStateEntry *snap = NULL;
    pthread_mutex_lock(&s_pipe_state_lock);
    if (s_pipe_state_count > 0 && s_pipe_states && s_env_payload_size > 0 &&
        atomic_load(&s_pipe_state_dirty)) {
        count = s_pipe_state_count;
        driver_id = s_env_driver_id;
        eds_flags = s_env_eds_flags;
        payload_size = s_env_payload_size;
        snap = (PipeStateEntry *)malloc((size_t)count * sizeof(PipeStateEntry));
        if (snap) memcpy(snap, s_pipe_states, (size_t)count * sizeof(PipeStateEntry));
        atomic_store(&s_pipe_state_dirty, false);
        atomic_store(&s_last_pipe_state_save_ns, get_time_ns());
    }
    pthread_mutex_unlock(&s_pipe_state_lock);
    if (snap) {
        write_pipe_state_snapshot(count, driver_id, eds_flags, payload_size, snap);
        free(snap);
    }
}

static void *manifest_writer_thread(void *arg) {
    (void)arg;
#ifdef __ANDROID__
    plat_thread_pin_background();
#endif
    for (;;) {
        pthread_mutex_lock(&s_manifest_io_lock);
        while (!s_manifest_io_pending) pthread_cond_wait(&s_manifest_io_cond, &s_manifest_io_lock);
        s_manifest_io_pending = false;
        pthread_mutex_unlock(&s_manifest_io_lock);
        save_manifest();
        save_pipe_states();
    }
    return NULL;
}

static void save_manifest_async(void) {
    if (!atomic_exchange(&s_manifest_writer_started, true)) {
        pthread_t th;
        if (pthread_create(&th, NULL, manifest_writer_thread, NULL) == 0) pthread_detach(th);
        else atomic_store(&s_manifest_writer_started, false);
    }
    if (atomic_load(&s_manifest_writer_started)) {
        atomic_store(&s_last_manifest_save_ns, get_time_ns());
        atomic_store(&s_last_pipe_state_save_ns, get_time_ns());
        pthread_mutex_lock(&s_manifest_io_lock);
        s_manifest_io_pending = true;
        pthread_cond_signal(&s_manifest_io_cond);
        pthread_mutex_unlock(&s_manifest_io_lock);
    } else {
        save_manifest();
        save_pipe_states();
    }
}

void precompute_init(void) {
    memset(&g_precompute_stats, 0, sizeof(g_precompute_stats));
    atomic_store(&g_precompute_stats.is_warmup_active, true);
    g_precompute_stats.warmup_start_ns = get_time_ns();
    load_manifest();
    pthread_mutex_lock(&s_pipe_state_lock);
    if (s_env_payload_size > 0) load_pipe_states_locked();
    else {
        s_pipe_state_count = 0;
        s_pipe_state_loaded = false;
    }
    pthread_mutex_unlock(&s_pipe_state_lock);
}

void precompute_shutdown(void) {
    save_manifest();
    save_pipe_states();
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
    save_pipe_states();
}

void precompute_tick(void) {
    bool m_dirty = atomic_load(&s_manifest_dirty);
    bool ps_dirty = atomic_load(&s_pipe_state_dirty);
    if (!m_dirty && !ps_dirty) return;
    uint64_t now = get_time_ns();
    uint64_t last_m = atomic_load(&s_last_manifest_save_ns);
    uint64_t last_ps = atomic_load(&s_last_pipe_state_save_ns);
    bool m_due = m_dirty && (!last_m || now - last_m >= 3000000000ull);
    bool ps_due = ps_dirty && (!last_ps || now - last_ps >= 10000000000ull);
    if (!m_due && !ps_due) return;
    save_manifest_async();
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
        atomic_store(&s_manifest_dirty, true);
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

void precompute_set_env(uint32_t driver_id, uint32_t eds_flags, uint32_t payload_size) {
    pthread_mutex_lock(&s_pipe_state_lock);
    bool changed = !s_pipe_state_loaded ||
                   s_env_driver_id != driver_id ||
                   s_env_eds_flags != eds_flags ||
                   s_env_payload_size != payload_size;
    s_env_driver_id = driver_id;
    s_env_eds_flags = eds_flags;
    s_env_payload_size = payload_size;
    if (changed) {
        atomic_store(&s_pipe_state_dirty, false);
        load_pipe_states_locked();
    }
    pthread_mutex_unlock(&s_pipe_state_lock);
}

void precompute_record_pipe_state(uint64_t pipe_hash, uint64_t prog_hash, const void *payload, uint32_t payload_size) {
    if (!pipe_hash || !prog_hash || !payload || payload_size == 0 || payload_size > MAX_PIPE_STATE_PAYLOAD) return;

    pthread_mutex_lock(&s_pipe_state_lock);
    if (s_env_payload_size != payload_size || !ensure_pipe_state_buf_locked()) {
        pthread_mutex_unlock(&s_pipe_state_lock);
        return;
    }
    for (uint32_t i = 0; i < s_pipe_state_count; i++) {
        if (s_pipe_states[i].pipe_hash == pipe_hash) {
            pthread_mutex_unlock(&s_pipe_state_lock);
            return;
        }
    }
    if (s_pipe_state_count < MAX_PIPE_STATES) {
        PipeStateEntry *e = &s_pipe_states[s_pipe_state_count++];
        e->pipe_hash = pipe_hash;
        e->prog_hash = prog_hash;
        memcpy(e->payload, payload, payload_size);
        atomic_store(&g_precompute_stats.pipelines_cached, s_pipe_state_count);
        atomic_store(&s_pipe_state_dirty, true);
    }
    pthread_mutex_unlock(&s_pipe_state_lock);
}

uint32_t precompute_lookup_prog_states(uint64_t prog_hash, uint64_t *out_pipe_hashes, void *out_payloads,
                                       uint32_t max_out, uint32_t payload_size) {
    if (!prog_hash || !out_pipe_hashes || !out_payloads || max_out == 0 ||
        payload_size == 0 || payload_size > MAX_PIPE_STATE_PAYLOAD) {
        return 0;
    }
    uint32_t found = 0;
    pthread_mutex_lock(&s_pipe_state_lock);
    if (s_pipe_states && s_env_payload_size == payload_size) {
        uint8_t *dst = (uint8_t *)out_payloads;
        for (uint32_t i = 0; i < s_pipe_state_count && found < max_out; i++) {
            if (s_pipe_states[i].prog_hash == prog_hash) {
                out_pipe_hashes[found] = s_pipe_states[i].pipe_hash;
                memcpy(dst + (size_t)found * payload_size, s_pipe_states[i].payload, payload_size);
                found++;
            }
        }
    }
    pthread_mutex_unlock(&s_pipe_state_lock);
    return found;
}

bool precompute_is_known_pipe_state(uint64_t pipe_hash) {
    if (!pipe_hash) return false;
    bool known = false;
    pthread_mutex_lock(&s_pipe_state_lock);
    if (s_pipe_states) {
        for (uint32_t i = 0; i < s_pipe_state_count; i++) {
            if (s_pipe_states[i].pipe_hash == pipe_hash) {
                known = true;
                break;
            }
        }
    }
    pthread_mutex_unlock(&s_pipe_state_lock);
    return known;
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

void precompute_get_snapshot(PrecomputeSnapshot *out) {
    if (!out) return;
    out->shaders_registered = atomic_load(&g_precompute_stats.shaders_registered);
    out->shaders_translated = atomic_load(&g_precompute_stats.shaders_translated);
    out->shaders_compiled = atomic_load(&g_precompute_stats.shaders_compiled);
    out->pipelines_manifest_total = atomic_load(&g_precompute_stats.pipelines_manifest_total);
    out->pipelines_prewarmed = atomic_load(&g_precompute_stats.pipelines_prewarmed);
    out->cache_hits = atomic_load(&g_precompute_stats.cache_hits);
    out->cache_misses = atomic_load(&g_precompute_stats.cache_misses);
    out->is_warmup_active = atomic_load(&g_precompute_stats.is_warmup_active);
}
