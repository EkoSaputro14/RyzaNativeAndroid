// Android: the game's command lists translated on a render thread of their own (LARECOMP_VK_ASYNC=0: off).
//
// The game's thread records its commands into its own command memory (cmdbuf.c) and, at submit, used to translate
// them to Vulkan right there: 3.5-5.5 ms of each frame, a third of a 60 fps frame on top of the game's own work. Here
// the game's thread only queues the request and goes on; the render thread runs the same translation in the same order.
// That is the contract a real GPU has too: the game reuses command memory, and rewrites what the GPU reads, only after
// waiting on a fence - so the fences cover the translation: a fence gets a logical number when it is queued (or when
// its command list is), and a wait first waits for the render thread to reach it, then for the GPU as before.
//
// Requests: EXEC (a command list), PRESENT (the window's half of backend_present: the game's half - pacing, input,
// tests - stays on the game's thread), FENCE (nvnQueueFenceSync), TEX_REG / SMP_REG / TEX_POOL_FIN / TEX_DESTROY
// (FIFO-ordered descriptor and texture retirement updates without synchronous guest-thread queue drains).
#ifdef __ANDROID__
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../nvn.h"
#include "../../platform.h"
#include "../../rt.h"
#include "stall_trace.h"
#include "../../settings.h"

enum {
    REQ_EXEC,
    REQ_PRESENT,
    REQ_FENCE,
    REQ_FLUSH,
    REQ_TEX_REG,
    REQ_SMP_REG,
    REQ_TEX_POOL_FIN,
    REQ_TEX_DESTROY
};
typedef struct { int type; uint64_t a; void *p; uint64_t id; } Req;

typedef struct {
    NvnDescPool *pool;
    int id;
    bool has_view;
    NvnTexture *tex;
    NvnTextureView view;
} AsyncTexReg;

typedef struct {
    NvnDescPool *pool;
    int id;
    NvnSamplerDesc desc;
} AsyncSmpReg;

#define QN 4096  // requests in flight (a frame has a few dozen)
static Req q[QN];
static AsyncTexReg q_tex[QN];
static uint64_t q_head, q_tail;   // q_tail: next id to queue (ids start at 1), q_head: next to run
static uint64_t q_done;           // the last id the render thread finished
static int q_presents;            // PRESENT requests queued and not finished (the game's lead)
static int q_waiters;             // number of guest threads currently waiting in vk_async_wait_id
static int q_push_waiters;        // number of guest threads waiting on ring space / ahead cap in push_inner
static bool q_render_sleeping;    // true only while render_main sleeps in pthread_cond_wait(&q_more, &q_lock)
static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t q_more = PTHREAD_COND_INITIALIZER, q_less = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t p_lock = PTHREAD_MUTEX_INITIALIZER;  // producers: an id, its list's fences and its push together
pthread_t g_vk_render_thread;  // (also read by device.c renderer_present_count, many times a draw)
bool g_vk_render_started;
static int g_vk_render_tid;
static int on = -1;

// The game's thread may run this many frames ahead of the render thread (the translation of one frame overlaps the
// game's work on the next; more only adds latency). LARECOMP_VK_ASYNC_AHEAD=n.
static int ahead = 1;

// The render thread's work for its last frame (requests run between two presents, ns): the frame pacer's other half
static uint64_t frame_work, last_frame_work, frame_cpu, last_frame_cpu;
static uint64_t frame_idle_ns, frame_coalesce_ns;
static uint64_t frame_spin_ns, frame_other_req_ns, frame_pre_present_ns;
static uint64_t frame_exec_req_ns, frame_pres_req_ns, frame_exec_submit_ns, frame_ring_begin_ns, frame_post_present_ns;
static uint64_t last_frame_other_req_ns, last_frame_pre_present_ns;
static uint64_t last_frame_exec_req_ns, last_frame_pres_req_ns, last_frame_exec_submit_ns, last_frame_ring_begin_ns, last_frame_post_present_ns;
static int active_req_type = -1;
static uint64_t active_req_start_ns;
uint64_t vk_async_frame_work_ns(void) { return __atomic_load_n(&last_frame_work, __ATOMIC_RELAXED); }
uint64_t vk_async_frame_cpu_ns(void) { return __atomic_load_n(&last_frame_cpu, __ATOMIC_RELAXED); }
uint64_t vk_async_take_idle_ns(void) {
    return __atomic_exchange_n(&frame_idle_ns, 0, __ATOMIC_RELAXED) +
           __atomic_exchange_n(&frame_spin_ns, 0, __ATOMIC_RELAXED);
}
uint64_t vk_async_take_coalesce_ns(void) { return __atomic_exchange_n(&frame_coalesce_ns, 0, __ATOMIC_RELAXED); }
void vk_async_note_pre_present_ns(uint64_t ns) { __atomic_fetch_add(&frame_pre_present_ns, ns, __ATOMIC_RELAXED); }
void vk_async_note_exec_submit_ns(uint64_t ns) { __atomic_fetch_add(&frame_exec_submit_ns, ns, __ATOMIC_RELAXED); }
void vk_async_note_ring_begin_ns(uint64_t ns) { __atomic_fetch_add(&frame_ring_begin_ns, ns, __ATOMIC_RELAXED); }
void vk_async_note_post_present_ns(uint64_t ns) { __atomic_fetch_add(&frame_post_present_ns, ns, __ATOMIC_RELAXED); }
int vk_async_render_tid(void) { return __atomic_load_n(&g_vk_render_tid, __ATOMIC_RELAXED); }

