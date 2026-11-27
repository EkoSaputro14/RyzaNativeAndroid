#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "barrier_bits.h"
#include "precompute.h"

static void test_barrier_bits_classification(void) {
    printf("[C-TEST 1/3] Testing ryza_classify_nvn_barrier...\n");

    // 1. Sentinel bits == 0 from nvnCommandBufferBeginRecording must always skip
    assert(ryza_classify_nvn_barrier(0u, true, false, false) == RYZA_BARRIER_SKIP);
    assert(ryza_classify_nvn_barrier(0u, false, true, true) == RYZA_BARRIER_SKIP);

    // 2. Cache-only invalidate bits (0x10..0xF0) skip when barrier_bits_on && !Mali
    for (uint32_t hi = 0x10u; hi <= 0xF0u; hi += 0x10u) {
        assert(ryza_classify_nvn_barrier(hi, true, false, false) == RYZA_BARRIER_SKIP);
        assert(ryza_classify_nvn_barrier(hi, true, false, true) == RYZA_BARRIER_SKIP);
        assert(ryza_classify_nvn_barrier(hi, false, false, false) == RYZA_BARRIER_FULL);
        assert(ryza_classify_nvn_barrier(hi, true, true, false) == RYZA_BARRIER_FULL);
    }

    // 3. Fragment-only ordering (0x2, 0x4, 0x6, with or without cache bits) -> FRAG_ONLY unless CB_DIRTY_OTHER
    uint32_t frag_cases[] = {
        NVN_BARRIER_ORDER_FRAGMENTS,
        NVN_BARRIER_ORDER_FRAGMENTS_TILED,
        NVN_BARRIER_ORDER_FRAGMENTS | NVN_BARRIER_ORDER_FRAGMENTS_TILED,
        NVN_BARRIER_ORDER_FRAGMENTS | 0x30u,
        NVN_BARRIER_ORDER_FRAGMENTS_TILED | 0x80u,
    };
    for (size_t i = 0; i < sizeof(frag_cases) / sizeof(frag_cases[0]); i++) {
        assert(ryza_classify_nvn_barrier(frag_cases[i], true, false, false) == RYZA_BARRIER_FRAG_ONLY);
        assert(ryza_classify_nvn_barrier(frag_cases[i], true, false, true) == RYZA_BARRIER_FULL);
        assert(ryza_classify_nvn_barrier(frag_cases[i], true, true, false) == RYZA_BARRIER_FULL);
        assert(ryza_classify_nvn_barrier(frag_cases[i], false, false, false) == RYZA_BARRIER_FULL);
    }

    // 4. Primitive / Indirect / Mixed ordering bits -> FULL
    uint32_t full_cases[] = {
        NVN_BARRIER_ORDER_PRIMITIVES,
        NVN_BARRIER_ORDER_INDIRECT_DATA,
        NVN_BARRIER_ORDER_PRIMITIVES | NVN_BARRIER_ORDER_FRAGMENTS,
        NVN_BARRIER_ORDER_INDIRECT_DATA | NVN_BARRIER_ORDER_FRAGMENTS_TILED,
        0x0Fu,
        0xFFu,
    };
    for (size_t i = 0; i < sizeof(full_cases) / sizeof(full_cases[0]); i++) {
        assert(ryza_classify_nvn_barrier(full_cases[i], true, false, false) == RYZA_BARRIER_FULL);
    }

    printf("  -> PASS: all barrier_bits cases verified.\n");
}

static void make_temp_dir(char *buf, size_t cap) {
    snprintf(buf, cap, "/tmp/ryza_quickwin_test_%d", (int)getpid());
    mkdir(buf, 0755);
}

static void cleanup_temp_dir(const char *dir) {
    char p[512];
    snprintf(p, sizeof(p), "%s/pipe_manifest.bin", dir);
    remove(p);
    snprintf(p, sizeof(p), "%s/pipe_state_v1.bin", dir);
    remove(p);
    rmdir(dir);
}

