#include "ryza_forensics.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <pthread.h>

#if defined(__ANDROID__)
#include <jni.h>
extern void *SDL_GetAndroidJNIEnv(void);
extern void *SDL_GetAndroidActivity(void);
extern const char *SDL_GetAndroidExternalStoragePath(void);
extern const char *SDL_GetAndroidInternalStoragePath(void);
#endif

extern const char *rt_data_root(void);

RyzaDrawForensic g_ryza_captured_draws[RYZA_MAX_CAPTURED_DRAWS] = {0};
uint32_t g_ryza_captured_draw_count = 0;
uint64_t g_ryza_3d_draw_count = 0;
uint64_t g_ryza_character_draw_count = 0;
bool g_ryza_capture_freeze = false;
bool g_ryza_forensic_enabled = false;
char g_ryza_session_id[32] = {0};
static pthread_mutex_t g_ryza_capture_mutex = PTHREAD_MUTEX_INITIALIZER;
static volatile int g_ryza_auto_dump_pending = 0;
static volatile uint32_t g_ryza_auto_dump_draw = 0;

void ryza_capture_lock(void) { pthread_mutex_lock(&g_ryza_capture_mutex); }
void ryza_capture_unlock(void) { pthread_mutex_unlock(&g_ryza_capture_mutex); }

void ryza_request_auto_dump(uint32_t draw_idx) {
    g_ryza_auto_dump_draw = draw_idx;
    __atomic_store_n(&g_ryza_auto_dump_pending, 1, __ATOMIC_RELEASE);
}

void ryza_process_pending_auto_dump(void) {
    if (__atomic_exchange_n(&g_ryza_auto_dump_pending, 0, __ATOMIC_ACQ_REL)) {
        fprintf(stderr, "[RYZA-FORENSIC] AUTO-DUMP EXECUTING ON UI THREAD (Draw %u)\n", g_ryza_auto_dump_draw);
        ryza_export_diagnostic_log();
    }
}

RyzaDumpResult g_ryza_dump_result = {
    .status = RYZA_DUMP_STATUS_IDLE,
    .error_code = 0,
    .error_msg = "IDLE",
    .target_path = "",
    .bytes_written = 0,
    .timestamp = ""
};

RyzaTestResult g_ryza_test_result = {
    .ran = false,
    .passed = false,
    .error_code = 0,
    .error_msg = "",
    .target_path = "",
    .bytes_written = 0,
    .bytes_read = 0
};

#ifndef RYZA_BUILD_COMMIT
#define RYZA_BUILD_COMMIT "local"
#endif
#ifndef RYZA_VERSION_CODE
#define RYZA_VERSION_CODE "49"
#endif
#ifndef RYZA_VERSION_NAME
#define RYZA_VERSION_NAME "0.3.2-forensic-p17"
#endif
#ifndef RYZA_BUILD_VARIANT
#define RYZA_BUILD_VARIANT "precomputed-render-pipeline"
#endif

const char *ryza_get_build_fingerprint(void) {
    static char buf[192];
    static bool init = false;
    if (!init) {
        snprintf(buf, sizeof(buf), "%s (%s, vc=%s, %s %s) [%s]",
                 RYZA_BUILD_COMMIT, RYZA_VERSION_NAME, RYZA_VERSION_CODE,
                 __DATE__, __TIME__, RYZA_BUILD_VARIANT);
        init = true;
    }
    return buf;
}

void ryza_forensics_set_enabled(bool enabled) {
    ryza_capture_lock();
    g_ryza_forensic_enabled = enabled;
    ryza_capture_unlock();
}

void ryza_forensics_init(void) {
    const char *e = getenv("LARECOMP_FORENSIC_CAPTURE");
    g_ryza_forensic_enabled = (e && atoi(e) != 0);
    g_ryza_captured_draw_count = 0;
    g_ryza_3d_draw_count = 0;
    g_ryza_character_draw_count = 0;
    g_ryza_capture_freeze = false;
    memset(g_ryza_captured_draws, 0, sizeof(g_ryza_captured_draws));

    if (!g_ryza_session_id[0]) {
        snprintf(g_ryza_session_id, sizeof(g_ryza_session_id), "SESS-%lx", (unsigned long)time(NULL));
    }
}

