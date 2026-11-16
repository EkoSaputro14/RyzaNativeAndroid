#include "stall_trace.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#if defined(__ANDROID__) || defined(__linux__)
#include <sys/syscall.h>
#endif
#include "../../rt.h"

#define STALL_RING_CAP 32
#define STALL_MIN_EVENT_NS 5000000ull  // 5 ms threshold to enter diagnostic event ring

typedef struct {
    uint64_t seq;
    uint64_t frame_id;
    uint64_t start_ns;
    uint64_t end_ns;
    uint64_t duration_ns;
    int tid;
    uint64_t guest_tid;
    int waited_on_tid;
    char thread_name[24];
    char subsystem[16];
    char reason[40];
    char waited_on_name[24];
    char active_work[64];
} StallEvent;

static StallEvent g_ring[STALL_RING_CAP];
static _Atomic uint64_t g_ring_seq;
static pthread_mutex_t g_ring_lock = PTHREAD_MUTEX_INITIALIZER;

static _Atomic uint64_t g_frame_tex_uploads;
static _Atomic uint64_t g_frame_tex_bytes;
static _Atomic uint64_t g_frame_tex_deswizzle_ns;
static _Atomic uint64_t g_frame_acquire_ns;
static _Atomic uint64_t g_frame_lock_wait_ns;
static _Atomic uint64_t g_frame_guest_exec_ns;

// Cumulative stall counters across the session (for summary / Stage-1 validation)
static _Atomic uint64_t g_warn_50;
static _Atomic uint64_t g_severe_100;
static _Atomic uint64_t g_crit_250;
static _Atomic uint64_t g_ext_500;
static _Atomic uint64_t g_max_frame_ns;

static __thread int tls_tid = -1;
static __thread uint64_t tls_guest_tid = 0;
static __thread char tls_role[24] = "host";
static __thread char tls_active_work[64] = "";

static int query_os_tid(void) {
#if defined(__ANDROID__) || defined(__linux__)
#ifdef SYS_gettid
    return (int)syscall(SYS_gettid);
#else
    return (int)syscall(__NR_gettid);
#endif
#else
    return (int)(uintptr_t)pthread_self();
#endif
}

int stall_thread_tid(void) {
    if (tls_tid <= 0) tls_tid = query_os_tid();
    return tls_tid;
}

const char *stall_thread_name(void) {
    return tls_role;
}

void stall_set_thread_name(const char *role, uint64_t guest_tid) {
    tls_tid = query_os_tid();
    tls_guest_tid = guest_tid;
    if (role && *role) {
        if (guest_tid) snprintf(tls_role, sizeof tls_role, "%s-t%llx", role, (unsigned long long)guest_tid);
        else snprintf(tls_role, sizeof tls_role, "%s", role);
    }
}

void stall_note_active_work(const char *work) {
    if (!work) tls_active_work[0] = '\0';
    else snprintf(tls_active_work, sizeof tls_active_work, "%s", work);
}

void stall_record_lock_wait(const char *subsystem, const char *lock_name, uint64_t wait_ns) {
    if (!wait_ns) return;
    atomic_fetch_add_explicit(&g_frame_lock_wait_ns, wait_ns, memory_order_relaxed);
    if (wait_ns >= STALL_MIN_EVENT_NS) {
        uint64_t end_ns = plat_now_ns();
        uint64_t start_ns = end_ns > wait_ns ? end_ns - wait_ns : 0;
        char reason[40];
        snprintf(reason, sizeof reason, "lock:%s", lock_name ? lock_name : "mutex");
        stall_record_wait_event(0, start_ns, end_ns, subsystem ? subsystem : "lock", reason, 0, "lock-holder", NULL);
    }
}

void stall_record_wait_event(uint64_t frame_id, uint64_t start_ns, uint64_t end_ns,
                             const char *subsystem, const char *reason,
                             int waited_on_tid, const char *waited_on_name,
                             const char *active_work) {
    if (end_ns <= start_ns) return;
    uint64_t dur = end_ns - start_ns;
    if (dur < STALL_MIN_EVENT_NS) return;
    const char *work = (active_work && *active_work) ? active_work : tls_active_work;
    int tid = stall_thread_tid();

    pthread_mutex_lock(&g_ring_lock);
    uint64_t seq = atomic_fetch_add_explicit(&g_ring_seq, 1, memory_order_relaxed);
    StallEvent *e = &g_ring[seq & (STALL_RING_CAP - 1)];
    e->seq = seq + 1;
    e->frame_id = frame_id;
    e->start_ns = start_ns;
    e->end_ns = end_ns;
    e->duration_ns = dur;
    e->tid = tid;
    e->guest_tid = tls_guest_tid;
    e->waited_on_tid = waited_on_tid;
    snprintf(e->thread_name, sizeof e->thread_name, "%s", tls_role);
    snprintf(e->subsystem, sizeof e->subsystem, "%s", subsystem ? subsystem : "unknown");
    snprintf(e->reason, sizeof e->reason, "%s", reason ? reason : "wait");
    snprintf(e->waited_on_name, sizeof e->waited_on_name, "%s", waited_on_name ? waited_on_name : "-");
    snprintf(e->active_work, sizeof e->active_work, "%s", work && *work ? work : "-");
    pthread_mutex_unlock(&g_ring_lock);
}

