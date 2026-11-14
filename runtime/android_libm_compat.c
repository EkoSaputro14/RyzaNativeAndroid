#ifdef __ANDROID__
#include <dlfcn.h>
#include <stddef.h>

// Compatibility shims exported from libmain.so (which carries DF_1_GLOBAL).
// Standalone botwgame.so builds compiled without -lm (e.g. build_recomp.py) can
// reference libm.so symbols such as fegetround/fesetround while libm.so sits
// outside the app namespace's global group. Forwarding to the real libm.so via
// dlsym ensures dlopen(botwgame.so) resolves them without recompiling botwgame.so.

static void *resolve_libm_sym(const char *name) {
    static void *volatile g_libm = NULL;
    void *h = __atomic_load_n(&g_libm, __ATOMIC_ACQUIRE);
    if (!h) {
        h = dlopen("libm.so", RTLD_NOW | RTLD_GLOBAL);
        if (h) {
            void *expected = NULL;
            if (!__atomic_compare_exchange_n(&g_libm, &expected, h, 0, __ATOMIC_RELEASE, __ATOMIC_ACQUIRE)) {
                h = expected;
            }
        }
    }
    return h ? dlsym(h, name) : NULL;
}

#define LIBM_EXPORT __attribute__((visibility("default")))

LIBM_EXPORT int fegetround(void) {
    typedef int (*fn_t)(void);
    static fn_t volatile fn = NULL;
    fn_t f = __atomic_load_n(&fn, __ATOMIC_ACQUIRE);
    if (!f) {
        f = (fn_t)resolve_libm_sym("fegetround");
        __atomic_store_n(&fn, f, __ATOMIC_RELEASE);
    }
    return f ? f() : 0;
}

LIBM_EXPORT int fesetround(int round) {
    typedef int (*fn_t)(int);
    static fn_t volatile fn = NULL;
    fn_t f = __atomic_load_n(&fn, __ATOMIC_ACQUIRE);
    if (!f) {
        f = (fn_t)resolve_libm_sym("fesetround");
        __atomic_store_n(&fn, f, __ATOMIC_RELEASE);
    }
    return f ? f(round) : -1;
}

#define LIBM_UNARY_D(name) \
LIBM_EXPORT double name(double x) { \
    typedef double (*fn_t)(double); \
    static fn_t volatile fn = NULL; \
    fn_t f = __atomic_load_n(&fn, __ATOMIC_ACQUIRE); \
    if (!f) { f = (fn_t)resolve_libm_sym(#name); __atomic_store_n(&fn, f, __ATOMIC_RELEASE); } \
    return f ? f(x) : x; \
}

#define LIBM_UNARY_F(name) \
LIBM_EXPORT float name(float x) { \
    typedef float (*fn_t)(float); \
    static fn_t volatile fn = NULL; \
    fn_t f = __atomic_load_n(&fn, __ATOMIC_ACQUIRE); \
    if (!f) { f = (fn_t)resolve_libm_sym(#name); __atomic_store_n(&fn, f, __ATOMIC_RELEASE); } \
    return f ? f(x) : x; \
}

LIBM_EXPORT double fma(double x, double y, double z) {
    typedef double (*fn_t)(double, double, double);
    static fn_t volatile fn = NULL;
    fn_t f = __atomic_load_n(&fn, __ATOMIC_ACQUIRE);
    if (!f) { f = (fn_t)resolve_libm_sym("fma"); __atomic_store_n(&fn, f, __ATOMIC_RELEASE); }
    return f ? f(x, y, z) : (x * y + z);
}

LIBM_EXPORT float fmaf(float x, float y, float z) {
    typedef float (*fn_t)(float, float, float);
    static fn_t volatile fn = NULL;
    fn_t f = __atomic_load_n(&fn, __ATOMIC_ACQUIRE);
    if (!f) { f = (fn_t)resolve_libm_sym("fmaf"); __atomic_store_n(&fn, f, __ATOMIC_RELEASE); }
    return f ? f(x, y, z) : (x * y + z);
}

LIBM_UNARY_D(sqrt)
LIBM_UNARY_F(sqrtf)
LIBM_UNARY_D(rint)
LIBM_UNARY_F(rintf)
LIBM_UNARY_D(nearbyint)
LIBM_UNARY_F(nearbyintf)
LIBM_UNARY_D(ceil)
LIBM_UNARY_F(ceilf)
LIBM_UNARY_D(floor)
LIBM_UNARY_F(floorf)
LIBM_UNARY_D(trunc)
LIBM_UNARY_F(truncf)
LIBM_UNARY_D(round)
LIBM_UNARY_F(roundf)

LIBM_EXPORT double fmod(double x, double y) {
    typedef double (*fn_t)(double, double);
    static fn_t volatile fn = NULL;
    fn_t f = __atomic_load_n(&fn, __ATOMIC_ACQUIRE);
    if (!f) { f = (fn_t)resolve_libm_sym("fmod"); __atomic_store_n(&fn, f, __ATOMIC_RELEASE); }
    return f ? f(x, y) : 0.0;
}

LIBM_EXPORT float fmodf(float x, float y) {
    typedef float (*fn_t)(float, float);
    static fn_t volatile fn = NULL;
    fn_t f = __atomic_load_n(&fn, __ATOMIC_ACQUIRE);
    if (!f) { f = (fn_t)resolve_libm_sym("fmodf"); __atomic_store_n(&fn, f, __ATOMIC_RELEASE); }
    return f ? f(x, y) : 0.0f;
}

#endif