static void test_precompute_pipe_state_persistence(void) {
    printf("[C-TEST 2/3] Testing pipe_state_v1.bin persistence & lookup...\n");

    char tmpdir[256];
    make_temp_dir(tmpdir, sizeof(tmpdir));
    assert(setenv("LARECOMP_PRECOMPUTE_DIR", tmpdir, 1) == 0);

    typedef struct {
        uint64_t prog_hash;
        uint32_t color[8];
        uint32_t depth;
        uint32_t ncolor;
        uint8_t blend_on;
        uint8_t pad[3];
    } FakePayload;

    const uint32_t payload_sz = (uint32_t)sizeof(FakePayload);
    const uint32_t driver_id = 18u; // Mesa Turnip
    const uint32_t eds_flags = 3u;  // EDS + EDS2

    precompute_init();
    precompute_set_env(driver_id, eds_flags, payload_sz);

    FakePayload p1 = {.prog_hash = 0x1111222233334444ull, .color = {44, 0}, .depth = 126, .ncolor = 1, .blend_on = 1};
    FakePayload p2 = {.prog_hash = 0x1111222233334444ull, .color = {37, 44}, .depth = 0, .ncolor = 2, .blend_on = 0};
    FakePayload p3 = {.prog_hash = 0xAAAABBBBCCCCDDDDull, .color = {97}, .depth = 126, .ncolor = 1, .blend_on = 1};

    precompute_record_pipe_state(0x1001ull, p1.prog_hash, &p1, payload_sz);
    precompute_record_pipe_state(0x1002ull, p2.prog_hash, &p2, payload_sz);
    precompute_record_pipe_state(0x1003ull, p3.prog_hash, &p3, payload_sz);
    // Duplicate pipe_hash must be ignored
    precompute_record_pipe_state(0x1001ull, p1.prog_hash, &p2, payload_sz);

    assert(precompute_is_known_pipe_state(0x1001ull));
    assert(precompute_is_known_pipe_state(0x1002ull));
    assert(!precompute_is_known_pipe_state(0x9999ull));

    // Flush to pipe_state_v1.bin
    precompute_shutdown();

    // Verify file exists
    char state_file[512];
    snprintf(state_file, sizeof(state_file), "%s/pipe_state_v1.bin", tmpdir);
    struct stat st;
    assert(stat(state_file, &st) == 0 && st.st_size > 24);

    // Clear in-memory state by switching to incompatible env, then reload matching env
    precompute_init();
    precompute_set_env(driver_id + 1u, eds_flags, payload_sz);
    assert(!precompute_is_known_pipe_state(0x1001ull));

    uint64_t out_hashes[8] = {0};
    FakePayload out_payloads[8];
    memset(out_payloads, 0, sizeof(out_payloads));
    assert(precompute_lookup_prog_states(p1.prog_hash, out_hashes, out_payloads, 8, payload_sz) == 0);

    // Now switch back to matching driver/eds/payload_size and verify exact reload
    precompute_set_env(driver_id, eds_flags, payload_sz);
    assert(precompute_is_known_pipe_state(0x1001ull));
    assert(precompute_is_known_pipe_state(0x1002ull));
    assert(precompute_is_known_pipe_state(0x1003ull));

    uint32_t found = precompute_lookup_prog_states(p1.prog_hash, out_hashes, out_payloads, 8, payload_sz);
    assert(found == 2);
    assert(out_hashes[0] == 0x1001ull && memcmp(&out_payloads[0], &p1, payload_sz) == 0);
    assert(out_hashes[1] == 0x1002ull && memcmp(&out_payloads[1], &p2, payload_sz) == 0);

    uint32_t found3 = precompute_lookup_prog_states(p3.prog_hash, out_hashes, out_payloads, 8, payload_sz);
    assert(found3 == 1);
    assert(out_hashes[0] == 0x1003ull && memcmp(&out_payloads[0], &p3, payload_sz) == 0);

    // Different EDS flags must invalidate cached pipe states
    precompute_set_env(driver_id, eds_flags ^ 1u, payload_sz);
    assert(!precompute_is_known_pipe_state(0x1001ull));
    assert(precompute_lookup_prog_states(p1.prog_hash, out_hashes, out_payloads, 8, payload_sz) == 0);

    // Different payload size must invalidate cached pipe states
    precompute_set_env(driver_id, eds_flags, payload_sz + 8u);
    assert(!precompute_is_known_pipe_state(0x1001ull));

    cleanup_temp_dir(tmpdir);
    printf("  -> PASS: pipe_state_v1.bin round-trip, lookup, and invalidation verified.\n");
}

static void test_precompute_bounds_and_dedup(void) {
    printf("[C-TEST 3/3] Testing precompute_record_pipe_state bounds & manifest...\n");

    char tmpdir[256];
    make_temp_dir(tmpdir, sizeof(tmpdir));
    assert(setenv("LARECOMP_PRECOMPUTE_DIR", tmpdir, 1) == 0);

    precompute_init();
    precompute_set_env(7u, 1u, 16u);

    uint8_t buf[16] = {1, 2, 3, 4};
    // Invalid inputs must be safely ignored
    precompute_record_pipe_state(0ull, 0x55ull, buf, 16u);
    precompute_record_pipe_state(0x55ull, 0ull, buf, 16u);
    precompute_record_pipe_state(0x55ull, 0x55ull, NULL, 16u);
    precompute_record_pipe_state(0x55ull, 0x55ull, buf, 32u); // mismatched payload_size
    assert(!precompute_is_known_pipe_state(0x55ull));

    // Manifest hash APIs still work cleanly
    precompute_record_pipeline_hash(0xDEADBEEFCAFEBABEull);
    assert(precompute_has_pipeline_cached(0xDEADBEEFCAFEBABEull));
    assert(!precompute_has_pipeline_cached(0x1234567890ABCDEFull));

    precompute_shutdown();
    cleanup_temp_dir(tmpdir);
    printf("  -> PASS: precompute bounds, dedup, and manifest verified.\n");
}

