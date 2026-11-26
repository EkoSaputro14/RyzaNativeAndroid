#include "settings.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hle.h"
#include "input_bind.h"
#ifdef __ANDROID__
#include <SDL3/SDL.h>
#include <sys/system_properties.h>
#endif

#define SETTINGS_FILE settings_file()

// LARECOMP_SETTINGS=<path>: another settings file (test runs never touch the player's larecomp.ini)
static const char *settings_file(void) {
    const char *e = getenv("LARECOMP_SETTINGS");
    if (e && *e) return e;
    static int moved;  // (the project's former name: the player's settings carry over, once)
    if (!moved) {
        moved = 1;
        FILE *f = fopen("botwrecompandroid.ini", "rb");
        if (f) fclose(f);
        else rename("botwrecomp.ini", "botwrecompandroid.ini");
    }
    return "botwrecompandroid.ini";
}

Settings g_settings = {
    .aspect = ASPECT_UNLOCKED,
    .fullscreen = false,
    .volume = 1.0f,
    .reverb = true,
    .movie_audio = true,
    .motion = true,
#ifdef __ANDROID__
    .show_fps = true,  // (the performance overlay, ui/perf_overlay.mm: on until turned off in the menu)
    .touch_controls = true,
    .overlay_pos = {-1, -1},
    .touch_pos = {{-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1},
                  {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}},
    .touch_scheme = 1,
    .touch_pos2 = {{-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1},
                   {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1},
                   {-1, -1}, {-1, -1}, {-1, -1}},
    .touch_scale = 1.0f,
    .touch_opacity = 1.0f,
    .touch_drag_sens = 1.0f,
    .touch_aim_sens = 1.0f,
    .touch_talk = true,
    .gpu_heap = -1,
    .inplace_reads = -1,
    .turnip_lrz = -1,
    .event_fps_unlock = false,  // (some events count frames: hle/framepace.c in_event)
    .fps_mode = 1,  // (mhrise-arch v2: 30 fps native KTGL step eliminates slow-motion; paired with Auto-FIFO 2x FG for 60Hz smooth)
    .res_mode = 1,
    .render_scale = 0.50f,  // (mhrise-arch v3: 0.50x internal 3D scale ~640x360, cuts Adreno 618 fragment load by 75%)
    .shadow_scale = 0.25f,  // (mhrise-arch exp: 0.25x shadow map scale, reduces shadow fillrate by 93.75%)
#else
    .show_fps = false,
    .render_scale = 1.0f,
    .shadow_scale = 1.0f,
#endif
#ifdef __ANDROID__
    .upscaler = UPSCALE_BILINEAR,  // (configurable in botwrecompandroid.ini: 0/bilinear, 1/fsr1/sharp, 2/sgsr1)
    .native_code = true,       // (an experiment: runtime/native.c)
    .sync_lag = 1,             // (1 = 1-frame CPU/GPU pipelining default; 0 = off, 2 = store-guarded lag)
    .out_lines = 720,          // (720p default: cuts GPU fillrate on Adreno 6xx from 1440p down by 4.8x)
    .heap_mb = 2560,           // (2560 MB default guest heap target on 6 GB Android devices; configurable 1536..3584 MB)
    .swapchain_images = 4,     // (mhrise-arch v2: 4 swapchain images default for smooth Auto-FIFO 2x presentation; configurable 2..5)
    .present_mode = 2,         // (mhrise-arch v2: 2 = FIFO default for even alternating VSync pacing; 0 = Immediate, 1 = Mailbox)
    .fp16_shaders = 1,         // (1 = FP16 RelaxedPrecision fragment shaders on Adreno default; 0 = FP32)
    .async_spin_us = 500,      // (500 us spin-wait in async render/wait loop to keep SD732G clocks up when ADPF unavailable)
    .target_fps = 30,          // (mhrise-arch v2: 30 fps real native game pace; SurfaceFlinger gets 60 Hz when 2x frame_gen is active)
    .skip_postfx = 1,          // (mhrise-arch exp: cull heavy KTGL post-FX passes by default for high FPS)
    .gpu_unswizzle = 0,        // (0 = CPU tegra_deswizzle [safe default], 1 = GPU compute deswizzle)
    .fast_depth = 1,           // (mhrise-arch: Adreno LRZ preservation enabled by default)
    .tight_barriers = 1,       // (mhrise-arch: redundant barrier elision enabled by default on Adreno)
    .barrier_bits = 1,         // (mhrise-recomp: skip unordered barriers & narrow fragment-only barriers on Adreno)
    .tiler_store_none = 1,     // (mhrise-arch: STORE_OP_NONE + pass lookahead + tight renderArea enabled by default)
    .frame_gen = 1,            // (mhrise-arch v2: 2x Light DIS optical-flow FG with Auto-FIFO 30->60 + overload auto-backoff)
    .async_shaders = 1,        // (mhrise-arch v2: non-blocking background graphics shader/pipeline workers; skip 1-2 frames vs stutter)
    .gpu_mirrors = 1,          // (mhrise-arch v2: persistent page-tracked GPU mirrors + 3-ring direct mirrors)
    .gpu_writeback = 0,        // (mhrise-arch v2: mass GPU->CPU RT write-back disabled per mhrise-recomp; explicit copies preserved)
    .profile_ver = 3,          // (mhrise-arch v3: one-time mobile Adreno profile migration)
#else
    .upscaler = UPSCALE_SHARP,  // (MetalFX on macOS, AMD FSR 1 on Windows)
#endif
};

