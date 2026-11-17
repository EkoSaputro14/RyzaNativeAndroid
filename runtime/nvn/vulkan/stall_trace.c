// Low-overhead frame & thread stall instrumentation (Phase 2 + Sprint 2).
#include "stall_trace.h"
#include "rt.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef __ANDROID__
#include <sys/syscall.h>
#endif

// TLS thread identity & current work stage (zero-cost reads on hot paths).
static __thread char tls_name[32];
static __thread int tls_tid;
static __thread const char *tls_active_work = "idle";

static int get_os_tid(void) {
    if (tls_tid) return tls_tid;
#ifdef __ANDROID__
    tls_tid = (int)gettid();
#elif defined(__linux__)
    tls_tid = (int)syscall(SYS_gettid);
#else
    tls_tid = 1;
#endif
    return tls_tid;
}

void stall_set_thread_name(const char *role, uint64_t guest_tid) {
    int tid = get_os_tid();
    if (guest_tid)
        snprintf(tls_name, sizeof tls_name, "%s:t%llu", role ? role : "guest", (unsigned long long)guest_tid);
    else
        snprintf(tls_name, sizeof tls_name, "%s", role ? role : "thread");
    (void)tid;
}

const char *stall_thread_name(void) {
    if (!tls_name[0]) snprintf(tls_name, sizeof tls_name, "tid-%d", get_os_tid());
    return tls_name;
}

int stall_thread_tid(void) {
    return get_os_tid();
}

void stall_note_active_work(const char *work) {
    tls_active_work = work ? work : "idle";
}

// ================================================================== Ring buffer of slow wait events (>= 2 ms)
#define STALL_RING_CAP 64
#define STALL_MIN_EVENT_NS 2000000ull  // 2 ms minimum to enter ring

typedef struct {
    uint64_t seq;
    uint64_t frame_id;
    uint64_t start_ns;
    uint64_t end_ns;
    uint64_t duration_ns;
    int tid;
    int waited_on_tid;
    char thread_name[24];
    char subsystem[16];
    char reason[24];
    char waited_on_name[24];
    char active_work[24];
} StallEvent;

static StallEvent g_ring[STALL_RING_CAP];
static _Atomic uint64_t g_ring_seq;
static pthread_mutex_t g_ring_lock = PTHREAD_MUTEX_INITIALIZER;

// Per-frame atomic accumulators.
static _Atomic uint64_t g_frame_tex_uploads;
static _Atomic uint64_t g_frame_tex_bytes;
static _Atomic uint64_t g_frame_tex_deswizzle_ns;
static _Atomic uint64_t g_frame_acquire_ns;
static _Atomic uint64_t g_frame_lock_wait_ns;
static _Atomic uint64_t g_frame_bg_lock_wait_ns;
static _Atomic uint64_t g_frame_guest_exec_ns;

// Sprint 2: Per-frame texture burst breakdown & top-3 slowest textures in the current frame.
#define TOP_TEX_CAP 3
static pthread_mutex_t g_tex_prof_lock = PTHREAD_MUTEX_INITIALIZER;
static StallTexUploadDetail g_frame_top_tex[TOP_TEX_CAP];
static uint32_t g_frame_top_tex_n;
static uint64_t g_frame_tex_first_cnt;
static uint64_t g_frame_tex_reupload_cnt;
static uint64_t g_frame_tex_bcn_cnt;
static uint64_t g_frame_tex_astc_cnt;
static uint64_t g_frame_tex_hash_ns;
static uint64_t g_frame_tex_staging_ns;
static uint64_t g_frame_tex_commit_ns;
static uint64_t g_frame_tex_max_single_deswizzle_ns;
static uint64_t g_frame_tex_max_single_bytes;

// Sprint 2: Cumulative evaluation counters across the session.
static _Atomic uint64_t g_tex_over1mb_over50ms_count;
static _Atomic uint64_t g_tex_max_deswizzle_ns;
static _Atomic uint64_t g_nvnsync_calls;
static _Atomic uint64_t g_nvnsync_async_ns;
static _Atomic uint64_t g_nvnsync_gpu_ns;
static _Atomic uint64_t g_nvnsync_max_ns;
static _Atomic uint64_t g_t80_prio_inv_count;
static _Atomic uint64_t g_t80_prio_inv_ns;

