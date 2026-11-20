#include "watch.h"

#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#include <time.h>

#include <atomic>
#include <vector>
#include <algorithm>
#include <cstring>
#include <cstdio>

namespace {

constexpr uintptr_t kPage = 4096;
constexpr uint8_t kWatched = 1;
constexpr uint8_t kOpen    = 2;

uintptr_t g_map_base = 0;
size_t    g_heap_size = 0;

std::atomic<uint32_t>* g_version = nullptr;
std::atomic<uint8_t>*  g_state = nullptr;

std::atomic<uint64_t> g_faults{0};
std::atomic<uint64_t> g_protects{0};
std::atomic<uint64_t> g_protect_ns{0};

std::atomic_flag g_lock = ATOMIC_FLAG_INIT;

struct SpinLock {
    SpinLock() {
        while (g_lock.test_and_set(std::memory_order_acquire)) {
#if defined(__aarch64__) || defined(__arm__)
            __asm__ volatile("yield" ::: "memory");
#endif
        }
    }
    ~SpinLock() {
        g_lock.clear(std::memory_order_release);
    }
};

bool g_handler_installed = false;
struct sigaction g_old_segv{};

inline int64_t heap_page_index(uintptr_t addr) {
    if (g_map_base && addr >= g_map_base && addr < g_map_base + g_heap_size) {
        return static_cast<int64_t>((addr - g_map_base) / kPage);
    }
    return -1;
}

inline uint64_t now_monotonic_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

// Menangani page fault penulisan CPU
bool on_write_fault(uintptr_t addr) {
    const int64_t page_idx = heap_page_index(addr);
    if (page_idx < 0) return false;

    SpinLock lock;
    const uint8_t s = g_state[page_idx].load(std::memory_order_relaxed);
    if (!(s & kWatched)) return false;

    void* aligned_page = reinterpret_cast<void*>(addr & ~(kPage - 1));
    if (mprotect(aligned_page, kPage, PROT_READ | PROT_WRITE) != 0) {
        return false;
    }

    g_version[page_idx].fetch_add(1, std::memory_order_relaxed);
    g_state[page_idx].store(s | kOpen, std::memory_order_relaxed);
    g_faults.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void segv_handler(int sig, siginfo_t* info, void* ctx) {
    if (info && (info->si_code == SEGV_ACCERR || info->si_code == SEGV_MAPERR)) {
        if (on_write_fault(reinterpret_cast<uintptr_t>(info->si_addr))) {
            return;
        }
    }

    if (g_old_segv.sa_flags & SA_SIGINFO) {
        if (g_old_segv.sa_sigaction) g_old_segv.sa_sigaction(sig, info, ctx);
    } else if (g_old_segv.sa_handler == SIG_DFL) {
        signal(sig, SIG_DFL);
        raise(sig);
    } else if (g_old_segv.sa_handler != SIG_IGN) {
        g_old_segv.sa_handler(sig);
    }
}

void install_segv_handler() {
    if (g_handler_installed) return;
    g_handler_installed = true;

    struct sigaction sa{};
    sa.sa_sigaction = segv_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &g_old_segv);
}

// Menggabungkan contiguous pages untuk mencegah VMA fragmentation
inline void add_contiguous_run(std::vector<std::pair<uintptr_t, size_t>>& runs, uintptr_t addr, size_t len = kPage) {
    if (!runs.empty() && runs.back().first + runs.back().second == addr) {
        runs.back().second += len;
    } else {
        runs.push_back({addr, len});
    }
}

void protect_runs_batch(const std::vector<std::pair<uintptr_t, size_t>>& runs) {
    if (runs.empty()) return;
    const uint64_t t0 = now_monotonic_ns();
    for (const auto& [addr, len] : runs) {
        mprotect(reinterpret_cast<void*>(addr), len, PROT_READ);
    }
    g_protect_ns.fetch_add(now_monotonic_ns() - t0, std::memory_order_relaxed);
    g_protects.fetch_add(runs.size(), std::memory_order_relaxed);
}

} // namespace

