#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    size_t offset;
    size_t length;
} RyzaWatchRun;

// Inisialisasi heap pengawasan memori game (StandardAllocator / HEAP_BASE)
void ryza_watch_init_heap(uintptr_t map_base, size_t size);

// Cek apakah rentang memori berada di dalam heap yang diawasi
bool ryza_watch_is_watchable(uintptr_t address, size_t size);

// Wajib dipanggil sebelum syscall kernel (pread, recv, read) menulis langsung ke memori game
void ryza_watch_before_kernel_write(void* p, size_t size);

// Reset tracking range (e.g. saat heap di-reset)
void ryza_watch_reset_range(uint64_t lo, uint64_t hi);

// Mengumpulkan halaman kotor (kompatibel langsung dengan contract plat_written_pages)
// Mengembalikan jumlah page yang berubah, atau -1 jika di luar range
int ryza_watch_get_written_pages(uint64_t addr, uint64_t size, uint64_t *pages_out, int max_pages, uint64_t *page_size_out);

// Handler fault saat SIGSEGV (bisa dipanggil langsung dari crash_handler)
bool ryza_watch_on_fault(uintptr_t fault_addr);

// Mengumpulkan halaman yang berubah sejak pemanggilan terakhir:
// - keys_in_out: array uint64_t penyimpan versi per-page (ukuran: (size + 4095) / 4096)
// - runs_out: array output untuk rentang byte dirty [offset, length]
// - max_runs: kapasitas array runs_out
// - out_num_runs: jumlah rentang dirty yang ditemukan
void ryza_watch_collect(uintptr_t address, size_t size,
                        uint64_t* keys_in_out, size_t num_keys,
                        RyzaWatchRun* runs_out, size_t max_runs, size_t* out_num_runs);

// Diagnostik statistik performa
uint64_t ryza_watch_get_faults(void);
uint64_t ryza_watch_get_protects(void);
uint64_t ryza_watch_get_protect_ns(void);

#ifdef __cplusplus
}

#include <vector>
#include <utility>

namespace ryza::watch {

inline void init_heap(uintptr_t map_base, size_t size) {
    ryza_watch_init_heap(map_base, size);
}

inline bool watchable(uintptr_t address, size_t size) {
    return ryza_watch_is_watchable(address, size);
}

inline void before_kernel_write(void* p, size_t size) {
    ryza_watch_before_kernel_write(p, size);
}

// C++ Helper menggunakan std::vector
void collect(uintptr_t address, size_t size,
             std::vector<uint64_t>& keys,
             std::vector<std::pair<size_t, size_t>>* runs);

inline uint64_t faults() { return ryza_watch_get_faults(); }
inline uint64_t protects() { return ryza_watch_get_protects(); }
inline uint64_t protect_ns() { return ryza_watch_get_protect_ns(); }

} // namespace ryza::watch
#endif