void stall_note_tex_upload(uint64_t bytes, uint64_t deswizzle_ns) {
    atomic_fetch_add_explicit(&g_frame_tex_uploads, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_frame_tex_bytes, bytes, memory_order_relaxed);
    if (deswizzle_ns) atomic_fetch_add_explicit(&g_frame_tex_deswizzle_ns, deswizzle_ns, memory_order_relaxed);
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

    // Find the dominant overlapping wait event in the ring to populate primary wait reason / tid.
    StallEvent snapshot[STALL_RING_CAP];
    int n_overlap = 0;
    int dom = -1;
    pthread_mutex_lock(&g_ring_lock);
    for (int i = 0; i < STALL_RING_CAP; i++) {
        const StallEvent *e = &g_ring[i];
        if (!e->seq) continue;
        // Overlaps [frame_start_ns - 5ms, frame_end_ns + 5ms]
        if (e->end_ns + STALL_MIN_EVENT_NS >= m->frame_start_ns &&
            e->start_ns <= m->frame_end_ns + STALL_MIN_EVENT_NS) {
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
        "backend_exec_ms=%.1f vk_cpu_prep_ms=%.1f gpu_ms=%.1f gpu_wait_ms=%.1f (acq=%.1f) present_ms=%.1f "
        "tex_uploads=%llu tex_bytes=%.2fMB tex_deswizzle_ms=%.1f lock_wait_ms=%.2f "
        "draws=%llu submits=%llu compiles=+%llu(%d pending) "
        "tid=%d thread=%s reason=%s waited_on=%d(%s)\n",
        sev, (unsigned long long)m->frame_id, m->total_ns / 1e6,
        (unsigned long long)m->frame_start_ns, (unsigned long long)m->frame_end_ns,
        m->guest_exec_ns / 1e6, (m->wait_game_queue_ns + m->wait_game_coalesce_ns) / 1e6,
        m->wait_game_queue_ns / 1e6, m->wait_game_coalesce_ns / 1e6,
        m->backend_exec_ns / 1e6, m->vk_cpu_prep_ns / 1e6,
        m->gpu_ns / 1e6, m->gpu_wait_ns / 1e6, m->acquire_ns / 1e6, m->present_ns / 1e6,
        (unsigned long long)m->tex_uploads, m->tex_upload_bytes / 1048576.0, m->tex_deswizzle_ns / 1e6,
        m->lock_wait_ns / 1e6,
        (unsigned long long)m->draws, (unsigned long long)m->submits,
        (unsigned long long)m->compiles_delta, m->compiles_pending,
        diag_tid, diag_tname, reason, waited_tid, waited_name);

    // For Critical (>250 ms) and Extreme (>500 ms) stalls, log causal linkage records.
    if (m->total_ns > STALL_THRESH_CRIT_NS) {
        if (n_overlap == 0) {
            LOG("[stall-diag] frame=%llu sev=%s link: thread=%s(%d) -> subsystem=%s -> work=backend_exec:%.1fms,tex_up:%llu(%.2fMB),gpu_wait:%.1fms\n",
                (unsigned long long)m->frame_id, sev, stall_thread_name(), stall_thread_tid(),
                m->tex_deswizzle_ns > m->backend_exec_ns / 3 ? "texture" : (m->gpu_wait_ns > m->backend_exec_ns ? "vulkan" : "exec"),
                m->backend_exec_ns / 1e6, (unsigned long long)m->tex_uploads, m->tex_upload_bytes / 1048576.0, m->gpu_wait_ns / 1e6);
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
    snprintf(buf, cap, "stalls(>50/100/250/500ms)=%llu/%llu/%llu/%llu max=%.1fms",
             (unsigned long long)w, (unsigned long long)s, (unsigned long long)c, (unsigned long long)e,
             mx / 1e6);
}

void stall_trace_reset(void) {
    pthread_mutex_lock(&g_ring_lock);
    memset(g_ring, 0, sizeof g_ring);
    pthread_mutex_unlock(&g_ring_lock);
    atomic_store_explicit(&g_frame_tex_uploads, 0, memory_order_relaxed);
    atomic_store_explicit(&g_frame_tex_bytes, 0, memory_order_relaxed);
    atomic_store_explicit(&g_frame_tex_deswizzle_ns, 0, memory_order_relaxed);
    atomic_store_explicit(&g_frame_acquire_ns, 0, memory_order_relaxed);
    atomic_store_explicit(&g_frame_lock_wait_ns, 0, memory_order_relaxed);
    atomic_store_explicit(&g_frame_guest_exec_ns, 0, memory_order_relaxed);
    atomic_store_explicit(&g_warn_50, 0, memory_order_relaxed);
    atomic_store_explicit(&g_severe_100, 0, memory_order_relaxed);
    atomic_store_explicit(&g_crit_250, 0, memory_order_relaxed);
    atomic_store_explicit(&g_ext_500, 0, memory_order_relaxed);
    atomic_store_explicit(&g_max_frame_ns, 0, memory_order_relaxed);
}