extern "C" {

void ryza_watch_init_heap(uintptr_t map_base, size_t size) {
    if (!size) return;
    g_map_base = map_base;
    g_heap_size = size;

    const size_t total_pages = (size + kPage - 1) / kPage;
    void* v_mem = mmap(nullptr, total_pages * sizeof(std::atomic<uint32_t>),
                       PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    void* s_mem = mmap(nullptr, total_pages * sizeof(std::atomic<uint8_t>),
                       PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);

    g_version = static_cast<std::atomic<uint32_t>*>(v_mem);
    g_state   = static_cast<std::atomic<uint8_t>*>(s_mem);
}

bool ryza_watch_is_watchable(uintptr_t address, size_t size) {
    return (g_map_base != 0) && (address >= g_map_base) && (address + size <= g_map_base + g_heap_size);
}

void ryza_watch_before_kernel_write(void* p, size_t size) {
    if (!size || !g_map_base || !g_handler_installed) return;
    const uintptr_t start = reinterpret_cast<uintptr_t>(p) & ~(kPage - 1);
    const uintptr_t end   = reinterpret_cast<uintptr_t>(p) + size;

    SpinLock lock;
    for (uintptr_t a = start; a < end; a += kPage) {
        const int64_t h = heap_page_index(a);
        if (h < 0) continue;
        const uint8_t s = g_state[h].load(std::memory_order_relaxed);
        if (s & kWatched) {
            mprotect(reinterpret_cast<void*>(a), kPage, PROT_READ | PROT_WRITE);
            g_version[h].fetch_add(1, std::memory_order_relaxed);
            g_state[h].store(s | kOpen, std::memory_order_relaxed);
        }
    }
}

void ryza_watch_collect(uintptr_t address, size_t size,
                        uint64_t* keys_in_out, size_t num_keys,
                        RyzaWatchRun* runs_out, size_t max_runs, size_t* out_num_runs) {
    install_segv_handler();

    const size_t page_count = ((address & (kPage - 1)) + size + kPage - 1) / kPage;
    size_t runs_count = 0;
    std::vector<std::pair<uintptr_t, size_t>> close_runs;

    {
        SpinLock lock;
        for (size_t p = 0; p < page_count; p++) {
            const uintptr_t page_addr = (address & ~(kPage - 1)) + p * kPage;
            const int64_t h = heap_page_index(page_addr);
            if (h < 0) continue;

            const uint8_t s = g_state[h].load(std::memory_order_relaxed);
            if (!(s & kWatched) || (s & kOpen)) {
                g_state[h].store((s | kWatched) & ~kOpen, std::memory_order_relaxed);
                g_version[h].fetch_add(1, std::memory_order_relaxed);
                add_contiguous_run(close_runs, page_addr);
            }

            const uint64_t cur_key = (static_cast<uint64_t>(h) << 32) | g_version[h].load(std::memory_order_relaxed);
            if (p < num_keys && keys_in_out[p] == cur_key) {
                continue; // Page tidak berubah
            }

            if (p < num_keys) {
                keys_in_out[p] = cur_key;
            }

            if (runs_out && runs_count < max_runs) {
                const size_t off = p * kPage - (address & (kPage - 1));
                const size_t begin = (p == 0) ? 0 : off;
                const size_t len = std::min<size_t>(size, off + kPage) - begin;

                if (runs_count > 0 && runs_out[runs_count - 1].offset + runs_out[runs_count - 1].length == begin) {
                    runs_out[runs_count - 1].length += len;
                } else {
                    runs_out[runs_count].offset = begin;
                    runs_out[runs_count].length = len;
                    runs_count++;
                }
            }
        }

        protect_runs_batch(close_runs);
    }

    if (out_num_runs) {
        *out_num_runs = runs_count;
    }
}

uint64_t ryza_watch_get_faults(void) { return g_faults.load(std::memory_order_relaxed); }
uint64_t ryza_watch_get_protects(void) { return g_protects.load(std::memory_order_relaxed); }
uint64_t ryza_watch_get_protect_ns(void) { return g_protect_ns.load(std::memory_order_relaxed); }

bool ryza_watch_on_fault(uintptr_t fault_addr) {
    return on_write_fault(fault_addr);
}

void ryza_watch_reset_range(uint64_t lo, uint64_t hi) {
    if (!g_map_base || !g_state || hi <= lo || hi <= g_map_base || lo >= g_map_base + g_heap_size) return;
    uint64_t a = lo < g_map_base ? g_map_base : (lo & ~(kPage - 1));
    uint64_t e = hi > g_map_base + g_heap_size ? g_map_base + g_heap_size : ((hi + kPage - 1) & ~(kPage - 1));
    if (e <= a) return;

    SpinLock lock;
    uint64_t p0 = (a - g_map_base) / kPage;
    uint64_t p1 = (e - g_map_base) / kPage;
    for (uint64_t p = p0; p < p1; p++) {
        g_state[p].store(0, std::memory_order_relaxed);
        g_version[p].fetch_add(1, std::memory_order_relaxed);
    }
    mprotect((void *)(uintptr_t)a, e - a, PROT_READ | PROT_WRITE);
}

int ryza_watch_get_written_pages(uint64_t addr, uint64_t size, uint64_t *pages_out, int max_pages, uint64_t *page_size_out) {
    if (!g_map_base || !g_state || !size) return -1;
    if (addr < g_map_base || addr + size > g_map_base + g_heap_size) return -1;

    if (page_size_out) *page_size_out = kPage;
    install_segv_handler();

    uint64_t p0 = (addr - g_map_base) / kPage;
    uint64_t p1 = (addr + size - g_map_base + kPage - 1) / kPage;
    int n = 0;
    std::vector<std::pair<uintptr_t, size_t>> close_runs;

    {
        SpinLock lock;
        for (uint64_t p = p0; p < p1; p++) {
            const uint8_t s = g_state[p].load(std::memory_order_relaxed);
            if (!(s & kWatched) || (s & kOpen)) {
                if (pages_out && n < max_pages) {
                    pages_out[n] = g_map_base + (p * kPage);
                }
                n++;

                g_state[p].store((s | kWatched) & ~kOpen, std::memory_order_relaxed);
                g_version[p].fetch_add(1, std::memory_order_relaxed);
                add_contiguous_run(close_runs, g_map_base + (p * kPage));
            }
        }
        protect_runs_batch(close_runs);
    }

    return n > max_pages ? max_pages : n;
}

} // extern "C"

namespace ryza::watch {

void collect(uintptr_t address, size_t size,
             std::vector<uint64_t>& keys,
             std::vector<std::pair<size_t, size_t>>* runs) {
    const size_t page_count = ((address & (kPage - 1)) + size + kPage - 1) / kPage;
    if (keys.size() != page_count) keys.assign(page_count, ~0ull);

    std::vector<RyzaWatchRun> runs_buf;
    if (runs) runs_buf.resize(page_count);

    size_t num_runs = 0;
    ryza_watch_collect(address, size, keys.data(), keys.size(),
                       runs ? runs_buf.data() : nullptr, runs_buf.size(), &num_runs);

    if (runs && num_runs > 0) {
        for (size_t i = 0; i < num_runs; i++) {
            runs->push_back({runs_buf[i].offset, runs_buf[i].length});
        }
    }
}

} // namespace ryza::watch
