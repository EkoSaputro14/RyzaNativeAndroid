#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Low-overhead frame & thread stall instrumentation (Phase 2 + Sprint 2).
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
    uint64_t vk_submit_ns;
    uint64_t gpu_ns;
    uint64_t gpu_wait_ns;
    uint64_t acquire_ns;
    uint64_t present_ns;
    uint64_t sync_wait_ns;
    uint64_t gcopy_ns;
    uint64_t page_fault_ns;
    uint64_t page_fault_count;
    uint64_t tex_uploads;
    uint64_t tex_upload_bytes;
    uint64_t tex_deswizzle_ns;
    uint64_t lock_wait_ns;
    uint64_t bg_lock_wait_ns;
    uint64_t draws;
    uint64_t submits;
    uint64_t compiles_delta;
    int compiles_pending;
} StallFrameMetrics;

// Per-texture upload breakdown (Sprint 2: evaluate >1 MB deswizzle vs staging vs commit).
typedef struct StallTexUploadDetail {
    uint64_t addr;
    uint64_t total_bytes;
    uint64_t staged_bytes;
    uint32_t width, height, depth;
    uint32_t levels, layers;
    uint32_t nvn_format;
    uint32_t bc_vf;
    uint8_t astc_bw, astc_bh;
    bool was_uploaded;
    bool is_dynamic;
    bool is_linear;
    bool is_rt;
    uint64_t hash_ns;
    uint64_t deswizzle_ns;
    uint64_t staging_ns;
    uint64_t commit_ns;
    uint64_t total_ns;
} StallTexUploadDetail;

// Detailed kernel synchronization / lock / condvar / arbiter wait record (Sprint 2).
typedef struct StallKernelWaitDetail {
    uint64_t frame_id;
    uint64_t start_ns;
    uint64_t end_ns;
    uint64_t wait_ns;
    int64_t timeout_ns;
    const char *wait_kind_str;
    uint64_t waiter_id;
    int waiter_host_tid;
    int waiter_prio;
    int waiter_core;
    uint64_t wait_addr;
    uint64_t wait_mutex;
    uint32_t wait_tag;
    uint32_t wait_result;
    uint64_t owner_id;
    int owner_host_tid;
    int owner_prio;
    int owner_core;
    uint64_t owner_lr;
    uint64_t waker_id;
    int waker_host_tid;
    int waker_prio;
    int waker_core;
    uint64_t waker_lr;
    uint64_t waiter_bt[4];
} StallKernelWaitDetail;

// Register current thread's human-readable role and optional guest thread ID (cached in TLS).
void stall_set_thread_name(const char *role, uint64_t guest_tid);
const char *stall_thread_name(void);
int stall_thread_tid(void);

// Lock wait and wait-event recording (only invoked on contended locks or slow waits).
void stall_record_lock_wait(const char *subsystem, const char *lock_name, uint64_t wait_ns);
void stall_record_bg_lock_wait(const char *subsystem, const char *lock_name, uint64_t wait_ns);
void stall_record_wait_event(uint64_t frame_id, uint64_t start_ns, uint64_t end_ns,
                             const char *subsystem, const char *reason,
                             int waited_on_tid, const char *waited_on_name,
                             const char *active_work);

// Sprint 2: Per-call nvnQueueFinish / nvnSyncWait telemetry.
void stall_record_nvn_sync_call(const char *op, uint64_t frame_id,
                                uint64_t start_ns, uint64_t async_done_ns, uint64_t end_ns,
                                uint64_t caller_lr, int64_t timeout_ns,
                                uint64_t fence_lseq, uint64_t target_seq,
                                uint64_t submitted_seq, uint64_t completed_seq);

// Sprint 2: Detailed kernel wait & priority-inversion diagnostics.
void stall_record_kernel_wait_detail(const StallKernelWaitDetail *d);

// Per-frame incremental counters (render thread & shared lock waits).
void stall_note_tex_upload(uint64_t bytes, uint64_t deswizzle_ns);
void stall_note_tex_upload_detail(const StallTexUploadDetail *d);
void stall_take_tex_stats(uint64_t *uploads, uint64_t *bytes, uint64_t *deswizzle_ns);
void stall_note_acquire(uint64_t acquire_ns);
uint64_t stall_take_acquire_ns(void);
uint64_t stall_take_lock_wait_ns(void);
uint64_t stall_take_bg_lock_wait_ns(void);
void stall_note_guest_window(uint64_t exec_ns);
uint64_t stall_take_guest_exec_ns(void);
void stall_note_active_work(const char *work);

// Frame boundary evaluation: updates stall buckets & logs only when total_ns > 50 ms.
void stall_on_frame_end(const StallFrameMetrics *m);
void stall_format_summary(char *buf, size_t cap);
void stall_trace_reset(void);