void kernel_blocking_begin(void);        // kernel.c: core 0 to the game's other threads while a guest thread waits here
void kernel_blocking_end(void);
void vk_execute_now(uint64_t handle);    // exec.c: translate and submit a command list
void vk_execute_list_batch(uint64_t handle, bool force_submit);
void vk_flush_pending_work(void);
uint64_t vk_submitted_seq(void);
bool vk_batch_threshold_reached(void);
bool vk_has_pending_commands(void);
uint64_t vk_submitted_seq(void);
void vk_present_now(NvnTexture *t, uint64_t asked);  // device.c: the window's half of backend_present
void nvn_sync_set_seq(uint64_t guest, uint64_t seq);
uint64_t renderer_fence_seq(void);
uint64_t renderer_present_count(void);
void nvn_async_apply_tex_reg(NvnDescPool *d, int id, NvnTexture *t, const NvnTextureView *view, bool has_view);
void nvn_async_apply_smp_reg(NvnDescPool *d, int id, const NvnSamplerDesc *desc);
void nvn_async_apply_tex_pool_fin(NvnDescPool *d);
void nvn_async_apply_tex_destroy(NvnTexture *t);

// Reusable command-list snapshot pool to avoid per-submit malloc/realloc/free churn.
#define SNAP_POOL_N 32
static struct { uint8_t *buf; size_t cap; } snap_pool[SNAP_POOL_N];
static int snap_pool_count;
static pthread_mutex_t snap_pool_lock = PTHREAD_MUTEX_INITIALIZER;

static uint8_t *snap_pool_acquire(size_t min_cap, size_t *out_cap) {
    pthread_mutex_lock(&snap_pool_lock);
    for (int i = snap_pool_count - 1; i >= 0; i--) {
        if (snap_pool[i].cap >= min_cap) {
            uint8_t *b = snap_pool[i].buf;
            size_t c = snap_pool[i].cap;
            snap_pool[i] = snap_pool[--snap_pool_count];
            pthread_mutex_unlock(&snap_pool_lock);
            *out_cap = c;
            return b;
        }
    }
    if (snap_pool_count > 0) {
        uint8_t *b = snap_pool[--snap_pool_count].buf;
        size_t c = snap_pool[snap_pool_count].cap;
        pthread_mutex_unlock(&snap_pool_lock);
        *out_cap = c;
        return b;
    }
    pthread_mutex_unlock(&snap_pool_lock);
    *out_cap = min_cap > 0x10000 ? min_cap : 0x10000;
    return malloc(*out_cap);
}

static void snap_pool_release(uint8_t *b, size_t cap) {
    if (!b) return;
    if (cap <= (4u << 20)) {
        pthread_mutex_lock(&snap_pool_lock);
        if (snap_pool_count < SNAP_POOL_N) {
            snap_pool[snap_pool_count].buf = b;
            snap_pool[snap_pool_count].cap = cap;
            snap_pool_count++;
            pthread_mutex_unlock(&snap_pool_lock);
            return;
        }
        pthread_mutex_unlock(&snap_pool_lock);
    }
    free(b);
}

bool vk_async_on(void) {
    if (on < 0) {
        const char *e = getenv("LARECOMP_VK_ASYNC");  // (on by default: =0 turns it off)
        on = !e || atoi(e) != 0;
        const char *a = getenv("LARECOMP_VK_ASYNC_AHEAD");
        if (a && atoi(a) > 0) ahead = atoi(a);
        if (on) LOG("[vk] async: command lists translated on a render thread (%d frame%s ahead)\n", ahead, ahead > 1 ? "s" : "");
    }
    return on;
}

bool vk_async_is_render_thread(void) { return g_vk_render_started && pthread_equal(pthread_self(), g_vk_render_thread); }

// Thread CPU time excludes GPU fences, swapchain acquisition and time descheduled by Android.
static uint64_t render_cpu_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int async_spin_us(void) {
    static int spin_us = -1;
    if (spin_us < 0) {
        const char *e = getenv("LARECOMP_ASYNC_SPIN_US");
        if (e && *e) {
            int v = atoi(e);
            spin_us = v < 0 ? 0 : (v > 2000 ? 2000 : v);
        } else {
            int v = g_settings.async_spin_us;
            spin_us = v < 0 ? 0 : (v > 2000 ? 2000 : v);
        }
    }
    return spin_us;
}

static inline void cpu_spin_yield(uint32_t iter) {
#if defined(__aarch64__) || defined(__arm__)
    __builtin_arm_yield();
#endif
    if ((iter & 15u) == 15u) sched_yield();
}

static bool req_is_state_or_exec(int type) {
    return type == REQ_EXEC || type == REQ_TEX_REG || type == REQ_SMP_REG;
}

