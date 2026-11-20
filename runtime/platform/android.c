// Android host layer (runtime/platform.h), arm64. Based on linux.c; what differs:
// - the guest window goes wherever the app process has room (android/common/guest_window.h) instead of a fixed
//   address: an app process is forked from the zygote, with its libraries already mapped, and can't re-exec itself;
// - stdout / stderr go to logcat (tag BOTWRecompAndroid);
// - no execinfo: backtraces through the unwinder; no re-exec.
#define _GNU_SOURCE
#include <SDL3/SDL.h>
#include <android/log.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <jni.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/ucontext.h>
#include <time.h>
#include <unistd.h>
#include <unwind.h>

#include "../paths.h"
#include "../platform.h"
#include "../rt.h"
#include "../../android/common/guest_window.h"
#include "../watch.h"

uint64_t g_guest_lo;  // the window's start (rt.h GUEST_LO), set by plat_reserve_guest before main()

// ---- the log in a file as well (Settings > Prelaunch > Driver compatibility, to send with a report; from
// LASRecompAndroid): botwrecomp-log.txt in the folder the player picked (BOTWRecompAndroidActivity.openLog, the run
// before's kept beside it), else <data root>/botwrecomp.log (Android lets only adb read there). The first lines (before
// the setting is read, or a later switch on) wait in a buffer, so the file starts with the driver's and the GPU's lines.
static pthread_mutex_t g_logf_lock = PTHREAD_MUTEX_INITIALIZER;
static FILE *g_logf;
static int g_crash_log_fd = -1;             // dup of g_logf fd for async-signal-safe crash writes
static volatile int g_in_signal_crash;
static bool g_logf_in_folder;               // g_logf is in the picked folder (else the data root)
static bool g_folder_started, g_root_started;  // this run's file was made there (a later switch on appends)
static char g_early[64 << 10];
static size_t g_early_n;

static void log_file_line(const char *line) {
    struct timespec ts, mono;
    clock_gettime(CLOCK_REALTIME, &ts);
    clock_gettime(CLOCK_MONOTONIC, &mono);
    uint64_t now_ns = (uint64_t)mono.tv_sec * 1000000000ull + (uint64_t)mono.tv_nsec;
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    char stamp[24];
    int sn = snprintf(stamp, sizeof stamp, "%02d:%02d:%02d.%03ld ", tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000);
    size_t ln = strlen(line);
    bool urgent = strstr(line, "FATAL") != NULL || strstr(line, "SIGSEGV") != NULL ||
                  strstr(line, "[watchdog]") != NULL || strstr(line, "[android]") != NULL;
    static uint64_t last_flush_ns;
    static uint32_t unbuffered_lines;
    pthread_mutex_lock(&g_logf_lock);
    if (g_logf) {
        fprintf(g_logf, "%s%s\n", stamp, line);
        unbuffered_lines++;
        if (urgent || unbuffered_lines >= 32 || !last_flush_ns || now_ns - last_flush_ns >= 50000000ull) {
            fflush(g_logf);
            unbuffered_lines = 0;
            last_flush_ns = now_ns;
        }
    } else if (!g_folder_started && !g_root_started && g_early_n + (size_t)sn + ln + 1 <= sizeof g_early) {
        memcpy(g_early + g_early_n, stamp, (size_t)sn);
        memcpy(g_early + g_early_n + sn, line, ln);
        g_early_n += (size_t)sn + ln;
        g_early[g_early_n++] = '\n';
    }
    pthread_mutex_unlock(&g_logf_lock);
}

static int picked_folder_log(bool first) {  // BOTWRecompAndroidActivity.openLog: a descriptor, -1 without a usable folder
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    jobject act = (jobject)SDL_GetAndroidActivity();
    if (!env || !act) return -1;
    int fd = -1;
    jclass c = (*env)->GetObjectClass(env, act);
    jmethodID m = (*env)->GetStaticMethodID(env, c, "openLog", "(Z)I");
    if (m) fd = (*env)->CallStaticIntMethod(env, c, m, (jboolean)first);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); fd = -1; }
    (*env)->DeleteLocalRef(env, c);
    (*env)->DeleteLocalRef(env, act);
    return fd;
}

int android_open_log_named(const char *name, bool first) {
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    jobject act = (jobject)SDL_GetAndroidActivity();
    if (!env || !act) return -1;
    int fd = -1;
    jclass c = (*env)->GetObjectClass(env, act);
    jmethodID m = (*env)->GetStaticMethodID(env, c, "openLogFile", "(Ljava/lang/String;Z)I");
    if (m) {
        jstring js = (*env)->NewStringUTF(env, name);
        fd = (*env)->CallStaticIntMethod(env, c, m, js, (jboolean)first);
        if (js) (*env)->DeleteLocalRef(env, js);
    }
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); fd = -1; }
    (*env)->DeleteLocalRef(env, c);
    (*env)->DeleteLocalRef(env, act);
    return fd;
}

int android_open_log_for_read(const char *name) {
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    jobject act = (jobject)SDL_GetAndroidActivity();
    if (!env || !act) return -1;
    int fd = -1;
    jclass c = (*env)->GetObjectClass(env, act);
    jmethodID m = (*env)->GetStaticMethodID(env, c, "openLogFileForRead", "(Ljava/lang/String;)I");
    if (m) {
        jstring js = (*env)->NewStringUTF(env, name);
        fd = (*env)->CallStaticIntMethod(env, c, m, js);
        if (js) (*env)->DeleteLocalRef(env, js);
    }
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); fd = -1; }
    (*env)->DeleteLocalRef(env, c);
    (*env)->DeleteLocalRef(env, act);
    return fd;
}

void android_share_log_file(const char *name) {
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    jobject act = (jobject)SDL_GetAndroidActivity();
    if (!env || !act) return;
    jclass c = (*env)->GetObjectClass(env, act);
    jmethodID m = (*env)->GetStaticMethodID(env, c, "shareLogFile", "(Ljava/lang/String;)V");
    if (m) {
        jstring js = (*env)->NewStringUTF(env, name);
        (*env)->CallStaticVoidMethod(env, c, m, js);
        if (js) (*env)->DeleteLocalRef(env, js);
    }
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    (*env)->DeleteLocalRef(env, c);
    (*env)->DeleteLocalRef(env, act);
}

void android_view_log_file(const char *name) {
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    jobject act = (jobject)SDL_GetAndroidActivity();
    if (!env || !act) return;
    jclass c = (*env)->GetObjectClass(env, act);
    jmethodID m = (*env)->GetStaticMethodID(env, c, "viewLogFile", "(Ljava/lang/String;)V");
    if (m) {
        jstring js = (*env)->NewStringUTF(env, name);
        (*env)->CallStaticVoidMethod(env, c, m, js);
        if (js) (*env)->DeleteLocalRef(env, js);
    }
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    (*env)->DeleteLocalRef(env, c);
    (*env)->DeleteLocalRef(env, act);
}

bool android_delete_log_file(const char *name) {
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    jobject act = (jobject)SDL_GetAndroidActivity();
    if (!env || !act) return false;
    jboolean res = JNI_FALSE;
    jclass c = (*env)->GetObjectClass(env, act);
    jmethodID m = (*env)->GetStaticMethodID(env, c, "deleteLogFile", "(Ljava/lang/String;)Z");
    if (m) {
        jstring js = (*env)->NewStringUTF(env, name);
        res = (*env)->CallStaticBooleanMethod(env, c, m, js);
        if (js) (*env)->DeleteLocalRef(env, js);
    }
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); res = JNI_FALSE; }
    (*env)->DeleteLocalRef(env, c);
    (*env)->DeleteLocalRef(env, act);
    return res == JNI_TRUE;
}

void android_export_log_file(const char *name) {
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    jobject act = (jobject)SDL_GetAndroidActivity();
    if (!env || !act) return;
    jclass c = (*env)->GetObjectClass(env, act);
    jmethodID m = (*env)->GetStaticMethodID(env, c, "exportLogFile", "(Ljava/lang/String;)V");
    if (m) {
        jstring js = (*env)->NewStringUTF(env, name);
        (*env)->CallStaticVoidMethod(env, c, m, js);
        if (js) (*env)->DeleteLocalRef(env, js);
    }
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    (*env)->DeleteLocalRef(env, c);
    (*env)->DeleteLocalRef(env, act);
}

char *android_list_log_files(void) {
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    jobject act = (jobject)SDL_GetAndroidActivity();
    if (!env || !act) return NULL;
    char *res = NULL;
    jclass c = (*env)->GetObjectClass(env, act);
    jmethodID m = (*env)->GetStaticMethodID(env, c, "listLogFiles", "()Ljava/lang/String;");
    if (m) {
        jstring s = (jstring)(*env)->CallStaticObjectMethod(env, c, m);
        if (s) {
            const char *chars = (*env)->GetStringUTFChars(env, s, NULL);
            if (chars) {
                res = strdup(chars);
                (*env)->ReleaseStringUTFChars(env, s, chars);
            }
            (*env)->DeleteLocalRef(env, s);
        }
    }
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    (*env)->DeleteLocalRef(env, c);
    (*env)->DeleteLocalRef(env, act);
    return res;
}

// This run's log so far into a new file: the data root's file when there is one (the folder was picked later), else
// the lines kept from the start
static void log_file_begin(FILE *f, const char *root_file) {
    FILE *src = g_root_started ? fopen(root_file, "r") : NULL;
    if (src) {
        char b[16384];
        for (size_t n; (n = fread(b, 1, sizeof b, src)) > 0;) fwrite(b, 1, n, f);
        fclose(src);
    } else {
        fwrite(g_early, 1, g_early_n, f);
    }
    fflush(f);
}

void android_log_file(bool on) {
    char root[1100], root_prev[1100];
    snprintf(root, sizeof root, "%s/botwrecomp.log", rt_data_root());
    snprintf(root_prev, sizeof root_prev, "%s/botwrecomp-previous.log", rt_data_root());
    pthread_mutex_lock(&g_logf_lock);
    if (!on) {
        if (g_crash_log_fd >= 0) { close(g_crash_log_fd); g_crash_log_fd = -1; }
        if (g_logf) fclose(g_logf);
        g_logf = NULL;
    } else if (!g_logf || !g_logf_in_folder) {  // (a folder picked while the data root's file is open: it moves)
        if (g_logf) fflush(g_logf);
        int fd = picked_folder_log(!g_folder_started);
        FILE *f = fd >= 0 ? fdopen(fd, "a") : NULL;
        if (fd >= 0 && !f) close(fd);
        if (f) {
            if (!g_folder_started) log_file_begin(f, root);
            if (g_crash_log_fd >= 0) { close(g_crash_log_fd); g_crash_log_fd = -1; }
            if (g_logf) fclose(g_logf);
            g_logf = f;
            g_crash_log_fd = dup(fileno(g_logf));
            g_logf_in_folder = g_folder_started = true;
        } else if (!g_logf) {
            if (!g_root_started) rename(root, root_prev);  // (a crash's log survives the next start)
            g_logf = fopen(root, g_root_started ? "a" : "w");
            if (g_logf && !g_root_started) log_file_begin(g_logf, root);
            if (g_logf) {
                if (g_crash_log_fd >= 0) close(g_crash_log_fd);
                g_crash_log_fd = dup(fileno(g_logf));
            }
            g_root_started |= g_logf != NULL;
            g_logf_in_folder = false;
        }
    }
    pthread_mutex_unlock(&g_logf_lock);
}