void ryza_rearm_capture(void) {
    ryza_capture_lock();
    g_ryza_forensic_enabled = true;
    g_ryza_capture_freeze = false;
    g_ryza_captured_draw_count = 0;
    memset(g_ryza_captured_draws, 0, sizeof(g_ryza_captured_draws));
    ryza_capture_unlock();
    fprintf(stderr, "[RYZA-FORENSIC] Capture buffer cleared and re-armed.\n");
}

RyzaDrawForensic *ryza_allocate_capture_slot(uint32_t icount, uint32_t attr_mask, bool has_bones, bool is_character, bool is_deformed) {
    if (!g_ryza_forensic_enabled || g_ryza_capture_freeze) return NULL;

    static uint32_t s_idx_char = 0; // 0..15: Character Skinned Meshes
    static uint32_t s_idx_def = 16;  // 16..23: Deformed Meshes
    static uint32_t s_idx_env = 24;  // 24..31: Static Environment / Prop
    uint32_t slot = 0;

    if (is_deformed) {
        slot = s_idx_def++;
        if (s_idx_def > 23) s_idx_def = 16;
    } else if (has_bones || is_character) {
        slot = s_idx_char++;
        if (s_idx_char > 15) s_idx_char = 0;
    } else {
        slot = s_idx_env++;
        if (s_idx_env > 31) s_idx_env = 24;
    }

    if (slot >= RYZA_MAX_CAPTURED_DRAWS) slot = 0;
    RyzaDrawForensic *d = &g_ryza_captured_draws[slot];

    // If locked by character corruption and this is not a new deformation, preserve it
    if (d->is_valid && d->is_locked && !is_deformed) {
        return NULL;
    }

    memset(d, 0, sizeof(*d));
    d->is_valid = true;
    d->is_character = (has_bones || is_character);
    d->is_deformed = is_deformed;
    d->is_locked = is_deformed; // Automatically lock deformed submesh slot

    if (is_deformed) {
        if (has_bones || is_character) {
            snprintf(d->body_part, sizeof(d->body_part), "DEFORMED Ryza Submesh");
        } else {
            snprintf(d->body_part, sizeof(d->body_part), "DEFORMED Static/Env Submesh");
        }
    } else if (has_bones) {
        snprintf(d->body_part, sizeof(d->body_part), "Ryza Skinned Body / Bones");
    } else if (is_character) {
        snprintf(d->body_part, sizeof(d->body_part), "Ryza Character Submesh");
    } else {
        snprintf(d->body_part, sizeof(d->body_part), "Environment Mesh #%u", slot >= 24 ? (slot - 24) : slot);
    }

    if (slot + 1 > g_ryza_captured_draw_count) {
        g_ryza_captured_draw_count = slot + 1;
    }
    return d;
}

// Open existing log file stream using the existing application logger mechanism
static FILE *ryza_open_diagnostic_stream(const char *filename, char *out_resolved, size_t out_len, int *out_fd) {
    if (out_fd) *out_fd = -1;
    out_resolved[0] = '\0';
    FILE *f = NULL;
    int fd = -1;

#if defined(__ANDROID__)
    // 1. Direct integration with existing Android log mechanism: android_open_log_named
    fd = android_open_log_named(filename, true);
    if (fd >= 0) {
        if (out_fd) *out_fd = fd;
        snprintf(out_resolved, out_len, "Log System: %s", filename);
        f = fdopen(fd, "w");
        if (f) return f;
    }
#endif

    // 2. Existing app-specific storage directory fallback (rt_data_root)
    char cand[512];
    const char *root = rt_data_root();
    if (root && root[0]) {
        snprintf(cand, sizeof(cand), "%s/%s", root, filename);
        f = fopen(cand, "w");
        if (f) {
            snprintf(out_resolved, out_len, "%s", cand);
            return f;
        }
    }

#if defined(__ANDROID__)
    const char *ext = SDL_GetAndroidExternalStoragePath();
    if (ext && ext[0]) {
        snprintf(cand, sizeof(cand), "%s/%s", ext, filename);
        f = fopen(cand, "w");
        if (f) {
            snprintf(out_resolved, out_len, "%s", cand);
            return f;
        }
    }
#endif

    // 3. Fallback direct POSIX paths
    const char *posix_paths[] = {
        "/sdcard/Android/data/com.botwrecomp.android/files/",
        "/storage/emulated/0/Android/data/com.botwrecomp.android/files/",
        "/sdcard/Download/",
        "/storage/emulated/0/Download/"
    };
    for (size_t i = 0; i < sizeof(posix_paths) / sizeof(posix_paths[0]); i++) {
        snprintf(cand, sizeof(cand), "%s%s", posix_paths[i], filename);
        f = fopen(cand, "w");
        if (f) {
            snprintf(out_resolved, out_len, "%s", cand);
            return f;
        }
    }

    return NULL;
}

