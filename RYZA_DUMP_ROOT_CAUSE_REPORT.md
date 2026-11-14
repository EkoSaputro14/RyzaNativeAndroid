# RYZA DUMP ROOT CAUSE REPORT
**Analisis Komparatif: Mengapa Log Existing Berhasil Sementara Forensic Dump Sebelumnya Gagal**

---

## 1. Perbedaan Mendasar Jalur I/O

| Aspek | Sistem Log Aplikasi Existing (`botwrecomp-log.txt`) | Forensic Dump Lama (`ryza_forensic_dump.txt`) |
|---|---|---|
| **Eksekusi I/O** | Native code memanggil Java via JNI (`openLog`), Java meminta Android OS membuat file via SAF / App-specific File, lalu menduplikasi File Descriptor (`dup fd`) dan mengembalikannya ke C. | Native C langsung mengeksekusi `fopen("/sdcard/Download/ryza_forensic_dump.txt", "w")`. |
| **Izin Android** | **Legal & Kompatibel** dengan kebijakan Scoped Storage Android 10, 11, 12, 13, dan 14. | **Ilegal di Android 10+**. Menghasilkan `errno = 13 (EACCES: Permission Denied)` karena path `/sdcard/Download/` tidak dapat ditulis langsung via native POSIX tanpa ContentResolver MediaStore / SAF. |
| **Fallback Path** | Menggunakan `context.getExternalFilesDir(null)` (`/storage/emulated/0/Android/data/com.botwrecomp.android/files/`). | Menggunakan path hardcoded yang salah: `/sdcard/Android/data/com.retro.botwrecompandroid/files/`. Package name berbeda sehingga folder tidak pernah ada (`errno = 2, ENOENT`). |
| **Error Handling** | Menggunakan try-catch komprehensif di Java, logging ke logcat, dan fallback otomatis antar storage tier. | `if (!f) return;` secara senyap tanpa mencatat status error atau menginformasikan HUD. |
| **Registrasi Berkas** | Terdaftar dalam URI SAF persisted permissions dan terbaca oleh File Manager di folder yang dipilih pengguna. | Terisolasi di memori native dan tidak terdaftar di ContentResolver / SAF mana pun. |

---

## 2. Analisis Cache Build Lama ("798e4b5-diag")

- **Gejala:** Pengguna melaporkan HUD masih menampilkan `BUILD: 798e4b5-diag` meskipun commit Git lokal sudah berada di `d74432b` atau `8640960`.
- **Root Cause:**
  1. Gradle Android NDK / CMake menggunakan timestamp hashing pada target source. Jika source C++ di luar direktori `app/src/main/cpp/` diubah tanpa menyentuh file proyek Android, Ninja menganggap target `libmain.so` up-to-date (`UP-TO-DATE`).
  2. `versionCode` APK tidak dinaikkan secara eksplisit. Android PackageInstaller menolak menimpa native shared library jika `versionCode` sama.
- **Solusi Permanen yang Diterapkan:**
  1. Menaikkan `versionCode` ke `37` (`versionName = 0.3.2-forensic-p4`).
  2. Menyisipkan definisi `-DRYZA_BUILD_COMMIT=\"c0f3ea7\"` langsung di `CMakeLists.txt` sehingga setiap perubahan commit Git memicu relink otomatis pada `libmain.so`.

---

## 3. Resolusi: Unifikasi Arsitektur

Dengan refactoring di commit `c0f3ea7`, `ryza_forensics.c` **TIDAK LAGI** memiliki jalur I/O terpisah.
Ketika tombol `[Force Dump Log Now]` ditekan atau auto-dump terpicu:
1. Native runtime memanggil `android_open_log_for_write("ryza_forensic_dump.txt", false)`.
2. Java bridge menjalankan `BOTWRecompAndroidActivity.openLogFile("ryza_forensic_dump.txt", false)`.
3. File dibuat di folder SAF yang dipilih pengguna (atau App-Specific External Dir jika belum memilih).
4. File descriptor diteruskan ke C, dibuka via `fdopen(fd, "a")`, data diserialisasi, di-flush, dan ditutup dengan aman.
5. Berkas langsung muncul di daftar `Active Log Files` di menu Settings dan siap di-Share / Export / View.