static void *render_main(void *arg) {
    (void)arg;
#ifdef __ANDROID__
    plat_thread_pin_fast();
#endif
    stall_set_thread_name("vk-render", 0);
    __atomic_store_n(&g_vk_render_tid, stall_thread_tid(), __ATOMIC_RELAXED);
    crash_handler_install();
    // Keep the established ADPF signal by default. CPU-only timing is an explicit experiment:
    // excluding descheduling/waits can understate the work seen by Android's frequency governor.
    const char *timing = getenv("LARECOMP_ADPF_CPU_TIME");
    bool cpu_timing = timing && atoi(timing) != 0;
    const int spin_us = async_spin_us();
    pthread_mutex_lock(&q_lock);
    for (;;) {
        while (q_head == q_tail) {
            if (vk_has_pending_commands() && __atomic_load_n(&q_waiters, __ATOMIC_RELAXED) > 0) {
                pthread_mutex_unlock(&q_lock);
                vk_flush_pending_work();
                pthread_mutex_lock(&q_lock);
                continue;
            }
            if (spin_us > 0) {
                uint64_t head_snap = q_head;
                pthread_mutex_unlock(&q_lock);
                uint64_t spin_t0 = plat_now_ns();
                uint64_t spin_limit = (uint64_t)spin_us * 1000ull;
                uint32_t iter = 0;
                while (__atomic_load_n(&q_tail, __ATOMIC_ACQUIRE) == head_snap) {
                    if (vk_has_pending_commands() && __atomic_load_n(&q_waiters, __ATOMIC_RELAXED) > 0) break;
                    if (plat_now_ns() - spin_t0 >= spin_limit) break;
                    cpu_spin_yield(++iter);
                }
                __atomic_fetch_add(&frame_spin_ns, plat_now_ns() - spin_t0, __ATOMIC_RELAXED);
                pthread_mutex_lock(&q_lock);
                if (q_head != q_tail) break;
                if (vk_has_pending_commands() && __atomic_load_n(&q_waiters, __ATOMIC_RELAXED) > 0) continue;
            }
            uint64_t w0 = plat_now_ns();
            q_render_sleeping = true;
            pthread_cond_wait(&q_more, &q_lock);
            q_render_sleeping = false;
            uint64_t w1 = plat_now_ns();
            __atomic_fetch_add(&frame_idle_ns, w1 - w0, __ATOMIC_RELAXED);
            if (w1 - w0 >= 5000000ull) {
                stall_record_wait_event(renderer_present_count(), w0, w1, "async",
                                        "wait_game_queue_empty", 0, "guest-main", "idle");
            }
        }
        Req r = q[q_head % QN];
        AsyncTexReg tex_batch[64];
        int tex_batch_n = 0;
        uint64_t batch_last_id = r.id;
        bool next_needs_submit = false;
        if (r.type == REQ_EXEC) {
            for (uint64_t scan = q_head + 1; scan < q_tail; scan++) {
                int nt = q[scan % QN].type;
                if (nt == REQ_TEX_REG || nt == REQ_SMP_REG || nt == REQ_FENCE) continue;
                next_needs_submit = (nt == REQ_PRESENT) || (nt == REQ_FLUSH && q[scan % QN].a == 0);
                break;
            }
        } else if (r.type == REQ_TEX_REG) {
            uint64_t avail = q_tail - q_head;
            if (avail > 64) avail = 64;
            for (uint64_t i = 0; i < avail; i++) {
                const Req *qi = &q[(q_head + i) % QN];
                if (qi->type != REQ_TEX_REG) break;
                tex_batch[tex_batch_n++] = q_tex[(q_head + i) % QN];
                batch_last_id = qi->id;
            }
        }
        pthread_mutex_unlock(&q_lock);
        uint64_t t0 = plat_now_ns(), cpu0 = cpu_timing ? render_cpu_ns() : 0;
        __atomic_store_n(&active_req_type, r.type, __ATOMIC_RELEASE);
        __atomic_store_n(&active_req_start_ns, t0, __ATOMIC_RELEASE);
        extern void vk_tl(const char *what, uint64_t a, uint64_t b);  // (exec.c: LARECOMP_FRAME_TIMELINE)
        if (r.type == REQ_EXEC || r.type == REQ_PRESENT || r.type == REQ_FENCE)
            vk_tl(r.type == REQ_EXEC ? "render: list start" : r.type == REQ_PRESENT ? "render: present start" : "render: fence", r.id, 0);
        if (r.type == REQ_EXEC) {
            stall_note_active_work("exec_list");
            uint8_t *copy_buf = (uint8_t *)r.p;
            size_t copy_cap = copy_buf ? *(size_t *)copy_buf : 0;
            uint64_t list_ptr = copy_buf ? (uint64_t)(uintptr_t)(copy_buf + 16) : r.a;
            vk_execute_list_batch(list_ptr, next_needs_submit);
            if (copy_buf) snap_pool_release(copy_buf, copy_cap);
            stall_note_active_work(NULL);
        } else if (r.type == REQ_PRESENT) {
            stall_note_active_work("present");
            vk_present_now((NvnTexture *)r.p, r.a);
            stall_note_active_work(NULL);
        } else if (r.type == REQ_FLUSH) {
            uint64_t sub = vk_submitted_seq();
            bool urgent = (r.a == 0) || (r.a > sub);
            if (vk_has_pending_commands() && (urgent || vk_batch_threshold_reached())) {
                vk_flush_pending_work();
            }
        } else if (r.type == REQ_FENCE) {
            extern bool vk_gmem_placed(void);
            if (vk_gmem_placed() || getenv("LARECOMP_FENCE_SUBMIT")) vk_flush_pending_work();
            uint64_t fseq = renderer_fence_seq();
            if (!fseq) fseq = vk_submitted_seq();
            nvn_sync_set_seq(r.a, fseq);
        } else if (r.type == REQ_TEX_REG) {
            for (int i = 0; i < tex_batch_n; i++) {
                const AsyncTexReg *tr = &tex_batch[i];
                nvn_async_apply_tex_reg(tr->pool, tr->id, tr->tex, &tr->view, tr->has_view);
            }
        } else if (r.type == REQ_SMP_REG) {
            AsyncSmpReg *sr = (AsyncSmpReg *)r.p;
            if (sr) {
                nvn_async_apply_smp_reg(sr->pool, sr->id, &sr->desc);
                free(sr);
            }
        } else if (r.type == REQ_TEX_POOL_FIN) {
            nvn_async_apply_tex_pool_fin((NvnDescPool *)r.p);
        } else if (r.type == REQ_TEX_DESTROY) {
            nvn_async_apply_tex_destroy((NvnTexture *)r.p);
        }
        if (r.type == REQ_EXEC || r.type == REQ_PRESENT)
            vk_tl(r.type == REQ_EXEC ? "render: list end" : "render: present end", r.id, 0);
        uint64_t elapsed = plat_now_ns() - t0;
        __atomic_store_n(&active_req_type, -1, __ATOMIC_RELEASE);
        if (r.type == REQ_EXEC) {
            __atomic_fetch_add(&frame_exec_req_ns, elapsed, __ATOMIC_RELAXED);
        } else if (r.type == REQ_PRESENT) {
            __atomic_fetch_add(&frame_pres_req_ns, elapsed, __ATOMIC_RELAXED);
        } else {
            __atomic_fetch_add(&frame_other_req_ns, elapsed, __ATOMIC_RELAXED);
        }
        frame_cpu += cpu_timing ? render_cpu_ns() - cpu0 : elapsed;
        frame_work += elapsed;
        if (r.type == REQ_PRESENT) {
            uint64_t exec_ns = __atomic_load_n(&frame_exec_req_ns, __ATOMIC_RELAXED);
            uint64_t pres_ns = __atomic_load_n(&frame_pres_req_ns, __ATOMIC_RELAXED);
            uint64_t other_ns = __atomic_load_n(&frame_other_req_ns, __ATOMIC_RELAXED);
            uint64_t prep_ns = __atomic_load_n(&frame_pre_present_ns, __ATOMIC_RELAXED);
            uint64_t sub_ns = __atomic_load_n(&frame_exec_submit_ns, __ATOMIC_RELAXED);
            uint64_t ring_ns = __atomic_load_n(&frame_ring_begin_ns, __ATOMIC_RELAXED);
            uint64_t post_ns = __atomic_load_n(&frame_post_present_ns, __ATOMIC_RELAXED);
            __atomic_store_n(&last_frame_work, frame_work, __ATOMIC_RELAXED);
            extern void plat_render_work(uint64_t work_ns, uint64_t target_ns);
            __atomic_store_n(&last_frame_cpu, frame_cpu, __ATOMIC_RELAXED);
            __atomic_store_n(&last_frame_exec_req_ns, exec_ns, __ATOMIC_RELAXED);
            __atomic_store_n(&last_frame_pres_req_ns, pres_ns, __ATOMIC_RELAXED);
            __atomic_store_n(&last_frame_other_req_ns, other_ns, __ATOMIC_RELAXED);
            __atomic_store_n(&last_frame_pre_present_ns, prep_ns, __ATOMIC_RELAXED);
            __atomic_store_n(&last_frame_exec_submit_ns, sub_ns, __ATOMIC_RELAXED);
            __atomic_store_n(&last_frame_ring_begin_ns, ring_ns, __ATOMIC_RELAXED);
            __atomic_store_n(&last_frame_post_present_ns, post_ns, __ATOMIC_RELAXED);
            {
                static uint64_t last_log_ns, sub_diag_frames;
                sub_diag_frames++;
                uint64_t now_ns = plat_now_ns();
                if (!last_log_ns || now_ns - last_log_ns >= 5000000000ull) {
                    last_log_ns = now_ns;
                    extern uint64_t g_tex_stat_reg, g_tex_stat_reg_dedup;
                    extern void vk_take_submit_breakdown_v10(uint64_t out[11]);
                    uint64_t tot = __atomic_load_n(&g_tex_stat_reg, __ATOMIC_RELAXED);
                    uint64_t dedup = __atomic_load_n(&g_tex_stat_reg_dedup, __ATOMIC_RELAXED);
                    double pct = tot ? (100.0 * (double)dedup / (double)tot) : 0.0;
                    uint64_t sb[11] = {0};
                    vk_take_submit_breakdown_v10(sb);
                    double fdiv = sub_diag_frames ? (double)sub_diag_frames : 1.0;
                    sub_diag_frames = 0;
                    LOG("[tex-reg] total=%llu dedup=%llu (%.1f%%) frame_cpu=%.1fms exec_req=%.1fms(submit=%.1fms) pres_req=%.1fms(pre=%.1fms ring=%.1fms post=%.1fms) other_req=%.1fms\n",
                        (unsigned long long)tot, (unsigned long long)dedup, pct,
                        frame_cpu / 1e6, exec_ns / 1e6, sub_ns / 1e6,
                        pres_ns / 1e6, prep_ns / 1e6, ring_ns / 1e6, post_ns / 1e6,
                        other_ns / 1e6);
                    LOG("[sub-diag] per_frame(n=%.0f): endp=%.1fms slot=%.1fms vc=%.1fms endcb=%.1fms qsub_s=%.1fms qsub_p=%.1fms pkhr=%.1fms retire=%.1fms sem_r=%.1fms sem_g=%.1fms(%.1f calls)\n",
                        fdiv, sb[0] / 1e6 / fdiv, sb[1] / 1e6 / fdiv, sb[2] / 1e6 / fdiv,
                        sb[3] / 1e6 / fdiv, sb[4] / 1e6 / fdiv, sb[8] / 1e6 / fdiv,
                        sb[9] / 1e6 / fdiv, sb[5] / 1e6 / fdiv, sb[10] / 1e6 / fdiv,
                        sb[6] / 1e6 / fdiv, sb[7] / fdiv);
                }
            }
            uint64_t report_ns = frame_work > frame_cpu ? frame_work : frame_cpu;
            plat_render_work(report_ns, 16666667ull);  // (ADPF: clocks for 60 fps on this thread too)
            frame_work = frame_cpu = 0;
            __atomic_store_n(&frame_exec_req_ns, 0, __ATOMIC_RELAXED);
            __atomic_store_n(&frame_pres_req_ns, 0, __ATOMIC_RELAXED);
            __atomic_store_n(&frame_other_req_ns, 0, __ATOMIC_RELAXED);
            __atomic_store_n(&frame_pre_present_ns, 0, __ATOMIC_RELAXED);
            __atomic_store_n(&frame_exec_submit_ns, 0, __ATOMIC_RELAXED);
            __atomic_store_n(&frame_ring_begin_ns, 0, __ATOMIC_RELAXED);
            __atomic_store_n(&frame_post_present_ns, 0, __ATOMIC_RELAXED);
        }
        pthread_mutex_lock(&q_lock);
        q_head += (uint64_t)(tex_batch_n > 0 ? tex_batch_n : 1);
        if (tex_batch_n > 0) r.id = batch_last_id;
        __atomic_store_n(&q_done, r.id, __ATOMIC_RELEASE);
        if (r.type == REQ_PRESENT) q_presents--;
        if (__atomic_load_n(&q_waiters, __ATOMIC_RELAXED) > 0 || q_push_waiters > 0) {
            pthread_cond_broadcast(&q_less);
        }
    }
    return NULL;
}

