// Loads the recompiled game code library (runtime/gamelib.h).
#include "gamelib.h"
#include <SDL3/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#ifdef __ANDROID__
#include <adrenotools/driver.h>
#endif
#endif

#include "paths.h"
#include "platform.h"
#include "rt.h"

static const GameLibTables *g_tables;
static char g_error[256];

// The library's id file (<library>.id, written next to it by the build): its GAMELIB_ID, read without loading it.
static bool id_matches(const char *lib) {
    char path[1100], id[128] = {0};
    snprintf(path, sizeof path, "%s.id", lib);
    FILE *f = fopen(path, "rb");
    if (!f) return true;
    size_t n = fread(id, 1, sizeof id - 1, f);
    fclose(f);
    while (n && (id[n - 1] == 10 || id[n - 1] == 13)) id[--n] = 0;
    return !strcmp(id, GAMELIB_ID) || !strcmp(id, "dev");
}

static bool exists(const char *p) {
    FILE *f = fopen(p, "rb");
    if (f) fclose(f);
    return f != NULL;
}

#ifdef __ANDROID__
#include <sys/stat.h>
static bool copy_to(const char *from, const char *to) {
    FILE *in = fopen(from, "rb");
    if (!in) return false;
    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s.tmp", to);
    FILE *out = fopen(tmp, "wb");
    bool ok = out != NULL;
    static char buf[1 << 20];
    for (size_t n; ok && (n = fread(buf, 1, sizeof buf, in)) > 0;) ok = fwrite(buf, 1, n, out) == n;
    fclose(in);
    if (out) ok &= fclose(out) == 0;
    ok = ok && rename(tmp, to) == 0;
    if (!ok) remove(tmp);
    return ok;
}

// A 1.6.0 / 1.8.2 library in that version's extracted/ (put there over adb: android/push_game.sh) newer than the one in
// internal storage, or another size, is copied in with its id - as BOTWRecompAndroidActivity does 1.0.0's. Once per
// process: a copy that fails isn't tried again every frame.
// LARECOMP_GAME_LIB_SRC=<file> (development): that library instead - extracted/ of a version the app set up belongs to
// the app, and adb can't write there, but it can write to the app's files/ root.
static void sync_from_game_dir(const char *game_dir, const char *lib) {
    char src[1100], dir[1100];
    const char *e = getenv("LARECOMP_GAME_LIB_SRC");
    if (e && *e) snprintf(src, sizeof src, "%s", e);
    else snprintf(src, sizeof src, "%s/%s", game_dir, GAMELIB_NAME);
    struct stat s, d;
    if (stat(src, &s) != 0 || (stat(lib, &d) == 0 && d.st_size == s.st_size && d.st_mtime >= s.st_mtime)) return;
    static char tried[1100];
    if (!strcmp(tried, lib)) return;
    snprintf(tried, sizeof tried, "%s", lib);
    snprintf(dir, sizeof dir, "%s", lib);
    char *sl = strrchr(dir, '/');
    if (sl) *sl = 0, mkdir(dir, 0755);
    char sid[1200], did[1200];
    snprintf(sid, sizeof sid, "%s.id", src);
    snprintf(did, sizeof did, "%s.id", lib);
    bool ok = copy_to(src, lib);
    if (exists(sid)) copy_to(sid, did);
    LOG("[gamelib] %s -> %s: %s\n", src, lib, ok ? "copied" : "copy failed");
}
#endif

// The places to look, in order (gamelib.h). Returns how many were filled.
static int candidates(const char *game_dir, char out[8][1100]) {
    int n = 0;
    const char *env = getenv("LARECOMP_GAME_LIB");
    if (env && *env) snprintf(out[n++], 1100, "%s", env);

#ifdef __ANDROID__
    // Cek juga apakah botwgame.so ada langsung di nativeLibraryDir (misal di-bundle di APK)
    const char *nld = rt_native_lib_dir();
    if (nld && *nld) {
        char nld_path[1024];
        snprintf(nld_path, sizeof nld_path, "%s/%s", nld, GAMELIB_NAME);
        if (exists(nld_path)) {
            snprintf(out[n++], 1100, "%s", nld_path);
        }
    }

    // Di Android, storage eksternal (/storage/emulated/...) di-mount noexec.
    // dlopen() HANYA diizinkan dari internal storage (/data/user/0/.../files/v.../botwgame.so).
    // Sinkronkan dari rt_data_root() atau game_dir ke internal storage!
    const char *in = SDL_GetAndroidInternalStoragePath();
    const char *data_root = rt_data_root();
    if (in && *in) {
        char own[1024];
        int v = game_dir ? rt_game_dir_version(game_dir) : 100;
        if (!v) v = 100;
        snprintf(own, sizeof own, "%s/v%d/%s", in, v, GAMELIB_NAME);

        // Sync dari data_root bila ada
        if (data_root && *data_root) {
            sync_from_game_dir(data_root, own);
        }
        if (game_dir && (!data_root || strcmp(data_root, game_dir))) {
            sync_from_game_dir(game_dir, own);
        }
        snprintf(out[n++], 1100, "%s", own);
    }
    return n;
#endif

    // Fallback path lainnya (non-Android / root)
    const char *data_root_other = rt_data_root();
    if (data_root_other && *data_root_other) {
        snprintf(out[n++], 1100, "%s/%s", data_root_other, GAMELIB_NAME);
    }
    if (game_dir && (!data_root_other || strcmp(data_root_other, game_dir))) {
        snprintf(out[n++], 1100, "%s/%s", game_dir, GAMELIB_NAME);
    }

    char own[1024];
    if (game_dir && rt_version_game_lib(rt_game_dir_version(game_dir), own, sizeof own)) {
#ifdef __ANDROID__
        sync_from_game_dir(game_dir, own);
#endif
        snprintf(out[n++], 1100, "%s", own);
        return n;
    }
    char exe_side[1024];
    if (rt_resource(GAMELIB_NAME, NULL, exe_side, sizeof exe_side) && exists(exe_side))
        snprintf(out[n++], 1100, "%s", exe_side);
    return n;
}