// Cumulative stall bucket counters (since boot/reset).
static _Atomic uint64_t g_warn_50;
static _Atomic uint64_t g_severe_100;
static _Atomic uint64_t g_crit_250;
static _Atomic uint64_t g_ext_500;
static _Atomic uint64_t g_max_frame_ns;

void stall_record_lock_wait(const char *subsystem, const char *lock_name, uint64_t wait_ns) {
    atomic_fetch_add_explicit(&g_frame_lock_wait_ns, wait_ns, memory_order_relaxed);
    if (wait_ns >= STALL_MIN_EVENT_NS) {
        uint64_t end_ns = plat_now_ns();
        uint64_t start_ns = end_ns > wait_ns ? end_ns - wait_ns : 0;
        stall_record_wait_event(0, start_ns, end_ns, subsystem ? subsystem : "lock",
                                lock_name ? lock_name : "mutex", 0, "lock_holder", tls_active_work);
    }
}

void stall_record_bg_lock_wait(const char *subsystem, const char *lock_name, uint64_t wait_ns) {
    atomic_fetch_add_explicit(&g_frame_bg_lock_wait_ns, wait_ns, memory_order_relaxed);
    (void)subsystem;
    (void)lock_name;
}

void stall_record_wait_event(uint64_t frame_id, uint64_t start_ns, uint64_t end_ns,
                             const char *subsystem, const char *reason,
                             int waited_on_tid, const char *waited_on_name,
                             const char *active_work) {
    uint64_t dur = end_ns >= start_ns ? end_ns - start_ns : 0;
    if (dur < STALL_MIN_EVENT_NS) return;
    uint64_t seq = atomic_fetch_add_explicit(&g_ring_seq, 1, memory_order_relaxed) + 1;
    pthread_mutex_lock(&g_ring_lock);
    StallEvent *e = &g_ring[(seq - 1) % STALL_RING_CAP];
    e->seq = seq;
    e->frame_id = frame_id;
    e->start_ns = start_ns;
    e->end_ns = end_ns;
    e->duration_ns = dur;
    e->tid = get_os_tid();
    e->waited_on_tid = waited_on_tid;
    snprintf(e->thread_name, sizeof e->thread_name, "%s", stall_thread_name());
    snprintf(e->subsystem, sizeof e->subsystem, "%s", subsystem ? subsystem : "unknown");
    snprintf(e->reason, sizeof e->reason, "%s", reason ? reason : "wait");
    snprintf(e->waited_on_name, sizeof e->waited_on_name, "%s", waited_on_name ? waited_on_name : "-");
    snprintf(e->active_work, sizeof e->active_work, "%s", active_work ? active_work : tls_active_work);
    pthread_mutex_unlock(&g_ring_lock);
}

void stall_record_nvn_sync_call(const char *op, uint64_t frame_id,
                                uint64_t start_ns, uint64_t async_done_ns, uint64_t end_ns,
                                uint64_t caller_lr, int64_t timeout_ns,
                                uint64_t fence_lseq, uint64_t target_seq,
                                uint64_t submitted_seq, uint64_t completed_seq) {
    uint64_t total_ns = end_ns > start_ns ? end_ns - start_ns : 0;
    uint64_t async_ns = async_done_ns > start_ns ? async_done_ns - start_ns : 0;
    uint64_t gpu_ns = end_ns > async_done_ns ? end_ns - async_done_ns : 0;
    atomic_fetch_add_explicit(&g_nvnsync_calls, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_nvnsync_async_ns, async_ns, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_nvnsync_gpu_ns, gpu_ns, memory_order_relaxed);
    uint64_t prev_max = atomic_load_explicit(&g_nvnsync_max_ns, memory_order_relaxed);
    while (total_ns > prev_max &&
           !atomic_compare_exchange_weak_explicit(&g_nvnsync_max_ns, &prev_max, total_ns,
                                                  memory_order_relaxed, memory_order_relaxed)) {}
    static int log_all = -1;
    if (log_all < 0) log_all = getenv("LARECOMP_LOG_NVNSYNC") != NULL;
    if (total_ns >= 5000000ull || log_all) {
        LOG("[nvn-sync] op=%s frame=%llu total_ms=%.2f (async_ms=%.2f gpu_ms=%.2f) "
            "caller_lr=0x%llx timeout_ns=%lld lseq=%llu target_seq=%llu sub_seq=%llu done_seq=%llu "
            "tid=%d thread=%s\n",
            op ? op : "nvnSync", (unsigned long long)frame_id,
            total_ns / 1e6, async_ns / 1e6, gpu_ns / 1e6,
            (unsigned long long)caller_lr, (long long)timeout_ns,
            (unsigned long long)fence_lseq, (unsigned long long)target_seq,
            (unsigned long long)submitted_seq, (unsigned long long)completed_seq,
            get_os_tid(), stall_thread_name());
    }
}

