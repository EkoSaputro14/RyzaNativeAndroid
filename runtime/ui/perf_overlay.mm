// Performance overlay on Android (the in-game menu's Show FPS, ui/menu.mm; on by default there), like the ones Android
// emulators show: frame rate and frame time with a graph, CPU clusters and this app's CPU share, the GPU and its driver
// and how much of the time the CPU waits on it (apps can't read the GPU's own load), memory, temperatures, battery
// power and Android's thermal state (runtime/platform/android.c plat_device_stats; the battery and thermal state come
// from the activity, BOTWRecompAndroidActivity.reportDeviceStats). Full, or compact (one line); a finger drags it
// anywhere on the screen (ui/touch_controls.mm, g_settings.overlay_pos) - LASRecompAndroid's.
#ifdef __ANDROID__
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <algorithm>

#include "imgui.h"

extern "C" {
extern int g_exec_mode;
extern uint64_t g_bench_native_calls, g_bench_recomp_calls;
extern uint64_t g_bench_native_ns, g_bench_recomp_ns;
extern uint64_t g_forensic_dispatches, g_forensic_dcache_hits, g_forensic_dcache_misses;
extern uint64_t g_forensic_fallbacks, g_forensic_hle_calls, g_forensic_svc_calls;
extern uint64_t g_forensic_dispatch_overhead_ns;
void vk_get_forensic_stats(double *cpu_exec_ms, double *gpu_ms, uint64_t *draws);
void vk_get_frame_split_stats(double *guest_exec_ms, double *sync_wait_ms, double *present_ms, double *copy_fault_ms);
#include "platform.h"
#include "../settings.h"
uint64_t vk_cum_wait_ns(void);  // vulkan/exec.c
const char *vk_gpu_desc(void);  // vulkan/device.c
#include "../nvn/vulkan/ryza_forensics.h"
#include "../nvn/vulkan/precompute.h"
}

#define FT_N 120  // frame times in the graph

static ImVec4 grade(float v, float good, float ok) {  // green / yellow / red
    return v >= good ? ImVec4(0.45f, 0.95f, 0.45f, 1) : v >= ok ? ImVec4(1.0f, 0.85f, 0.3f, 1) : ImVec4(1.0f, 0.4f, 0.35f, 1);
}

// Where it was last drawn (pixels: x, y, w, h): ui/touch_controls.mm drags it with a finger (g_settings.overlay_pos)
float g_perf_rect[4];

static void place(ImVec2 display, float scale) {  // its saved place, kept on the screen at its last size
    ImVec2 p(16 * scale, 16 * scale);
    if (g_settings.overlay_pos[0] >= 0 && g_settings.overlay_pos[1] >= 0)
        p = ImVec2(g_settings.overlay_pos[0] * display.x, g_settings.overlay_pos[1] * display.y);
    p.x = fminf(fmaxf(p.x, 0), fmaxf(display.x - g_perf_rect[2], 0));
    p.y = fminf(fmaxf(p.y, 0), fmaxf(display.y - g_perf_rect[3], 0));
    ImGui::SetNextWindowPos(p);
}

static void record(void) {
    ImVec2 p = ImGui::GetWindowPos(), z = ImGui::GetWindowSize();
    g_perf_rect[0] = p.x, g_perf_rect[1] = p.y, g_perf_rect[2] = z.x, g_perf_rect[3] = z.y;
}