static void log_line(const char *line) {
    __android_log_write(ANDROID_LOG_INFO, "BOTWRecompAndroid", line);
    log_file_line(line);
}

// ---- logcat: the runtime logs with fprintf(stderr), as on the desktop; a thread forwards the lines
// LARECOMP_LOG_FILE=<absolute path>: every line also goes there (logcat drops lines when a debug frame logs thousands)
static void *log_pump(void *arg) {
    int fd = (int)(intptr_t)arg;
    char buf[4096];
    size_t n = 0;
    const char *path = getenv("LARECOMP_LOG_FILE");
    int out = path ? open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644) : -1;
    for (;;) {
        ssize_t r = read(fd, buf + n, sizeof buf - 1 - n);
        if (r <= 0) break;
        if (out >= 0 && write(out, buf + n, (size_t)r) < 0) { close(out); out = -1; }
        n += (size_t)r;
        char *start = buf, *nl;
        while ((nl = memchr(start, '\n', (size_t)(buf + n - start)))) {
            *nl = 0;
            log_line(start);
            start = nl + 1;
        }
        n = (size_t)(buf + n - start);
        memmove(buf, start, n);
        if (n == sizeof buf - 1) {  // (a line longer than the buffer: in pieces)
            buf[n] = 0;
            log_line(buf);
            n = 0;
        }
    }
    return NULL;
}

static void log_to_logcat(void) {
    static bool done;
    if (done) return;
    done = true;
    int p[2];
    if (pipe(p) != 0) return;
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);
    dup2(p[1], STDOUT_FILENO);
    dup2(p[1], STDERR_FILENO);
    close(p[1]);
    pthread_t t;
    if (pthread_create(&t, NULL, log_pump, (void *)(intptr_t)p[0]) == 0) pthread_detach(t);
}

// ---- guest address space
// Called by runtime/main.c before main() with GUEST_LO / GUEST_HI, while the window has no place yet (GUEST_LO = 0):
// picks one (the lowest 4 GB-aligned free hole above 4 GB and below 2^39) and sets g_guest_lo. Called with a set
// window (it never moves while the process lives), it claims exactly that range, as on Linux.
bool plat_reserve_guest(uint64_t lo, uint64_t hi) {
    log_to_logcat();
    if (!g_guest_lo) {
        uint64_t base = gw_reserve(hi - lo, GW_ALIGN, GW_FLOOR, GW_VA_LIMIT);
        if (!base) {
            fprintf(stderr, "FATAL: no room for the guest window (%#llx bytes below 2^39)\n", (unsigned long long)(hi - lo));
            return false;
        }
        g_guest_lo = base;
        fprintf(stderr, "[android] guest window %#llx-%#llx\n", (unsigned long long)base, (unsigned long long)(base + hi - lo));
        const char *last = getenv("LARECOMP_LAST_EXIT");  // (the activity: ApplicationExitInfo)
        if (last && *last) fprintf(stderr, "[android] the last run ended: %s\n", last);
        return true;
    }
    void *p = mmap((void *)(uintptr_t)lo, hi - lo, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED) return false;
    if (p != (void *)(uintptr_t)lo) { munmap(p, hi - lo); return false; }
    return true;
}

static uint64_t wt_lo, wt_hi;  // (page write tracking, below)
static uint8_t *wt_state;
static void wt_lock(void);
static void wt_unlock(void);

// Fresh zero pages over the whole window (the old ones are released at once).
bool plat_reset_guest(uint64_t lo, uint64_t hi) {
    if (wt_state && lo < wt_hi && hi > wt_lo) {  // (fresh pages are writable: nothing tracked there any more)
        uint64_t a = lo < wt_lo ? wt_lo : lo, e = hi > wt_hi ? wt_hi : hi;
        wt_lock();
        memset(wt_state + ((a - wt_lo) >> 12), 0, (size_t)((e - a) >> 12));
        wt_unlock();
    }
    return mmap((void *)(uintptr_t)lo, hi - lo, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED,
                -1, 0) == (void *)(uintptr_t)lo;
}

void plat_commit(uint64_t addr, uint64_t size) { (void)addr; (void)size; }

// ---- page write tracking (plat_written_pages), for the copy mode of Qualcomm's driver: there the heap isn't GPU
// memory, so the vertex cache and the texture checks need to know what the CPU wrote (else every vertex and index
// buffer was copied each frame: ~90 MB, ~7 ms of the render thread). Linux has no GetWriteWatch: a page asked about
// is made read-only; the first write to it faults (crash_handler -> wt_fault), marks it written and makes it
// writable again. A page not asked about before counts as written. The OS's own writes into guest memory (pread)
// can't fault - they fail with EFAULT - so those make their pages writable first (plat_write_prepare).
// LARECOMP_NO_WRITE_TRACKING=1: off.
static uint64_t wt_lo, wt_hi;
static uint8_t *wt_state;  // per 4 KB page: 0 not tracked (counts as written), 1 read-only since asked, 2 written since
                           // (all three: tentative definitions, declared above plat_reset_guest)
static atomic_flag wt_spin = ATOMIC_FLAG_INIT;  // (taken in the signal handler too: no mutex)
static bool wt_broken;  // (an mprotect failed - out of memory mappings: no new pages tracked)
static _Atomic uint64_t wt_fault_ns_acc, wt_fault_count_acc, wt_query_ns_acc;
static void wt_lock(void) {
    while (atomic_flag_test_and_set_explicit(&wt_spin, memory_order_acquire)) {
#if defined(__aarch64__) || defined(__arm__)
        __asm__ volatile("yield" ::: "memory");
#endif
    }
}
static void wt_unlock(void) { atomic_flag_clear_explicit(&wt_spin, memory_order_release); }

void plat_write_tracking_reset_range(uint64_t lo, uint64_t hi) {
    ryza_watch_reset_range(lo, hi);
    if (!wt_state || hi <= lo || hi <= wt_lo || lo >= wt_hi) return;
    uint64_t a = lo < wt_lo ? wt_lo : (lo & ~0xFFFull);
    uint64_t e = hi > wt_hi ? wt_hi : ((hi + 0xFFFull) & ~0xFFFull);
    if (e <= a) return;
    wt_lock();
    memset(wt_state + ((a - wt_lo) >> 12), 0, (size_t)((e - a) >> 12));
    wt_unlock();
}

void plat_write_tracking_enable(uint64_t lo, uint64_t hi) {
    if (getenv("LARECOMP_NO_WRITE_TRACKING")) return;
    ryza_watch_init_heap((uintptr_t)lo, (size_t)(hi - lo));
    if (wt_state) return;
    lo &= ~0xFFFull;
    hi = (hi + 0xFFF) & ~0xFFFull;
    uint8_t *s = calloc((size_t)((hi - lo) >> 12), 1);
    if (!s) return;
    wt_lo = lo; wt_hi = hi;
    wt_state = s;
    fprintf(stderr, "[android] page write tracking (batched runs) on %#llx-%#llx\n", (unsigned long long)lo, (unsigned long long)hi);
}

// (signal handler) a write to a page made read-only here: true when handled
// A fault on a page that should be writable already (not tracked, or written since) is reopened too, unless the same
// address keeps faulting: on an Adreno 650, a Mali-G57 and an Adreno 610 (issues #70, #72, #75, 1.8.2 at its title
// screen) the game's heap lock at 0x34000003c - the heap's first page, the first one tracked - faulted and wasn't
// taken for ours; not on the POCO F7 Ultra with the same path. wt_odd counts those (the crash report shows it).
static uint64_t wt_odd, wt_odd_addr;
static int wt_odd_repeats;

// Each page made read-only or writable on its own splits the window's mapping: after half an hour the process ran into
// Android's limit on mappings (~65000), mprotect failed, the page written to stayed read-only, and the same store
// faulted until it was taken for a real fault - a crash in the game's code on a page marked written (Mali-G720, #196;
// "page tracking 2"). Then the whole window is made writable again in one call (a single mapping), every page counts as
// written (state 0), and tracking starts over from there. (wt_lock held; signal-safe: no allocation, no logging.)
static volatile int wt_reopened;  // (times it happened: plat_written_pages logs it)
static bool wt_reopen_all(void) {
    if (mprotect((void *)(uintptr_t)wt_lo, wt_hi - wt_lo, PROT_READ | PROT_WRITE)) return false;
    // Preserve state 2 (written since last query) and convert state 1 (read-only) -> 3 (clean, needs re-arm).
    // Resetting all pages to 0 previously made every texture and vertex buffer appear modified at once (~858 uploads).
    size_t np = (size_t)((wt_hi - wt_lo) >> 12);
    for (size_t i = 0; i < np; i++) {
        if (wt_state[i] == 1) wt_state[i] = 3;
    }
    wt_reopened++;
    return true;
}

static bool wt_fault(uint64_t a) {
    if (!wt_state || a < wt_lo || a >= wt_hi) return false;
    uint64_t wt_t0 = plat_now_ns();
    uint64_t p = (a - wt_lo) >> 12, np = (wt_hi - wt_lo) >> 12;
    bool ours = false;
    wt_lock();
    if (wt_state[p] == 1) wt_odd_repeats = 0;
    else if (wt_odd_addr == a && ++wt_odd_repeats > 8) { wt_unlock(); return false; }  // (a real fault)
    else if (wt_odd_addr != a) wt_odd_addr = a, wt_odd_repeats = 0;
    if (wt_state[p] != 1) wt_odd++;
    // (2: another thread's fault on it got there first - writable already. The next page too: a store across the
    // boundary may report its first byte, in the page before the protected one)
    // (the next page only while still read-only: a real fault on this one must not retry forever)
    // Unprotect in 4-page (16 KB) aligned chunks to cut VMA fragmentation 4x and amortize SIGSEGV faults.
    uint64_t base_k = p & ~3ull;
    for (uint64_t k = base_k; k < base_k + 4 && k < np; k++) {
        if (k != p && wt_state[k] != 1) continue;
        if (k == p && (wt_state[k] == 0 || wt_state[k] == 3)) {  // (untracked/reopened: writable unless mprotect fails)
            if (mprotect((void *)(uintptr_t)(wt_lo + (k << 12)), 4096, PROT_READ | PROT_WRITE)) {
                if (errno == ENOMEM && wt_reopen_all()) ours = true;  // (out of mappings, above)
                break;
            }
            ours = true;
            continue;
        }
        wt_state[k] = 2;
        if (mprotect((void *)(uintptr_t)(wt_lo + (k << 12)), 4096, PROT_READ | PROT_WRITE)) {
            ours = wt_reopen_all();  // (out of mappings, above; else it stays a fault)
            break;
        }
        ours = true;
    }
    wt_unlock();
    if (ours) {
        atomic_fetch_add_explicit(&wt_fault_ns_acc, plat_now_ns() - wt_t0, memory_order_relaxed);
        atomic_fetch_add_explicit(&wt_fault_count_acc, 1, memory_order_relaxed);
    }
    return ours;
}