static uint64_t push_inner(int type, uint64_t a, void *p, const AsyncTexReg *tr) {
    pthread_mutex_lock(&q_lock);
    if (!g_vk_render_started) {
        g_vk_render_started = true;
        pthread_create(&g_vk_render_thread, NULL, render_main, NULL);
    }
    int max_ahead = (g_settings.sync_lag >= 1 && ahead < 2) ? 2 : ahead;
    if (q_tail - q_head >= QN || (type == REQ_PRESENT && q_presents >= max_ahead)) {
        bool pres_wait = (type == REQ_PRESENT && q_presents >= max_ahead);
        pthread_mutex_unlock(&q_lock);
        uint64_t w0 = plat_now_ns();
        kernel_blocking_begin();
        pthread_mutex_lock(&q_lock);
        q_push_waiters++;
        while (q_tail - q_head >= QN || (type == REQ_PRESENT && q_presents >= max_ahead)) pthread_cond_wait(&q_less, &q_lock);
        q_push_waiters--;
        pthread_mutex_unlock(&q_lock);
        kernel_blocking_end();
        uint64_t w1 = plat_now_ns();
        if (w1 - w0 >= 5000000ull) {
            stall_record_wait_event(renderer_present_count(), w0, w1, "async",
                                    pres_wait ? "async_present_ahead" : "async_ring_full",
                                    vk_async_render_tid(), "vk-render", "push");
        }
        pthread_mutex_lock(&q_lock);
    }
    uint64_t id = q_tail + 1;  // (ids 1, 2, ...: the request at index id - 1)
    if (type == REQ_EXEC || type == REQ_PRESENT || type == REQ_FENCE) {
        extern void vk_tl(const char *what, uint64_t a, uint64_t b);
        vk_tl(type == REQ_EXEC ? "game: list sent" : type == REQ_PRESENT ? "game: present sent" : "game: fence sent", id, 0);
    }
    uint64_t slot = (id - 1) % QN;
    q[slot] = (Req){type, a, p, id};
    if (tr) q_tex[slot] = *tr;
    __atomic_store_n(&q_tail, id, __ATOMIC_RELEASE);
    if (type == REQ_PRESENT) q_presents++;
    if (q_render_sleeping) pthread_cond_signal(&q_more);
    pthread_mutex_unlock(&q_lock);
    return id;
}

