// Android: which Vulkan driver the backend uses. LARECOMP_VK_DRIVER=<path of a driver library in the app's internal
// storage> (the activity sets it from the user's choice, e.g. Turnip): loaded through adrenotools, in a linker
// namespace of its own (its hooks are in nativeLibraryDir, LARECOMP_NATIVE_LIB_DIR). Otherwise, or if that fails, the
// system driver. volk then takes every Vulkan function from it (device.c: volkLoadInstance / volkLoadDevice).
#include <adrenotools/driver.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>

#include "vk.h"

extern const char *rt_data_root(void);
#if defined(__ANDROID__)
extern const char *SDL_GetAndroidExternalStoragePath(void);
extern const char *SDL_GetAndroidInternalStoragePath(void);
#endif

static void ensure_dir_recursive(const char *path) {
    if (!path || !path[0]) return;
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0777);
            *p = '/';
        }
    }
    mkdir(tmp, 0777);
}

void vk_android_setup_shader_cache(void) {
    const char *existing_mesa = getenv("MESA_SHADER_CACHE_DIR");
    if (existing_mesa && existing_mesa[0] && strncmp(existing_mesa, "/data/.cache", 12) != 0) {
        ensure_dir_recursive(existing_mesa);
        const char *existing_xdg = getenv("XDG_CACHE_HOME");
        if (existing_xdg && existing_xdg[0]) ensure_dir_recursive(existing_xdg);
        return;
    }

    const char *base = NULL;
#if defined(__ANDROID__)
    base = SDL_GetAndroidExternalStoragePath();
    if (!base || !base[0]) base = SDL_GetAndroidInternalStoragePath();
#endif
    if (!base || !base[0]) base = rt_data_root();
    if (!base || !base[0]) base = "/storage/emulated/0/Android/data/com.botwrecomp.android/files";

    char xdg_dir[512];
    char mesa_dir[512];
    snprintf(xdg_dir, sizeof(xdg_dir), "%s/cache", base);
    snprintf(mesa_dir, sizeof(mesa_dir), "%s/cache/mesa_shader_cache", base);

    ensure_dir_recursive(xdg_dir);
    ensure_dir_recursive(mesa_dir);

    setenv("XDG_CACHE_HOME", xdg_dir, 1);
    setenv("MESA_SHADER_CACHE_DIR", mesa_dir, 1);
    setenv("MESA_GLSL_CACHE_DIR", mesa_dir, 1);
    setenv("MESA_SHADER_CACHE_MAX_SIZE", "512M", 0);
    fprintf(stderr, "[vk] Turnip/Mesa shader cache dir configured: MESA_SHADER_CACHE_DIR=%s XDG_CACHE_HOME=%s\n",
            mesa_dir, xdg_dir);
}

// Does this driver offer a Vulkan GPU at all? (Turnip v26.0.0 R8, built for the A7xx: none on an Adreno 830 - the app
// stopped at "no Vulkan device" on every start, its settings out of reach.) 1.1 and up: below 1.3 device.c checks the
// extensions it needs instead (COMPAT_EXT).
static bool driver_has_gpu(PFN_vkGetInstanceProcAddr gipa) {
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    if (!create) return false;
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance inst;
    if (create(&ci, NULL, &inst) != VK_SUCCESS) return false;
    PFN_vkEnumeratePhysicalDevices enumerate = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties props = (PFN_vkGetPhysicalDeviceProperties)gipa(inst, "vkGetPhysicalDeviceProperties");
    PFN_vkDestroyInstance destroy = (PFN_vkDestroyInstance)gipa(inst, "vkDestroyInstance");
    bool ok = false;
    uint32_t n = 0;
    if (enumerate && props && enumerate(inst, &n, NULL) == VK_SUCCESS && n) {
        VkPhysicalDevice pd[8];
        if (n > 8) n = 8;
        if (enumerate(inst, &n, pd) >= 0)
            for (uint32_t i = 0; i < n && !ok; i++) {
                VkPhysicalDeviceProperties p;
                props(pd[i], &p);
                ok = p.apiVersion >= VK_API_VERSION_1_1;
            }
    }
    if (destroy) destroy(inst, NULL);
    return ok;
}

// The chosen driver the system's stood in for (its package's name; "" none): the start screen and the driver errors
// say so - before, a Turnip build for another Adreno generation fell back silently, and the "driver too old" error
// read as if Turnip lacked the extensions
static char g_rejected[128];
const char *vk_driver_rejected(void) { return g_rejected; }

void vk_android_load(void) {
    vk_android_setup_shader_cache();
    void *lib = NULL;
    const char *drv = getenv("LARECOMP_VK_DRIVER"), *hooks = getenv("LARECOMP_NATIVE_LIB_DIR");
    if (drv && *drv && hooks && *hooks) {
        const char *slash = strrchr(drv, '/');
        char dir[1024];
        if (slash && (size_t)snprintf(dir, sizeof dir, "%.*s/", (int)(slash - drv), drv) < sizeof dir)
            lib = adrenotools_open_libvulkan(RTLD_NOW | RTLD_LOCAL, ADRENOTOOLS_DRIVER_CUSTOM, NULL, hooks, dir, slash + 1,
                                             NULL, NULL);
        LOG("[vk] driver %s: %s\n", drv, lib ? "loaded through adrenotools" : "FAILED, using the system driver");
        PFN_vkGetInstanceProcAddr g = lib ? (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr") : NULL;
        if (lib && (!g || !driver_has_gpu(g))) {
            LOG("[vk] driver %s: no Vulkan 1.1+ GPU it supports here - using the system driver\n", drv);
            lib = NULL;  // (left loaded: a driver doesn't always unload cleanly)
        }
        if (!lib) {  // (the package's name: its folder, files/drivers/<name>/<library>)
            const char *name = drv;
            if (slash) {
                const char *p = slash;
                while (p > drv && p[-1] != '/') p--;
                name = p;
            }
            snprintf(g_rejected, sizeof g_rejected, "%.*s", slash && name < slash ? (int)(slash - name) : (int)strlen(name), name);
        }
    }
    if (!lib) lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) FATAL("no Vulkan driver: %s\n", dlerror());
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    if (!gipa) FATAL("the Vulkan driver has no vkGetInstanceProcAddr\n");
    volkInitializeCustom(gipa);
}