// Self-Test to verify storage write and readback integrity
void ryza_run_file_write_test(void) {
    memset(&g_ryza_test_result, 0, sizeof(g_ryza_test_result));
    g_ryza_test_result.ran = true;

    char resolved[256] = {0};
    int fd = -1;
    FILE *f = ryza_open_diagnostic_stream("ryza_storage_test.txt", resolved, sizeof(resolved), &fd);

    if (!f) {
        g_ryza_test_result.passed = false;
        g_ryza_test_result.error_code = errno;
        snprintf(g_ryza_test_result.error_msg, sizeof(g_ryza_test_result.error_msg),
                 "Failed to open test file: %s (errno %d)", strerror(errno), errno);
        snprintf(g_ryza_test_result.target_path, sizeof(g_ryza_test_result.target_path),
                 "%s", resolved[0] ? resolved : "None reachable");
        fprintf(stderr, "[RYZA-TEST] FAIL: %s\n", g_ryza_test_result.error_msg);
        return;
    }

    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char time_str[32];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);

    char test_payload[512];
    snprintf(test_payload, sizeof(test_payload),
             "=== RYZA STORAGE SELF-TEST VERIFICATION ===\n"
             "MAGIC: RYZA_VERIFY_640A_TEST_OK\n"
             "STATUS: PASS\n"
             "BUILD: %s\n"
             "TIMESTAMP: %s\n"
             "TARGET_PATH: %s\n"
             "===========================================\n",
             ryza_get_build_fingerprint(), time_str, resolved);

    size_t payload_len = strlen(test_payload);
    size_t written = fwrite(test_payload, 1, payload_len, f);
    fflush(f);
    if (fd >= 0) fsync(fd);
    fclose(f);

    g_ryza_test_result.bytes_written = written;
    snprintf(g_ryza_test_result.target_path, sizeof(g_ryza_test_result.target_path), "%s", resolved);

    // Read back verification
    int rfd = -1;
#if defined(__ANDROID__)
    rfd = android_open_log_for_read("ryza_storage_test.txt");
#endif

    FILE *rf = NULL;
    if (rfd >= 0) {
        rf = fdopen(rfd, "r");
    } else if (resolved[0] == '/') {
        rf = fopen(resolved, "r");
    }

    if (!rf) {
        g_ryza_test_result.passed = false;
        g_ryza_test_result.error_code = errno;
        snprintf(g_ryza_test_result.error_msg, sizeof(g_ryza_test_result.error_msg),
                 "Write OK (%zu bytes), but Readback failed: %s (errno %d)", written, strerror(errno), errno);
        fprintf(stderr, "[RYZA-TEST] FAIL READBACK: %s\n", g_ryza_test_result.error_msg);
        return;
    }

    char read_buf[512] = {0};
    size_t read_bytes = fread(read_buf, 1, sizeof(read_buf) - 1, rf);
    fclose(rf);

    g_ryza_test_result.bytes_read = read_bytes;
    if (read_bytes == written && strstr(read_buf, "RYZA_VERIFY_640A_TEST_OK") != NULL) {
        g_ryza_test_result.passed = true;
        g_ryza_test_result.error_code = 0;
        snprintf(g_ryza_test_result.error_msg, sizeof(g_ryza_test_result.error_msg),
                 "PASS (Verified %zu bytes match)", read_bytes);
        fprintf(stderr, "[RYZA-TEST] SUCCESS: Storage write & readback PASS on %s (%zu bytes)\n", resolved, read_bytes);
    } else {
        g_ryza_test_result.passed = false;
        g_ryza_test_result.error_code = -1;
        snprintf(g_ryza_test_result.error_msg, sizeof(g_ryza_test_result.error_msg),
                 "FAIL: Checksum mismatch (read %zu bytes, wrote %zu)", read_bytes, written);
        fprintf(stderr, "[RYZA-TEST] FAIL: Checksum mismatch on %s\n", resolved);
    }
}