bool gamelib_present(const char *game_dir) {
    if (g_tables) return true;
    char c[8][1100];
    int n = candidates(game_dir, c);
    for (int i = 0; i < n; i++)
        if (exists(c[i]) && id_matches(c[i])) return true;
    return false;
}

static const GameLibTables *open_lib(const char *path) {
#ifdef _WIN32
    HMODULE h = LoadLibraryA(path);
    if (!h) { snprintf(g_error, sizeof g_error, "can't load %s (error %lu)", path, GetLastError()); return NULL; }
    GameLibEntry entry = (GameLibEntry)(void *)GetProcAddress(h, GAMELIB_ENTRY);
#else
    void *h = NULL;
    h = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    if (!h) {
        const char *e = dlerror();
        LOG("[gamelib] dlopen(%s) failed: %s\n", path, e ? e : "unknown");
        snprintf(g_error, sizeof g_error, "%s", e ? e : path);
        return NULL;
    }
    GameLibEntry entry = (GameLibEntry)dlsym(h, GAMELIB_ENTRY);
#endif
    if (!entry) { snprintf(g_error, sizeof g_error, "%s: not game code", path); return NULL; }
    const GameLibTables *t = entry();
    if (!t || (t->abi != 1 && t->abi != GAMELIB_ABI) || !t->id || (strcmp(t->id, GAMELIB_ID) && strcmp(t->id, "dev"))) {
        snprintf(g_error, sizeof g_error, "%s was built for another version", path);
        return NULL;  // (left loaded: nothing of it runs)
    }
#ifdef __ANDROID__
    // its addresses follow the guest window wherever it is (gen.py --dyn-base: botwgame_gb)
    void (*set_base)(uint64_t) = (void (*)(uint64_t))dlsym(h, "botwgame_set_guest_base");
    if (!set_base) {
        snprintf(g_error, sizeof g_error, "%s: not built for Android (gen.py --dyn-base)", path);
        return NULL;
    }
    set_base(GUEST_LO);
#endif
    return t;
}

bool gamelib_load(const char *game_dir, char *why, size_t why_size) {
    static bool tried;
    if (!tried) {
        tried = true;
        snprintf(g_error, sizeof g_error, "the game code hasn't been built: set up the game files on the start screen");
        char c[8][1100];
        int n = candidates(game_dir, c);
        const char *env = getenv("LARECOMP_GAME_LIB");
        for (int i = 0; i < n && !g_tables; i++) {
            LOG("[gamelib] checking candidate %d: %s (exists: %d)\n", i, c[i], exists(c[i]));
            if (!exists(c[i])) continue;
            bool given = i == 0 && env && *env;  // LARECOMP_GAME_LIB: loaded as given, the library's own id decides
            if (!given && !id_matches(c[i])) {
                snprintf(g_error, sizeof g_error, "%s was built for another version", c[i]);
                continue;
            }
            g_tables = open_lib(c[i]);
            if (g_tables) {
                unsigned long long sub_cnt = (g_tables->abi >= 2 && g_tables->subsdk0_count) ? (unsigned long long)*g_tables->subsdk0_count : 0ull;
                LOG("[gamelib] %s (%llu + %llu + %llu functions, ABI %u)\n", c[i],
                    (unsigned long long)*g_tables->main_count,
                    (unsigned long long)*g_tables->sdk_count,
                    sub_cnt, g_tables->abi);
            }
        }
    }
    if (!g_tables && why) snprintf(why, why_size, "%s", g_error);
    return g_tables != NULL;
}

const GameLibTables *gamelib_tables(void) { return g_tables; }