// (the crash report) the tracking state of a guest page: -1 not tracked by this, else 0-2 (wt_state), and the odd faults
static int plat_write_tracking_state(uint64_t a, uint64_t *odd) {
    *odd = wt_odd;
    if (!wt_state || a < wt_lo || a >= wt_hi) return -1;
    return wt_state[(a - wt_lo) >> 12];
}

void plat_write_prepare(uint64_t addr, uint64_t size) {
    ryza_watch_before_kernel_write((void *)(uintptr_t)addr, (size_t)size);
    if (!wt_state || !size || addr + size <= wt_lo || addr >= wt_hi) return;
    uint64_t qt0 = plat_now_ns();
    uint64_t a = addr < wt_lo ? wt_lo : addr, e = addr + size > wt_hi ? wt_hi : addr + size;
    uint64_t p0 = (a - wt_lo) >> 12, p1 = (e - wt_lo + 0xFFF) >> 12;
    wt_lock();
    for (uint64_t p = p0; p < p1;) {
        if (wt_state[p] != 1) { p++; continue; }
        uint64_t q = p;
        while (q < p1 && wt_state[q] == 1) wt_state[q++] = 2;
        mprotect((void *)(uintptr_t)(wt_lo + (p << 12)), (q - p) << 12, PROT_READ | PROT_WRITE);
        p = q;
    }
    wt_unlock();
    atomic_fetch_add_explicit(&wt_query_ns_acc, plat_now_ns() - qt0, memory_order_relaxed);
}

int plat_written_pages(uint64_t addr, uint64_t size, uint64_t *pages, int max, uint64_t *page) {
    if (ryza_watch_is_watchable((uintptr_t)addr, (size_t)size)) {
        int r = ryza_watch_get_written_pages(addr, size, pages, max, page);
        if (r >= 0) return r;
    }
    if (!wt_state || addr < wt_lo || addr + size > wt_hi) return -1;
    *page = 4096;
    {   // (the process's mappings, every 5 minutes - how near the limit the tracked pages bring it; and wt_reopen_all)
        static int logged;
        static uint64_t next;
        uint64_t now = plat_now_ns();
        if (wt_reopened != logged) {
            logged = wt_reopened;
            fprintf(stderr, "[android] page write tracking: out of memory mappings, the window reopened whole (%d)\n", logged);
        }
        if (now >= next) {
            // (and whose: the tracked window's own, the GPU driver's device files, the rest - on a Mali the count stayed
            // at 62000 after the window was reopened whole, #249: not all of them the tracking's)
            static long every = -1;  // (LARECOMP_LOG_MAPS_S=<s>: another interval, a test)
            if (every < 0) every = getenv("LARECOMP_LOG_MAPS_S") && atol(getenv("LARECOMP_LOG_MAPS_S")) > 0 ? atol(getenv("LARECOMP_LOG_MAPS_S")) : 300;
            next = now + (uint64_t)every * 1000000000ull;
            FILE *f = fopen("/proc/self/maps", "r");
            int n = 0, in_window = 0, gpu = 0;
            char line[512];
            struct { char name[40]; int n; } top[24];  // (the others by name: [anon:scudo...], a library, a device)
            int ntop = 0;
            while (f && fgets(line, sizeof line, f)) {
                n++;
                unsigned long long lo = strtoull(line, NULL, 16);
                if (lo >= wt_lo && lo < wt_hi) { in_window++; continue; }
                if (strstr(line, "/dev/mali") || strstr(line, "/dev/kgsl") || strstr(line, "dmabuf") || strstr(line, "/dev/dri"))
                    gpu++;
                char *nm = line;  // (the 6th field: after five spaces-separated ones)
                for (int k = 0; k < 5 && nm; k++) { nm = strchr(nm, ' '); while (nm && *nm == ' ') nm++; }
                char key[40];
                snprintf(key, sizeof key, "%s", nm && *nm && *nm != '\n' ? nm : "(no name)");
                key[strcspn(key, "\n")] = 0;
                if (key[0] == '[') key[strcspn(key, ":]") + 1 < sizeof key ? strcspn(key, "]") + 1 : sizeof key - 1] = 0;
                int t = 0;
                while (t < ntop && strcmp(top[t].name, key)) t++;
                if (t == ntop && ntop < 24) snprintf(top[ntop].name, sizeof top[ntop].name, "%s", key), top[ntop++].n = 0;
                if (t < ntop) top[t].n++;
            }
            if (f) fclose(f);
            char most[160] = "";
            for (int r = 0, len = 0; r < 3; r++) {  // (the three most)
                int best = -1;
                for (int t = 0; t < ntop; t++) if (top[t].n > 0 && (best < 0 || top[t].n > top[best].n)) best = t;
                if (best < 0) break;
                len += snprintf(most + len, sizeof most - len, "%s%s %d", r ? ", " : "", top[best].name, top[best].n);
                top[best].n = -top[best].n;
            }
            fprintf(stderr, "[android] memory mappings: %d (the game's memory %d, the GPU driver's %d, other %d; most: %s)\n", n,
                    in_window, gpu, n - in_window - gpu, most);
        }
    }
    uint64_t qt0 = plat_now_ns();
    uint64_t p0 = (addr - wt_lo) >> 12, p1 = (addr + size - wt_lo + 0xFFF) >> 12;
    int n = 0;
    wt_lock();
    {   // LARECOMP_WT_REOPEN_TEST=<s> (test): the out-of-mappings path (wt_reopen_all) taken every that many seconds
        static int every = -1;
        static uint64_t next;
        if (every < 0) every = getenv("LARECOMP_WT_REOPEN_TEST") ? atoi(getenv("LARECOMP_WT_REOPEN_TEST")) : 0;
        uint64_t now = plat_now_ns();
        if (every > 0 && now >= next) {
            bool first = !next;
            next = now + (uint64_t)every * 1000000000ull;
            if (!first && wt_reopen_all()) { wt_unlock(); return -1; }
        }
    }
    for (uint64_t p = p0; p < p1;) {
        if (wt_state[p] == 1) { p++; continue; }
        uint64_t q = p;
        for (; q < p1 && wt_state[q] != 1; q++) {
            if (wt_state[q] != 3) {  // state 3: clean page reopened by wt_reopen_all (re-arm without marking dirty)
                if (n < max) pages[n] = wt_lo + (q << 12);
                n++;
            }
        }
        if (!wt_broken) {
            // (state first, then the protection: a write in between lands before the caller copies the page)
            for (uint64_t k = p; k < q; k++) wt_state[k] = 1;
            if (mprotect((void *)(uintptr_t)(wt_lo + (p << 12)), (q - p) << 12, PROT_READ)) {
                if (errno == ENOMEM && wt_reopen_all()) {
                    // Range stays writable with state 3/2 intact; return dirty count collected so far without -1 storm
                    wt_unlock();
                    atomic_fetch_add_explicit(&wt_query_ns_acc, plat_now_ns() - qt0, memory_order_relaxed);
                    return n > max ? max : n;
                }
                for (uint64_t k = p; k < q; k++) wt_state[k] = 0;
                mprotect((void *)(uintptr_t)(wt_lo + (p << 12)), (q - p) << 12, PROT_READ | PROT_WRITE);
                wt_broken = true;
                fprintf(stderr, "[android] page write tracking: mprotect failed (%d), no new pages tracked\n", errno);
            }
        }
        p = q;
    }
    wt_unlock();
    atomic_fetch_add_explicit(&wt_query_ns_acc, plat_now_ns() - qt0, memory_order_relaxed);
    return n > max ? max : n;
}

uint64_t plat_take_write_tracking_ns(uint64_t *faults_out, uint64_t *fault_ns_out, uint64_t *query_ns_out) {
    uint64_t f = atomic_exchange_explicit(&wt_fault_count_acc, 0, memory_order_relaxed);
    uint64_t fns = atomic_exchange_explicit(&wt_fault_ns_acc, 0, memory_order_relaxed);
    uint64_t qns = atomic_exchange_explicit(&wt_query_ns_acc, 0, memory_order_relaxed);
    if (faults_out) *faults_out = f;
    if (fault_ns_out) *fault_ns_out = fns;
    if (query_ns_out) *query_ns_out = qns;
    return fns + qns;
}


int plat_protect(uint64_t addr, uint64_t size, int prot) {
    int p = prot == PLAT_PROT_NONE ? PROT_NONE : prot == PLAT_PROT_READ ? PROT_READ : PROT_READ | PROT_WRITE;
    return mprotect((void *)(uintptr_t)addr, size, p);
}

// ---- time
uint64_t plat_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

void plat_sleep_until_ns(uint64_t t) {
    struct timespec ts = {.tv_sec = (time_t)(t / 1000000000u), .tv_nsec = (long)(t % 1000000000u)};
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {}
}

// Apps can't have real-time scheduling: the display priority Android gives its own frame threads instead.
void plat_thread_realtime(uint64_t period_ns) {
    (void)period_ns;
    setpriority(PRIO_PROCESS, (id_t)gettid(), -8);  // (THREAD_PRIORITY_DISPLAY; fails quietly if not allowed)
}

// ---- process
void plat_crash_write_raw(const char *s) {
    if (!s || !*s) return;
    __android_log_write(ANDROID_LOG_ERROR, "BOTWRecompAndroid", s);
    int fd = __atomic_load_n(&g_crash_log_fd, __ATOMIC_ACQUIRE);
    if (fd >= 0) {
        size_t len = strlen(s);
        ssize_t r = write(fd, s, len);
        (void)r;
        fsync(fd);
    }
}

void plat_flush_logs(void) {
    fflush(stdout);
    fflush(stderr);
    usleep(60000);  // give log_pump time to drain pipe into logcat + g_logf
    if (pthread_mutex_trylock(&g_logf_lock) == 0) {
        if (g_logf) {
            fflush(g_logf);
            fsync(fileno(g_logf));
        }
        pthread_mutex_unlock(&g_logf_lock);
    } else {
        int fd = __atomic_load_n(&g_crash_log_fd, __ATOMIC_ACQUIRE);
        if (fd >= 0) fsync(fd);
    }
}

void plat_init(void) {
    log_to_logcat();
    plat_crash_handler_install();
}

// "The executable": a file in the folder the app copies its shipped resources to (internal storage, files/res: the
// Baloo font, the icon, ...), so runtime/paths.c finds them next to it as on the desktop. Native libraries stay in
// nativeLibraryDir (LARECOMP_NATIVE_LIB_DIR, set by the activity).
bool plat_exe_path(char *buf, size_t size) {
    const char *files = SDL_GetAndroidInternalStoragePath();
    if (!files) return false;
    return (size_t)snprintf(buf, size, "%s/res/botwrecompandroid", files) < size;
}

void plat_exec(char *const argv[]) { (void)argv; }  // (an app process can't replace itself: the caller reports it)

// ---- diagnostics
// The fatal path stays on the calling thread. Keep its reason through the backtrace so an Android tester can
// capture the actual error without needing adb; raw signal faults still use the generic notice below.
static __thread char fatal_message[1024];
void rt_fatal_message(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(fatal_message, sizeof fatal_message, format, args);
    va_end(args);
    fprintf(stderr, "FATAL: %s", fatal_message);
    rt_fatal_trace();
}

const char *plat_symbol(uint64_t pc, uint64_t *offset) {
    Dl_info di = {0};
    if (!dladdr((void *)(uintptr_t)pc, &di) || !di.dli_sname) return NULL;
    if (offset) *offset = pc - (uint64_t)(uintptr_t)di.dli_saddr;
    return di.dli_sname;
}