static void serialize_forensics_to_file(FILE *f, const char *path, time_t now) {
    struct tm *tm_info = localtime(&now);
    char time_str[32];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);

    fprintf(f, "=======================================================\n");
    fprintf(f, "RYZA NATIVE ANDROID RUNTIME SKINNING FORENSIC DUMP (%s)\n", RYZA_VERSION_NAME);
    fprintf(f, "Build Fingerprint: %s\n", ryza_get_build_fingerprint());
    fprintf(f, "Session ID:        %s\n", g_ryza_session_id[0] ? g_ryza_session_id : "SESSION-RUNTIME");
    fprintf(f, "Dump Timestamp:    %s\n", time_str);
    fprintf(f, "Resolved Path:     %s\n", path);
    fprintf(f, "Total 3D Draws:    %llu\n", (unsigned long long)g_ryza_3d_draw_count);
    fprintf(f, "Character Draws:   %llu\n", (unsigned long long)g_ryza_character_draw_count);
    fprintf(f, "Captured Draws:    %u\n", g_ryza_captured_draw_count);
    fprintf(f, "Capture Freeze:    %s\n", g_ryza_capture_freeze ? "YES" : "NO");
    fprintf(f, "=======================================================\n\n");

    ryza_capture_lock();
    for (uint32_t i = 0; i < g_ryza_captured_draw_count; i++) {
        RyzaDrawForensic *d = &g_ryza_captured_draws[i];
        if (!d->is_valid) continue;

        fprintf(f, "--- [DRAW #%u: %s] Frame: %llu | DrawIdx: %u ---\n",
                i, d->body_part, (unsigned long long)d->frame, d->draw_idx);
        fprintf(f, "  Classification: Character=%s | Deformed=%s | Locked=%s\n",
                d->is_character ? "YES" : "NO",
                d->is_deformed ? "YES (HORIZONTAL STRETCH DETECTED)" : "NO",
                d->is_locked ? "YES" : "NO");
        fprintf(f, "  Shaders: VS=0x%llx | FS=0x%llx (Hash: 0x%016llx)\n",
                (unsigned long long)d->vs_code, (unsigned long long)d->fs_code,
                (unsigned long long)d->fs_hash);
        fprintf(f, "  AttrMask: 0x%08x | NAttrib: %u | NStream: %u\n",
                d->attr_mask, d->nattrib, d->nstream);
        fprintf(f, "  Pipeline ID: 0x%016llx\n", (unsigned long long)(d->vs_code ^ (d->fs_code << 1)));
        fprintf(f, "  IndexBuffer: Type=%u, Count=%u, Addr=0x%llx, Range=[%u, %u], BaseVtx=%d\n",
                d->itype, d->icount, (unsigned long long)d->iaddr, d->min_idx, d->max_idx, d->base_vertex);

        if (d->cbuf1_addr) {
            fprintf(f, "  VS_UBO_Primary (hw c%u @ 0x%llx, sz=%u):\n",
                    d->cbuf1_bank ? d->cbuf1_bank : 3, (unsigned long long)d->cbuf1_addr, d->cbuf1_size);
            fprintf(f, "    [%.4f, %.4f, %.4f, %.4f]\n", d->cbuf1_sample[0], d->cbuf1_sample[1], d->cbuf1_sample[2], d->cbuf1_sample[3]);
            fprintf(f, "    [%.4f, %.4f, %.4f, %.4f]\n", d->cbuf1_sample[4], d->cbuf1_sample[5], d->cbuf1_sample[6], d->cbuf1_sample[7]);
            fprintf(f, "    [%.4f, %.4f, %.4f, %.4f]\n", d->cbuf1_sample[8], d->cbuf1_sample[9], d->cbuf1_sample[10], d->cbuf1_sample[11]);
            fprintf(f, "    [%.4f, %.4f, %.4f, %.4f]\n", d->cbuf1_sample[12], d->cbuf1_sample[13], d->cbuf1_sample[14], d->cbuf1_sample[15]);
        }
        if (d->cbuf3_addr) {
            fprintf(f, "  VS_UBO_Secondary (hw c%u @ 0x%llx, sz=%u): sample first 2 vec4 blocks\n",
                    d->cbuf3_bank ? d->cbuf3_bank : 4, (unsigned long long)d->cbuf3_addr, d->cbuf3_size);
            for (int b = 0; b < 2; b++) {
                fprintf(f, "    Block[%d] r0: [%.4f, %.4f, %.4f, %.4f]\n", b, d->cbuf3_sample[b*16+0], d->cbuf3_sample[b*16+1], d->cbuf3_sample[b*16+2], d->cbuf3_sample[b*16+3]);
                fprintf(f, "    Block[%d] r1: [%.4f, %.4f, %.4f, %.4f]\n", b, d->cbuf3_sample[b*16+4], d->cbuf3_sample[b*16+5], d->cbuf3_sample[b*16+6], d->cbuf3_sample[b*16+7]);
                fprintf(f, "    Block[%d] r2: [%.4f, %.4f, %.4f, %.4f]\n", b, d->cbuf3_sample[b*16+8], d->cbuf3_sample[b*16+9], d->cbuf3_sample[b*16+10], d->cbuf3_sample[b*16+11]);
            }
        }

        fprintf(f, "  Attributes Layout:\n");
        for (uint32_t a = 0; a < RYZA_MAX_ATTRS; a++) {
            if (d->attr_mask & (1u << a)) {
                fprintf(f, "    Attr[%2u]: stream=%u, nvn_fmt=0x%02x, vk_fmt=%u%s, kind=%u, off=%u, stride=%u, buf=0x%llx (sz=%llu)\n",
                        a, d->attrs[a].stream, d->attrs[a].nvn_fmt, d->attrs[a].vk_fmt,
                        d->attrs[a].was_converted ? "(CPU-conv)" : "",
                        d->attrs[a].kind, d->attrs[a].off, d->attrs[a].stride,
                        (unsigned long long)d->attrs[a].buf_addr, (unsigned long long)d->attrs[a].buf_size);
                fprintf(f, "              raw=[0x%08x, 0x%08x, 0x%08x, 0x%08x] flt=[%.4f, %.4f, %.4f, %.4f]\n",
                        d->attrs[a].raw_data[0], d->attrs[a].raw_data[1], d->attrs[a].raw_data[2], d->attrs[a].raw_data[3],
                        d->attrs[a].float_data[0], d->attrs[a].float_data[1], d->attrs[a].float_data[2], d->attrs[a].float_data[3]);
            }
        }

        fprintf(f, "  Sampled Vertices (%u samples):\n", d->num_samples);
        for (uint32_t s = 0; s < d->num_samples; s++) {
            RyzaVertexSample *vs = &d->samples[s];
            fprintf(f, "    Sample[%u] VtxIdx=%u | Corrupted=%s | W_Valid=%s\n",
                    s, vs->vertex_idx, vs->is_corrupted ? "YES (HORIZONTAL STRETCH)" : "NO",
                    vs->w_valid ? "YES" : "NO (W<=0 or Inf/NaN)");
            fprintf(f, "      Pos:  [%.4f, %.4f, %.4f, %.4f]\n",
                    vs->pos[0], vs->pos[1], vs->pos[2], vs->pos[3]);
            fprintf(f, "      Norm: [%.4f, %.4f, %.4f, %.4f]\n",
                    vs->norm[0], vs->norm[1], vs->norm[2], vs->norm[3]);
            fprintf(f, "      BoneIndices: [%u, %u, %u, %u]\n",
                    vs->bone_indices[0], vs->bone_indices[1], vs->bone_indices[2], vs->bone_indices[3]);
            fprintf(f, "      BoneWeights: [%.4f, %.4f, %.4f, %.4f]\n",
                    vs->bone_weights[0], vs->bone_weights[1], vs->bone_weights[2], vs->bone_weights[3]);
            fprintf(f, "      Raw Attr0: [%.4f, %.4f, %.4f, %.4f]\n",
                    vs->attr_raw[0][0], vs->attr_raw[0][1], vs->attr_raw[0][2], vs->attr_raw[0][3]);
            fprintf(f, "      Raw Attr1: [%.4f, %.4f, %.4f, %.4f]\n",
                    vs->attr_raw[1][0], vs->attr_raw[1][1], vs->attr_raw[1][2], vs->attr_raw[1][3]);
            fprintf(f, "      Raw Attr2: [%.4f, %.4f, %.4f, %.4f]\n",
                    vs->attr_raw[2][0], vs->attr_raw[2][1], vs->attr_raw[2][2], vs->attr_raw[2][3]);
            fprintf(f, "      Raw Attr3: [%.4f, %.4f, %.4f, %.4f]\n",
                    vs->attr_raw[3][0], vs->attr_raw[3][1], vs->attr_raw[3][2], vs->attr_raw[3][3]);
            fprintf(f, "      ClipSpace:   [X=%.4f, Y=%.4f, Z=%.4f, W=%.4f] (X/W=%.4f)\n",
                    vs->clip_pos[0], vs->clip_pos[1], vs->clip_pos[2], vs->clip_pos[3],
                    vs->clip_pos[3] != 0.0f ? vs->clip_pos[0] / vs->clip_pos[3] : 999999.0f);
        }

        if (d->vs_glsl[0]) {
            fprintf(f, "  Vertex Shader GLSL Snippet (up to 150 lines):\n");
            const char *cur = d->vs_glsl;
            int line_count = 0;
            while (*cur && line_count < 150) {
                const char *next = strchr(cur, '\n');
                if (next) {
                    fprintf(f, "    %.*s\n", (int)(next - cur), cur);
                    cur = next + 1;
                } else {
                    fprintf(f, "    %s\n", cur);
                    break;
                }
                line_count++;
            }
        }

        fprintf(f, "  Rasterization/Depth:\n");
        fprintf(f, "    CullMode=%u, FrontFace=%u, DepthTest=%u, DepthWrite=%u, DepthOp=%u\n\n",
                d->cull_mode, d->front_face, d->depth_test, d->depth_write, d->depth_compare_op);
    }
    ryza_capture_unlock();
}

