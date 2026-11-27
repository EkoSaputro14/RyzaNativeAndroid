# Contributing to RyzaNativeAndroid

Thank you for helping improve **RyzaNativeAndroid** (*Atelier Ryza: Ever Darkness & the Secret Hideout* on Android via Native ARM64 execution / static recompilation and NVN-to-Vulkan translation). Bug reports, real-device hardware benchmarks, and focused code contributions are all welcome.

> [!NOTE]
> Parts of this codebase were authored and iterated with AI engineering assistance under human architectural direction, hardware profiling, and test verification. Human-written and AI-assisted contributions are equally welcome as long as every change is tested, minimal, and follows the guidelines below.

---

## Hard Rule: No Game Files or Copyrighted Assets

Never commit, upload, or attach any copyrighted game files or proprietary assets owned by **Koei Tecmo Games / Gust** or **Nintendo** to commits, issues, or pull requests:

- No game dumps (`Dump/`), `exefs`, `romfs`, NSP/XCI containers, or `prod.keys`.
- No extracted assets (`extracted/`), generated guest source files (`generated*/`), Switch fonts (`.bfttf` / `.ttf`), textures, 3D models, audio, shaders from dumps, or event/map scripts — not even a single sample file for testing.

Pull requests containing copyrighted game files or assets cannot be merged. Screenshots and short gameplay clips attached to bug reports or performance comparisons are welcome.

---

## Reporting Bugs & Benchmarks

Open an issue on **[EkoSaputro14/RyzaNativeAndroid Issues](https://github.com/EkoSaputro14/RyzaNativeAndroid/issues)** and include:

1. **Device & OS**: phone model, SoC, Android version (e.g., POCO X3 NFC, Snapdragon 732G, Android 10/11+).
2. **GPU & Vulkan Driver**: GPU name and exact driver (stock Qualcomm vs Mesa Turnip version).
3. **Game Version & Execution Mode**: Native (`EL0`) vs Recompiled, plus whether Expansion/DLC content is loaded.
4. **Configuration**: relevant values from `botwrecompandroid.ini` (`render_scale`, `shadow_scale`, `frame_gen`, `fast_depth`, `fp16_shaders`, `sync_lag`).
5. **Logs & Telemetry**: capture logs via **Settings > Files > Save a log file** or `adb logcat` (note: internal logcat tag `BOTWRecompAndroid` is kept for compatibility), plus `gpuprof_last.txt` if reporting a rendering or GPU performance regression.

---

## Building & Testing

Android (`arm64-v8a`, Vulkan) is the only application target.

### Android APK Build
With Android SDK/NDK installed and third-party libraries built in `android/deps/out` (see `android/deps/build_deps.sh`):

```bash
cd android/botwrecompandroid
./gradlew assembleDebug     # Debug APK (gradlew.bat on Windows)
./gradlew assembleRelease   # Release APK
```

> [!NOTE]
> Legacy path and identifier names (`com.botwrecomp.android`, `android/botwrecompandroid/`, `botwrecompandroid.ini`, `LARECOMP_*`) are retained intentionally so existing user saves, INI configurations, and build wiring do not break.

### Host Regression & Shader Checks (Required Before PR)
Run the Python architectural test suite, C unit tests, and `glslc` SPIR-V validation whenever touching runtime, Maxwell translation, or Vulkan code:

```bash
python3 companion/tests/test_ryza_fixes.py
gcc -O2 -Iruntime companion/tests/test_ryza_fixes.c -o /tmp/test_ryza_fixes_c -lm && /tmp/test_ryza_fixes_c
```

For shader translator changes (`runtime/nvn/maxwell/translate.c`), also verify any new GLSL constructs compile cleanly with `/usr/bin/glslc --target-env=vulkan1.3`.

### On-Device Verification Guidelines
- Test on real Android hardware and inspect screenshots visually for rendering artifacts.
- Verify zero shader/pipeline compile failures (`logcat`) and inspect `[vk]`, `[sub-diag]`, and on-demand per-pass `[gpuprof]` (`touch .../files/gpuprof_once` -> `gpuprof_last.txt`) metrics.
- Document A/B toggles via `getenv("LARECOMP_...")` where defined, and keep test saves isolated using `LARECOMP_SAVE_DIR` when needed.
- Any state scoped to a single guest boot must reset cleanly through `RT_ON_RESET` (`runtime/hle.h`).

---

## Project Layout

| Path | Role |
| :--- | :--- |
| `runtime/` | Core runtime: loader, kernel/thread HLE (`kernel.c`, `loader.c`, `services/`) |
| `runtime/hle/` | High-level Nintendo Switch SDK & KTGL game hooks (`game.c`, audio, input, display, fonts) |
| `runtime/nvn/maxwell/` | Maxwell bytecode decoder and GLSL/SPIR-V translator (`translate.c`, `shader.h`) |
| `runtime/nvn/vulkan/` | Vulkan backend, NVN command recording, memory mirrors, async queue, FrameGen, shader/pipeline caches |
| `runtime/ui/` | ImGui configuration overlay, performance HUD, custom Vulkan driver picker, touch controls |
| `android/botwrecompandroid/` | Android Gradle project, JNI entrypoints, CMake native build (`libmain.so`) |
| `companion/tests/` | Host regression suites (`test_ryza_fixes.py`, `test_ryza_fixes.c`) |
| `tools/` | Recompiler tooling and patch utilities (no game outputs committed) |

---

## Code Style & Pull Request Guidelines

- **Keep PRs focused**: target one clear fix, optimization, or feature per pull request.
- **Match surrounding style**: preserve existing C/C++/Objective-C++ formatting, naming, and concise comments explaining **why** (hardware quirks, KTGL behavior, synchronization invariants), not **what**.
- **Scope cleanly**: keep generic NVN/Vulkan/HLE engine improvements modular so upstream fixes remain easy to trace and port.
- **Verify before submitting**: include host test results and hardware test summary (device, Vulkan driver, `render_scale`/`shadow_scale`, FPS/GPU telemetry) in the PR description.