static uint64_t push(int type, uint64_t a, void *p) {
    return push_inner(type, a, p, NULL);
}

// Until the render thread has finished request `id` (0: nothing to wait for).
void vk_async_wait_id(uint64_t id) {
    if (!id || !g_vk_render_started || vk_async_is_render_thread()) return;
    uint64_t w0 = plat_now_ns();
    pthread_mutex_lock(&q_lock);
    if (q_done < id) {
        __atomic_fetch_add(&q_waiters, 1, __ATOMIC_RELAXED);
        pthread_cond_signal(&q_more);
        int wait_spin = async_spin_us();
        if (wait_spin > 500) wait_spin = 500;
        if (wait_spin > 0) {
            pthread_mutex_unlock(&q_lock);
            uint64_t spin_t0 = plat_now_ns();
            uint64_t spin_limit = (uint64_t)wait_spin * 1000ull;
            uint32_t iter = 0;
            while (__atomic_load_n(&q_done, __ATOMIC_ACQUIRE) < id) {
                if (plat_now_ns() - spin_t0 >= spin_limit) break;
                cpu_spin_yield(++iter);
            }
            pthread_mutex_lock(&q_lock);
        }
        while (q_done < id) pthread_cond_wait(&q_less, &q_lock);
        __atomic_fetch_sub(&q_waiters, 1, __ATOMIC_RELAXED);
    }
    pthread_mutex_unlock(&q_lock);
    uint64_t w1 = plat_now_ns();
    if (w1 - w0 >= 5000000ull) {
        stall_record_wait_event(renderer_present_count(), w0, w1, "async",
                                "vk_async_wait_id", vk_async_render_tid(), "vk-render", NULL);
    }
}