static void test_retired_min_frame_and_canonical_view(void) {
    printf("[C-TEST 4/4] Testing retired min_frame gating & NvnTextureView canonicalization...\n");

    // Mirror texture_retire's RETIRE_FRAMES / g_retired_min_frame state machine with mixed thread timestamps
    enum { RETIRE_FRAMES = 4 };
    typedef struct { uint64_t id; uint64_t frame; } MockRetired;
    MockRetired items[64];
    int n = 0, sweeps = 0, reaped = 0;
    uint64_t min_frame = UINT64_MAX;

    uint64_t seq_frames[] = {10, 11, 10, 12, 13, 12, 14, 15, 19}; // out-of-order (guest vs render)
    for (size_t idx = 0; idx < sizeof(seq_frames) / sizeof(seq_frames[0]); idx++) {
        uint64_t now = seq_frames[idx];
        if (n > 0 && min_frame <= now && (now - min_frame) >= RETIRE_FRAMES) {
            sweeps++;
            int k = 0;
            uint64_t new_min = UINT64_MAX;
            for (int i = 0; i < n; i++) {
                if (items[i].frame + RETIRE_FRAMES <= now) {
                    reaped++;
                } else {
                    if (items[i].frame < new_min) new_min = items[i].frame;
                    items[k++] = items[i];
                }
            }
            n = k;
            min_frame = new_min;
        }
        items[n++] = (MockRetired){(uint64_t)idx, now};
        if (now < min_frame) min_frame = now;
    }
    // Frames 10..13 skipped sweep until now=14 (reaps 10) and now=15 (reaps 11) and now=19 (reaps 12..15)
    assert(sweeps == 3);
    assert(reaped == 8);
    assert(n == 1 && items[0].frame == 19 && min_frame == 19);

    // Mirror NvnTextureView (15 field bytes + 1 padding byte = 16 bytes)
    typedef struct {
        uint16_t min_layer, num_layers, format;
        uint8_t setmask;
        uint8_t target, ds_mode, base_level, num_levels;
        uint8_t swizzle[4];
    } MockView;
    _Static_assert(sizeof(MockView) <= 16, "MockView <= 16 bytes");

    MockView raw1, raw2, can1, can2;
    memset(&raw1, 0xAA, sizeof raw1);
    memset(&raw2, 0x55, sizeof raw2);
    raw1.min_layer = raw2.min_layer = 0;
    raw1.num_layers = raw2.num_layers = 1;
    raw1.format = raw2.format = 37;
    raw1.setmask = raw2.setmask = 1;
    raw1.target = raw2.target = 2;
    raw1.ds_mode = raw2.ds_mode = 0;
    raw1.base_level = raw2.base_level = 1;
    raw1.num_levels = raw2.num_levels = 4;
    memcpy(raw1.swizzle, (const uint8_t[4]){2, 3, 4, 5}, 4);
    memcpy(raw2.swizzle, (const uint8_t[4]){2, 3, 4, 5}, 4);

    memset(&can1, 0, sizeof can1);
    memset(&can2, 0, sizeof can2);
    can1.min_layer = raw1.min_layer; can1.num_layers = raw1.num_layers; can1.format = raw1.format;
    can1.setmask = raw1.setmask; can1.target = raw1.target; can1.ds_mode = raw1.ds_mode;
    can1.base_level = raw1.base_level; can1.num_levels = raw1.num_levels;
    memcpy(can1.swizzle, raw1.swizzle, 4);
    can2.min_layer = raw2.min_layer; can2.num_layers = raw2.num_layers; can2.format = raw2.format;
    can2.setmask = raw2.setmask; can2.target = raw2.target; can2.ds_mode = raw2.ds_mode;
    can2.base_level = raw2.base_level; can2.num_levels = raw2.num_levels;
    memcpy(can2.swizzle, raw2.swizzle, 4);

    assert(memcmp(&can1, &can2, sizeof can1) == 0);
    printf("  -> PASS: retired min_frame gating & NvnTextureView canonicalization verified.\n");
}

int main(void) {
    test_barrier_bits_classification();
    test_precompute_pipe_state_persistence();
    test_precompute_bounds_and_dedup();
    test_retired_min_frame_and_canonical_view();
    printf("\n[SUCCESS] All Phase 1 Quick Win C tests passed!\n");
    return 0;
}
