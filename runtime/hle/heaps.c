// Game heaps (engine limits). The game carves its memory into SDK heaps of fixed sizes: nn::mem::StandardAllocator
// (general), nn::lmem unit heaps (fixed-size blocks: object pools) and frame heaps. A heap that runs out returns
// NULL and the game crashes further on (bushsanity: ~450 actors around Link -> null weak_ptr in f_70010d0f00).
// These wrappers call the SDK's own implementation (loader_find_symbol), log heap creation (LARECOMP_LOG_HEAPS=1)
// and every allocation that fails (once per caller), and can grow heaps (heap_scale) - a recomp has memory to spare.
#include <pthread.h>
#ifdef _WIN32
#include <malloc.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __ANDROID__
#include <sys/mman.h>
#include "../settings.h"
#endif

#include "../hle.h"

uint64_t loader_find_symbol(const char *name);
void guest_call(Cpu *c, uint64_t addr);

static uint64_t orig(const char *name, uint64_t *cache) {
    if (!*cache) *cache = loader_find_symbol(name);
    if (!*cache) FATAL("heaps: no SDK implementation of %s\n", name);
    return *cache;
}

static void fail_once(const char *what, uint64_t lr, uint64_t size) {
    static uint64_t seen[64];
    static int n;
    for (int i = 0; i < n; i++) if (seen[i] == lr) return;
    if (n < 64) seen[n++] = lr;
    LOG("[heaps] %s FAILED (size %#llx) from lr %llx\n", what, (unsigned long long)size, (unsigned long long)lr);
}

#define CALL(sym)                                    \
    do {                                             \
        static uint64_t a_;                          \
        uint64_t lr_ = c->x[30];                     \
        guest_call(c, orig(sym, &a_));               \
        c->x[30] = lr_;                              \
    } while (0)

// Unit heaps are fixed pools (e.g. 256 x 0x50-byte objects per loaded area: bushsanity's extra actors ran out at
// ~450 actors around Link). Every unit heap gets UNIT_SCALE times the units in a buffer of ours (the game's own
// buffer stays unused); freed again in DestroyUnitHeap. LARECOMP_UNIT_HEAP_SCALE overrides (1 = the game's sizes).
typedef struct { uint64_t head; void *mem; } UnitBuf;
static UnitBuf g_unit[256];
static pthread_mutex_t g_unit_lock = PTHREAD_MUTEX_INITIALIZER;

// Reboot: our unit-heap buffers (host memory, not in the guest window) go with the game that made them.
RT_ON_RESET(heaps_reset) {
    for (int i = 0; i < 256; i++)
        if (g_unit[i].mem) {
#ifdef _WIN32
            _aligned_free(g_unit[i].mem);
#else
            free(g_unit[i].mem);
#endif
            g_unit[i].mem = NULL;
        }
}

static int unit_scale(void) {
    static int s = -1;
    // Default to 1 so nn::lmem::CreateUnitHeap preserves the guest buffer c->x[0]
    // inside [GUEST_LO, GUEST_HI) and inside sead::UnitHeap [mStart, mStart + mSize).
    // Allocating via host posix_memalign outside the guest window breaks
    // sead::HeapMgr::findContainHeap(ptr) -> NULL -> cascading null reads/write at 0x0.
    if (s < 0) { const char *e = ENV("LARECOMP_UNIT_HEAP_SCALE"); s = e ? atoi(e) : 1; if (s < 1) s = 1; }
    return s;
}