// Non-blocking query whether render thread has processed request `id`
bool vk_async_is_done(uint64_t id) {
    if (!id || !g_vk_render_started || vk_async_is_render_thread()) return true;
    return __atomic_load_n(&q_done, __ATOMIC_ACQUIRE) >= id;
}

uint64_t vk_async_last_queued_id(void) {
    return __atomic_load_n(&q_tail, __ATOMIC_ACQUIRE);
}

bool vk_async_queue_idle(void) {
    if (!g_vk_render_started) return true;
    pthread_mutex_lock(&q_lock);
    bool idle = (q_head == q_tail);
    pthread_mutex_unlock(&q_lock);
    return idle;
}

// Flush only if target_seq lives inside the currently-open (unsubmitted) command buffer.
void vk_async_flush_if_needed(uint64_t target_seq) {
    if (!target_seq || target_seq <= vk_submitted_seq()) return;
    if (!vk_has_pending_commands() && vk_async_queue_idle()) return;
    if (!vk_async_on() || vk_async_is_render_thread()) {
        if (vk_has_pending_commands()) vk_flush_pending_work();
        return;
    }
    pthread_mutex_lock(&p_lock);
    pthread_mutex_lock(&q_lock);
    if (q_tail > q_head) {
        Req *last = &q[(q_tail - 1) % QN];
        if (last->type == REQ_FLUSH) {
            if (last->a != 0 && target_seq > last->a) last->a = target_seq;
            uint64_t existing_id = last->id;
            pthread_mutex_unlock(&q_lock);
            pthread_mutex_unlock(&p_lock);
            vk_async_wait_id(existing_id);
            return;
        }
    }
    pthread_mutex_unlock(&q_lock);
    uint64_t fid = push(REQ_FLUSH, target_seq, NULL);
    pthread_mutex_unlock(&p_lock);
    vk_async_wait_id(fid);
}

// Non-blocking flush kick for timeout_ns == 0 fence polls: ensures an open batched command buffer
// holding target_seq is queued for vkQueueSubmit without stalling the calling guest thread.
void vk_async_kick_flush(uint64_t target_seq) {
    if (!target_seq || target_seq <= vk_submitted_seq()) return;
    if (!vk_has_pending_commands()) return;
    if (!vk_async_on() || vk_async_is_render_thread()) {
        vk_flush_pending_work();
        return;
    }
    if (pthread_mutex_trylock(&p_lock) != 0) return;
    pthread_mutex_lock(&q_lock);
    if (!g_vk_render_started) {
        g_vk_render_started = true;
        pthread_create(&g_vk_render_thread, NULL, render_main, NULL);
    }
    if (q_tail > q_head) {
        Req *last = &q[(q_tail - 1) % QN];
        if (last->type == REQ_FLUSH) {
            if (last->a != 0 && target_seq > last->a) last->a = target_seq;
            pthread_cond_signal(&q_more);
            pthread_mutex_unlock(&q_lock);
            pthread_mutex_unlock(&p_lock);
            return;
        }
    }
    if (q_tail - q_head < QN) {
        uint64_t id = q_tail + 1;
        q[(id - 1) % QN] = (Req){REQ_FLUSH, target_seq, NULL, id};
        __atomic_store_n(&q_tail, id, __ATOMIC_RELEASE);
        pthread_cond_signal(&q_more);
    }
    pthread_mutex_unlock(&q_lock);
    pthread_mutex_unlock(&p_lock);
}