void perf_overlay_draw(ImVec2 display, float scale) {
    // frame times: this runs once per presented frame
    static float ft[FT_N];
    static int nft, head;
    static uint64_t last_ns;
    uint64_t now = SDL_GetTicksNS();
    if (last_ns) {
        ft[head] = (float)((now - last_ns) / 1e6);
        head = (head + 1) % FT_N;
        if (nft < FT_N) nft++;
    }
    last_ns = now;

    // the rest twice a second
    static uint64_t next_ns, t0, wait0;
    static int frames;
    static float fps, avg_ms, max_ms, wait_pct;
    static float p50_ms, p95_ms, p99_ms;
    static double s_overlay_cpu_exec_ms, s_overlay_gpu_ms;
    static double s_overlay_guest_ms, s_overlay_sync_ms, s_overlay_present_ms, s_overlay_copy_ms;
    static uint64_t s_overlay_draws;
    static PlatDeviceStats ds;
    frames++;
    if (now >= next_ns) {
        uint64_t w = vk_cum_wait_ns();
        if (t0) {
            double dt = (now - t0) / 1e9;
            fps = (float)(frames / dt);
            wait_pct = (float)((w - wait0) / 1e9 / dt * 100.0);
            if (wait_pct > 100) wait_pct = 100;
        }
        wait0 = w;
        frames = 0;
        t0 = now;
        next_ns = now + 500000000ull;
        float s = 0, mx = 0;
        for (int i = 0; i < nft; i++) { s += ft[i]; if (ft[i] > mx) mx = ft[i]; }
        avg_ms = nft ? s / nft : 0;
        max_ms = mx;

        if (nft > 0) {
            float sorted[FT_N];
            for (int i = 0; i < nft; i++) sorted[i] = ft[i];
            std::sort(sorted, sorted + nft);
            p50_ms = sorted[(int)(nft * 0.50f)];
            p95_ms = sorted[(int)(nft * 0.95f)];
            p99_ms = sorted[(int)(nft * 0.99f)];
        }
        vk_get_forensic_stats(&s_overlay_cpu_exec_ms, &s_overlay_gpu_ms, &s_overlay_draws);
        vk_get_frame_split_stats(&s_overlay_guest_ms, &s_overlay_sync_ms, &s_overlay_present_ms, &s_overlay_copy_ms);
        plat_device_stats(&ds);
    }

    place(display, scale);
    ImGui::SetNextWindowBgAlpha(0.55f);
    if (g_settings.overlay_style == 1) {  // compact: one line - FPS, the app's RAM, the SoC's and the battery's temperature
        ImGui::Begin("##perf", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                                         ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
        ImGui::SetWindowFontScale(0.62f);
        ImGui::TextColored(grade(fps, 55, 28), "FPS %.0f", fps);
        {
            const char *ms = g_exec_mode == 1 ? "Recomp" : g_exec_mode == 2 ? "Hybrid" : "Native";
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.4f, 0.85f, 1.0f, 1.0f), "[%s]", ms);
        }
        ImGui::SameLine();
        ImGui::Text("  RAM %u MB", ds.rss_mb);
        if (ds.soc_temp_c > 0) {
            ImGui::SameLine();
            ImGui::TextColored(grade(100 - ds.soc_temp_c, 35, 20), "  SoC %.0f\xc2\xb0" "C", ds.soc_temp_c);
        }
        if (ds.batt_temp_c > 0) {
            ImGui::SameLine();
            ImGui::TextColored(grade(100 - ds.batt_temp_c, 60, 55), "  Bat %.0f\xc2\xb0" "C", ds.batt_temp_c);
        }
        record();
        ImGui::End();
        return;
    }
    ImGui::Begin("##perf", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::SetWindowFontScale(0.62f);

    ImGui::TextColored(grade(fps, 55, 28), "%.0f FPS", fps);
    {
        const char *ms = g_exec_mode == 1 ? "Recomp" : g_exec_mode == 2 ? "Hybrid" : "Native";
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.4f, 0.85f, 1.0f, 1.0f), "[%s]", ms);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%.1f ms", fps > 0 ? 1000.0f / fps : 0.0f);
    float graph[FT_N];  // (oldest first)
    for (int i = 0; i < nft; i++) graph[i] = ft[(head - nft + i + FT_N) % FT_N];
    ImGui::PushStyleColor(ImGuiCol_PlotLines, grade(fps, 55, 28));
    ImGui::PlotLines("##ft", graph, nft, 0, NULL, 0.0f, 100.0f, ImVec2(260 * scale, 44 * scale));
    ImGui::PopStyleColor();
    ImGui::TextDisabled("frame avg %.1f ms  max %.0f ms", avg_ms, max_ms);
    ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.9f, 1.0f), "P50: %.1f ms | P95: %.1f ms | P99: %.1f ms", p50_ms, p95_ms, p99_ms);
    ImGui::Text("Render: CPU %.1f ms | GPU %.1f ms (%llu draws)", s_overlay_cpu_exec_ms, s_overlay_gpu_ms, (unsigned long long)s_overlay_draws);
    ImGui::Text("Frame split: game %.1f ms | sync %.1f ms | present %.1f ms | copy %.1f ms",
                s_overlay_guest_ms, s_overlay_sync_ms, s_overlay_present_ms, s_overlay_copy_ms);
    {
        uint64_t total_dc = g_forensic_dcache_hits + g_forensic_dcache_misses;
        double hit_rt = total_dc ? (g_forensic_dcache_hits * 100.0 / total_dc) : 100.0;
        ImGui::Text("Path: %s | RecCalls: %llu | Disp: %llu (%.1f%% hit)",
                    g_exec_mode == 1 ? "Recomp C" : g_exec_mode == 2 ? "Hybrid" : "Native EL0",
                    (unsigned long long)g_bench_recomp_calls,
                    (unsigned long long)g_forensic_dispatches,
                    hit_rt);
    }

    char cpu[96] = "";
    for (int i = 0, n = 0; i < ds.ncluster && n < (int)sizeof cpu - 8; i++)
        n += snprintf(cpu + n, sizeof cpu - n, "%s%u", i ? " / " : "", ds.cluster_mhz[i]);
    ImGui::Text("CPU %s MHz", cpu);
    ImGui::SameLine();
    ImGui::TextDisabled("app %.0f%%", ds.cpu_pct);

    PlatAdpfStats adpf;
    plat_adpf_stats(&adpf);
    if (adpf.state > 0 && adpf.threads > 0) {
        ImGui::TextColored(grade(adpf.target_ms - adpf.work_ms, 3.0f, 0.0f), "ADPF on (%d thr%s)",
                           adpf.threads, adpf.sessions > 1 ? ", split" : "");
        if (adpf.errors > 0) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.35f, 1), "err %d (%d)", adpf.errors, adpf.last_error);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("tgt %.1f ms", adpf.target_ms);
        if (adpf.render_ms > 0) {
            ImGui::TextDisabled("    game %.1f ms (cpu %d)  render %.1f ms (cpu %d)",
                                adpf.game_ms, adpf.game_cpu, adpf.render_ms, adpf.render_cpu);
        } else {
            ImGui::TextDisabled("    game %.1f ms (cpu %d)", adpf.game_ms, adpf.game_cpu);
        }
    } else if (adpf.state == 0) {
        ImGui::TextDisabled("ADPF: init...");
    } else {
        ImGui::TextDisabled("ADPF: %s (nice -10)", adpf.state == -2 ? "disabled" : "unavailable");
    }
    ImGui::Text("GPU %s", vk_gpu_desc());
    ImGui::TextColored(grade(100 - wait_pct, 50, 20), "    waiting on the GPU %.0f%%", wait_pct);
    ImGui::Text("RAM app %u MB", ds.rss_mb);
    ImGui::SameLine();
    ImGui::TextDisabled("free %u MB", ds.avail_mb);

    if (ds.soc_temp_c > 0) {
        ImGui::TextColored(grade(100 - ds.soc_temp_c, 35, 20), "SoC %.0f\xc2\xb0" "C", ds.soc_temp_c);
        ImGui::SameLine();
    }
    if (ds.batt_temp_c > 0) ImGui::TextColored(grade(100 - ds.batt_temp_c, 60, 55), "Battery %.0f\xc2\xb0" "C", ds.batt_temp_c);
    else ImGui::NewLine();
    if (ds.batt_pct >= 0) {
        if (ds.watts > 0) { ImGui::Text("Power %.1f W", ds.watts); ImGui::SameLine(); }
        ImGui::TextDisabled("battery %.0f%%", ds.batt_pct);
    }
    static const char *const thermal[] = {"normal", "light", "moderate", "severe", "critical", "emergency", "shutdown"};
    if (ds.thermal >= 0 && ds.thermal <= 6) {
        ImGui::TextColored(grade((float)(6 - ds.thermal), 6, 4), "Thermal: %s", thermal[ds.thermal]);
        if (ds.headroom > 0) {
            ImGui::SameLine();
            ImGui::TextDisabled("(headroom %.2f)", ds.headroom);
        }
    }

    ImGui::Separator();
    if (ImGui::CollapsingHeader("Performance Tuning (botwrecompandroid.ini)")) {
        bool ini_dirty = false;
        ImGui::Text("sync_lag:");
        ImGui::SameLine();
        for (int sl = 0; sl <= 2; sl++) {
            char lbl[16];
            snprintf(lbl, sizeof lbl, "%d##sl", sl);
            bool sel = (g_settings.sync_lag == sl);
            if (sel) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.55f, 0.30f, 1.0f));
            if (ImGui::SmallButton(lbl)) { g_settings.sync_lag = sl; ini_dirty = true; }
            if (sel) ImGui::PopStyleColor();
            if (sl < 2) ImGui::SameLine();
        }
        ImGui::SameLine();
        ImGui::Text("| swap_imgs:");
        ImGui::SameLine();
        for (int si = 3; si <= 5; si++) {
            char lbl[16];
            snprintf(lbl, sizeof lbl, "%d##si", si);
            bool sel = (g_settings.swapchain_images == si);
            if (sel) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.55f, 0.30f, 1.0f));
            if (ImGui::SmallButton(lbl)) { g_settings.swapchain_images = si; ini_dirty = true; }
            if (sel) ImGui::PopStyleColor();
            if (si < 5) ImGui::SameLine();
        }

        static const char *const pm_lbls[] = {"Imm", "Mailbox", "FIFO"};
        ImGui::Text("present:");
        ImGui::SameLine();
        for (int pm = 0; pm <= 2; pm++) {
            char lbl[24];
            snprintf(lbl, sizeof lbl, "%s##pm", pm_lbls[pm]);
            bool sel = (g_settings.present_mode == pm);
            if (sel) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.55f, 0.30f, 1.0f));
            if (ImGui::SmallButton(lbl)) { g_settings.present_mode = pm; ini_dirty = true; }
            if (sel) ImGui::PopStyleColor();
            if (pm < 2) ImGui::SameLine();
        }
        ImGui::SameLine();
        ImGui::Text("| upscaler:");
        ImGui::SameLine();
        static const struct { const char *name; int val; } up_opts[] = {{"Off", UPSCALE_BILINEAR}, {"Sharp", UPSCALE_FSR1}, {"SGSR", UPSCALE_SGSR1}};
        for (int u = 0; u < 3; u++) {
            char lbl[24];
            snprintf(lbl, sizeof lbl, "%s##up", up_opts[u].name);
            bool sel = (g_settings.upscaler == up_opts[u].val);
            if (sel) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.55f, 0.30f, 1.0f));
            if (ImGui::SmallButton(lbl)) { g_settings.upscaler = up_opts[u].val; ini_dirty = true; }
            if (sel) ImGui::PopStyleColor();
            if (u < 2) ImGui::SameLine();
        }

        static const float rs_presets[] = {0.50f, 0.60f, 0.68f, 0.75f, 1.00f};
        ImGui::Text("render_scale:");
        ImGui::SameLine();
        for (int i = 0; i < 5; i++) {
            char lbl[20];
            snprintf(lbl, sizeof lbl, "%.2fx##rs", rs_presets[i]);
            bool sel = fabsf(g_settings.render_scale - rs_presets[i]) < 0.02f;
            if (sel) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.55f, 0.30f, 1.0f));
            if (ImGui::SmallButton(lbl)) { g_settings.render_scale = rs_presets[i]; ini_dirty = true; }
            if (sel) ImGui::PopStyleColor();
            if (i < 4) ImGui::SameLine();
        }

        static const float sh_presets[] = {0.00f, 0.25f, 0.50f, 1.00f};
        ImGui::Text("shadow_scale:");
        ImGui::SameLine();
        for (int i = 0; i < 4; i++) {
            char lbl[20];
            if (sh_presets[i] == 0.00f) snprintf(lbl, sizeof lbl, "Off##sh");
            else snprintf(lbl, sizeof lbl, "%.2fx##sh", sh_presets[i]);
            bool sel = fabsf(g_settings.shadow_scale - sh_presets[i]) < 0.02f;
            if (sel) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.55f, 0.30f, 1.0f));
            if (ImGui::SmallButton(lbl)) { g_settings.shadow_scale = sh_presets[i]; ini_dirty = true; }
            if (sel) ImGui::PopStyleColor();
            if (i < 3) ImGui::SameLine();
        }
        ImGui::SameLine();
        ImGui::Text("| fp16:");
        ImGui::SameLine();
        if (ImGui::SmallButton(g_settings.fp16_shaders ? "ON##fp16" : "OFF##fp16")) {
            g_settings.fp16_shaders = g_settings.fp16_shaders ? 0 : 1;
            ini_dirty = true;
        }
        ImGui::SameLine();
        ImGui::Text("| skip_postfx:");
        ImGui::SameLine();
        if (ImGui::SmallButton(g_settings.skip_postfx ? "ON##postfx" : "OFF##postfx")) {
            g_settings.skip_postfx = g_settings.skip_postfx ? 0 : 1;
            ini_dirty = true;
        }
        ImGui::SameLine();
        ImGui::Text("| gpu_unsw:");
        ImGui::SameLine();
        if (ImGui::SmallButton(g_settings.gpu_unswizzle ? "ON##gunsw" : "OFF##gunsw")) {
            g_settings.gpu_unswizzle = g_settings.gpu_unswizzle ? 0 : 1;
            ini_dirty = true;
        }

        static const int tf_presets[] = {0, 20, 30, 45, 60};
        ImGui::Text("target_fps:");
        ImGui::SameLine();
        for (int i = 0; i < 5; i++) {
            char lbl[20];
            if (tf_presets[i] == 0) snprintf(lbl, sizeof lbl, "Off##tf");
            else snprintf(lbl, sizeof lbl, "%d##tf", tf_presets[i]);
            bool sel = (g_settings.target_fps == tf_presets[i]);
            if (sel) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.55f, 0.30f, 1.0f));
            if (ImGui::SmallButton(lbl)) { g_settings.target_fps = tf_presets[i]; ini_dirty = true; }
            if (sel) ImGui::PopStyleColor();
            if (i < 4) ImGui::SameLine();
        }

        static const int hm_presets[] = {2048, 2560, 3072, 3328};
        ImGui::Text("heap_mb:");
        ImGui::SameLine();
        for (int i = 0; i < 4; i++) {
            char lbl[20];
            snprintf(lbl, sizeof lbl, "%d##hm", hm_presets[i]);
            bool sel = (g_settings.heap_mb == hm_presets[i]);
            if (sel) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.55f, 0.30f, 1.0f));
            if (ImGui::SmallButton(lbl)) { g_settings.heap_mb = hm_presets[i]; ini_dirty = true; }
            if (sel) ImGui::PopStyleColor();
            if (i < 3) ImGui::SameLine();
        }
        if (ini_dirty) settings_save();
    }
    ImGui::Separator();
    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "BUILD: %s", ryza_get_build_fingerprint());
    ImGui::TextColored(ImVec4(0.4f, 0.9f, 1.0f, 1.0f), "3D Draws: %llu | Char: %llu | Captured: %u/32",
                       (unsigned long long)g_ryza_3d_draw_count,
                       (unsigned long long)g_ryza_character_draw_count,
                       g_ryza_captured_draw_count);
    {
        char pbuf[160];
        PrecomputeSnapshot ps;
        precompute_get_loading_progress(NULL, pbuf, sizeof(pbuf));
        precompute_get_snapshot(&ps);
        ImGui::TextDisabled("%s | Shaders: %u/%u | Pipes: %u",
                            pbuf, ps.shaders_compiled, ps.shaders_registered, ps.pipelines_manifest_total);
    }

    // ACTION BUTTONS
    if (ImGui::Button("Force Dump Log Now")) {
        ryza_export_diagnostic_log();
    }
    ImGui::SameLine();
    if (ImGui::Button("TEST FORENSIC FILE WRITE")) {
        ryza_run_file_write_test();
    }
    ImGui::SameLine();
    if (ImGui::Button(g_ryza_capture_freeze ? "Unfreeze" : "Freeze")) {
        g_ryza_capture_freeze = !g_ryza_capture_freeze;
    }
    ImGui::SameLine();
    if (ImGui::Button(g_ryza_forensic_enabled ? "Capture: ON" : "Capture: OFF")) {
        ryza_forensics_set_enabled(!g_ryza_forensic_enabled);
    }
    ImGui::SameLine();
    if (ImGui::Button("Re-arm")) {
        ryza_rearm_capture();
    }

    // FORCE DUMP DIAGNOSTIC STATUS
    if (g_ryza_dump_result.status == RYZA_DUMP_STATUS_RUNNING) {
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "FORCE DUMP: RUNNING...");
    } else if (g_ryza_dump_result.status == RYZA_DUMP_STATUS_SUCCESS) {
        ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), "FORCE DUMP: SUCCESS (%zu bytes)", g_ryza_dump_result.bytes_written);
        ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "  Path: %s", g_ryza_dump_result.target_path);
        ImGui::TextDisabled("  Time: %s", g_ryza_dump_result.timestamp);
    } else if (g_ryza_dump_result.status == RYZA_DUMP_STATUS_FAILED) {
        ImGui::TextColored(ImVec4(1.0f, 0.25f, 0.25f, 1.0f), "FORCE DUMP: FAILED (Err %d: %s)",
                           g_ryza_dump_result.error_code, g_ryza_dump_result.error_msg);
        ImGui::TextDisabled("  Target: %s | Written: %zu", g_ryza_dump_result.target_path, g_ryza_dump_result.bytes_written);
    } else {
        ImGui::TextDisabled("FORCE DUMP: IDLE");
    }

    // SELF-TEST DIAGNOSTIC STATUS
    if (g_ryza_test_result.ran) {
        if (g_ryza_test_result.passed) {
            ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), "STORAGE TEST: PASS (Wrote: %zu, Read: %zu)",
                               g_ryza_test_result.bytes_written, g_ryza_test_result.bytes_read);
            ImGui::TextDisabled("  Test Path: %s", g_ryza_test_result.target_path);
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.25f, 0.25f, 1.0f), "STORAGE TEST: FAIL (Err %d: %s)",
                               g_ryza_test_result.error_code, g_ryza_test_result.error_msg);
            ImGui::TextDisabled("  Target: %s", g_ryza_test_result.target_path);
        }
    }