void stall_record_kernel_wait_detail(const StallKernelWaitDetail *d) {
    if (!d) return;
    bool prio_inv = (d->waiter_id == 0x50 && d->owner_id != 0 &&
                     d->owner_prio >= 0 && d->waiter_prio >= 0 &&
                     d->owner_prio > d->waiter_prio);
    if (prio_inv) {
        atomic_fetch_add_explicit(&g_t80_prio_inv_count, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_t80_prio_inv_ns, d->wait_ns, memory_order_relaxed);
    }
    static int log_all = -1;
    if (log_all < 0) log_all = getenv("LARECOMP_LOG_WAIT_DIAG") != NULL;
    // Log detailed wait diagnostics when guest-main (t80) waits >= 5 ms, any priority inversion occurs,
    // any mutex wait >= 10 ms, or any worker condvar/arbiter wait >= 250 ms (or LARECOMP_LOG_WAIT_DIAG=1).
    bool should_log = log_all || prio_inv ||
                      (d->waiter_id == 0x50 && d->wait_ns >= 5000000ull) ||
                      (d->wait_ns >= 10000000ull && d->wait_kind_str && strcmp(d->wait_kind_str, "WAIT_MUTEX") == 0) ||
                      (d->wait_ns >= 250000000ull && d->timeout_ns >= 0);
    if (should_log) {
        LOG("[wait-diag] frame=%llu kind=%s wait_ms=%.2f timeout_ms=%.1f res=0x%x "
            "waiter=t%llu(tid=%d,prio=%d,core=%d) addr=0x%llx mtx=0x%llx tag=0x%x "
            "owner=t%llu(tid=%d,prio=%d,core=%d,lr=0x%llx) "
            "waker=t%llu(tid=%d,prio=%d,core=%d,lr=0x%llx) "
            "prio_inv=%d bt=[0x%llx,0x%llx,0x%llx,0x%llx]\n",
            (unsigned long long)d->frame_id,
            d->wait_kind_str ? d->wait_kind_str : "WAIT",
            d->wait_ns / 1e6,
            d->timeout_ns < 0 ? -1.0 : d->timeout_ns / 1e6,
            d->wait_result,
            (unsigned long long)d->waiter_id, d->waiter_host_tid, d->waiter_prio, d->waiter_core,
            (unsigned long long)d->wait_addr, (unsigned long long)d->wait_mutex, d->wait_tag,
            (unsigned long long)d->owner_id, d->owner_host_tid, d->owner_prio, d->owner_core,
            (unsigned long long)d->owner_lr,
            (unsigned long long)d->waker_id, d->waker_host_tid, d->waker_prio, d->waker_core,
            (unsigned long long)d->waker_lr,
            prio_inv ? 1 : 0,
            (unsigned long long)d->waiter_bt[0], (unsigned long long)d->waiter_bt[1],
            (unsigned long long)d->waiter_bt[2], (unsigned long long)d->waiter_bt[3]);
    }
}

void stall_note_tex_upload(uint64_t bytes, uint64_t deswizzle_ns) {
    atomic_fetch_add_explicit(&g_frame_tex_uploads, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_frame_tex_bytes, bytes, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_frame_tex_deswizzle_ns, deswizzle_ns, memory_order_relaxed);
}

static bool is_bcn_nvn_format(uint32_t f) {
    return (f >= 0x42 && f <= 0x49) || (f >= 0x67 && f <= 0x6c);
}

