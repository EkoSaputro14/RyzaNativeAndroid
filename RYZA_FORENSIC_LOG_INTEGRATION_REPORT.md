# RYZA FORENSIC LOG INTEGRATION REPORT
**Branch:** `experiment/precomputed-render-pipeline`  
**Commit:** `c0f3ea7`  
**Build:** `c0f3ea7-p4 (vc=37)`  
**Target:** Qualcomm Snapdragon 855 / Adreno 640 (Android, Turnip Mesa Vulkan)  
**Date:** 11 November 2026  

---

## 1. Ringkasan Eksekutif & Prinsip Integrasi

Sesuai instruksi dan koreksi requirement JAWIRTX:
- **TIDAK membuat** storage subsystem baru, Diagnostic Log Manager baru, atau halaman UI terpisah.
- **SEPENUHNYA mereuse** sistem log file existing yang sudah terbukti bekerja di `BOTWRecompAndroidActivity.java` dan `runtime/platform/android.c`.
- **Ryza Forensic Dump** (`ryza_forensic_dump.txt`) kini terdaftar secara resmi sebagai entitas log dalam ekosistem logger existing, sejajar dengan `botwrecomp-log.txt` (Application Log / Driver Compatibility Log) dan `botwrecomp-log-previous.txt`.

---

## 2. Pemetaan Komponen Existing Logger

| Fungsi Log | Implementasi Java Existing | Native C / C++ Interface |
|---|---|---|
| **Storage Resolution** | `BOTWRecompAndroidActivity.logTree()` via SAF `DocumentFile` dengan fallback ke App-Specific External Files Dir (`getExternalFilesDir(null)`) | `runtime/platform/android.c`: `android_open_log_for_write(filename, first)` |
| **File Creation & Rotation** | `openLogFile(String filename, boolean first)`: otomatis merotasi `filename` lama menjadi `filename_previous` | `android_open_log_for_write("ryza_forensic_dump.txt", false)` |
| **File Descriptor Bridge** | `cr.openFileDescriptor(targetUri, "wa")` -> dup native POSIX FD | Native C `fdopen(fd, "a")` |
| **Log Registry / Listing** | `BOTWRecompAndroidActivity.listLogFiles()`: memindai DocumentFile folder SAF & External Files Dir | `runtime/platform/android.c`: `android_list_log_files()` |
| **Log Viewer** | `BOTWRecompAndroidActivity.viewLogFile(name)`: `Intent.ACTION_VIEW` via `FileProvider` (`content://`) | `runtime/platform/android.c`: `android_view_log_file(name)` |
| **Share Mechanism** | `BOTWRecompAndroidActivity.shareLogFile(name)`: `Intent.ACTION_SEND` (MIME `text/plain`) via `FileProvider` | `runtime/platform/android.c`: `android_share_log_file(name)` |
| **Export Mechanism** | `BOTWRecompAndroidActivity.exportLogFile(name)`: SAF `ACTION_CREATE_DOCUMENT` | `runtime/platform/android.c`: `android_export_log_file(name)` |
| **Delete Mechanism** | `BOTWRecompAndroidActivity.deleteLogFile(name)`: `DocumentFile.delete()` / `File.delete()` | `runtime/platform/android.c`: `android_delete_log_file(name)` |

---

## 3. Integrasi UX (Settings & HUD)

1. **Halaman Settings Existing (`Settings > Prelaunch > Driver compatibility`)**:
   - Di bawah tombol `[Choose folder] / [Change folder]`, kini terdapat daftar **Active Log Files**:
     - `botwrecomp-log.txt` (Log aplikasi utama)
     - `botwrecomp-log-previous.txt`
     - **`ryza_forensic_dump.txt`** (Ditandai warna hijau khusus kategori Ryza Forensic)
   - Setiap item log memiliki tombol aksi native:
     - `[View]`: Membuka file secara instan menggunakan text editor / log viewer bawaan Android.
     - `[Share]`: Membuka Android Share Sheet (bisa langsung dikirim via Telegram, WhatsApp, Gmail, Drive).
     - `[Export]`: Memilih folder tujuan ekspor di luar aplikasi via SAF File Picker.
     - `[Delete]`: Menghapus file log untuk mengosongkan storage.

2. **HUD In-Game Overlay (`perf_overlay.mm`)**:
   - Tombol `[Force Dump Log Now]` langsung memanggil pipeline existing logger.
   - Setelah dump selesai (`FORCE DUMP: SUCCESS`), tombol `[Share Dump File]` dan `[Export Dump File]` langsung muncul di HUD sehingga pengguna tidak perlu keluar dari game untuk membagikan file dump.

---

## 4. Metadata Build & Distribusi

- **VersionCode:** `37` (Menaikkan versi dari 36 untuk memastikan pembaruan APK)
- **VersionName:** `0.3.2-forensic-p4`
- **Build Fingerprint:** `c0f3ea7-p4 (vc=37, Nov 11 2026) [precomputed-render-pipeline]`
- **APK SHA-256:** `eab8d4d445e5d9dbed2d2230e49ea446eb77d1447d998fcbfa8a96a8a48bfaa3`
- **Ukuran File APK:** 48.421.252 byte
- **URL Unduhan:**
  - `https://aplikasi-ta.ekohomelab.online/RyzaNativeAndroid-c0f3ea7-p4.apk`
  - `https://aplikasi-ta.ekohomelab.online/RyzaNativeAndroid-debug.apk`