// Until every request queued so far has run and any open batch is submitted (single coalesced wait).
void vk_async_drain(void) {
    if (!g_vk_render_started || vk_async_is_render_thread()) {
        if (vk_has_pending_commands()) {
            uint64_t f0 = plat_now_ns();
            vk_flush_pending_work();
            extern bool vk_in_sync_wait(void);
            extern void vk_record_nvn_sync_wait(uint64_t ns);
            if (!vk_in_sync_wait()) vk_record_nvn_sync_wait(plat_now_ns() - f0);
        }
        return;
    }
    uint64_t w0 = plat_now_ns();
    pthread_mutex_lock(&p_lock);
    pthread_mutex_lock(&q_lock);
    uint64_t q_depth0 = q_tail - q_head;
    int act_type0 = __atomic_load_n(&active_req_type, __ATOMIC_ACQUIRE);
    uint64_t act_t0 = __atomic_load_n(&active_req_start_ns, __ATOMIC_ACQUIRE);
    bool need_wait = (q_done < q_tail) || vk_has_pending_commands();
    uint64_t fid = 0;
    if (need_wait) {
        if (q_tail > q_head && (q_tail - 1) > q_head) {
            Req *last = &q[(q_tail - 1) % QN];
            if (last->type == REQ_FLUSH && last->a == 0) {
                fid = last->id;
            }
        }
        pthread_mutex_unlock(&q_lock);
        if (!fid) fid = push(REQ_FLUSH, 0, NULL);
    } else {
        pthread_mutex_unlock(&q_lock);
    }
    pthread_mutex_unlock(&p_lock);
    if (fid) {
        kernel_blocking_begin();
        vk_async_wait_id(fid);
        kernel_blocking_end();
    }
    uint64_t w1 = plat_now_ns();
    uint64_t wdt = w1 >= w0 ? w1 - w0 : 0;
    {
        extern bool vk_in_sync_wait(void);
        extern void vk_record_nvn_sync_wait(uint64_t ns);
        if (!vk_in_sync_wait()) vk_record_nvn_sync_wait(wdt);
    }
    if (wdt >= 50000000ull) {
        static uint64_t drain_logs;
        if (drain_logs++ < 60 || (drain_logs & 15u) == 0) {
            extern uint64_t g_tex_stat_reg, g_tex_stat_reg_dedup;
            uint64_t cur_other = __atomic_load_n(&frame_other_req_ns, __ATOMIC_RELAXED);
            uint64_t last_other = __atomic_load_n(&last_frame_other_req_ns, __ATOMIC_RELAXED);
            uint64_t cur_prep = __atomic_load_n(&frame_pre_present_ns, __ATOMIC_RELAXED);
            uint64_t last_prep = __atomic_load_n(&last_frame_pre_present_ns, __ATOMIC_RELAXED);
            uint64_t other_ns = cur_other > last_other ? cur_other : last_other;
            uint64_t prep_ns = cur_prep > last_prep ? cur_prep : last_prep;
            uint64_t exec_ns = __atomic_load_n(&last_frame_exec_req_ns, __ATOMIC_RELAXED);
            uint64_t pres_ns = __atomic_load_n(&last_frame_pres_req_ns, __ATOMIC_RELAXED);
            uint64_t sub_ns = __atomic_load_n(&last_frame_exec_submit_ns, __ATOMIC_RELAXED);
            uint64_t ring_ns = __atomic_load_n(&last_frame_ring_begin_ns, __ATOMIC_RELAXED);
            uint64_t post_ns = __atomic_load_n(&last_frame_post_present_ns, __ATOMIC_RELAXED);
            double act_ms = (act_type0 >= 0 && w0 >= act_t0) ? (double)(w0 - act_t0) / 1e6 : 0.0;
            LOG("[async-diag] drain_ms=%.1f q_depth=%llu active_req=%d(elapsed=%.1fms) spin_ms=%.1f other_req_ms=%.1f pre_present_ms=%.1f render_cpu_ms=%.1f exec=%.1f(sub=%.1f) pres=%.1f(ring=%.1f,post=%.1f) tex_reg=%llu(dedup=%llu) frame=%llu tid=%d\n",
                wdt / 1e6, (unsigned long long)q_depth0, act_type0, act_ms,
                __atomic_load_n(&frame_spin_ns, __ATOMIC_RELAXED) / 1e6,
                other_ns / 1e6, prep_ns / 1e6,
                __atomic_load_n(&last_frame_cpu, __ATOMIC_RELAXED) / 1e6,
                exec_ns / 1e6, sub_ns / 1e6, pres_ns / 1e6, ring_ns / 1e6, post_ns / 1e6,
                (unsigned long long)__atomic_load_n(&g_tex_stat_reg, __ATOMIC_RELAXED),
                (unsigned long long)__atomic_load_n(&g_tex_stat_reg_dedup, __ATOMIC_RELAXED),
                (unsigned long long)renderer_present_count(),
                stall_thread_tid());
        }
    }
    if (wdt >= 5000000ull) {
        stall_record_wait_event(renderer_present_count(), w0, w1, "async",
                                "vk_async_drain", vk_async_render_tid(), "vk-render", NULL);
    }
}

// The fences a command list signals (CMD_FENCE_SYNC, in it or in the lists it calls) get the list's request: a wait
// on one first waits for the render thread to have translated it (its GPU submission is only known then).
static void mark_fences(uint64_t p, uint64_t id, int depth) {
    for (int guard = 0; guard < 10000000; guard++) {
        const CmdRec *r = (const CmdRec *)(uintptr_t)p;
        if (r->op == CMD_END) return;
        if (r->op == CMD_JUMP) { p = r->x[0]; continue; }
        if (r->op == CMD_CALL && depth < 8) {
            const uint64_t *h = (const uint64_t *)r->extra;
            for (uint64_t i = 0; i < r->x[0]; i++) mark_fences(h[i], id, depth + 1);
        } else if (r->op == CMD_FENCE_SYNC && r->x[1]) {
            NvnSync *s = HOST(NvnSync, r->x[1]);
            if (!s) { s = calloc(1, sizeof *s); SET_HOST(r->x[1], s); }
            s->signaled = 1;
            s->lseq = id;
            extern void nvn_note_newest_fence_lseq(uint64_t lseq);
            nvn_note_newest_fence_lseq(id);
        }
        if (!r->size) return;
        p += r->size;
    }
}