void stall_note_tex_upload_detail(const StallTexUploadDetail *d) {
    if (!d) return;
    stall_note_tex_upload(d->staged_bytes ? d->staged_bytes : d->total_bytes, d->deswizzle_ns);
    if (!d->is_rt && d->total_bytes > (1ull << 20) && d->deswizzle_ns >= 50000000ull) {
        atomic_fetch_add_explicit(&g_tex_over1mb_over50ms_count, 1, memory_order_relaxed);
    }
    uint64_t prev_max = atomic_load_explicit(&g_tex_max_deswizzle_ns, memory_order_relaxed);
    while (d->deswizzle_ns > prev_max &&
           !atomic_compare_exchange_weak_explicit(&g_tex_max_deswizzle_ns, &prev_max, d->deswizzle_ns,
                                                  memory_order_relaxed, memory_order_relaxed)) {}

    pthread_mutex_lock(&g_tex_prof_lock);
    if (d->was_uploaded) g_frame_tex_reupload_cnt++;
    else g_frame_tex_first_cnt++;
    if (d->bc_vf || is_bcn_nvn_format(d->nvn_format)) g_frame_tex_bcn_cnt++;
    if (d->astc_bw) g_frame_tex_astc_cnt++;
    g_frame_tex_hash_ns += d->hash_ns;
    g_frame_tex_staging_ns += d->staging_ns;
    g_frame_tex_commit_ns += d->commit_ns;
    if (d->deswizzle_ns > g_frame_tex_max_single_deswizzle_ns) {
        g_frame_tex_max_single_deswizzle_ns = d->deswizzle_ns;
        g_frame_tex_max_single_bytes = d->total_bytes;
    }
    // Maintain top-3 slowest texture uploads in the current frame by total_ns.
    if (g_frame_top_tex_n < TOP_TEX_CAP) {
        g_frame_top_tex[g_frame_top_tex_n++] = *d;
    } else {
        uint32_t min_idx = 0;
        for (uint32_t i = 1; i < TOP_TEX_CAP; i++) {
            if (g_frame_top_tex[i].total_ns < g_frame_top_tex[min_idx].total_ns) min_idx = i;
        }
        if (d->total_ns > g_frame_top_tex[min_idx].total_ns) {
            g_frame_top_tex[min_idx] = *d;
        }
    }
    pthread_mutex_unlock(&g_tex_prof_lock);

    static int log_tex = -1;
    if (log_tex < 0) log_tex = getenv("LARECOMP_LOG_TEX_PROF") != NULL;
    // Immediately log any single texture upload taking >= 10 ms (or >1 MB taking >= 5 ms).
    if (log_tex || d->total_ns >= 10000000ull || (d->total_bytes >= (1ull << 20) && d->deswizzle_ns >= 5000000ull)) {
        LOG("[tex-prof] addr=0x%llx shape=%ux%ux%u(lv=%u,ly=%u) fmt=0x%x(bcn=%d,bc_cpu=%u,astc=%ux%u,lin=%d) "
            "bytes=%llu(staged=%llu) state=%s(dyn=%d,rt=%d) "
            "total_ms=%.2f (hash=%.2f deswizzle=%.2f staging=%.2f commit=%.2f)\n",
            (unsigned long long)d->addr, d->width, d->height, d->depth, d->levels, d->layers,
            d->nvn_format, is_bcn_nvn_format(d->nvn_format) ? 1 : 0, d->bc_vf, d->astc_bw, d->astc_bh,
            d->is_linear ? 1 : 0,
            (unsigned long long)d->total_bytes, (unsigned long long)d->staged_bytes,
            d->was_uploaded ? "reupload" : "first_upload",
            d->is_dynamic ? 1 : 0, d->is_rt ? 1 : 0,
            d->total_ns / 1e6, d->hash_ns / 1e6, d->deswizzle_ns / 1e6, d->staging_ns / 1e6, d->commit_ns / 1e6);
    }
}

void stall_take_tex_stats(uint64_t *uploads, uint64_t *bytes, uint64_t *deswizzle_ns) {
    uint64_t u = atomic_exchange_explicit(&g_frame_tex_uploads, 0, memory_order_relaxed);
    uint64_t b = atomic_exchange_explicit(&g_frame_tex_bytes, 0, memory_order_relaxed);
    uint64_t d = atomic_exchange_explicit(&g_frame_tex_deswizzle_ns, 0, memory_order_relaxed);
    if (uploads) *uploads = u;
    if (bytes) *bytes = b;
    if (deswizzle_ns) *deswizzle_ns = d;
}

void stall_note_acquire(uint64_t acquire_ns) {
    if (acquire_ns) atomic_fetch_add_explicit(&g_frame_acquire_ns, acquire_ns, memory_order_relaxed);
}

uint64_t stall_take_acquire_ns(void) {
    return atomic_exchange_explicit(&g_frame_acquire_ns, 0, memory_order_relaxed);
}

uint64_t stall_take_lock_wait_ns(void) {
    return atomic_exchange_explicit(&g_frame_lock_wait_ns, 0, memory_order_relaxed);
}

