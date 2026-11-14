# FORENSIC CAPTURE BUFFER & SLOTS VALIDATION

**Dokumen:** FORENSIC_CAPTURE_VALIDATION.md  
**Target:** Buffer Capture 32-Slot, Skinned Mesh Identification, CPU Stall Mitigation  
**Komponen:** `runtime/nvn/vulkan/ryza_forensics.c`, `runtime/nvn/vulkan/exec.c`, `runtime/ui/perf_overlay.mm`  

---

## 1. Audit Masalah Buffer Capture Lama (Mengapa "Captured: 16" & Slot Tertimpa)

1. **Kapasitas Slot Terbatas & Tak Terproteksi:**
   Pada build `798e4b5-diag`, kapasitas slot capture hanya 16 (`RYZA_MAX_CAPTURED_DRAWS 16`). Slot 0 s.d. 14 terisi oleh quad 108 index (lingkungan/billboard), sehingga mesh karakter baru muncul di slot 15 dan langsung berisiko tertimpa oleh draw call akhir frame (partikel/UI).
2. **Penyebab CPU Render 3319 ms (Drop ke 9 FPS):**
   Pada loop draw call Vulkan (`exec.c`), fungsi forensik melakukan pemindaian CPU penuh (`min_idx`/`max_idx`) atas seluruh `icount` indeks vertex pada **setiap draw call** (8.618 draw call per frame). Dengan jutaan iterasi CPU per frame, thread render tersendat ekstrem (3.319 ms).
3. **Penyebab Atribut A0–A6 Tampak Kosong di HUD:**
   Submesh selector di overlay ImGui secara default menunjuk ke `Submesh##rdraw = 0`. Slot 0 menyimpan draw call lingkungan statis dengan buffer address 0x0 / size 0 (menggunakan konstanta/uniform shader). Akibatnya pengguna melihat atribut A0–A6 kosong jika tidak menggeser slider ke slot karakter.

---

## 2. Arsitektur Capture Buffer Baru (32 Slot Terisolasi & Terproteksi)

### A. Zonasi Slot Khusus
- **Slot 0 – 15: Character Skinned Meshes**
  - Kriteria Alokasi: Mendeteksi bone indices format `0x27` (`VK_FORMAT_R8G8B8A8_UINT`), bone weights `0x25`/`0x2A`, vertex stride 124 byte, atau kombinasi format float 32-bit di offset 28/36.
  - Slot ini diisolasi sehingga **tidak dapat ditimpa oleh draw call lingkungan statis**.
- **Slot 16 – 23: Deformed Meshes (Horizontal Stretch Detected)**
  - Kriteria Alokasi: Terpicu saat nilai koordinat vertex X melompat di luar batas normal (`fabsf(pos.x) > 1000.0f` atau rasio X/Y abnormal).
  - Slot ini otomatis di-**LOCKED** dan memicu dump otomatis (`AUTO-DUMP TRIGGERED`).
- **Slot 24 – 31: Environment & Background Reference**
  - Menyimpan sampel lingkungan untuk perbandingan baseline geometri.

### B. Optimasi CPU Stalling (9 FPS -> Normal)
- Pemindaian indeks dibatasi maksimal 256 elemen per draw call yang dialokasikan, mencegah CPU bottleneck tanpa kehilangan akurasi rentang indeks.
- Jika capture dalam status `Freeze` (`g_ryza_capture_freeze`), pemindaian vertex sepenuhnya dilewati (zero CPU overhead).

### C. Smart HUD Submesh Selector
- Saat overlay dibuka, `selected_draw` tidak lagi default ke slot 0 (lingkungan kosong), melainkan **otomatis memilih slot pertama yang berisi karakter Ryza atau mesh terdeformasi**.
- Metadata A0–A6 langsung terisi dengan stream, offset, stride, format NVN, format Vulkan, dan data heksadesimal/float aktual.

---

## 3. Fitur Tombol Baru di HUD ImGui

1. **`[Force Dump Log Now]`**:
   Mengekspor buffer 32 slot ke berkas `ryza_forensic_dump.txt` dan `ryza_forensic_dump_YYYYMMDD_HHMMSS.txt` menggunakan pipeline 5-tier, sekaligus mencetak status hasil penulisan (SUCCESS / FAILED, target path, bytes written).
2. **`[TEST FORENSIC FILE WRITE]`**:
   Menjalankan self-test tulis-baca berkas test 512-byte dan memvalidasi integritas byte count serta checksum.
3. **`[Freeze / Unfreeze]`**:
   Mengunci buffer capture agar tidak ada draw call baru yang mengubah status saat pengguna menganalisis frame.
4. **`[Re-arm]`**:
   Membersihkan seluruh slot capture dan menyiapkan buffer untuk menangkap frame berikutnya dari awal.