// A copy of a submitted list, the lists it calls inlined: NVN reads the lists (in control memory) when they are
// submitted, so a game may record over them right after (BOTW does), while the render thread runs them later.
// Header layout in pooled buffer: first 16 bytes store capacity (size_t) for snap_pool_release; commands start at +16.
typedef struct { uint8_t *b; size_t n, cap; } Snap;
static void snap_add(Snap *s, uint64_t p, int depth) {
    for (int guard = 0; guard < 10000000; guard++) {
        const CmdRec *r = (const CmdRec *)(uintptr_t)p;
        if (r->op == CMD_END) return;
        if (r->op == CMD_JUMP) { p = r->x[0]; continue; }
        if (!r->size) return;
        if (r->op == CMD_CALL && depth < 8) {
            const uint64_t *h = (const uint64_t *)r->extra;
            for (uint64_t i = 0; i < r->x[0]; i++) snap_add(s, h[i], depth + 1);
        } else {
            if (s->n + r->size + sizeof(CmdRec) > s->cap) {
                s->cap = (s->n + r->size + sizeof(CmdRec)) * 2 > 0x10000 ? (s->n + r->size + sizeof(CmdRec)) * 2 : 0x10000;
                s->b = realloc(s->b, s->cap);
                if (!s->b) FATAL("vk: out of memory for a command list copy\n");
                *(size_t *)s->b = s->cap;
            }
            memcpy(s->b + s->n, r, r->size);
            s->n += r->size;
        }
        p += r->size;
    }
}
static uint8_t *snap_list(uint64_t handle) {
    Snap s = {0};
    s.b = snap_pool_acquire(0x10000, &s.cap);
    if (!s.b) FATAL("vk: out of memory for a command list copy\n");
    *(size_t *)s.b = s.cap;
    s.n = 16;
    snap_add(&s, handle, 0);
    if (s.n + sizeof(CmdRec) > s.cap) {
        s.cap = s.n + sizeof(CmdRec);
        s.b = realloc(s.b, s.cap);
        if (!s.b) FATAL("vk: out of memory for a command list copy\n");
        *(size_t *)s.b = s.cap;
    }
    memset(s.b + s.n, 0, sizeof(CmdRec));  // CMD_END
    ((CmdRec *)(s.b + s.n))->size = sizeof(CmdRec);
    return s.b;
}

void vk_async_execute(uint64_t handle) {
    uint8_t *copy = getenv("LARECOMP_NO_LIST_COPY") ? NULL : snap_list(handle);
    uint64_t list_ptr = copy ? (uint64_t)(uintptr_t)(copy + 16) : handle;
    pthread_mutex_lock(&p_lock);
    pthread_mutex_lock(&q_lock);
    uint64_t next = q_tail + 1;  // (the id the push gets: the list's fences carry it before the render thread sees them)
    pthread_mutex_unlock(&q_lock);
    mark_fences(list_ptr, next, 0);
    push(REQ_EXEC, list_ptr, copy);  // (the render thread recycles the copy once run)
    pthread_mutex_unlock(&p_lock);
}

void vk_async_present(NvnTexture *t) {
    pthread_mutex_lock(&p_lock);
    push(REQ_PRESENT, plat_now_ns(), t);  // (a: when the game handed it over: device.c vsync_update)
    pthread_mutex_unlock(&p_lock);
}

uint64_t vk_async_fence(uint64_t guest_sync) {
    pthread_mutex_lock(&p_lock);
    uint64_t id = push(REQ_FENCE, guest_sync, NULL);
    pthread_mutex_unlock(&p_lock);
    return id;
}

void vk_async_tex_reg(NvnDescPool *d, int id, NvnTexture *t, const NvnTextureView *view, bool has_view) {
    AsyncTexReg tr;
    tr.pool = d;
    tr.id = id;
    tr.tex = t;
    tr.has_view = has_view;
    if (has_view && view) tr.view = *view;
    else memset(&tr.view, 0, sizeof tr.view);
    pthread_mutex_lock(&p_lock);
    push_inner(REQ_TEX_REG, 0, NULL, &tr);
    pthread_mutex_unlock(&p_lock);
}

void vk_async_smp_reg(NvnDescPool *d, int id, const NvnSamplerDesc *desc) {
    AsyncSmpReg *sr = malloc(sizeof *sr);
    if (!sr) FATAL("vk: out of memory for async smp reg\n");
    sr->pool = d;
    sr->id = id;
    sr->desc = *desc;
    sr->desc.backend = NULL;
    pthread_mutex_lock(&p_lock);
    push(REQ_SMP_REG, 0, sr);
    pthread_mutex_unlock(&p_lock);
}

void vk_async_tex_pool_fin(NvnDescPool *d) {
    pthread_mutex_lock(&p_lock);
    push(REQ_TEX_POOL_FIN, 0, d);
    pthread_mutex_unlock(&p_lock);
}

void vk_async_tex_destroy(NvnTexture *t) {
    pthread_mutex_lock(&p_lock);
    push(REQ_TEX_DESTROY, 0, t);
    pthread_mutex_unlock(&p_lock);
}
#else
typedef int vk_async_unused;  // (Android only)
#endif