uint64_t stall_take_bg_lock_wait_ns(void) {
    return atomic_exchange_explicit(&g_frame_bg_lock_wait_ns, 0, memory_order_relaxed);
}

void stall_note_guest_window(uint64_t exec_ns) {
    if (exec_ns) atomic_fetch_add_explicit(&g_frame_guest_exec_ns, exec_ns, memory_order_relaxed);
}

uint64_t stall_take_guest_exec_ns(void) {
    return atomic_exchange_explicit(&g_frame_guest_exec_ns, 0, memory_order_relaxed);
}

static const char *severity_label(uint64_t total_ns) {
    if (total_ns > STALL_THRESH_EXTREME_NS) return "EXTREME";
    if (total_ns > STALL_THRESH_CRIT_NS) return "CRITICAL";
    if (total_ns > STALL_THRESH_SEVERE_NS) return "SEVERE";
    if (total_ns > STALL_THRESH_WARN_NS) return "WARNING";
    return NULL;
}

void stall_on_frame_end(const StallFrameMetrics *m) {
    // Snapshot and reset per-frame texture burst breakdown even on non-stall frames.
    StallTexUploadDetail top_tex[TOP_TEX_CAP];
    uint32_t top_tex_n = 0;
    uint64_t tex_first = 0, tex_reup = 0, tex_bcn = 0, tex_astc = 0;
    uint64_t tex_hash_ns = 0, tex_staging_ns = 0, tex_commit_ns = 0;
    uint64_t tex_max_single_deswizzle_ns = 0, tex_max_single_bytes = 0;
    pthread_mutex_lock(&g_tex_prof_lock);
    top_tex_n = g_frame_top_tex_n;
    for (uint32_t i = 0; i < top_tex_n; i++) top_tex[i] = g_frame_top_tex[i];
    tex_first = g_frame_tex_first_cnt;
    tex_reup = g_frame_tex_reupload_cnt;
    tex_bcn = g_frame_tex_bcn_cnt;
    tex_astc = g_frame_tex_astc_cnt;
    tex_hash_ns = g_frame_tex_hash_ns;
    tex_staging_ns = g_frame_tex_staging_ns;
    tex_commit_ns = g_frame_tex_commit_ns;
    tex_max_single_deswizzle_ns = g_frame_tex_max_single_deswizzle_ns;
    tex_max_single_bytes = g_frame_tex_max_single_bytes;
    g_frame_top_tex_n = 0;
    g_frame_tex_first_cnt = 0;
    g_frame_tex_reupload_cnt = 0;
    g_frame_tex_bcn_cnt = 0;
    g_frame_tex_astc_cnt = 0;
    g_frame_tex_hash_ns = 0;
    g_frame_tex_staging_ns = 0;
    g_frame_tex_commit_ns = 0;
    g_frame_tex_max_single_deswizzle_ns = 0;
    g_frame_tex_max_single_bytes = 0;
    pthread_mutex_unlock(&g_tex_prof_lock);

    if (!m) return;
    uint64_t prev_max = atomic_load_explicit(&g_max_frame_ns, memory_order_relaxed);
    while (m->total_ns > prev_max &&
           !atomic_compare_exchange_weak_explicit(&g_max_frame_ns, &prev_max, m->total_ns,
                                                  memory_order_relaxed, memory_order_relaxed)) {}

    const char *sev = severity_label(m->total_ns);
    if (!sev) return;

    if (m->total_ns > STALL_THRESH_EXTREME_NS) atomic_fetch_add_explicit(&g_ext_500, 1, memory_order_relaxed);
    else if (m->total_ns > STALL_THRESH_CRIT_NS) atomic_fetch_add_explicit(&g_crit_250, 1, memory_order_relaxed);
    else if (m->total_ns > STALL_THRESH_SEVERE_NS) atomic_fetch_add_explicit(&g_severe_100, 1, memory_order_relaxed);
    else atomic_fetch_add_explicit(&g_warn_50, 1, memory_order_relaxed);

    // Find the dominant overlapping wait event in the ring (all timestamps use plat_now_ns()).
    StallEvent snapshot[STALL_RING_CAP];
    int n_overlap = 0;
    int dom = -1;
    pthread_mutex_lock(&g_ring_lock);
    for (int i = 0; i < STALL_RING_CAP; i++) {
        const StallEvent *e = &g_ring[i];
        if (!e->seq) continue;
        bool time_overlap = (e->end_ns + STALL_MIN_EVENT_NS >= m->frame_start_ns &&
                             e->start_ns <= m->frame_end_ns + STALL_MIN_EVENT_NS);
        bool frame_match = (e->frame_id != 0 && (e->frame_id == m->frame_id || e->frame_id + 1 == m->frame_id));
        if (time_overlap || frame_match) {
            snapshot[n_overlap] = *e;
            if (dom < 0 || snapshot[n_overlap].duration_ns > snapshot[dom].duration_ns) dom = n_overlap;
            n_overlap++;
        }
    }
    pthread_mutex_unlock(&g_ring_lock);

    const char *reason = "cpu_or_submit";
    const char *waited_name = "-";
    int waited_tid = 0;
    int diag_tid = stall_thread_tid();
    const char *diag_tname = stall_thread_name();
    if (dom >= 0) {
        reason = snapshot[dom].reason;
        waited_name = snapshot[dom].waited_on_name;
        waited_tid = snapshot[dom].waited_on_tid;
        diag_tid = snapshot[dom].tid;
        diag_tname = snapshot[dom].thread_name;
    } else if (m->tex_deswizzle_ns > m->total_ns / 3 || m->tex_uploads >= 32) {
        reason = "tex_upload_burst";
        waited_name = "cpu-tex";
    } else if (m->gpu_wait_ns > m->total_ns / 3) {
        reason = m->acquire_ns > m->gpu_wait_ns / 2 ? "vk_acquire" : "vk_wait_seq";
        waited_name = "gpu-or-compositor";
    } else if (m->wait_game_queue_ns > m->total_ns / 2) {
        reason = "wait_game_starved";
        waited_name = "guest-main";
    } else if (m->backend_exec_ns > m->total_ns / 2) {
        reason = "backend_exec_cpu";
        waited_name = "vk-render";
    }

    LOG("[stall] severity=%s frame=%llu total_ms=%.1f start_ns=%llu end_ns=%llu "
        "guest_exec_ms=%.1f wait_game_ms=%.1f (queue=%.1f coalesce=%.1f) "
        "backend_exec_ms=%.1f vk_cpu_prep_ms=%.1f vk_submit_ms=%.1f gpu_ms=%.1f gpu_wait_ms=%.1f (acq=%.1f) present_ms=%.1f "
        "tex_uploads=%llu tex_bytes=%.2fMB tex_deswizzle_ms=%.1f lock_wait_ms=%.2f bg_lock_wait_ms=%.2f "
        "draws=%llu submits=%llu compiles=+%llu(%d pending) "
        "tid=%d thread=%s reason=%s waited_on=%d(%s)\n",
        sev, (unsigned long long)m->frame_id, m->total_ns / 1e6,
        (unsigned long long)m->frame_start_ns, (unsigned long long)m->frame_end_ns,
        m->guest_exec_ns / 1e6, (m->wait_game_queue_ns + m->wait_game_coalesce_ns) / 1e6,
        m->wait_game_queue_ns / 1e6, m->wait_game_coalesce_ns / 1e6,
        m->backend_exec_ns / 1e6, m->vk_cpu_prep_ns / 1e6, m->vk_submit_ns / 1e6,
        m->gpu_ns / 1e6, m->gpu_wait_ns / 1e6, m->acquire_ns / 1e6, m->present_ns / 1e6,
        (unsigned long long)m->tex_uploads, m->tex_upload_bytes / 1048576.0, m->tex_deswizzle_ns / 1e6,
        m->lock_wait_ns / 1e6, m->bg_lock_wait_ns / 1e6,
        (unsigned long long)m->draws, (unsigned long long)m->submits,
        (unsigned long long)m->compiles_delta, m->compiles_pending,
        diag_tid, diag_tname, reason, waited_tid, waited_name);

    // Log per-frame texture burst breakdown and top-3 slowest textures whenever a stall has >= 8 texture uploads or >= 10 ms of texture work.
    if (m->tex_uploads >= 8 || m->tex_deswizzle_ns + tex_hash_ns + tex_staging_ns + tex_commit_ns >= 10000000ull) {
        LOG("[tex-burst] frame=%llu uploads=%llu(first=%llu,reup=%llu,bcn=%llu,astc=%llu) bytes=%.2fMB "
            "hash_ms=%.2f deswizzle_ms=%.2f staging_ms=%.2f commit_ms=%.2f submit_ms=%.2f "
            "max_single_deswizzle_ms=%.2f(%.2fMB) over1mb_50ms_total=%llu\n",
            (unsigned long long)m->frame_id,
            (unsigned long long)m->tex_uploads, (unsigned long long)tex_first, (unsigned long long)tex_reup,
            (unsigned long long)tex_bcn, (unsigned long long)tex_astc,
            m->tex_upload_bytes / 1048576.0,
            tex_hash_ns / 1e6, m->tex_deswizzle_ns / 1e6, tex_staging_ns / 1e6, tex_commit_ns / 1e6,
            m->vk_submit_ns / 1e6,
            tex_max_single_deswizzle_ns / 1e6, tex_max_single_bytes / 1048576.0,
            (unsigned long long)atomic_load_explicit(&g_tex_over1mb_over50ms_count, memory_order_relaxed));
        for (uint32_t i = 0; i < top_tex_n; i++) {
            const StallTexUploadDetail *d = &top_tex[i];
            LOG("[tex-prof] frame=%llu rank=%u addr=0x%llx shape=%ux%ux%u(lv=%u,ly=%u) fmt=0x%x(bcn=%d,bc_cpu=%u,astc=%ux%u,lin=%d) "
                "bytes=%llu(staged=%llu) state=%s(dyn=%d,rt=%d) "
                "total_ms=%.2f (hash=%.2f deswizzle=%.2f staging=%.2f commit=%.2f)\n",
                (unsigned long long)m->frame_id, i + 1,
                (unsigned long long)d->addr, d->width, d->height, d->depth, d->levels, d->layers,
                d->nvn_format, is_bcn_nvn_format(d->nvn_format) ? 1 : 0, d->bc_vf, d->astc_bw, d->astc_bh,
                d->is_linear ? 1 : 0,
                (unsigned long long)d->total_bytes, (unsigned long long)d->staged_bytes,
                d->was_uploaded ? "reupload" : "first_upload",
                d->is_dynamic ? 1 : 0, d->is_rt ? 1 : 0,
                d->total_ns / 1e6, d->hash_ns / 1e6, d->deswizzle_ns / 1e6, d->staging_ns / 1e6, d->commit_ns / 1e6);
        }
    }

    // For Critical (>250 ms) and Extreme (>500 ms) stalls, log causal linkage records.
    if (m->total_ns > STALL_THRESH_CRIT_NS) {
        if (n_overlap == 0) {
            LOG("[stall-diag] frame=%llu sev=%s link: thread=%s(%d) -> subsystem=%s -> work=backend_exec:%.1fms(submit:%.1fms),tex_up:%llu(%.2fMB),gpu_wait:%.1fms\n",
                (unsigned long long)m->frame_id, sev, stall_thread_name(), stall_thread_tid(),
                m->tex_deswizzle_ns > m->backend_exec_ns / 3 ? "texture" : (m->gpu_wait_ns > m->backend_exec_ns ? "vulkan" : "exec"),
                m->backend_exec_ns / 1e6, m->vk_submit_ns / 1e6,
                (unsigned long long)m->tex_uploads, m->tex_upload_bytes / 1048576.0, m->gpu_wait_ns / 1e6);
        } else {
            int logged = 0;
            for (int i = 0; i < n_overlap && logged < 6; i++) {
                const StallEvent *e = &snapshot[i];
                LOG("[stall-diag] frame=%llu sev=%s link: thread=%s(%d) -> subsystem=%s reason=%s dur=%.1fms [%llu..%llu] waited_on=%s(%d) active_work=%s\n",
                    (unsigned long long)m->frame_id, sev, e->thread_name, e->tid,
                    e->subsystem, e->reason, e->duration_ns / 1e6,
                    (unsigned long long)e->start_ns, (unsigned long long)e->end_ns,
                    e->waited_on_name, e->waited_on_tid, e->active_work);
                logged++;
            }
        }
    }
}