// A graphics driver the game can't run on: BOTWRecompAndroidActivity.driverErrorDialog offers to install another one
// (or go back to the system's) and starts the app again - the Settings rows that choose the driver are out of reach
// when it fails before the start screen (LAS Recomp for Android 1c05238, from LASRecomp-Android-Plus, MIT). False: no
// dialog shown.
static bool driver_error_dialog(const char *text) {
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    jobject act = (jobject)SDL_GetAndroidActivity();
    if (!env || !act) return false;
    bool shown = false;
    jclass c = (*env)->GetObjectClass(env, act);
    jmethodID m = (*env)->GetStaticMethodID(env, c, "driverErrorDialog", "(Ljava/lang/String;)Z");
    jstring s = (*env)->NewStringUTF(env, text);
    if (m && s) shown = (*env)->CallStaticBooleanMethod(env, c, m, s);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); shown = false; }
    if (s) (*env)->DeleteLocalRef(env, s);
    (*env)->DeleteLocalRef(env, c);
    (*env)->DeleteLocalRef(env, act);
    return shown;
}

void plat_fatal_notice(void) {
    plat_flush_logs();
    if (__atomic_load_n(&g_in_signal_crash, __ATOMIC_RELAXED)) return;
    char message[1400];
    snprintf(message, sizeof message, "BOTWRecompAndroid stopped because of an error.\n\n%s\n\n"
             "Please include a screenshot of this message and your app/driver version in your bug report.\n"
             "More details: logcat, tag BOTWRecompAndroid.", fatal_message[0] ? fatal_message : "The runtime encountered a fatal error.");
    if (strcasestr(fatal_message, "vulkan") && driver_error_dialog(message)) return;  // (the driver's own errors)
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "BOTWRecompAndroid", message, NULL);
}

const char *rt_host_fn_name(uint64_t pc, uint64_t *offset);     // runtime/dispatch.c
uint64_t rt_host_fn_entry(uint64_t pc, uint64_t *offset);       // runtime/dispatch.c
const char *loader_symbol_at(uint64_t addr, uint64_t *offset);  // runtime/loader.c
extern Cpu *kernel_current_cpu(void);                           // runtime/kernel.c

typedef struct { void **pcs; int n, max; } Trace;
static _Unwind_Reason_Code trace_step(struct _Unwind_Context *ctx, void *arg) {
    Trace *t = arg;
    uintptr_t pc = _Unwind_GetIP(ctx);
    if (pc && t->n < t->max) t->pcs[t->n++] = (void *)pc;
    return t->n < t->max ? _URC_NO_REASON : _URC_END_OF_STACK;
}

void plat_print_backtrace(void) {
    void *bt[64];
    Trace t = {bt, 0, 64};
    _Unwind_Backtrace(trace_step, &t);
    for (int i = 0; i < t.n; i++) {
        uint64_t pc = (uint64_t)(uintptr_t)bt[i], off = 0;
        Dl_info di = {0};
        if (!dladdr(bt[i], &di)) di.dli_fname = NULL;
        // Resolve recompiled guest functions (f_<guest_addr>) first across all modules
        // (including botwgame.so, which is a separate shared object from libmain.so).
        const char *s = rt_host_fn_name(pc, &off);
        if (!s) s = plat_symbol(pc, &off);
        uint64_t gfn_off = 0, gsym_off = 0;
        uint64_t gfn = rt_host_fn_entry(pc, &gfn_off);
        const char *gsym = gfn ? loader_symbol_at(gfn, &gsym_off) : NULL;
        const char *mod = di.dli_fname ? (strrchr(di.dli_fname, '/') ? strrchr(di.dli_fname, '/') + 1 : di.dli_fname) : "?";
        uint64_t moff = pc - (uint64_t)(uintptr_t)di.dli_fbase;
        if (s && gsym)
            fprintf(stderr, "  #%-2d %016llx %s+%llx [%s+%llx] (%s+%llx)\n", i, (unsigned long long)pc,
                    s, (unsigned long long)off, gsym, (unsigned long long)gsym_off, mod, (unsigned long long)moff);
        else if (s)
            fprintf(stderr, "  #%-2d %016llx %s+%llx (%s+%llx)\n", i, (unsigned long long)pc, s, (unsigned long long)off,
                    mod, (unsigned long long)moff);
        else
            fprintf(stderr, "  #%-2d %016llx %s+%llx\n", i, (unsigned long long)pc, mod, (unsigned long long)moff);
    }
}

// The game's code reading the null page (an address under 64 KB): the load answers zero and the code goes on. The game
// checks some pointers that are still being set up only by what they point to - a rune's or the Slate camera's target
// tag (0x88 off a null or -1 pointer, #235 #241 #247) - and both modes crashed there. Only plain loads into general
// registers (LDR / LDRB / LDRH / LDRS*, LDP) without a base write-back; in the game's code only (recompiled:
// botwgame.so; Native: the guest window). Each place logged once. LARECOMP_NO_NULL_READS=1: off.
static bool null_read(ucontext_t *uc, uint64_t fa, uint64_t pc) {
    static int off = -1;
    if (off < 0) off = getenv("LARECOMP_NO_NULL_READS") != NULL;
    if (off || fa >= 0x10000 || (pc & 3)) return false;
    bool game = pc >= GUEST_LO && pc < GUEST_HI;
    if (!game) {
        Dl_info di = {0};
        game = dladdr((void *)(uintptr_t)pc, &di) && di.dli_fname && strstr(di.dli_fname, "botwgame");
    }
    if (!game) return false;
    uint32_t in = *(const uint32_t *)(uintptr_t)pc;
    int rt = in & 31, rt2 = -1, rn = (in >> 5) & 31;
    if ((in & 0x3B000000u) == 0x39000000u) {  // (LDR* unsigned offset: opc 01, or 10 / 11 sign-extending but PRFM)
        int opc = (in >> 22) & 3, size = in >> 30;
        if (opc == 0 || (opc == 2 && size == 3)) return false;
    } else if ((in & 0x3B200C00u) == 0x38000000u || (in & 0x3B200C00u) == 0x38200800u) {  // (unscaled; register offset)
        int opc = (in >> 22) & 3, size = in >> 30;
        if (opc == 0 || (opc == 2 && size == 3)) return false;
    } else if ((in & 0x3BC00000u) == 0x29400000u) {  // (LDP / LDPSW, signed offset: no write-back)
        rt2 = (in >> 10) & 31;
    } else return false;
    uint64_t base_val = rn < 31 ? uc->uc_mcontext.regs[rn] : uc->uc_mcontext.sp;
    if (rt < 31) uc->uc_mcontext.regs[rt] = 0;
    if (rt2 >= 0 && rt2 < 31) uc->uc_mcontext.regs[rt2] = 0;
    uc->uc_mcontext.pc = pc + 4;
    static uint64_t seen_pc[64], seen_fa[64];
    static int nseen;
    bool known = false;
    for (int i = 0; i < nseen && !known; i++) known = (seen_pc[i] == pc && seen_fa[i] == fa);
    if (!known && nseen < 64) {
        int idx = nseen++;
        seen_pc[idx] = pc;
        seen_fa[idx] = fa;
        Dl_info di = {0};
        uint64_t mo = dladdr((void *)(uintptr_t)pc, &di) ? pc - (uint64_t)(uintptr_t)di.dli_fbase : pc;
        uint64_t fn_off = 0, lr_fn_off = 0;
        const char *fn = rt_host_fn_name(pc, &fn_off);
        uint64_t hlr = uc->uc_mcontext.regs[30];
        const char *lr_fn = rt_host_fn_name(hlr, &lr_fn_off);
        Cpu *c = kernel_current_cpu();
        fprintf(stderr,
                "[android] the game's code read address %#llx (base x%d=%#llx -> dst x%d): zero, and on "
                "(tid=%d, %s+%llx, fn=%s+%#llx, host_lr=%#llx [%s+%#llx], guest_lr=%#llx)\n",
                (unsigned long long)fa, rn, (unsigned long long)base_val, rt, (int)gettid(),
                game && pc >= GUEST_LO && pc < GUEST_HI ? "guest" : "botwgame.so",
                (unsigned long long)mo, fn ? fn : "?", (unsigned long long)fn_off,
                (unsigned long long)hlr, lr_fn ? lr_fn : "?", (unsigned long long)lr_fn_off,
                (unsigned long long)(c ? c->x[30] : 0));
        if (idx < 8 && c) {
            fprintf(stderr,
                    "[android] null_read[%d] guest regs: x0=%#llx x1=%#llx x2=%#llx x3=%#llx x8=%#llx x19=%#llx x20=%#llx x21=%#llx\n",
                    idx, (unsigned long long)c->x[0], (unsigned long long)c->x[1],
                    (unsigned long long)c->x[2], (unsigned long long)c->x[3],
                    (unsigned long long)c->x[8], (unsigned long long)c->x[19],
                    (unsigned long long)c->x[20], (unsigned long long)c->x[21]);
        }
    }
    return true;
}

// (installed through ART's signal chain: ART still handles its own faults first)
static void crash_handler(int sig, siginfo_t *si, void *ctx) {
    ucontext_t *uc = ctx;
    static volatile int depth;
    if (sig == SIGABRT && depth++ > 0) { signal(SIGABRT, SIG_DFL); abort(); }  // (nested: just die)
    uint64_t fa = (uint64_t)(uintptr_t)si->si_addr;
    uint64_t pc = uc->uc_mcontext.pc, lr = uc->uc_mcontext.regs[30];
    {  // (the fault's ESR, when the kernel gives it: WnR, bit 6)
        extern int g_fault_wnr;
        g_fault_wnr = -1;
        struct _aarch64_ctx *h = (struct _aarch64_ctx *)(void *)uc->uc_mcontext.__reserved;
        for (int i = 0; i < 64 && h->magic && h->size; i++, h = (struct _aarch64_ctx *)((char *)h + h->size))
            if (h->magic == ESR_MAGIC) { g_fault_wnr = (int)((((struct esr_context *)h)->esr >> 6) & 1); break; }
    }
    if (rt_fault_watch(fa, pc)) return;  // (debugging watches first: a watched page may be write-tracked too)
    {   // (the null page read: zero, above)
        extern int g_fault_wnr;
        if (sig == SIGSEGV && g_fault_wnr != 1 && null_read(uc, fa, pc)) return;
    }
    if (sig == SIGSEGV && (ryza_watch_on_fault(fa) || wt_fault(fa))) return;  // (write tracking - whatever the code: mprotect decides, #70)
    if (sig == SIGSEGV) {  // (what the next report needs: the kernel's code, and the page's tracking state)
        extern int g_fault_wnr;
        uint64_t odd;
        int st = plat_write_tracking_state(fa, &odd);
        fprintf(stderr, "*** SIGSEGV code %d (%s), write %d, page tracking %d, odd write faults %llu\n", si->si_code,
                si->si_code == SEGV_ACCERR ? "no access" : si->si_code == SEGV_MAPERR ? "not mapped" : "other", g_fault_wnr,
                st, (unsigned long long)odd);
    }
    __atomic_store_n(&g_in_signal_crash, 1, __ATOMIC_RELEASE);
    char what[32];
    snprintf(what, sizeof what, "signal %d", sig);
    if (pc >= GUEST_LO && pc < GUEST_HI) {  // (the game's own code on the CPU: its registers are the signal's, not the Cpu's)
        extern Cpu *kernel_current_cpu(void);  // (kernel.c)
        Cpu *c = kernel_current_cpu();
        if (c) {
            memcpy(c->x, uc->uc_mcontext.regs, 31 * sizeof c->x[0]);
            c->sp = uc->uc_mcontext.sp;
            c->pc = pc;
        }
    }
    rt_fault_report(what, fa, pc, lr);
}