// The game's own text languages: its RomFS carries these per region (regionUS/USen|USes|USfr, regionEU/EUen|EUes|...,
// regionJP/JPja, regionKR/KRko, regionTW/TWzh, regionCN/CNzh), and the game picks one from the system language it
// reads back (runtime/services/am.c), exactly as it does on the console. Codes as the Switch reports them.
// Brazilian Portuguese (regionUS/USpt, Pack/Bootup_USpt.pack) came with the game's v1.8.2 (runtime/language.c in
// SavageWindRecomp); the older versions are told English instead.
// Chinese is zh-Hans / zh-Hant, as GetDesiredLanguage answers since system 4.0 (and Eden): the older zh-CN / zh-TW
// gave Japanese text (issue #15).
// "es" / "fr" give Spain's and France's text (regionEU); Latin America's Spanish and Canada's French are the region US
// ones (Pack/Bootup_USes.pack, Bootup_USfr.pack, in every version), told as es-419 / fr-CA (issue #128).
const char *const g_language_codes[N_LANGUAGES] = {"auto", "ja", "en-US", "es", "fr", "de", "it", "nl", "ru", "ko",
                                                   "zh-Hans", "zh-Hant", "pt-BR", "es-419", "fr-CA"};
const char *const g_language_names[N_LANGUAGES] = {"Auto", "Japanese", "English", "Spanish", "French", "German",
                                                   "Italian", "Dutch", "Russian", "Korean", "Chinese (Simplified)",
                                                   "Chinese (Traditional)", "Portuguese (Brazil)",
                                                   "Spanish (Latin America)", "French (Canada)"};

// The app's own menus (ui/i18n.mm): the game's Language when one is chosen - one setting for both, read once (the menus
// change when the app starts again) - else the phone's first preferred language, which is the app's own when Android
// 13+ has one set for it (res/xml/locales_config.xml)
const char *settings_menu_language(void) {
#ifdef __ANDROID__
    static const char *lang;
    if (!lang) {
        char l[16] = "", country[16] = "";
        const char *e = getenv("LARECOMP_HOST_LANG");
        int chosen = g_settings.language;
        if (!(e && *e) && chosen > LANG_AUTO && chosen < N_LANGUAGES) {
            static const char *const menu[N_LANGUAGES] = {"en", "ja", "en", "es", "fr", "de", "it", "nl", "ru", "ko",
                                                          "zh-Hans", "zh-Hant", "pt", "es", "fr"};  // (g_language_codes' order)
            return lang = menu[chosen];
        }
        if (e && *e) {
            snprintf(l, sizeof l, "%s", e);
            char *dash = strchr(l, '-');
            if (dash) snprintf(country, sizeof country, "%s", dash + 1), *dash = 0;
        } else {  // (the phone's first preferred language, through SDL)
            int n = 0;
            SDL_Locale **loc = SDL_GetPreferredLocales(&n);
            if (loc && n > 0 && loc[0]->language) {
                snprintf(l, sizeof l, "%s", loc[0]->language);
                if (loc[0]->country) snprintf(country, sizeof country, "%s", loc[0]->country);
            }
            SDL_free(loc);
        }
        static const char *const own[] = {"pt", "es", "fr", "de", "it", "nl", "ru", "ja", "ko"};
        lang = "en";
        for (size_t i = 0; i < sizeof own / sizeof *own; i++)
            if (!strcmp(l, own[i])) lang = own[i];
        if (!strcmp(l, "zh"))  // (Taiwan, Hong Kong, Macau: traditional characters)
            lang = !strcmp(country, "TW") || !strcmp(country, "HK") || !strcmp(country, "MO") || !strcmp(country, "Hant")
                       ? "zh-Hant" : "zh-Hans";
    }
    return lang;
#else
    return "en";
#endif
}

bool settings_host_portuguese(void) { return !strcmp(settings_menu_language(), "pt"); }