void stall_format_summary(char *buf, size_t cap) {
    if (!buf || !cap) return;
    uint64_t w = atomic_load_explicit(&g_warn_50, memory_order_relaxed);
    uint64_t s = atomic_load_explicit(&g_severe_100, memory_order_relaxed);
    uint64_t c = atomic_load_explicit(&g_crit_250, memory_order_relaxed);
    uint64_t e = atomic_load_explicit(&g_ext_500, memory_order_relaxed);
    uint64_t mx = atomic_load_explicit(&g_max_frame_ns, memory_order_relaxed);
    uint64_t sync_n = atomic_exchange_explicit(&g_nvnsync_calls, 0, memory_order_relaxed);
    uint64_t sync_a = atomic_exchange_explicit(&g_nvnsync_async_ns, 0, memory_order_relaxed);
    uint64_t sync_g = atomic_exchange_explicit(&g_nvnsync_gpu_ns, 0, memory_order_relaxed);
    uint64_t sync_mx = atomic_exchange_explicit(&g_nvnsync_max_ns, 0, memory_order_relaxed);
    uint64_t pinv_n = atomic_load_explicit(&g_t80_prio_inv_count, memory_order_relaxed);
    uint64_t pinv_ns = atomic_load_explicit(&g_t80_prio_inv_ns, memory_order_relaxed);
    uint64_t tex50 = atomic_load_explicit(&g_tex_over1mb_over50ms_count, memory_order_relaxed);
    uint64_t tex_mx = atomic_load_explicit(&g_tex_max_deswizzle_ns, memory_order_relaxed);
    snprintf(buf, cap,
             "stalls(>50/100/250/500ms)=%llu/%llu/%llu/%llu max=%.1fms | "
             "sync=%llux(async=%.1fms,gpu=%.1fms,max=%.1fms) | "
             "t80_pinv=%llux(%.1fms) | tex>1MB>50ms=%llu(max_desw=%.1fms)",
             (unsigned long long)w, (unsigned long long)s, (unsigned long long)c, (unsigned long long)e,
             mx / 1e6,
             (unsigned long long)sync_n, sync_a / 1e6, sync_g / 1e6, sync_mx / 1e6,
             (unsigned long long)pinv_n, pinv_ns / 1e6,
             (unsigned long long)tex50, tex_mx / 1e6);
}