void plat_crash_handler_install(void) {
    static __thread bool ss_installed;
    if (!ss_installed) {
        ss_installed = true;
        void *sp = malloc(128 * 1024);
        if (sp) {
            stack_t ss = {.ss_sp = sp, .ss_size = 128 * 1024};
            sigaltstack(&ss, NULL);
        }
    }
    static atomic_bool sa_installed;
    if (!atomic_exchange(&sa_installed, true)) {
        struct sigaction sa = {0};
        sa.sa_sigaction = crash_handler;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigaction(SIGSEGV, &sa, NULL);
        sigaction(SIGBUS, &sa, NULL);
        sigaction(SIGILL, &sa, NULL);
        sigaction(SIGFPE, &sa, NULL);
        sigaction(SIGABRT, &sa, NULL);
    }
}

// ---- child processes (none of the desktop's helpers exist on Android yet: docs/las/ANDROID-PORT.md, Fases 6-7)
#include <ftw.h>
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;

PlatProc plat_spawn(const char *const argv[], const char *log_path, bool append_log) {
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, log_path ? log_path : "/dev/null",
                                     O_WRONLY | O_CREAT | (append_log ? O_APPEND : O_TRUNC), 0644);
    posix_spawn_file_actions_adddup2(&fa, 1, 2);
    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&at, 0);
    pid_t pid;
    int rc = posix_spawn(&pid, argv[0], &fa, &at, (char *const *)argv, environ);
    posix_spawnattr_destroy(&at);
    posix_spawn_file_actions_destroy(&fa);
    return rc ? 0 : (PlatProc)pid;
}

void plat_proc_kill(PlatProc p) {
    kill(-(pid_t)p, SIGKILL);
    waitpid((pid_t)p, NULL, 0);
}

int plat_proc_wait(PlatProc p, bool block) {
    int st = 0;
    pid_t r = waitpid((pid_t)p, &st, block ? 0 : WNOHANG);
    if (r == 0) return -1;
    if (r < 0) return 255;
    return WIFEXITED(st) ? WEXITSTATUS(st) : WIFSIGNALED(st) ? 128 + WTERMSIG(st) : 255;  // (128+n: stopped by signal n)
}

static int rm_entry(const char *path, const struct stat *sb, int flag, struct FTW *ftw) {
    (void)sb, (void)flag, (void)ftw;
    return remove(path);
}
bool plat_remove_tree(const char *dir) { return nftw(dir, rm_entry, 16, FTW_DEPTH | FTW_PHYS) == 0; }

void plat_terminate(void) { _exit(0); }

// ---- device state for the performance overlay (runtime/ui/perf_overlay.mm)
#include <dirent.h>
#include <jni.h>
#include <stdatomic.h>

// What the activity reports once a second (BOTWRecompAndroidActivity.reportDeviceStats: public Android APIs; apps can't read
// the battery or most thermal sensors in /sys).
static pthread_mutex_t g_ds_lock = PTHREAD_MUTEX_INITIALIZER;
static PlatDeviceStats g_ds = {.batt_temp_c = -1, .batt_pct = -1, .thermal = -1, .headroom = -1};

JNIEXPORT void JNICALL Java_com_botwrecompandroid_BOTWRecompAndroidActivity_nativeDeviceStats(JNIEnv *env, jclass cls, jfloat batt_temp_c,
                                                                            jfloat batt_pct, jfloat watts, jint thermal,
                                                                            jfloat headroom) {
    (void)env, (void)cls;
    pthread_mutex_lock(&g_ds_lock);
    g_ds.batt_temp_c = batt_temp_c;
    g_ds.batt_pct = batt_pct;
    g_ds.watts = watts;
    g_ds.thermal = thermal;
    g_ds.headroom = headroom;
    pthread_mutex_unlock(&g_ds_lock);
    // LARECOMP_LOG_POWER=1 (a test, on battery only: on the charger the current is the charger's): the power draw each
    // second with the CPU clusters' clocks, this process's CPU share and the A/B side (LARECOMP_AB)
    static int log_power = -1;
    if (log_power < 0) log_power = getenv("LARECOMP_LOG_POWER") != NULL;
    if (log_power) {
        extern int vk_ab(const char *what);
        extern uint64_t renderer_present_count(void);
        PlatDeviceStats s;
        plat_device_stats(&s);
        char mhz[64] = {0};
        for (int i = 0, n = 0; i < s.ncluster && n < (int)sizeof mhz - 8; i++)
            n += snprintf(mhz + n, sizeof mhz - n, "%s%u", i ? "/" : "", s.cluster_mhz[i]);
        LOG("[power] %.2f W battery %.1f C soc %.1f C cpu %.0f%% mhz %s thermal %d headroom %.2f frame %llu ab %d\n", watts,
            batt_temp_c, s.soc_temp_c, s.cpu_pct, mhz, thermal, headroom, (unsigned long long)renderer_present_count(), vk_ab(NULL));
    }
}

static uint64_t read_u64(const char *path) {
    char buf[64] = {0};
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    return n > 0 ? strtoull(buf, NULL, 10) : 0;
}

// The CPU clusters (cpufreq policies) and the thermal zones an app may read, found once.
static char g_policy[8][96];
static int g_npolicy = -1;
static char g_tzone[96];  // a readable SoC / CPU temperature (most aren't readable by apps)

static void find_sources(void) {
    if (g_npolicy >= 0) return;
    g_npolicy = 0;
    DIR *d = opendir("/sys/devices/system/cpu/cpufreq");
    if (d) {
        char names[8][32];
        int n = 0;
        for (struct dirent *e; (e = readdir(d)) && n < 8;)
            if (!strncmp(e->d_name, "policy", 6)) snprintf(names[n++], sizeof names[0], "%s", e->d_name);
        closedir(d);
        for (int i = 0; i < n; i++)  // (in core order: policy0, policy2, policy6 ...)
            for (int j = i + 1; j < n; j++)
                if (atoi(names[j] + 6) < atoi(names[i] + 6)) { char t[32]; memcpy(t, names[i], 32); memcpy(names[i], names[j], 32); memcpy(names[j], t, 32); }
        for (int i = 0; i < n; i++)
            snprintf(g_policy[g_npolicy++], sizeof g_policy[0], "/sys/devices/system/cpu/cpufreq/%s/scaling_cur_freq", names[i]);
    }
    int best = -1;  // (prefer CPU / SoC zones; any readable one beats none)
    for (int z = 0; z < 128; z++) {
        char tp[96], type[64] = {0}, path[96];
        snprintf(tp, sizeof tp, "/sys/class/thermal/thermal_zone%d/type", z);
        int fd = open(tp, O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        ssize_t n = read(fd, type, sizeof type - 1);
        close(fd);
        if (n <= 0) continue;
        snprintf(path, sizeof path, "/sys/class/thermal/thermal_zone%d/temp", z);
        uint64_t v = read_u64(path);
        if (!v || v > 200000) continue;
        int score = strstr(type, "cpu") ? 3 : strstr(type, "soc") || strstr(type, "aoss") ? 2 : 1;
        if (score > best) { best = score; snprintf(g_tzone, sizeof g_tzone, "%s", path); }
    }
    fprintf(stderr, "[perf] %d CPU clusters, temperature from %s\n", g_npolicy, g_tzone[0] ? g_tzone : "(none readable)");
}

void plat_device_stats(PlatDeviceStats *out) {
    plat_refresh_fast_affinity();
    find_sources();
    pthread_mutex_lock(&g_ds_lock);
    *out = g_ds;
    pthread_mutex_unlock(&g_ds_lock);
    out->ncluster = g_npolicy;
    for (int i = 0; i < g_npolicy; i++) out->cluster_mhz[i] = (uint32_t)(read_u64(g_policy[i]) / 1000);
    out->soc_temp_c = g_tzone[0] ? (float)read_u64(g_tzone) / 1000.0f : -1;
    // this process's CPU time since the last call, as a share of every core
    static uint64_t last_ticks, last_ns;
    char buf[1024] = {0};
    int fd = open("/proc/self/stat", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        ssize_t n = read(fd, buf, sizeof buf - 1);
        close(fd);
        const char *p = n > 0 ? strrchr(buf, ')') : NULL;  // (fields after the command name: utime = 14th, stime = 15th)
        unsigned long long ut = 0, st = 0;
        if (p && sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &ut, &st) == 2) {
            uint64_t ticks = ut + st, ns = plat_now_ns();
            long hz = sysconf(_SC_CLK_TCK), cores = sysconf(_SC_NPROCESSORS_ONLN);
            if (last_ns && ns > last_ns && hz > 0 && cores > 0)
                out->cpu_pct = (float)((double)(ticks - last_ticks) / hz / ((ns - last_ns) / 1e9) / cores * 100.0);
            last_ticks = ticks, last_ns = ns;
        }
    }
    // memory: this process's resident set, and what the system has available
    out->rss_mb = out->avail_mb = 0;
    FILE *f = fopen("/proc/self/status", "re");
    if (f) {
        char line[256];
        while (fgets(line, sizeof line, f)) if (!strncmp(line, "VmRSS:", 6)) out->rss_mb = (uint32_t)(atoll(line + 6) / 1024);
        fclose(f);
    }
    if ((f = fopen("/proc/meminfo", "re"))) {
        char line[256];
        while (fgets(line, sizeof line, f)) if (!strncmp(line, "MemAvailable:", 13)) out->avail_mb = (uint32_t)(atoll(line + 13) / 1024);
        fclose(f);
    }
}

// ---- ADPF (Android Dynamic Performance Framework, API 33+): the presenting thread's work per frame against its
// target. Android's governor otherwise keeps a thread that works ~65% of each 60 fps frame on a middling clock (or
// core), and a frame with more of the game's work than usual (2-3x in places) overran: the overworld fell back to its
// 30 fps mode. Loaded at run time (minSdk 30). LARECOMP_NO_ADPF=1: off.
typedef struct APerformanceHintManager APerformanceHintManager;
typedef struct APerformanceHintSession APerformanceHintSession;

#ifndef SYS_sched_setattr
#define SYS_sched_setattr __NR_sched_setattr
#endif
#ifndef SYS_gettid
#define SYS_gettid __NR_gettid
#endif
#ifndef SCHED_ATTR_SIZE_VER1
#define SCHED_ATTR_SIZE_VER1 56
#endif
#ifndef SCHED_FLAG_KEEP_POLICY
#define SCHED_FLAG_KEEP_POLICY 0x08
#endif
#ifndef SCHED_FLAG_KEEP_PARAMS
#define SCHED_FLAG_KEEP_PARAMS 0x10
#endif
#ifndef SCHED_FLAG_UTIL_CLAMP_MIN
#define SCHED_FLAG_UTIL_CLAMP_MIN 0x20
#endif