void ryza_export_diagnostic_log(void) {
    g_ryza_dump_result.status = RYZA_DUMP_STATUS_RUNNING;

    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char time_str[32];
    strftime(time_str, sizeof(time_str), "%Y%m%d_%H%M%S", tm_info);

    char timestamped_filename[64];
    snprintf(timestamped_filename, sizeof(timestamped_filename), "ryza_forensic_dump_%s.txt", time_str);

    char resolved_primary[256] = {0};
    int fd_primary = -1;
    FILE *f_primary = ryza_open_diagnostic_stream("ryza_forensic_dump.txt", resolved_primary, sizeof(resolved_primary), &fd_primary);

    if (!f_primary) {
        g_ryza_dump_result.status = RYZA_DUMP_STATUS_FAILED;
        g_ryza_dump_result.error_code = errno;
        snprintf(g_ryza_dump_result.error_msg, sizeof(g_ryza_dump_result.error_msg),
                 "Failed opening dump file: %s (errno %d)", strerror(errno), errno);
        snprintf(g_ryza_dump_result.target_path, sizeof(g_ryza_dump_result.target_path), "%s", resolved_primary);
        fprintf(stderr, "[RYZA-FORENSIC] EXPORT FAILED: %s\n", g_ryza_dump_result.error_msg);
        return;
    }

    serialize_forensics_to_file(f_primary, resolved_primary, now);
    long bytes = ftell(f_primary);
    fflush(f_primary);
    if (fd_primary >= 0) fsync(fd_primary);
    fclose(f_primary);

    g_ryza_dump_result.status = RYZA_DUMP_STATUS_SUCCESS;
    g_ryza_dump_result.error_code = 0;
    snprintf(g_ryza_dump_result.error_msg, sizeof(g_ryza_dump_result.error_msg), "SUCCESS");
    snprintf(g_ryza_dump_result.target_path, sizeof(g_ryza_dump_result.target_path), "%s", resolved_primary);
    g_ryza_dump_result.bytes_written = bytes > 0 ? (size_t)bytes : 0;
    strftime(g_ryza_dump_result.timestamp, sizeof(g_ryza_dump_result.timestamp), "%H:%M:%S", tm_info);

    fprintf(stderr, "[RYZA-FORENSIC] EXPORT SUCCESS: %zu bytes written to %s\n", g_ryza_dump_result.bytes_written, resolved_primary);

    // Also attempt writing timestamped backup copy
    char resolved_ts[256] = {0};
    int fd_ts = -1;
    FILE *f_ts = ryza_open_diagnostic_stream(timestamped_filename, resolved_ts, sizeof(resolved_ts), &fd_ts);
    if (f_ts) {
        serialize_forensics_to_file(f_ts, resolved_ts, now);
        fflush(f_ts);
        if (fd_ts >= 0) fsync(fd_ts);
        fclose(f_ts);
        fprintf(stderr, "[RYZA-FORENSIC] Timestamped copy exported to: %s\n", resolved_ts);
    }
}