void stall_trace_reset(void) {
    pthread_mutex_lock(&g_ring_lock);
    memset(g_ring, 0, sizeof g_ring);
    pthread_mutex_unlock(&g_ring_lock);
    pthread_mutex_lock(&g_tex_prof_lock);
    g_frame_top_tex_n = 0;
    g_frame_tex_first_cnt = 0;
    g_frame_tex_reupload_cnt = 0;
    g_frame_tex_bcn_cnt = 0;
    g_frame_tex_astc_cnt = 0;
    g_frame_tex_hash_ns = 0;
    g_frame_tex_staging_ns = 0;
    g_frame_tex_commit_ns = 0;
    g_frame_tex_max_single_deswizzle_ns = 0;
    g_frame_tex_max_single_bytes = 0;
    pthread_mutex_unlock(&g_tex_prof_lock);
    atomic_store_explicit(&g_frame_tex_uploads, 0, memory_order_relaxed);
    atomic_store_explicit(&g_frame_tex_bytes, 0, memory_order_relaxed);
    atomic_store_explicit(&g_frame_tex_deswizzle_ns, 0, memory_order_relaxed);
    atomic_store_explicit(&g_frame_acquire_ns, 0, memory_order_relaxed);
    atomic_store_explicit(&g_frame_lock_wait_ns, 0, memory_order_relaxed);
    atomic_store_explicit(&g_frame_bg_lock_wait_ns, 0, memory_order_relaxed);
    atomic_store_explicit(&g_frame_guest_exec_ns, 0, memory_order_relaxed);
    atomic_store_explicit(&g_tex_over1mb_over50ms_count, 0, memory_order_relaxed);
    atomic_store_explicit(&g_tex_max_deswizzle_ns, 0, memory_order_relaxed);
    atomic_store_explicit(&g_nvnsync_calls, 0, memory_order_relaxed);
    atomic_store_explicit(&g_nvnsync_async_ns, 0, memory_order_relaxed);
    atomic_store_explicit(&g_nvnsync_gpu_ns, 0, memory_order_relaxed);
    atomic_store_explicit(&g_nvnsync_max_ns, 0, memory_order_relaxed);
    atomic_store_explicit(&g_t80_prio_inv_count, 0, memory_order_relaxed);
    atomic_store_explicit(&g_t80_prio_inv_ns, 0, memory_order_relaxed);
    atomic_store_explicit(&g_warn_50, 0, memory_order_relaxed);
    atomic_store_explicit(&g_severe_100, 0, memory_order_relaxed);
    atomic_store_explicit(&g_crit_250, 0, memory_order_relaxed);
    atomic_store_explicit(&g_ext_500, 0, memory_order_relaxed);
    atomic_store_explicit(&g_max_frame_ns, 0, memory_order_relaxed);
}