// 3-cluster / big.LITTLE CPU topology state:
// Hot game threads (t80 main + guest workers), presenting thread, render thread, and shader workers are pinned
// together onto Big/Prime cores (> min_mhz, e.g. CPU 4..7 on SD855 4L+3B+1P) so they share uniform high clocks
// without creating a main-vs-worker speed race, while audio/vsync/disk/watch threads stay on Little cores (0..3).
// LARECOMP_PIN=0 disables cluster pinning.
static cpu_set_t g_all_cores;
static cpu_set_t g_fast_cores;
static cpu_set_t g_gold_cores;
static cpu_set_t g_prime_cores;
static cpu_set_t g_active_fast_cores;
static cpu_set_t g_little_cores;
static pthread_once_t g_topo_once = PTHREAD_ONCE_INIT;
static bool g_has_clusters;
static bool g_pin_enabled;
static int g_ncores_conf;
static int g_gold_cnt, g_prime_cnt;
static uint64_t g_fast_core_mask, g_gold_core_mask, g_prime_core_mask, g_active_fast_mask;
static bool g_prime_avoided;
static bool g_all_fallback;
static pthread_mutex_t g_fast_tid_lock = PTHREAD_MUTEX_INITIALIZER;
static int32_t g_fast_tids[64];
static int g_fast_ntids;

static long read_cpu_long(const char *path) {
    FILE *f = fopen(path, "re");
    if (!f) return -1;
    long v = -1;
    if (fscanf(f, "%ld", &v) != 1) v = -1;
    fclose(f);
    return v;
}

static bool cpu_is_online(int c) {
    char path[96];
    snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/online", c);
    long v = read_cpu_long(path);
    return v != 0;  // cpu0 or kernels without 'online' node count as online
}

static void init_cpu_topology(void) {
    const char *pin_env = getenv("LARECOMP_PIN");
    g_pin_enabled = !pin_env || atoi(pin_env) != 0;

    if (sched_getaffinity(0, sizeof g_all_cores, &g_all_cores) != 0) {
        CPU_ZERO(&g_all_cores);
        for (int c = 0; c < 8; c++) CPU_SET(c, &g_all_cores);
    }

    long nconf = sysconf(_SC_NPROCESSORS_CONF);
    if (nconf < 8) nconf = 8;
    if (nconf > 64) nconf = 64;
    if (nconf > CPU_SETSIZE) nconf = CPU_SETSIZE;
    g_ncores_conf = (int)nconf;

    long mhz[64] = {0};
    // Probe policy*/related_cpus first so hotplugged-offline cores are still classified accurately.
    DIR *pd = opendir("/sys/devices/system/cpu/cpufreq");
    if (pd) {
        for (struct dirent *e; (e = readdir(pd));) {
            if (strncmp(e->d_name, "policy", 6) != 0) continue;
            char pmax[128], prel[128];
            snprintf(pmax, sizeof pmax, "/sys/devices/system/cpu/cpufreq/%s/cpuinfo_max_freq", e->d_name);
            long pm = read_cpu_long(pmax);
            if (pm <= 0) continue;
            for (int kind = 0; kind < 2; kind++) {
                snprintf(prel, sizeof prel, "/sys/devices/system/cpu/cpufreq/%s/%s",
                         e->d_name, kind == 0 ? "related_cpus" : "affected_cpus");
                FILE *rf = fopen(prel, "re");
                if (!rf) continue;
                int cpu = -1;
                while (fscanf(rf, "%d", &cpu) == 1) {
                    if (cpu >= 0 && cpu < 64 && mhz[cpu] <= 0) mhz[cpu] = pm;
                }
                fclose(rf);
                break;
            }
        }
        closedir(pd);
    }

    long min_mhz = 0, max_mhz = 0;
    int n = 0;
    for (int c = 0; c < g_ncores_conf; c++) {
        if (mhz[c] <= 0) {
            char path[96];
            snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", c);
            long v = read_cpu_long(path);
            if (v > 0) mhz[c] = v;
        }
        if (mhz[c] > 0) {
            if (!min_mhz || mhz[c] < min_mhz) min_mhz = mhz[c];
            if (mhz[c] > max_mhz) max_mhz = mhz[c];
            n = c + 1;
        }
    }

    CPU_ZERO(&g_fast_cores);
    CPU_ZERO(&g_gold_cores);
    CPU_ZERO(&g_prime_cores);
    CPU_ZERO(&g_little_cores);
    int fast_cnt = 0, little_cnt = 0, gold_cnt = 0, prime_cnt = 0;
    uint64_t fast_mask = 0, little_mask = 0, gold_mask = 0, prime_mask = 0;
    if (min_mhz > 0 && max_mhz > min_mhz) {
        for (int c = 0; c < n; c++) {
            if (mhz[c] <= 0) continue;
            if (mhz[c] > min_mhz) {
                CPU_SET(c, &g_fast_cores);
                fast_cnt++;
                fast_mask |= (1ull << c);
                if (mhz[c] < max_mhz) {
                    CPU_SET(c, &g_gold_cores);
                    gold_cnt++;
                    gold_mask |= (1ull << c);
                }
            } else {
                CPU_SET(c, &g_little_cores);
                little_cnt++;
                little_mask |= (1ull << c);
            }
        }
        if (gold_cnt > 0) {
            for (int c = 0; c < n; c++) {
                if (mhz[c] == max_mhz) {
                    CPU_SET(c, &g_prime_cores);
                    prime_cnt++;
                    prime_mask |= (1ull << c);
                }
            }
        } else {
            g_gold_cores = g_fast_cores;
            gold_cnt = fast_cnt;
            gold_mask = fast_mask;
        }
    }

    g_gold_cnt = gold_cnt;
    g_prime_cnt = prime_cnt;
    g_fast_core_mask = fast_mask;
    g_gold_core_mask = gold_mask;
    g_prime_core_mask = prime_mask;
    g_active_fast_cores = g_fast_cores;
    g_active_fast_mask = fast_mask;
    g_has_clusters = (fast_cnt > 0 && little_cnt > 0);
    LOG("[android] CPU topology (%d cores): fast=0x%llx (%d cores, gold=0x%llx prime=0x%llx, >%ld MHz), little=0x%llx (%d cores, %ld MHz), pin=%s\n",
        n, (unsigned long long)fast_mask, fast_cnt,
        (unsigned long long)gold_mask, (unsigned long long)prime_mask, min_mhz / 1000,
        (unsigned long long)little_mask, little_cnt, min_mhz / 1000,
        (g_pin_enabled && g_has_clusters) ? "on" : "off");
}

static void register_fast_tid(int32_t tid) {
    if (tid <= 0) return;
    pthread_mutex_lock(&g_fast_tid_lock);
    bool seen = false;
    for (int i = 0; i < g_fast_ntids; i++) {
        if (g_fast_tids[i] == tid) { seen = true; break; }
    }
    if (!seen && g_fast_ntids < (int)(sizeof(g_fast_tids) / sizeof(g_fast_tids[0])))
        g_fast_tids[g_fast_ntids++] = tid;
    pthread_mutex_unlock(&g_fast_tid_lock);
}