// LANG_AUTO: the host's own language (LANG=es_ES.UTF-8 -> es), en-US when it is none of the game's. Linux only:
// macOS / Windows apps get no locale variables from the system, so Auto is English there (pick one in settings).
static const char *host_language(void) {
#ifndef __linux__
    return "en-US";
#endif
#ifdef __ANDROID__
    // (Android: no locale variables. The app's preferred language first - the phone's, or the app's own one set in
    // Android 13+'s per-app languages - then the system's as a property, "pt-BR", "es-US", "zh-Hant-TW")
    static char prop[PROP_VALUE_MAX];
    if (!prop[0]) {
        int n = 0;
        SDL_Locale **loc = SDL_GetPreferredLocales(&n);
        if (loc && n > 0 && loc[0]->language)
            snprintf(prop, sizeof prop, "%s%s%s", loc[0]->language, loc[0]->country ? "-" : "",
                     loc[0]->country ? loc[0]->country : "");
        SDL_free(loc);
    }
    if (!prop[0] && __system_property_get("persist.sys.locale", prop) <= 0) __system_property_get("ro.product.locale", prop);
    const char *e = prop;
#else
    const char *e = getenv("LC_ALL");
#endif
#ifndef __ANDROID__
    if (!e || !*e) e = getenv("LC_MESSAGES");
    if (!e || !*e) e = getenv("LANG");
#endif
    if (!e || !*e || !strcmp(e, "C") || !strcmp(e, "POSIX")) return "en-US";
    char l[8] = {0};
    size_t n = 0;
    for (const char *p = e; *p && *p != '.' && *p != '@' && *p != '_' && *p != '-' && n < sizeof l - 1; p++) l[n++] = *p;
    // (the country after the language: "es-MX", "es_AR.UTF-8", "es-419")
    char country[8] = {0};
    const char *sep = e + n;
    if (*sep == '_' || *sep == '-')
        for (size_t k = 0; sep[k + 1] && sep[k + 1] != '.' && sep[k + 1] != '@' && sep[k + 1] != '-' && k < sizeof country - 1; k++)
            country[k] = sep[k + 1];
    if (!strcmp(l, "es")) return country[0] && strcmp(country, "ES") ? "es-419" : "es";  // (Spain's, else the Americas')
    if (!strcmp(l, "fr")) return !strcmp(country, "CA") ? "fr-CA" : "fr";
    static const struct { const char *loc, *code; } map[] = {{"ja", "ja"}, {"ko", "ko"}, {"ru", "ru"}, {"nl", "nl"},
                                                             {"de", "de"}, {"it", "it"}};
    for (size_t i = 0; i < sizeof map / sizeof *map; i++)
        if (!strcmp(l, map[i].loc)) return map[i].code;
    if (!strcmp(l, "zh"))  // (TW / HK, Hant: traditional)
        return strstr(e, "TW") || strstr(e, "HK") || strstr(e, "Hant") ? "zh-Hant" : "zh-Hans";
    if (!strcmp(l, "pt")) return "pt-BR";  // (the only Portuguese the game has)
    return "en-US";
}

const char *settings_language(void) {
    int i = g_settings.language;
    const char *code = i > LANG_AUTO && i < N_LANGUAGES ? g_language_codes[i] : host_language();
    extern int g_game_version;
    if (!strcmp(code, "pt-BR") && g_game_version && g_game_version < 182) return "en-US";  // (no Portuguese before 1.8.2)
    return code;
}

const char *settings_language_name(void) {
    const char *c = settings_language();
    for (int i = 0; i < N_LANGUAGES; i++)
        if (!strcmp(c, g_language_codes[i])) return g_language_names[i];
    return "English";
}

static Settings g_defaults;  // (the defaults above, kept before the file overrides them: settings_reset_defaults)
static bool g_have_defaults;

void settings_reset_defaults(void) {
    if (!g_have_defaults) return;
    const Settings *d = &g_defaults;
    g_settings.aspect = d->aspect;
    g_settings.fullscreen = d->fullscreen;
    g_settings.res_mode = d->res_mode;
    g_settings.render_scale = d->render_scale;
    g_settings.shadow_scale = d->shadow_scale;
    g_settings.upscaler = d->upscaler;
    g_settings.out_lines = d->out_lines;
    g_settings.antialias = d->antialias;
    g_settings.volume = d->volume;
    g_settings.reverb = d->reverb;
    g_settings.movie_audio = d->movie_audio;
    g_settings.show_fps = d->show_fps;
    g_settings.motion = d->motion;
    g_settings.motion_source = d->motion_source;
    g_settings.motion_flat = d->motion_flat;
#ifdef __ANDROID__
    g_settings.touch_controls = d->touch_controls;
    g_settings.overlay_style = d->overlay_style;
    g_settings.fps_mode = d->fps_mode;
    g_settings.event_fps_unlock = d->event_fps_unlock;
    g_settings.shader_int32 = d->shader_int32;
    g_settings.sync_lag = d->sync_lag;
    g_settings.heap_mb = d->heap_mb;
    g_settings.swapchain_images = d->swapchain_images;
    g_settings.present_mode = d->present_mode;
    g_settings.fp16_shaders = d->fp16_shaders;
    g_settings.async_spin_us = d->async_spin_us;
    g_settings.target_fps = d->target_fps;
    g_settings.skip_postfx = d->skip_postfx;
    g_settings.gpu_unswizzle = d->gpu_unswizzle;
    g_settings.fast_depth = d->fast_depth;
    g_settings.tight_barriers = d->tight_barriers;
    g_settings.barrier_bits = d->barrier_bits;
    g_settings.tiler_store_none = d->tiler_store_none;
    g_settings.frame_gen = d->frame_gen;
    g_settings.async_shaders = d->async_shaders;
    g_settings.gpu_mirrors = d->gpu_mirrors;
    g_settings.gpu_writeback = d->gpu_writeback;
    g_settings.profile_ver = d->profile_ver;
    g_settings.native_code = d->native_code;
    g_settings.files_in_place = d->files_in_place;
#endif
}

