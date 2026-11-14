# RYZA RUNTIME CAPTURE VALIDATION
**Validasi Pipeline Capture Karakter & Pencegahan Degradasi Performa**

---

## 1. Identifikasi & Pemisahan Karakter vs Quad Environment

Pada pengujian sebelumnya, buffer capture cepat jenuh oleh 16 quad environment billboard (108 index draw calls) yang dirender di awal frame, sehingga mesh skinned Ryza yang dirender ribuan draw call setelahnya tidak pernah tertangkap.

### Mekanisme Filter Baru (`exec.c:7100-7170`):
1. **Filtering Billboard Kecil:**
   Draw call dengan `icount == 108` atau `icount < 200` tanpa atribut skinning langsung diabaikan dari ring buffer capture karakter.
2. **Prioritas Skinned Character Mesh:**
   - Draw call dengan `icount > 1000` atau format atribut ber-stride 124 byte (format khas karakter Ryza) diklasifikasikan sebagai `is_character = true`.
   - Disimpan di **Slot Prioritas Karakter (Slot 0 s.d 15)**.
3. **Deteksi Anomali Horizontal Polygon Stretching:**
   - Jika vertex sample menghasilkan koordinat di luar bounding box wajar (`|pos.x| > 25.0f` atau `|pos.x| > 5.0 * |pos.y|`), draw call diklasifikasikan sebagai `is_deformed = true`.
   - Disimpan di **Slot Deformed Mesh (Slot 16 s.d 23)** dan status slot dikunci (`LOCKED`).
   - Memicu auto-dump ke file log secara otomatis.
4. **Environment Baseline:**
   - Slot 24 s.d 31 dialokasikan khusus untuk sampel draw call lingkungan non-karakter sebagai pembanding baseline.

---

## 2. Eliminasi CPU Render Stall (3319 ms -> < 16 ms)

- **Masalah Sebelumnya:** Loop CPU memindai seluruh array indeks (`min_idx`/`max_idx`) untuk semua 8.618 draw call per frame, menyebabkan CPU render time melonjak hingga 3.319 ms dan FPS drop ke 9.
- **Optimasi:**
  - Evaluasi indeks hanya dijalankan untuk draw call yang lolos filter karakter dan masuk ke slot capture.
  - Jumlah elemen yang diperiksa dibatasi maksimal 256 elemen per capture slot.
  - Menghilangkan stall CPU sehingga target 30–60 FPS tetap terjaga.

---

## 3. Struktur Data Capture Terverifikasi dalam Dump

Setiap record capture dalam `ryza_forensic_dump.txt` memuat:
- **Build Fingerprint & Session ID:** Memastikan integritas data terhadap versi binary game.
- **Pipeline ID & Shader Hashes:**
  - `Pipeline ID`: Kombinasi 64-bit VS code hash dan FS code hash.
  - `VS Code Address` & `FS Code Address` serta `FS Hash`.
- **Atribut Vertex Lengkap (A0 s.d A6):**
  - Stride, buffer address, format NVN, offset, dan stream binding.
  - Atribut posisi (A0: Float3/Float4), normal (A1), UV (A2), Tangent (A3).
  - Atribut skinning: Bone Indices (A4) dan Bone Weights (A5/A6).
- **Constant Buffer Transformasi:**
  - `ConstantBuffer1`: Matriks View & Projection (16 float).
  - `ConstantBuffer3`: Bone Matrix Palette (sampel matriks rotasi/translasi tulang Ryza).
- **Index Buffer Metadata:** Format (U16/U32), index count, range `[min_idx, max_idx]`, dan base vertex.
- **Deteksi Nilai Ekstrem (NaN / Inf):** Validasi integritas koordinat vertex sebelum rendering.