void plat_refresh_fast_affinity(void) {
    pthread_once(&g_topo_once, init_cpu_topology);
    if (!g_pin_enabled || !g_has_clusters) return;

    static atomic_uint_fast64_t next_check_ns;
    uint64_t now = plat_now_ns();
    uint64_t due = atomic_load_explicit(&next_check_ns, memory_order_relaxed);
    if (now < due) return;
    if (!atomic_compare_exchange_strong_explicit(&next_check_ns, &due, now + 500000000ull,
                                                 memory_order_relaxed, memory_order_relaxed))
        return;

    long little_cur_max = 0, fast_cur_max = 0;
    long gold_cur_max = 0, gold_cap_max = 0, prime_cur_max = 0, prime_cap_max = 0;
    int gold_on = 0, prime_on = 0;
    cpu_set_t online_fast, online_gold, online_all;
    CPU_ZERO(&online_fast);
    CPU_ZERO(&online_gold);
    CPU_ZERO(&online_all);
    uint64_t online_fast_mask = 0, online_gold_mask = 0, online_all_mask = 0;

    for (int c = 0; c < g_ncores_conf; c++) {
        if (!cpu_is_online(c)) continue;
        CPU_SET(c, &online_all);
        if (c < 64) online_all_mask |= (1ull << c);

        char pcur[96], pcap[96];
        snprintf(pcur, sizeof pcur, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", c);
        snprintf(pcap, sizeof pcap, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_max_freq", c);
        long cur = read_cpu_long(pcur);

        if (CPU_ISSET(c, &g_little_cores) || (c < 4 && !CPU_ISSET(c, &g_fast_cores))) {
            if (cur > little_cur_max) little_cur_max = cur;
        }
        if (!CPU_ISSET(c, &g_fast_cores)) continue;

        long cap = read_cpu_long(pcap);
        CPU_SET(c, &online_fast);
        if (c < 64) online_fast_mask |= (1ull << c);
        if (cur > fast_cur_max) fast_cur_max = cur;

        if (CPU_ISSET(c, &g_prime_cores)) {
            prime_on++;
            if (cur > prime_cur_max) prime_cur_max = cur;
            if (cap > prime_cap_max) prime_cap_max = cap;
        } else if (CPU_ISSET(c, &g_gold_cores)) {
            gold_on++;
            CPU_SET(c, &online_gold);
            if (c < 64) online_gold_mask |= (1ull << c);
            if (cur > gold_cur_max) gold_cur_max = cur;
            if (cap > gold_cap_max) gold_cap_max = cap;
        }
    }

    bool avoid_prime = false;
    if (g_prime_cnt > 0 && gold_on > 0) {
        if (prime_on == 0) avoid_prime = true;
        else if (prime_cap_max > 0 && gold_cap_max > 0 && prime_cap_max + 150000 < gold_cap_max) avoid_prime = true;
        else if (prime_cur_max > 0 && gold_cur_max > 0 && prime_cur_max + 200000 < gold_cur_max && prime_cur_max < 1500000)
            avoid_prime = true;
    }

    bool fallback_all = (fast_cur_max > 0 && little_cur_max > 0 && fast_cur_max < little_cur_max);

    cpu_set_t target;
    uint64_t target_mask;
    if (fallback_all) {
        target = online_all_mask ? online_all : g_all_cores;
        target_mask = online_all_mask ? online_all_mask : 0xffull;
    } else if (avoid_prime) {
        target = online_gold_mask ? online_gold : g_gold_cores;
        target_mask = online_gold_mask ? online_gold_mask : g_gold_core_mask;
    } else {
        target = online_fast_mask ? online_fast : g_fast_cores;
        target_mask = online_fast_mask ? online_fast_mask : g_fast_core_mask;
    }

    pthread_mutex_lock(&g_fast_tid_lock);
    bool changed = (avoid_prime != g_prime_avoided) || (fallback_all != g_all_fallback) || (target_mask != g_active_fast_mask);
    g_active_fast_cores = target;
    g_active_fast_mask = target_mask;
    g_prime_avoided = avoid_prime;
    g_all_fallback = fallback_all;
    if (changed && target_mask) {
        int alive = 0;
        for (int i = 0; i < g_fast_ntids; i++) {
            pid_t tid = (pid_t)g_fast_tids[i];
            errno = 0;
            if (sched_setaffinity(tid, sizeof target, &target) != 0 && errno == ESRCH) continue;
            g_fast_tids[alive++] = g_fast_tids[i];
        }
        g_fast_ntids = alive;
        LOG("[android] thermal-aware affinity: %s (fast=%ld MHz, little=%ld MHz, gold=%ld/%ld MHz, prime=%ld/%ld MHz, mask=0x%llx, threads=%d)\n",
            fallback_all ? "thermal throttle (fast < little) -> all cores (0xff)"
                         : (avoid_prime ? "avoiding throttled prime -> gold cluster" : "all fast cores active"),
            fast_cur_max / 1000, little_cur_max / 1000,
            gold_cur_max / 1000, gold_cap_max / 1000,
            prime_cur_max / 1000, prime_cap_max / 1000,
            (unsigned long long)target_mask, alive);
    }
    pthread_mutex_unlock(&g_fast_tid_lock);
}

static int read_sys_uclamp_min_max(void) {
    static atomic_int cached_sys_max = -1;
    int v = atomic_load(&cached_sys_max);
    if (v >= 0) return v;
    int res = 1024;
    FILE *f = fopen("/proc/sys/kernel/sched_util_clamp_min", "re");
    if (f) {
        int val = 0;
        if (fscanf(f, "%d", &val) == 1 && val > 0 && val <= 1024) res = val;
        fclose(f);
    }
    atomic_store(&cached_sys_max, res);
    return res;
}

static int apply_nice_fallback(pid_t target_tid) {
    static const int prios[] = {-10, -8, -4, -2};
    for (size_t i = 0; i < sizeof(prios) / sizeof(prios[0]); i++) {
        errno = 0;
        if (setpriority(PRIO_PROCESS, (id_t)target_tid, prios[i]) == 0) return prios[i];
    }
    errno = 0;
    int cur = getpriority(PRIO_PROCESS, (id_t)target_tid);
    return errno == 0 ? cur : 0;
}

void plat_boost_thread_tid(int32_t tid) {
    pid_t target_tid = tid > 0 ? (pid_t)tid : (pid_t)syscall(SYS_gettid);
    apply_nice_fallback(target_tid);
    plat_uclamp_min_tid((int32_t)target_tid);
}

// Per-TID sched_setattr(SCHED_FLAG_UTIL_CLAMP_MIN) with kernel cap read from /proc/sys/kernel/sched_util_clamp_min.
// Default 768 (75% of 1024) forces CPU clusters onto higher OPPs even when ADPF is unavailable (e.g. Android < 12).
// On older 4.14 kernels where sched_attr lacks uclamp (E2BIG) or seccomp blocks sched_setattr, falls back cleanly to
// per-TID setpriority() nice boost without spamming "Argument list too long".
void plat_uclamp_min_tid(int32_t tid) {
    static atomic_bool unsupported;
    static atomic_int accepted_want;
    static atomic_int want_cached = -1;

    int want = atomic_load(&want_cached);
    if (want < 0) {
        const char *e = getenv("LARECOMP_UCLAMP_MIN");
        int raw = e ? atoi(e) : 768;
        if (raw < 0) raw = 0;
        if (raw > 1024) raw = 1024;
        int sys_max = read_sys_uclamp_min_max();
        if (sys_max > 0 && raw > sys_max) raw = sys_max;
        want = raw;
        atomic_store(&want_cached, want);
    }
    if (!want) return;

    pid_t target_tid = tid > 0 ? (pid_t)tid : (pid_t)syscall(SYS_gettid);
    if (atomic_load(&unsupported)) {
        apply_nice_fallback(target_tid);
        return;
    }

    int cached = atomic_load(&accepted_want);
    int tries[5] = {cached > 0 ? cached : want, 563, 512, 384, 256};
    int ntries = cached > 0 ? 1 : 5;
    long r = -1;
    int applied = want;
    int last_err = 0;
    for (int i = 0; i < ntries; i++) {
        int val = tries[i];
        if (i > 0 && val >= want) continue;
        struct {
            uint32_t size;
            uint32_t sched_policy;
            uint64_t sched_flags;
            int32_t sched_nice;
            uint32_t sched_priority;
            uint64_t sched_runtime;
            uint64_t sched_deadline;
            uint64_t sched_period;
            uint32_t sched_util_min;
            uint32_t sched_util_max;
        } attr;
        memset(&attr, 0, sizeof attr);
        attr.size = SCHED_ATTR_SIZE_VER1;
        attr.sched_flags = SCHED_FLAG_KEEP_POLICY | SCHED_FLAG_KEEP_PARAMS | SCHED_FLAG_UTIL_CLAMP_MIN;
        attr.sched_util_min = (uint32_t)val;

        errno = 0;
        r = syscall(SYS_sched_setattr, target_tid, &attr, 0);
        last_err = errno;
        if (r != 0 && last_err == EINVAL) {
            // Some backported kernels support SCHED_FLAG_UTIL_CLAMP_MIN without KEEP_POLICY | KEEP_PARAMS
            int pol = sched_getscheduler(target_tid);
            if (pol < 0) pol = SCHED_OTHER;
            errno = 0;
            int cur_nice = getpriority(PRIO_PROCESS, (id_t)target_tid);
            if (errno != 0) cur_nice = 0;
            struct sched_param sp;
            memset(&sp, 0, sizeof sp);
            if (pol != SCHED_OTHER && pol != SCHED_BATCH && pol != SCHED_IDLE)
                sched_getparam(target_tid, &sp);
            memset(&attr, 0, sizeof attr);
            attr.size = SCHED_ATTR_SIZE_VER1;
            attr.sched_policy = (uint32_t)pol;
            attr.sched_flags = SCHED_FLAG_UTIL_CLAMP_MIN;
            attr.sched_nice = cur_nice;
            attr.sched_priority = (uint32_t)sp.sched_priority;
            attr.sched_util_min = (uint32_t)val;
            errno = 0;
            r = syscall(SYS_sched_setattr, target_tid, &attr, 0);
            last_err = errno;
        }
        if (r == 0) {
            applied = val;
            if (cached <= 0) atomic_store(&accepted_want, val);
            break;
        }
        if (last_err != E2BIG) break;
    }

    int nice_applied = 0;
    if (r != 0) {
        if (last_err == E2BIG || last_err == ENOSYS || last_err == EINVAL || last_err == EPERM || last_err == EACCES)
            atomic_store(&unsupported, true);
        nice_applied = apply_nice_fallback(target_tid);
    }

    static atomic_int logged;
    if (!atomic_exchange(&logged, 1)) {
        if (r == 0) {
            LOG("[android] uclamp.min %d (req %d, sys_max %d) via sched_setattr: ok\n",
                applied, want, read_sys_uclamp_min_max());
        } else if (last_err == E2BIG || last_err == ENOSYS) {
            LOG("[android] uclamp.min unavailable in kernel sched_setattr (errno=%d); using per-TID nice boost (%d)\n",
                last_err, nice_applied);
        } else {
            LOG("[android] uclamp.min %d rejected (errno=%d: %s); nice boost=%d\n",
                want, last_err, strerror(last_err), nice_applied);
        }
    }
}

void plat_thread_pin_fast(void) {
    pthread_once(&g_topo_once, init_cpu_topology);
    if (g_pin_enabled && g_has_clusters) {
        register_fast_tid((int32_t)syscall(SYS_gettid));
        plat_refresh_fast_affinity();
        pthread_mutex_lock(&g_fast_tid_lock);
        cpu_set_t mask = g_active_fast_cores;
        pthread_mutex_unlock(&g_fast_tid_lock);
        sched_setaffinity(0, sizeof mask, &mask);
    }
    plat_boost_thread_tid(0);
}

void plat_thread_pin_background(void) {
    pthread_once(&g_topo_once, init_cpu_topology);
    if (g_pin_enabled && g_has_clusters) sched_setaffinity(0, sizeof g_little_cores, &g_little_cores);
}

void plat_thread_unpin(void) {
    plat_thread_pin_background();
}

// One session for both threads by default (the presenting thread and the render thread; LARECOMP_ADPF_SPLIT=1 for
// two separate sessions). The presenting thread reports the frame's critical path (max of both threads' busy times).
typedef struct AWorkDuration AWorkDuration;
static int loaded;  // 0 untried, 1 on, -1 off, -2 disabled
static int s_split = -1;
static int s_errors, s_last_error, s_recreated;
static int32_t s_presenting_tid, s_render_tid;
static int s_session_threads;
static float s_target_ms, s_work_ms, s_game_ms, s_render_ms;
static int s_game_cpu = -1, s_render_cpu = -1;

static pthread_mutex_t adpf_lock = PTHREAD_MUTEX_INITIALIZER;

// Caller holds adpf_lock, including initialization and session replacement.
static void adpf_work_locked(int slot, uint64_t work_ns, uint64_t target_ns, uint64_t gpu_ns, uint64_t game_ns) {
    static APerformanceHintManager *(*get_manager)(void);
    static APerformanceHintSession *(*create)(APerformanceHintManager *, const int32_t *, size_t, int64_t);
    static int (*report)(APerformanceHintSession *, int64_t);
    static int (*retarget)(APerformanceHintSession *, int64_t);
    static int (*set_threads)(APerformanceHintSession *, const int32_t *, size_t);
    static void (*close_session)(APerformanceHintSession *);
    static int state[2];  // per slot: 0 untried, 1 on, -1 off
    static APerformanceHintSession *session[2];
    static uint64_t target[2];
    // (API 35+: a work duration with the GPU's time - the governor then raises the GPU's clock too)
    static AWorkDuration *(*wd_create)(void);
    static void (*wd_start)(AWorkDuration *, int64_t), (*wd_total)(AWorkDuration *, int64_t), (*wd_cpu)(AWorkDuration *, int64_t),
        (*wd_gpu)(AWorkDuration *, int64_t);
    static int (*report2)(APerformanceHintSession *, AWorkDuration *);
    static AWorkDuration *wd;
    // Always apply fast-core pinning, per-TID uclamp/nice boost, and CPU telemetry even if ADPF is unavailable.
    static uint64_t s_last_render_ns;
    static bool s_uclamp_applied[2];
    if (!s_uclamp_applied[slot & 1]) {
        s_uclamp_applied[slot & 1] = true;
        plat_thread_pin_fast();
    }
    plat_refresh_fast_affinity();
    if (slot == 0) {  // Presenting thread (game's main loop)
        s_presenting_tid = (int32_t)gettid();
        register_fast_tid(s_presenting_tid);
        s_game_cpu = sched_getcpu();
        if (s_last_render_ns > work_ns) work_ns = s_last_render_ns;
        float gms = (float)(game_ns ? game_ns : work_ns) / 1e6f;
        s_game_ms = s_game_ms > 0 ? s_game_ms * 0.9f + gms * 0.1f : gms;
        float wms = (float)work_ns / 1e6f;
        if (s_render_ms > wms) wms = s_render_ms;
        s_work_ms = wms;
        s_target_ms = (float)target_ns / 1e6f;
    } else {          // Render thread (Vulkan command translation)
        s_render_tid = (int32_t)gettid();
        register_fast_tid(s_render_tid);
        s_render_cpu = sched_getcpu();
        s_last_render_ns = work_ns;
        float rms = (float)work_ns / 1e6f;
        s_render_ms = s_render_ms > 0 ? s_render_ms * 0.9f + rms * 0.1f : rms;
        if (rms > s_work_ms) s_work_ms = rms;
        if (s_target_ms <= 0.0f) s_target_ms = (float)target_ns / 1e6f;
    }

    if (loaded < 0 || state[slot] < 0) return;
    if (!loaded) {
        if (getenv("LARECOMP_NO_ADPF")) { loaded = -2; return; }
        loaded = -1;
        void *lib = dlopen("libandroid.so", RTLD_NOW);
        if (!lib) return;
        get_manager = (APerformanceHintManager * (*)(void)) dlsym(lib, "APerformanceHint_getManager");
        create = (APerformanceHintSession * (*)(APerformanceHintManager *, const int32_t *, size_t, int64_t))
            dlsym(lib, "APerformanceHint_createSession");
        report = (int (*)(APerformanceHintSession *, int64_t))dlsym(lib, "APerformanceHint_reportActualWorkDuration");
        retarget = (int (*)(APerformanceHintSession *, int64_t))dlsym(lib, "APerformanceHint_updateTargetWorkDuration");
        set_threads = (int (*)(APerformanceHintSession *, const int32_t *, size_t))dlsym(lib, "APerformanceHint_setThreads");
        close_session = (void (*)(APerformanceHintSession *))dlsym(lib, "APerformanceHint_closeSession");
        if (!get_manager || !create || !report || !retarget) return;
        wd_create = (AWorkDuration * (*)(void)) dlsym(lib, "AWorkDuration_create");
        wd_start = (void (*)(AWorkDuration *, int64_t))dlsym(lib, "AWorkDuration_setWorkPeriodStartTimestampNanos");
        wd_total = (void (*)(AWorkDuration *, int64_t))dlsym(lib, "AWorkDuration_setActualTotalDurationNanos");
        wd_cpu = (void (*)(AWorkDuration *, int64_t))dlsym(lib, "AWorkDuration_setActualCpuDurationNanos");
        wd_gpu = (void (*)(AWorkDuration *, int64_t))dlsym(lib, "AWorkDuration_setActualGpuDurationNanos");
        report2 = (int (*)(APerformanceHintSession *, AWorkDuration *))dlsym(lib, "APerformanceHint_reportActualWorkDuration2");
        if (wd_create && wd_start && wd_total && wd_cpu && wd_gpu && report2) wd = wd_create();
        LOG("[android] performance hints with the GPU's time: %s\n", wd ? "yes" : "no (API < 35)");
        loaded = 1;
    }
    if (s_split < 0) s_split = getenv("LARECOMP_ADPF_SPLIT") != NULL;

    // Target duration adjustment: default 20% margin (headroom for the governor to ramp clocks up before missing
    // vsync, target ~13.3 ms for 60 fps; LARECOMP_ADPF_MARGIN=n to change, =0 to disable; LARECOMP_ADPF_TARGET_US=n)
    static int target_override_us = -1, margin_pct = -1;
    if (target_override_us < 0) {
        const char *to = getenv("LARECOMP_ADPF_TARGET_US");
        target_override_us = to ? atoi(to) : 0;
        const char *mp = getenv("LARECOMP_ADPF_MARGIN");
        margin_pct = mp ? atoi(mp) : 20;
    }
    if (target_override_us > 0) target_ns = (uint64_t)target_override_us * 1000ull;
    else if (margin_pct > 0 && margin_pct < 100) target_ns = target_ns * (100 - margin_pct) / 100;

    if (slot == 0) s_target_ms = (float)target_ns / 1e6f;

    APerformanceHintManager *m = get_manager();
    if (!m) return;

    static uint64_t last_report_ns[2];
    if (!s_split) {
        // --- Single session mode (both threads in session[0]) ---
        static bool s_tried_add_render;
        if (state[0] < 0) return;
        if (!session[0]) {
            int32_t tids[2];
            int ntids = 0;
            if (s_presenting_tid > 0) tids[ntids++] = s_presenting_tid;
            if (s_render_tid > 0) tids[ntids++] = s_render_tid;
            if (ntids > 0) {
                // Mark failed before create() so unsupported devices never retry create() and leak Binder proxies
                state[0] = -1;
                session[0] = create(m, tids, (size_t)ntids, (int64_t)target_ns);
                if (session[0]) {
                    s_session_threads = ntids;
                    target[0] = target_ns;
                    state[0] = 1;
                    LOG("[android] performance hint single session (%d thread%s): on\n", ntids, ntids > 1 ? "s" : "");
                } else {
                    state[1] = -1;
                    loaded = -1;  // Disable ADPF cleanly so overlay never reports "ADPF on (0 thr)"
                    s_errors++;
                    s_last_error = errno;
                    if (s_presenting_tid > 0) plat_boost_thread_tid(s_presenting_tid);
                    if (s_render_tid > 0) plat_boost_thread_tid(s_render_tid);
                    LOG("[android] performance hint single session: not available (%s); nice -10 fallback active\n",
                        strerror(errno));
                }
            }
        } else if (!s_tried_add_render && s_session_threads < 2 && s_presenting_tid > 0 && s_render_tid > 0) {
            // Try adding the second thread to the active session once
            s_tried_add_render = true;
            int32_t tids[2] = {s_presenting_tid, s_render_tid};
            int ok = 0;
            if (set_threads) {
                int r = set_threads(session[0], tids, 2);
                if (r == 0) { ok = 1; s_session_threads = 2; }
                else { s_errors++; s_last_error = r; }
            }
            if (!ok) {
                if (close_session) close_session(session[0]);
                session[0] = create(m, tids, 2, (int64_t)target_ns);
                if (session[0]) {
                    s_session_threads = 2;
                    s_recreated++;
                    LOG("[android] performance hint session recreated with 2 threads: on\n");
                } else {
                    state[0] = state[1] = -1;
                    loaded = -1;
                    s_errors++;
                    s_last_error = errno;
                    if (s_presenting_tid > 0) plat_boost_thread_tid(s_presenting_tid);
                    if (s_render_tid > 0) plat_boost_thread_tid(s_render_tid);
                }
            }
        }
        if (!session[0]) return;
        // Allow render thread (slot 1) to report directly when its work exceeds target_ns so cpu 6 stays boosted
        if (slot != 0 && work_ns <= target_ns) return;
        // Rate limit ADPF Binder IPC to <= 125 Hz (>= 8 ms between reports)
        uint64_t now_ns = plat_now_ns();
        if (last_report_ns[0] && now_ns - last_report_ns[0] < 8000000ull) return;
        last_report_ns[0] = now_ns;

        if (target_ns != target[0]) {
            int r = retarget(session[0], (int64_t)target_ns);
            if (r != 0) { s_errors++; s_last_error = r; }
            target[0] = target_ns;
        }
        if (gpu_ns && wd) {
            int64_t now = (int64_t)plat_now_ns(), cpu = (int64_t)(work_ns ? work_ns : 1), gpu = (int64_t)gpu_ns;
            wd_start(wd, now - (cpu > gpu ? cpu : gpu));
            wd_total(wd, cpu > gpu ? cpu : gpu);
            wd_cpu(wd, cpu);
            wd_gpu(wd, gpu);
            int r = report2(session[0], wd);
            if (r != 0) {
                s_errors++;
                s_last_error = r;
                static bool told;
                if (!told) { told = true; LOG("[android] performance hint with GPU: error %d\n", r); }
            } else {
                return;
            }
        }
        if (work_ns) {
            int r = report(session[0], (int64_t)work_ns);
            if (r != 0) {
                s_errors++;
                s_last_error = r;
                static bool told;
                if (!told) { told = true; LOG("[android] performance hint report error %d\n", r); }
            }
        }
    } else {
        // --- Split session mode (one session per thread) ---
        if (!state[slot]) {
            state[slot] = -1;
            int32_t tid = (int32_t)gettid();
            session[slot] = create(m, &tid, 1, (int64_t)target_ns);
            LOG("[android] performance hint session (%s thread): %s\n", slot ? "render" : "presenting",
                session[slot] ? "on" : "not available");
            if (!session[slot]) {
                s_errors++;
                s_last_error = errno;
                plat_boost_thread_tid(tid);
                if (!session[0] && !session[1] && state[0] < 0 && state[1] < 0) loaded = -1;
                return;
            }
            target[slot] = target_ns;
            state[slot] = 1;
            s_session_threads = (session[0] ? 1 : 0) + (session[1] ? 1 : 0);
        }
        if (!session[slot]) return;
        uint64_t now_ns = plat_now_ns();
        if (last_report_ns[slot] && now_ns - last_report_ns[slot] < 8000000ull) return;
        last_report_ns[slot] = now_ns;
        if (target_ns != target[slot]) {
            int r = retarget(session[slot], (int64_t)target_ns);
            if (r != 0) { s_errors++; s_last_error = r; }
            target[slot] = target_ns;
        }
        if (slot == 0 && gpu_ns && wd) {
            int64_t now = (int64_t)plat_now_ns(), cpu = (int64_t)(work_ns ? work_ns : 1), gpu = (int64_t)gpu_ns;
            wd_start(wd, now - (cpu > gpu ? cpu : gpu));
            wd_total(wd, cpu > gpu ? cpu : gpu);
            wd_cpu(wd, cpu);
            wd_gpu(wd, gpu);
            int r = report2(session[slot], wd);
            if (r != 0) { s_errors++; s_last_error = r; }
            else return;
        }
        if (work_ns) {
            int r = report(session[slot], (int64_t)work_ns);
            if (r != 0) { s_errors++; s_last_error = r; }
        }
    }
}

static void adpf_work(int slot, uint64_t work_ns, uint64_t target_ns, uint64_t gpu_ns, uint64_t game_ns) {
    pthread_mutex_lock(&adpf_lock);
    adpf_work_locked(slot, work_ns, target_ns, gpu_ns, game_ns);
    pthread_mutex_unlock(&adpf_lock);
}

void plat_frame_work(uint64_t work_ns, uint64_t target_ns) { adpf_work(0, work_ns, target_ns, 0, work_ns); }
void plat_frame_work_gpu(uint64_t cpu_ns, uint64_t gpu_ns, uint64_t target_ns, uint64_t game_ns) {
    adpf_work(0, cpu_ns, target_ns, gpu_ns, game_ns);
}
void plat_render_work(uint64_t work_ns, uint64_t target_ns) { adpf_work(1, work_ns, target_ns, 0, 0); }

void plat_adpf_stats(PlatAdpfStats *out) {
    if (!out) return;
    pthread_mutex_lock(&adpf_lock);
    memset(out, 0, sizeof(*out));
    out->state = loaded;
    out->sessions = s_split ? 2 : 1;
    out->threads = s_session_threads;
    out->errors = s_errors;
    out->last_error = s_last_error;
    out->recreated = s_recreated;
    out->target_ms = s_target_ms;
    out->work_ms = s_work_ms;
    out->game_ms = s_game_ms;
    out->render_ms = s_render_ms;
    out->game_cpu = s_game_cpu;
    out->render_cpu = s_render_cpu;
    pthread_mutex_unlock(&adpf_lock);
}