// nn::lmem::CreateUnitHeap(void *addr, size_t size, size_t unit, int align, int opt, HeapHead *head)
HLE_FUNC("_ZN2nn4lmem14CreateUnitHeapEPvmmiiPNS0_6detail8HeapHeadE", heap_create_unit) {
    uint64_t lr = c->x[30], size = c->x[1], unit = c->x[2];
    int k = unit_scale();
    void *mem = NULL;
    if (k > 1 && unit) {
        uint64_t big = size * (uint64_t)k;
#ifdef _WIN32
        mem = _aligned_malloc(big, 0x1000);
        if (mem) {
#else
        if (posix_memalign(&mem, 0x1000, big) == 0) {
#endif
            memset(mem, 0, big);
            c->x[0] = (uint64_t)(uintptr_t)mem;
            c->x[1] = big;
        } else {
            mem = NULL;
        }
    }
    uint64_t head = c->x[5];
    CALL("_ZN2nn4lmem14CreateUnitHeapEPvmmiiPNS0_6detail8HeapHeadE");
    if (mem) {
        pthread_mutex_lock(&g_unit_lock);
        for (int i = 0; i < 256; i++)
            if (!g_unit[i].mem) { g_unit[i].head = c->x[0] ? c->x[0] : head; g_unit[i].mem = mem; break; }
        pthread_mutex_unlock(&g_unit_lock);
    }
    if (ENV("LARECOMP_LOG_HEAPS"))
        LOG("[heaps] unit heap %#llx bytes, unit %#llx (%llu units, x%d) from lr %llx -> %llx\n", (unsigned long long)size,
            (unsigned long long)unit, unit ? (unsigned long long)(size / unit) : 0ull, mem ? k : 1, (unsigned long long)lr,
            (unsigned long long)c->x[0]);
}

HLE_FUNC("_ZN2nn4lmem15DestroyUnitHeapEPNS0_6detail8HeapHeadE", heap_destroy_unit) {
    uint64_t head = c->x[0];
    CALL("_ZN2nn4lmem15DestroyUnitHeapEPNS0_6detail8HeapHeadE");
    pthread_mutex_lock(&g_unit_lock);
    for (int i = 0; i < 256; i++)
        if (g_unit[i].mem && g_unit[i].head == head) {
#ifdef _WIN32
            _aligned_free(g_unit[i].mem);
#else
            free(g_unit[i].mem);
#endif
            g_unit[i].mem = NULL;
            break;
        }
    pthread_mutex_unlock(&g_unit_lock);
}

HLE_FUNC("_ZN2nn4lmem20AllocateFromUnitHeapEPNS0_6detail8HeapHeadE", heap_alloc_unit) {
    uint64_t lr = c->x[30], head = c->x[0];
    CALL("_ZN2nn4lmem20AllocateFromUnitHeapEPNS0_6detail8HeapHeadE");
    if (!c->x[0]) fail_once("unit heap allocation", lr, head);
}

#ifdef __ANDROID__
extern bool vk_gmem_placed(void);
extern void plat_write_prepare(uint64_t addr, uint64_t size);

// Intercept guest SDK memset when Atelier Ryza debug-fills >=16 MB heap arenas with 0xef poison
// so we never fault in 1.58 GB of physical pages on anonymous or placed/dma-buf GPU heap mappings.
HLE_FUNC("memset", hle_guest_memset) {
    uint64_t dst = c->x[0], val = c->x[1] & 0xffu, len = c->x[2];
    if (val == 0xef && len >= (16ull << 20) && dst >= HEAP_BASE && dst < HEAP_BASE + HEAP_MAX) {
        size_t head = len > 0x10000 ? 0x10000 : (size_t)len;
        plat_write_prepare(dst, head);
        memset((void *)(uintptr_t)dst, (int)val, head);
        LOG("[heaps] skipped %llu MB 0xef debug-poison memset at %#llx (saved physical RAM commit)\n",
            (unsigned long long)(len >> 20), (unsigned long long)dst);
        return;
    }
    if (dst >= GUEST_LO && dst < GUEST_HI && len <= GUEST_HI - dst) {
        if (len >= 4096) plat_write_prepare(dst, len);
        memset((void *)(uintptr_t)dst, (int)val, (size_t)len);
        return;
    }
    CALL("memset");
}

// Atelier Ryza debug-fills newly carved sub-heaps (including the 1579 MB 0x62b00000 StandardAllocator)
// with 0xef via memset right before calling Initialize, faulting in >1.5 GB of unused physical RAM.
// Reclaim those 0xef-poisoned physical pages back to demand-zero before Initialize writes its header.
static void reclaim_poisoned_arena(uint64_t addr, uint64_t size) {
    if (size < (16ull << 20) || vk_gmem_placed()) return;
    if (addr < HEAP_BASE || addr >= HEAP_BASE + HEAP_MAX || size > HEAP_BASE + HEAP_MAX - addr) return;
    uint64_t lo = (addr + 0x10000ull) & ~0xfffull;
    uint64_t hi = (addr + size) & ~0xfffull;
    if (hi > lo) {
        madvise((void *)(uintptr_t)lo, (size_t)(hi - lo), MADV_DONTNEED);
        LOG("[heaps] reclaimed %llu MB of pre-poisoned physical pages in [%#llx, %#llx) (heap_mb=%d)\n",
            (unsigned long long)((hi - lo) >> 20), (unsigned long long)lo, (unsigned long long)hi, g_settings.heap_mb);
    }
}
#endif

// nn::lmem::CreateFrameHeap(void *addr, size_t size, int opt)
HLE_FUNC("_ZN2nn4lmem15CreateFrameHeapEPvmi", heap_create_frame) {
    uint64_t lr = c->x[30], addr = c->x[0], size = c->x[1];
#ifdef __ANDROID__
    reclaim_poisoned_arena(addr, size);
#else
    (void)addr;
#endif
    CALL("_ZN2nn4lmem15CreateFrameHeapEPvmi");
    if (ENV("LARECOMP_LOG_HEAPS"))
        LOG("[heaps] frame heap %#llx bytes from lr %llx\n", (unsigned long long)size, (unsigned long long)lr);
}

HLE_FUNC("_ZN2nn4lmem21AllocateFromFrameHeapEPNS0_6detail8HeapHeadEmi", heap_alloc_frame) {
    uint64_t lr = c->x[30], size = c->x[1];
    CALL("_ZN2nn4lmem21AllocateFromFrameHeapEPNS0_6detail8HeapHeadEmi");
    if (!c->x[0]) fail_once("frame heap allocation", lr, size);
}

// nn::mem::StandardAllocator::Initialize(void *addr, size_t size[, bool])
HLE_FUNC("_ZN2nn3mem17StandardAllocator10InitializeEPvm", heap_std_init) {
    uint64_t lr = c->x[30], addr = c->x[1], size = c->x[2];
#ifdef __ANDROID__
    reclaim_poisoned_arena(addr, size);
#else
    (void)addr;
#endif
    CALL("_ZN2nn3mem17StandardAllocator10InitializeEPvm");
    if (ENV("LARECOMP_LOG_HEAPS"))
        LOG("[heaps] standard allocator %#llx bytes from lr %llx\n", (unsigned long long)size, (unsigned long long)lr);
}

HLE_FUNC("_ZN2nn3mem17StandardAllocator10InitializeEPvmb", heap_std_init_b) {
    uint64_t lr = c->x[30], addr = c->x[1], size = c->x[2];
#ifdef __ANDROID__
    reclaim_poisoned_arena(addr, size);
#else
    (void)addr;
#endif
    CALL("_ZN2nn3mem17StandardAllocator10InitializeEPvmb");
    if (ENV("LARECOMP_LOG_HEAPS"))
        LOG("[heaps] standard allocator %#llx bytes (b) from lr %llx\n", (unsigned long long)size, (unsigned long long)lr);
}

HLE_FUNC("_ZN2nn3mem17StandardAllocator8AllocateEm", heap_std_alloc) {
    uint64_t lr = c->x[30], size = c->x[1];
    CALL("_ZN2nn3mem17StandardAllocator8AllocateEm");
    if (!c->x[0] && size) fail_once("standard allocation", lr, size);
}

HLE_FUNC("_ZN2nn3mem17StandardAllocator8AllocateEmm", heap_std_alloc_a) {
    uint64_t lr = c->x[30], size = c->x[1];
    CALL("_ZN2nn3mem17StandardAllocator8AllocateEmm");
    if (!c->x[0] && size) fail_once("standard allocation (aligned)", lr, size);
}