void settings_load(void) {
    static bool loaded;
    if (!g_have_defaults) g_defaults = g_settings, g_have_defaults = true;
    if (loaded) return;
    loaded = true;
    FILE *f = fopen(SETTINGS_FILE, "r");
    if (!f) {
        settings_save();
        LOG("[settings] created default %s (sync_lag=%d, heap_mb=%d, swapchain_images=%d, present_mode=%d, upscaler=%d, render_scale=%.2f, shadow_scale=%.2f, fp16_shaders=%d)\n",
            SETTINGS_FILE, g_settings.sync_lag, g_settings.heap_mb, g_settings.swapchain_images,
            g_settings.present_mode, g_settings.upscaler, g_settings.render_scale,
            g_settings.shadow_scale, g_settings.fp16_shaders);
        return;
    }
#ifdef __ANDROID__
    char line[640];  // (touch_layout: 24 numbers)
#else
    char line[256];
#endif
    bool tip_key = false, aspect_key = false, output_key = false;
#ifdef __ANDROID__
    int loaded_profile_ver = 0;
#endif
    while (fgets(line, sizeof line, f)) {
        char key[64];
#ifdef __ANDROID__
        char val[512] = "";  // (a key with nothing after "=": mods_off when every mod is on)
        if (sscanf(line, " %63[^= ] = %511[^\r\n]", key, val) < 1) continue; val[strcspn(val, "\r\n")] = 0;
#else
        char val[128] = "";
        if (sscanf(line, " %63[^= ] = %127[^\r\n]", key, val) < 1) continue; val[strcspn(val, "\r\n")] = 0;
#endif
        if (!strcmp(key, "aspect")) g_settings.aspect = !strcmp(val, "original") ? ASPECT_ORIGINAL : ASPECT_UNLOCKED;
        else if (!strcmp(key, "fullscreen")) g_settings.fullscreen = atoi(val) != 0;
        else if (!strcmp(key, "volume")) g_settings.volume = (float)atof(val);
        else if (!strcmp(key, "reverb")) g_settings.reverb = atoi(val) != 0;
        else if (!strcmp(key, "movie_audio")) g_settings.movie_audio = atoi(val) != 0;
        else if (!strcmp(key, "show_fps")) g_settings.show_fps = atoi(val) != 0;
        else if (!strcmp(key, "motion")) g_settings.motion = atoi(val) != 0;
        else if (!strcmp(key, "motion_source")) g_settings.motion_source = atoi(val) >= 0 && atoi(val) <= 2 ? atoi(val) : 0;
        else if (!strcmp(key, "motion_flat")) g_settings.motion_flat = atoi(val) != 0;
#ifdef __ANDROID__
        else if (!strcmp(key, "overlay")) g_settings.overlay_style = !strcmp(val, "compact");
        else if (!strcmp(key, "overlay_pos")) sscanf(val, "%f,%f", &g_settings.overlay_pos[0], &g_settings.overlay_pos[1]);
#endif
#ifdef __ANDROID__
        else if (!strcmp(key, "touch_controls")) g_settings.touch_controls = atoi(val) != 0;
        else if (!strcmp(key, "cutscenes_60")) g_settings.event_fps_unlock = atoi(val) != 0;
        else if (!strcmp(key, "frame_rate")) g_settings.fps_mode = !strcmp(val, "30") ? 1 : !strcmp(val, "60") ? 2 : !strcmp(val, "20") ? 3 : !strcmp(val, "45") ? 4 : 0;
        else if (!strcmp(key, "shader_int32")) g_settings.shader_int32 = atoi(val) != 0;
        else if (!strcmp(key, "sync_lag")) {
            int sl = (!strcmp(val, "true") || !strcmp(val, "on")) ? 1
                   : (!strcmp(val, "false") || !strcmp(val, "off")) ? 0 : atoi(val);
            g_settings.sync_lag = sl < 0 ? 0 : (sl > 2 ? 2 : sl);
        }
        else if (!strcmp(key, "native_code")) g_settings.native_code = atoi(val) != 0;
        else if (!strcmp(key, "files_in_place")) g_settings.files_in_place = atoi(val) != 0;
        else if (!strcmp(key, "gpu_heap")) g_settings.gpu_heap = !strcmp(val, "auto") ? -1 : atoi(val) != 0;
        else if (!strcmp(key, "inplace_reads")) g_settings.inplace_reads = !strcmp(val, "auto") ? -1 : atoi(val) != 0;
        else if (!strcmp(key, "turnip_lrz")) g_settings.turnip_lrz = !strcmp(val, "auto") ? -1 : atoi(val) != 0;
        else if (!strcmp(key, "log_file")) g_settings.log_file = atoi(val) != 0;
        else if (!strcmp(key, "gpu_model")) snprintf(g_settings.gpu_model, sizeof g_settings.gpu_model, "%s", val);
        else if (!strcmp(key, "driver_env")) snprintf(g_settings.driver_env, sizeof g_settings.driver_env, "%s", val);
        else if (!strcmp(key, "heap_file")) g_settings.heap_file = atoi(val) != 0;
        else if (!strcmp(key, "heap_mb")) {
            int hm = atoi(val);
            g_settings.heap_mb = hm <= 0 ? 2560 : (hm < 1536 ? 1536 : (hm > 3584 ? 3584 : hm));
        }
        else if (!strcmp(key, "swapchain_images")) {
            int si = atoi(val);
            g_settings.swapchain_images = si < 2 ? 2 : (si > 5 ? 5 : si);
        }
        else if (!strcmp(key, "present_mode")) {
            if (!strcmp(val, "immediate") || !strcmp(val, "0")) g_settings.present_mode = 0;
            else if (!strcmp(val, "fifo") || !strcmp(val, "vsync") || !strcmp(val, "2")) g_settings.present_mode = 2;
            else g_settings.present_mode = 1;
        }
        else if (!strcmp(key, "fp16_shaders")) {
            g_settings.fp16_shaders = (!strcmp(val, "0") || !strcmp(val, "off") || !strcmp(val, "false")) ? 0 : 1;
        }
        else if (!strcmp(key, "async_spin_us")) {
            int sp = atoi(val);
            g_settings.async_spin_us = sp < 0 ? 0 : (sp > 2000 ? 2000 : sp);
        }
        else if (!strcmp(key, "target_fps")) {
            int tf = atoi(val);
            g_settings.target_fps = tf <= 0 ? 0 : (tf > 120 ? 120 : tf);
        }
        else if (!strcmp(key, "skip_postfx")) {
            g_settings.skip_postfx = (!strcmp(val, "1") || !strcmp(val, "on") || !strcmp(val, "true")) ? 1 : 0;
        }
        else if (!strcmp(key, "gpu_unswizzle")) {
            g_settings.gpu_unswizzle = (!strcmp(val, "0") || !strcmp(val, "off") || !strcmp(val, "false")) ? 0 : 1;
        }
        else if (!strcmp(key, "fast_depth")) {
            g_settings.fast_depth = (!strcmp(val, "0") || !strcmp(val, "off") || !strcmp(val, "false")) ? 0 : 1;
        }
        else if (!strcmp(key, "tight_barriers")) {
            g_settings.tight_barriers = (!strcmp(val, "0") || !strcmp(val, "off") || !strcmp(val, "false")) ? 0 : 1;
        }
        else if (!strcmp(key, "barrier_bits")) {
            g_settings.barrier_bits = (!strcmp(val, "0") || !strcmp(val, "off") || !strcmp(val, "false")) ? 0 : 1;
        }
        else if (!strcmp(key, "tiler_store_none")) {
            g_settings.tiler_store_none = (!strcmp(val, "0") || !strcmp(val, "off") || !strcmp(val, "false")) ? 0 : 1;
        }
        else if (!strcmp(key, "frame_gen")) {
            if (!strcmp(val, "off") || !strcmp(val, "0") || !strcmp(val, "false")) g_settings.frame_gen = 0;
            else if (!strcmp(val, "balanced") || !strcmp(val, "2")) g_settings.frame_gen = 2;
            else g_settings.frame_gen = 1;
        }
        else if (!strcmp(key, "async_shaders")) {
            g_settings.async_shaders = (!strcmp(val, "0") || !strcmp(val, "off") || !strcmp(val, "false")) ? 0 : 1;
        }
        else if (!strcmp(key, "gpu_mirrors")) {
            g_settings.gpu_mirrors = (!strcmp(val, "0") || !strcmp(val, "off") || !strcmp(val, "false")) ? 0 : 1;
        }
        else if (!strcmp(key, "gpu_writeback")) {
            g_settings.gpu_writeback = (!strcmp(val, "1") || !strcmp(val, "on") || !strcmp(val, "true")) ? 1 : 0;
        }
        else if (!strcmp(key, "profile_ver")) {
            loaded_profile_ver = atoi(val);
            g_settings.profile_ver = loaded_profile_ver;
        }
        else if (!strcmp(key, "touch_scale")) g_settings.touch_scale = (float)atof(val);
        else if (!strcmp(key, "touch_opacity")) g_settings.touch_opacity = (float)atof(val);
        else if (!strcmp(key, "touch_drag_camera")) g_settings.touch_drag_camera = atoi(val) != 0;
        else if (!strcmp(key, "touch_drag_sens")) g_settings.touch_drag_sens = (float)atof(val);
        else if (!strcmp(key, "stick_deadzone")) g_settings.stick_deadzone = fminf(fmaxf((float)atof(val), 0), 0.4f);
        else if (!strcmp(key, "touch_sticks_hidden")) g_settings.touch_sticks_hidden = atoi(val) != 0;
        else if (!strcmp(key, "touch_half_areas")) g_settings.touch_half_areas = atoi(val) != 0;
        else if (!strcmp(key, "touch_layout")) {  // x,y per control (a share of the screen; negative: default)
            char *q = val;
            for (int i = 0; i < TOUCH_NCTL * 2 && *q; i++) {
                g_settings.touch_pos[i / 2][i % 2] = strtof(q, &q);
                while (*q == ',' || *q == ' ') q++;
            }
        }
        else if (!strcmp(key, "touch_scheme")) g_settings.touch_scheme = atoi(val);
        else if (!strcmp(key, "touch_aim_sens")) g_settings.touch_aim_sens = (float)atof(val);
        else if (!strcmp(key, "touch_talk")) g_settings.touch_talk = atoi(val) != 0;
        else if (!strcmp(key, "touch_sizes") || !strcmp(key, "touch_icons_sizes")) {  // (a size per control)
            bool two = key[6] == 'i';
            float *s = two ? g_settings.touch_size2 : g_settings.touch_size;
            int n = two ? TOUCH_NCTL2 : TOUCH_NCTL;
            char *q = val;
            for (int i = 0; i < n && *q; i++) {
                s[i] = strtof(q, &q);
                while (*q == ',' || *q == ' ') q++;
            }
        }
        else if (!strcmp(key, "touch_icons_layout")) {  // (the icon layout's: as touch_layout)
            char *q = val;
            for (int i = 0; i < TOUCH_NCTL2 * 2 && *q; i++) {
                g_settings.touch_pos2[i / 2][i % 2] = strtof(q, &q);
                while (*q == ',' || *q == ' ') q++;
            }
        }
#endif
        else if (!strcmp(key, "resolution")) {
            int n = atoi(val);
            g_settings.res_mode = !strcmp(val, "dynamic") ? RES_DYNAMIC :
                                  (!strcmp(val, "100%") || !strcmp(val, "native")) ? RES_MATCH :
                                  (!strcmp(val, "75%") || !strcmp(val, "67%")) ? RES_Q75 :
                                  !strcmp(val, "50%") ? RES_Q50 : !strcmp(val, "25%") ? RES_Q25 :
                                  (n >= 1 && n <= 4) ? n : RES_MATCH;  // (5x-8x were removed)
        }
        else if (!strcmp(key, "render_scale")) {
            float rs = (float)atof(val);
            if (rs >= 0.25f && rs <= 2.0f) g_settings.render_scale = rs;
        }
        else if (!strcmp(key, "shadow_scale")) {
            float ss = (float)atof(val);
            if (ss >= 0.0f && ss <= 1.0f) g_settings.shadow_scale = ss;
        }
        else if (!strcmp(key, "upscaler"))  // (0/off/bilinear, 1/sharp/fsr1/cas/metalfx, 2/sgsr1)
            g_settings.upscaler = (!strcmp(val, "bilinear") || !strcmp(val, "off") || !strcmp(val, "0")) ? UPSCALE_BILINEAR
#ifndef __APPLE__
                                : (!strcmp(val, "sgsr1") || !strcmp(val, "2")) ? UPSCALE_SGSR1
#endif
                                : UPSCALE_SHARP;
        else if (!strcmp(key, "output")) {  // ("auto", "screen", or the final image's lines: 1080 / 900 / 720)
            int n = atoi(val);
            g_settings.out_lines = !strcmp(val, "auto") ? OUT_AUTO : n == 1080 || n == 900 || n == 720 ? n : 0;
            output_key = true;
        }
        else if (!strcmp(key, "output_resolution") && !output_key) {  // (0.1.11 and before: "screen" was only the default,
            int n = atoi(val);                                         // written by every save - read as Auto now)
            g_settings.out_lines = n == 1080 || n == 900 || n == 720 ? n : g_defaults.out_lines;
        }
        else if (!strcmp(key, "antialias")) g_settings.antialias = !strcmp(val, "off") ? AA_OFF : !strcmp(val, "fxaa") ? AA_FXAA : AA_SMAA;
        else if (!strcmp(key, "language")) {  // ("auto" or one of the game's codes; anything else: auto)
            g_settings.language = LANG_AUTO;
            const char *v = !strcmp(val, "zh-CN") ? "zh-Hans" : !strcmp(val, "zh-TW") ? "zh-Hant" : val;  // (saved before #15)
            for (int i = 1; i < N_LANGUAGES; i++)
                if (!strcmp(v, g_language_codes[i])) g_settings.language = i;
        }
        else if (!strcmp(key, "menu_tip_seen")) g_settings.menu_tip_seen = atoi(val) != 0, tip_key = true;
        else if (!strcmp(key, "mods_off")) snprintf(g_settings.mods_off, sizeof g_settings.mods_off, "%s", val);
        else if (!strcmp(key, "aspect_asked")) g_settings.aspect_asked = atoi(val) != 0, aspect_key = true;
        else if (!strcmp(key, "exec_mode")) {
            extern int g_exec_mode;
            g_exec_mode = atoi(val);
        }
        else bind_settings_load(key, val);  // bind_pad_* / bind_key_*: runtime/hle/bindings.c
    }
    fclose(f);
    if (!tip_key) g_settings.menu_tip_seen = true;  // (a settings file from before the note: not a first launch)
    if (!aspect_key) g_settings.aspect_asked = true;  // (likewise: the aspect ratio was chosen already)
    if (g_settings.volume < 0) g_settings.volume = 0;
    if (g_settings.volume > 2) g_settings.volume = 2;
#ifdef __ANDROID__
    if (loaded_profile_ver < 3) {
        g_settings.render_scale = g_defaults.render_scale;
        g_settings.shadow_scale = g_defaults.shadow_scale;
        g_settings.fp16_shaders = g_defaults.fp16_shaders;
        g_settings.skip_postfx = g_defaults.skip_postfx;
        g_settings.present_mode = g_defaults.present_mode;
        g_settings.swapchain_images = g_defaults.swapchain_images;
        g_settings.target_fps = g_defaults.target_fps;
        g_settings.fps_mode = g_defaults.fps_mode;
        g_settings.sync_lag = g_defaults.sync_lag;
        g_settings.async_spin_us = g_defaults.async_spin_us;
        g_settings.frame_gen = g_defaults.frame_gen;
        g_settings.async_shaders = g_defaults.async_shaders;
        g_settings.gpu_mirrors = g_defaults.gpu_mirrors;
        g_settings.gpu_writeback = g_defaults.gpu_writeback;
        g_settings.fast_depth = g_defaults.fast_depth;
        g_settings.tight_barriers = g_defaults.tight_barriers;
        g_settings.barrier_bits = g_defaults.barrier_bits;
        g_settings.tiler_store_none = g_defaults.tiler_store_none;
        g_settings.gpu_unswizzle = g_defaults.gpu_unswizzle;
        g_settings.profile_ver = 3;
        LOG("[settings] migrated legacy %s (profile_ver %d -> 3) to mhrise-arch v3 mobile Adreno defaults\n",
            SETTINGS_FILE, loaded_profile_ver);
        settings_save();
    }
    if (g_settings.render_scale <= 0.0f) g_settings.render_scale = 0.50f;
    if (g_settings.shadow_scale < 0.0f) g_settings.shadow_scale = 0.25f;
#endif
    extern int g_exec_mode;
    LOG("[settings] loaded %s (sync_lag=%d, heap_mb=%d, swapchain_images=%d, present_mode=%d, upscaler=%d, render_scale=%.2f, shadow_scale=%.2f, fp16_shaders=%d, target_fps=%d, skip_postfx=%d, gpu_unswizzle=%d, fast_depth=%d, tight_barriers=%d, barrier_bits=%d, tiler_store_none=%d, frame_gen=%d, async_shaders=%d, gpu_mirrors=%d, gpu_writeback=%d, exec_mode=%d)\n",
        SETTINGS_FILE, g_settings.sync_lag, g_settings.heap_mb, g_settings.swapchain_images,
        g_settings.present_mode, g_settings.upscaler, g_settings.render_scale,
        g_settings.shadow_scale, g_settings.fp16_shaders, g_settings.target_fps, g_settings.skip_postfx, g_settings.gpu_unswizzle,
        g_settings.fast_depth, g_settings.tight_barriers, g_settings.barrier_bits, g_settings.tiler_store_none, g_settings.frame_gen,
        g_settings.async_shaders, g_settings.gpu_mirrors, g_settings.gpu_writeback, g_exec_mode);
}

