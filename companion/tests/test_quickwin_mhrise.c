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

int main(void) {
    test_barrier_bits_classification();
    test_precompute_pipe_state_persistence();
    test_precompute_bounds_and_dedup();
    printf("\n[SUCCESS] All Phase 1 Quick Win C tests passed!\n");
    return 0;
}
