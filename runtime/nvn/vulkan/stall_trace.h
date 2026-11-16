#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Low-overhead frame & thread stall instrumentation (Phase 2).
// Thresholds: Warning > 50 ms, Severe > 100 ms, Critical > 250 ms, Extreme > 500 ms.
#define STALL_THRESH_WARN_NS     50000000ull
#define STALL_THRESH_SEVERE_NS  100000000ull
#define STALL_THRESH_CRIT_NS    250000000ull
#define STALL_THRESH_EXTREME_NS 500000000ull

// Per-frame telemetry snapshot passed by vk_prof_frame at present boundary.
typedef struct {
    uint64_t frame_id;
    uint64_t frame_start_ns;
    uint64_t frame_end_ns;
    uint64_t total_ns;
    uint64_t guest_exec_ns;
    uint64_t wait_game_queue_ns;
    uint64_t wait_game_coalesce_ns;
    uint64_t backend_exec_ns;
    uint64_t vk_cpu_prep_ns;
    uint64_t gpu_ns;
    uint64_t gpu_wait_ns;
    uint64_t acquire_ns;
    uint64_t present_ns;
    uint64_t tex_uploads;
    uint64_t tex_upload_bytes;
    uint64_t tex_deswizzle_ns;
    uint64_t lock_wait_ns;
    uint64_t draws;
    uint64_t submits;
    uint64_t compiles_delta;
    int compiles_pending;
} StallFrameMetrics;

// Register current thread's human-readable role and optional guest thread ID (cached in TLS).
void stall_set_thread_name(const char *role, uint64_t guest_tid);
const char *stall_thread_name(void);
int stall_thread_tid(void);

// Lock wait and wait-event recording (only invoked on contended locks or slow waits).
void stall_record_lock_wait(const char *subsystem, const char *lock_name, uint64_t wait_ns);
void stall_record_wait_event(uint64_t frame_id, uint64_t start_ns, uint64_t end_ns,
                             const char *subsystem, const char *reason,
                             int waited_on_tid, const char *waited_on_name,
                             const char *active_work);

// Per-frame incremental counters (render thread & shared lock waits).
void stall_note_tex_upload(uint64_t bytes, uint64_t deswizzle_ns);
void stall_take_tex_stats(uint64_t *uploads, uint64_t *bytes, uint64_t *deswizzle_ns);
void stall_note_acquire(uint64_t acquire_ns);
uint64_t stall_take_acquire_ns(void);
uint64_t stall_take_lock_wait_ns(void);
void stall_note_guest_window(uint64_t exec_ns);
uint64_t stall_take_guest_exec_ns(void);
void stall_note_active_work(const char *work);

// Frame boundary evaluation: updates stall buckets & logs only when total_ns > 50 ms.
void stall_on_frame_end(const StallFrameMetrics *m);
void stall_format_summary(char *buf, size_t cap);
void stall_trace_reset(void);