void settings_save(void) {
    FILE *f = fopen(SETTINGS_FILE, "w");
    if (!f) { LOG("[settings] cannot write %s\n", SETTINGS_FILE); return; }
    fprintf(f, "# BOTWRecompAndroid settings (edited from the in-game menu: F1, Esc, or Minus+Plus on a controller)\n");
    fprintf(f, "aspect = %s\n", g_settings.aspect == ASPECT_ORIGINAL ? "original" : "unlocked");
    fprintf(f, "# Internal 3D scene resolution scale (0.50 .. 1.00; default 0.68)\n");
    if (g_settings.render_scale > 0.0f) fprintf(f, "render_scale = %.2f\n", g_settings.render_scale);
    fprintf(f, "# Shadow map resolution scale (0.25, 0.50, 1.00; default 1.00)\n");
    if (g_settings.shadow_scale >= 0.0f) fprintf(f, "shadow_scale = %.2f\n", g_settings.shadow_scale);
    fprintf(f, "fullscreen = %d\n", g_settings.fullscreen);
    fprintf(f, "volume = %.2f\n", g_settings.volume);
    fprintf(f, "reverb = %d\n", g_settings.reverb);
    fprintf(f, "movie_audio = %d\n", g_settings.movie_audio);
    fprintf(f, "show_fps = %d\n", g_settings.show_fps);
    fprintf(f, "motion = %d\nmotion_source = %d\nmotion_flat = %d\n", g_settings.motion, g_settings.motion_source,
            g_settings.motion_flat);
#ifdef __ANDROID__
    fprintf(f, "overlay = %s\n", g_settings.overlay_style == 1 ? "compact" : "full");
    fprintf(f, "overlay_pos = %.4f,%.4f\n", g_settings.overlay_pos[0], g_settings.overlay_pos[1]);
#endif
#ifdef __ANDROID__
    fprintf(f, "touch_controls = %d\n", g_settings.touch_controls);
    fprintf(f, "frame_rate = %s\n", g_settings.fps_mode == 1 ? "30" : g_settings.fps_mode == 2 ? "60" : g_settings.fps_mode == 3 ? "20" :
               g_settings.fps_mode == 4 ? "45" : "auto");
    fprintf(f, "cutscenes_60 = %d\n", g_settings.event_fps_unlock);
    fprintf(f, "shader_int32 = %d\nsync_lag = %d\nnative_code = %d\nfiles_in_place = %d\n", g_settings.shader_int32,
            g_settings.sync_lag, g_settings.native_code, g_settings.files_in_place);
    static const char *const tri[] = {"auto", "0", "1"};  // (-1 / 0 / 1)
#define TRI(v) tri[(v) < 0 ? 0 : (v) ? 2 : 1]
    fprintf(f, "gpu_heap = %s\ninplace_reads = %s\nturnip_lrz = %s\nlog_file = %d\ngpu_model = %s\n", TRI(g_settings.gpu_heap),
            TRI(g_settings.inplace_reads), TRI(g_settings.turnip_lrz), g_settings.log_file, g_settings.gpu_model);
    fprintf(f, "driver_env = %s\nheap_file = %d\n", g_settings.driver_env, g_settings.heap_file);
    fprintf(f, "# Pipeline & memory tuning: sync_lag (0=off, 1=1-frame pipeline, 2=store guard), heap_mb (1536..3584, default 2560)\n");
    fprintf(f, "# Swapchain & shader tuning: swapchain_images (2..5, default 3), present_mode (0=immediate, 1=mailbox, 2=fifo), fp16_shaders (0=FP32, 1=FP16)\n");
    fprintf(f, "heap_mb = %d\nswapchain_images = %d\npresent_mode = %d\nfp16_shaders = %d\nasync_spin_us = %d\n",
            g_settings.heap_mb, g_settings.swapchain_images, g_settings.present_mode,
            g_settings.fp16_shaders, g_settings.async_spin_us);
    fprintf(f, "# Pacer, post-FX culling & GPU texture unswizzle: target_fps (0=uncapped, 20, 30, 45, 60; default 30), skip_postfx (0=off, 1=skip heavy KTGL post-FX), gpu_unswizzle (1=GPU compute deswizzle, 0=CPU)\n");
    fprintf(f, "target_fps = %d\nskip_postfx = %d\ngpu_unswizzle = %d\n", g_settings.target_fps, g_settings.skip_postfx, g_settings.gpu_unswizzle);
    fprintf(f, "# mhrise-recomp full architecture: fast_depth (1=Adreno LRZ), tight_barriers (1=elide empty barriers), tiler_store_none (1=STORE_OP_NONE + tight renderArea), frame_gen (0=off, 1=2x Light, 2=2x Balanced), async_shaders (1=non-blocking), gpu_mirrors (1=page-tracked+3-ring direct), gpu_writeback (0=off)\n");
    fprintf(f, "fast_depth = %d\ntight_barriers = %d\nbarrier_bits = %d\ntiler_store_none = %d\nframe_gen = %d\nasync_shaders = %d\ngpu_mirrors = %d\ngpu_writeback = %d\nprofile_ver = %d\n",
            g_settings.fast_depth, g_settings.tight_barriers, g_settings.barrier_bits, g_settings.tiler_store_none, g_settings.frame_gen,
            g_settings.async_shaders, g_settings.gpu_mirrors, g_settings.gpu_writeback, g_settings.profile_ver);
#undef TRI
    fprintf(f, "touch_drag_camera = %d\ntouch_drag_sens = %.2f\ntouch_sticks_hidden = %d\ntouch_half_areas = %d\n",
            g_settings.touch_drag_camera, g_settings.touch_drag_sens, g_settings.touch_sticks_hidden,
            g_settings.touch_half_areas);
    fprintf(f, "stick_deadzone = %.2f\n", g_settings.stick_deadzone);
    fprintf(f, "touch_scale = %.2f\ntouch_opacity = %.2f\ntouch_layout = ", g_settings.touch_scale, g_settings.touch_opacity);
    for (int i = 0; i < TOUCH_NCTL; i++)
        fprintf(f, "%s%.4f,%.4f", i ? "," : "", g_settings.touch_pos[i][0], g_settings.touch_pos[i][1]);
    fprintf(f, "\ntouch_scheme = %d\ntouch_aim_sens = %.2f\ntouch_talk = %d\ntouch_icons_layout = ", g_settings.touch_scheme,
            g_settings.touch_aim_sens, g_settings.touch_talk ? 1 : 0);
    for (int i = 0; i < TOUCH_NCTL2; i++)
        fprintf(f, "%s%.4f,%.4f", i ? "," : "", g_settings.touch_pos2[i][0], g_settings.touch_pos2[i][1]);
    fprintf(f, "\ntouch_sizes = ");
    for (int i = 0; i < TOUCH_NCTL; i++) fprintf(f, "%s%.2f", i ? "," : "", g_settings.touch_size[i]);
    fprintf(f, "\ntouch_icons_sizes = ");
    for (int i = 0; i < TOUCH_NCTL2; i++) fprintf(f, "%s%.2f", i ? "," : "", g_settings.touch_size2[i]);
    fprintf(f, "\n");
#endif
    fprintf(f, "upscaler = %s\n", g_settings.upscaler == UPSCALE_METALFX ? "metalfx" : g_settings.upscaler == UPSCALE_FSR1 ? "fsr1" :
                                   g_settings.upscaler == UPSCALE_SGSR1 ? "sgsr1" : "bilinear");
    if (g_settings.out_lines > 0) fprintf(f, "output = %d\n", g_settings.out_lines);
    else fprintf(f, "output = %s\n", g_settings.out_lines == OUT_AUTO ? "auto" : "screen");
    fprintf(f, "antialias = %s\n", g_settings.antialias == AA_SMAA ? "smaa" : g_settings.antialias == AA_FXAA ? "fxaa" : "off");
    if (g_settings.res_mode == RES_DYNAMIC) fprintf(f, "resolution = dynamic\n");
    else if (g_settings.res_mode == RES_Q75) fprintf(f, "resolution = 75%%\n");
    else if (g_settings.res_mode == RES_Q25) fprintf(f, "resolution = 25%%\n");
    else if (g_settings.res_mode == RES_Q50) fprintf(f, "resolution = 50%%\n");
    else if (g_settings.res_mode >= 1 && g_settings.res_mode <= 8) fprintf(f, "resolution = %dx\n", g_settings.res_mode);
    else fprintf(f, "resolution = match\n");
    fprintf(f, "language = %s\n", g_language_codes[g_settings.language > LANG_AUTO && g_settings.language < N_LANGUAGES
                                                     ? g_settings.language : LANG_AUTO]);
    fprintf(f, "aspect_asked = %d\n", g_settings.aspect_asked);
    fprintf(f, "menu_tip_seen = %d\n", g_settings.menu_tip_seen);
    fprintf(f, "mods_off = %s\n", g_settings.mods_off);
    extern int g_exec_mode;
    if (g_exec_mode != 0) fprintf(f, "exec_mode = %d\n", g_exec_mode);
    bind_settings_save(f);
    fclose(f);
}