#if defined(__ANDROID__)
    if (g_ryza_dump_result.status == RYZA_DUMP_STATUS_SUCCESS) {
        if (ImGui::Button("Share Dump File")) {
            android_share_log_file("ryza_forensic_dump.txt");
        }
        ImGui::SameLine();
        if (ImGui::Button("Export Dump File")) {
            android_export_log_file("ryza_forensic_dump.txt");
        }
    }
#endif

    if (g_ryza_captured_draw_count > 0) {
        static int selected_draw = -1;
        // Cari slot pertama yang berisi karakter atau mesh terdeformasi agar tidak menampilkan environment kosong
        if (selected_draw < 0 || selected_draw >= (int)g_ryza_captured_draw_count) {
            selected_draw = 0;
            for (uint32_t i = 0; i < g_ryza_captured_draw_count; i++) {
                if (g_ryza_captured_draws[i].is_valid && (g_ryza_captured_draws[i].is_character || g_ryza_captured_draws[i].is_deformed)) {
                    selected_draw = (int)i;
                    break;
                }
            }
        }

        ImGui::SliderInt("Submesh##rdraw", &selected_draw, 0, (int)g_ryza_captured_draw_count - 1);
        RyzaDrawForensic *d = &g_ryza_captured_draws[selected_draw];
        ImGui::TextColored(d->is_deformed ? ImVec4(1.0f, 0.2f, 0.2f, 1.0f) :
                           (d->is_character ? ImVec4(0.2f, 1.0f, 0.4f, 1.0f) : ImVec4(1.0f, 0.5f, 0.8f, 1.0f)),
                           "[%s]", d->body_part);
        ImGui::TextDisabled("Frame %llu Draw #%u | VS: %llx FS: %llx",
                            (unsigned long long)d->frame, d->draw_idx,
                            (unsigned long long)d->vs_code, (unsigned long long)d->fs_code);
        ImGui::Text("Indices: %u (range %u..%u) | AttrMask: 0x%08x",
                    d->icount, d->min_idx, d->max_idx, d->attr_mask);

        for (uint32_t a = 0; a < 8; a++) {
            if (d->attr_mask & (1u << a)) {
                ImGui::TextDisabled("A%u: strm=%u off=%u strd=%u fmt=0x%02x k=%u raw=0x%08x",
                                    a, d->attrs[a].stream, d->attrs[a].off, d->attrs[a].stride,
                                    d->attrs[a].nvn_fmt, d->attrs[a].kind, d->attrs[a].raw_data[0]);
            }
        }

        if (d->num_samples > 0) {
            RyzaVertexSample *s = &d->samples[0];
            ImGui::Text("Vtx #%u Pos: (%.2f, %.2f, %.2f)", s->vertex_idx, s->pos[0], s->pos[1], s->pos[2]);
            ImGui::Text("Bones: [%u, %u, %u, %u] W: (%.2f, %.2f, %.2f, %.2f)",
                        s->bone_indices[0], s->bone_indices[1], s->bone_indices[2], s->bone_indices[3],
                        s->bone_weights[0], s->bone_weights[1], s->bone_weights[2], s->bone_weights[3]);
            ImGui::TextColored((s->w_valid && !s->is_corrupted) ? ImVec4(0.4f, 1.0f, 0.4f, 1.0f) : ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
                               "ClipPos: (%.2f, %.2f, %.2f, W=%.2f) X/W=%.2f%s",
                               s->clip_pos[0], s->clip_pos[1], s->clip_pos[2], s->clip_pos[3],
                               s->clip_pos[3] != 0.0f ? s->clip_pos[0] / s->clip_pos[3] : 9999.0f,
                               s->is_corrupted ? " [CORRUPT]" : "");
        }
    }
    record();
    ImGui::End();
}
#endif
