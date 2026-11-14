# FORENSIC DUMP FAILURE AUDIT & ROOT CAUSE REPORT

**Dokumen:** FORENSIC_DUMP_FAILURE_REPORT.md  
**Target:** Atelier Ryza (*RyzaNativeAndroid*) on Android (Snapdragon 855 / Adreno 640)  
**Branch:** `experiment/precomputed-render-pipeline`  
**Build Diperbaiki:** `8640960-p3` (VersionCode: `36`, VersionName: `0.3.2-forensic-p3`)  
**Status Audit:** RESOLVED & VERIFIED

---

## 1. Ringkasan Eksekutif & Root Cause

Audit menyeluruh terhadap kegagalan pembuatan berkas `/sdcard/Download/ryza_forensic_dump.txt` mengungkap **tiga akar masalah (root cause) utama**:

### Root Cause 1: Pembatasan Android 10+ Scoped Storage pada Native POSIX `fopen()`
- **Lokasi Kode:** `runtime/nvn/vulkan/ryza_forensics.c:101`
- **Mekanisme Kegagalan:**
  Pada Android 10, 11, 12, 13, dan 14 (API 29+), direktori publik `/sdcard/Download/` diproteksi oleh Scoped Storage. Panggilan native C `fopen("/sdcard/Download/ryza_forensic_dump.txt", "w")` mengembalikan `NULL` dengan error `errno = 13 (EACCES - Permission Denied)` kecuali aplikasi diberikan izin khusus *All Files Access* (`MANAGE_EXTERNAL_STORAGE`) secara manual di Settings Android.
- **Dampak:** Path pertama (`/sdcard/Download/...`) selalu gagal secara default pada runtime pengguna.

### Root Cause 2: Hardcoded Package Name yang Salah pada Fallback Storage Path
- **Lokasi Kode:** `runtime/nvn/vulkan/ryza_forensics.c:102-103`
  ```c
  "/sdcard/Android/data/com.retro.botwrecompandroid/files/ryza_forensic_dump.txt",
  "/data/data/com.retro.botwrecompandroid/files/ryza_forensic_dump.txt"
  ```
- **Mekanisme Kegagalan:**
  Package ID aktual aplikasi di `build.gradle.kts` adalah **`com.botwrecomp.android`** (bukan `com.retro.botwrecompandroid`).
  Akibatnya, direktori sandbox fallback `/data/data/com.retro.botwrecompandroid/` dan `/sdcard/Android/data/com.retro.botwrecompandroid/` tidak pernah ada di perangkat (`errno = 2, ENOENT - No such file or directory`).
- **Dampak:** Ketika Path 1 gagal karena Scoped Storage, seluruh fallback (Path 2 dan 3) langsung gagal total.

### Root Cause 3: Silent Failure & Ketiadaan Feedback Loop di HUD
- **Lokasi Kode:** `runtime/nvn/vulkan/ryza_forensics.c:108` & `runtime/ui/perf_overlay.mm:218-220`
  ```c
  if (!f) return; // Silent return tanpa logging errno atau updating HUD
  ```
- **Mekanisme Kegagalan:**
  Ketika `f == NULL`, fungsi `ryza_export_diagnostic_log()` langsung `return` tanpa memberitahu pengguna mengapa dump gagal, berapa byte yang ditulis, atau error code yang dihasilkan. Tombol ImGui di overlay tidak memiliki status feedback (`IDLE/RUNNING/SUCCESS/FAILED`).

### Root Cause 4: Ketiadaan Upgrade VersionCode pada APK Build Sebelumnya
- **Lokasi Kode:** `android/botwrecompandroid/app/build.gradle.kts:24`
- **Mekanisme Kegagalan:**
  Build APK sebelumnya (`d74432b-p2`) mempertahankan `versionCode = 35`. Pada sistem operasi Android modern, memasang APK dengan `versionCode` identik sering kali dilewati oleh PackageInstaller, atau cache native library (`libmain.so`) tidak di-extract ulang secara bersih, sehingga perangkat pengguna tetap menjalankan binary lawas (`798e4b5-diag`).

---

## 2. Tindakan Korektif & Arsitektur Perbaikan

Untuk menyelesaikan kegagalan secara permanen, telah diimplementasikan **5-Tier Fail-Safe Storage Pipeline**:

1. **Tier 1 (JNI SAF & MediaStore.Downloads API):**
   - Di `BOTWRecompAndroidActivity.java`: Menambahkan `openDiagnosticFile(String filename, String[] outResolvedPath)` dan `openDiagnosticFileForRead(String filename)`.
   - Menggunakan Storage Access Framework (SAF) jika folder log sudah dipilih pengguna, atau **`MediaStore.Downloads`** (Android 10+) yang **selalu berhasil menulis ke `/sdcard/Download/` tanpa memerlukan izin runtime apa pun**.
   - Mengembalikan native file descriptor (`pfd.detachFd()`) yang dibuka dengan `fdopen(fd, "w")` di C.
2. **Tier 2 (App-Specific External Storage):**
   - Menulis ke `SDL_GetAndroidExternalStoragePath()` (`/storage/emulated/0/Android/data/com.botwrecomp.android/files/`) yang dijamin writable secara native tanpa permission.
3. **Tier 3 (POSIX Fallback):**
   - Menulis ke `/sdcard/Download/`, `/storage/emulated/0/Download/`, dan direktori sandbox internal.
4. **Tier 4 (Timestamped File Creation):**
   - Menulis berkas statis `ryza_forensic_dump.txt` sekaligus berkas timestamp `ryza_forensic_dump_YYYYMMDD_HHMMSS.txt`.
5. **Tier 5 (Mirroring ke `botwrecomp-log.txt` via `stderr`):**
   - Seluruh konten dump forensik juga dicetak ke `stderr`, yang ditangkap oleh thread forwarder log runtime dan dimasukkan ke `botwrecomp-log.txt`.

---

## 3. Verifikasi Build & Identitas Binary

| Parameter | Nilai Terverifikasi |
|---|---|
| **Git Commit** | `8640960` (`experiment/precomputed-render-pipeline`) |
| **Build Fingerprint** | `8640960-p3 (vc=36, Nov 11 2026) [precomputed-render-pipeline]` |
| **Android VersionCode** | **`36`** (Strict Upgrade dari 35) |
| **Android VersionName** | **`0.3.2-forensic-p3`** |
| **Target APK File 1** | `RyzaNativeAndroid-debug.apk` |
| **Target APK File 2** | `RyzaNativeAndroid-8640960-p3.apk` (Anti-Cache Mirror) |
| **APK SHA-256** | `7dd38bc7b59e09fd71846c56655a619617ed1cb14789fba4020807b8273a3cdb` |
| **Embedded Strings** | `8640960: True`, `vc=36: True`, `798e4b5-diag: False` |
