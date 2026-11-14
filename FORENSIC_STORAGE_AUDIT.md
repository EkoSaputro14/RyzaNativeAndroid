# ANDROID STORAGE & PATH PERMISSIONS AUDIT

**Dokumen:** FORENSIC_STORAGE_AUDIT.md  
**Target:** Qualcomm Snapdragon 855 / Adreno 640 (Android 10 s.d. Android 14)  
**Komponen:** Native File I/O, JNI MediaStore, SAF Storage Tree, Scoped Storage  

---

## 1. Analisis Model Storage Android Terhadap Native C File I/O

| API Level | Mekanisme Storage | Perilaku Native `fopen("/sdcard/Download/...")` | Solusi Robust |
|---|---|---|---|
| **Android 9 kebawah (API <= 28)** | Legacy External Storage | Berhasil jika `WRITE_EXTERNAL_STORAGE` diberikan | POSIX direct open |
| **Android 10 (API 29)** | Scoped Storage (Legacy opt-in) | Gagal tanpa `requestLegacyExternalStorage="true"` | `MediaStore.Downloads` atau SAF |
| **Android 11 - 14 (API 30 - 34)** | Strict Scoped Storage | **SELALU GAGAL (EACCES: errno 13)** tanpa `MANAGE_EXTERNAL_STORAGE` | **`MediaStore.Downloads`** via JNI & SAF Tree |

---

## 2. Audit Jalur File (Paths) Aktual

### A. Jalur yang Sebelumnya Gagal
1. `/sdcard/Download/ryza_forensic_dump.txt`
   - **Hasil:** `EACCES` (Permission Denied).
   - **Penyebab:** Scoped Storage membatasi penulisan langsung berkas di luar app-specific folder.
2. `/sdcard/Android/data/com.retro.botwrecompandroid/files/...`
   - **Hasil:** `ENOENT` (No such file or directory).
   - **Penyebab:** Kesalahan penamaan package (`com.retro.botwrecompandroid` vs `com.botwrecomp.android`).

### B. Jalur Baru yang Terverifikasi Writable (5-Tier Fail-Safe)
1. **Tier 1 (JNI MediaStore / SAF Tree):**
   - Menggunakan `BOTWRecompAndroidActivity.openDiagnosticFile()`:
     - Jika pengguna telah memilih folder log (SAF), berkas dibuat langsung di root folder log pengguna via `DocumentsContract.createDocument()`.
     - Jika belum ada folder SAF, berkas dibuat di `/sdcard/Download/ryza_forensic_dump.txt` via `MediaStore.Downloads`. Berkas ini dapat langsung dibuka oleh semua File Manager dan WhatsApp/Telegram tanpa memerlukan izin khusus.
2. **Tier 2 (App-Specific External Storage):**
   - Path: `/storage/emulated/0/Android/data/com.botwrecomp.android/files/ryza_forensic_dump.txt`
   - Dijamin 100% writable oleh native code (`fopen`) pada semua versi Android tanpa runtime permission.
3. **Tier 3 (Public Download Fallback):**
   - Path: `Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS)`
4. **Tier 4 (Timestamped Unique File):**
   - Path: `ryza_forensic_dump_YYYYMMDD_HHMMSS.txt` untuk memastikan riwayat dump tidak tertimpa tanpa sengaja.
5. **Tier 5 (Mirroring Stderr / `botwrecomp-log.txt`):**
   - Seluruh payload forensic dump dipancarkan ke `stderr`, memastikan dump tersimpan di dalam berkas `botwrecomp-log.txt`.

---

## 3. Implementasi Self-Test (`TEST FORENSIC FILE WRITE`)

Untuk membuktikan kemampuan tulis-baca storage secara empiris sebelum pengguna mengekspor log karakter, ditambahkan fitur **Self-Test File I/O**:

1. **Tombol HUD:** `[TEST FORENSIC FILE WRITE]`
2. **Langkah Eksekusi Otomatis:**
   - Membuka berkas `ryza_storage_test.txt` melalui pipeline 5-tier.
   - Menulis payload 512 byte berisi magic header `RYZA_VERIFY_640A_TEST_OK`, fingerprint build, dan timestamp.
   - Menjalankan `fflush(f)` dan `fsync(fileno(f))` untuk memastikan buffer hardware disk terisi.
   - Menutup berkas (`fclose`).
   - Membuka kembali berkas dalam mode baca (`"r"`).
   - Membaca ulang isi berkas dan mencocokkan checksum/magic string serta jumlah byte (`bytes_read == bytes_written`).
3. **Hasil Tampilan Real-Time di HUD:**
   - **PASS:** Teks hijau menyala: `STORAGE TEST: PASS (Wrote: 512, Read: 512)` disertai path target aktual.
   - **FAIL:** Teks merah menyala: `STORAGE TEST: FAIL (Err <errno>: <strerror>)` disertai path yang gagal.
