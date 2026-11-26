# libmain.so: the BOTWRecompAndroid runtime for Android, with the
# Vulkan backend and the ImGui UI. The game code is not in it: botwgame.so, generated with gen.py --dyn-base and built
# from the player's own game files, is loaded at run time (docs/las/ANDROID-PORT.md).
cmake_minimum_required(VERSION 3.22)
project(botwrecompandroid C CXX ASM)

set(R ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../..)   # the repository
get_filename_component(R ${R} ABSOLUTE)
set(DEPS ${R}/android/deps/out)                        # android/deps/build_deps.sh
if(NOT EXISTS ${DEPS}/lib/libSDL3.so)
    message(FATAL_ERROR "run android/deps/build_deps.sh first")
endif()
if(NOT BOTW_GEN_DIR)
    message(FATAL_ERROR "BOTW_GEN_DIR (the Gradle build's generated files: smaa_hlsl.inc) is not set")
endif()

include(${R}/android/gamelib_id.cmake)  # (GAMELIB_ID: the same as botwgame.so's, android/gamelib)
message(STATUS "GAMELIB_ID ${GAMELIB_ID}")

add_subdirectory(${R}/third_party/libadrenotools ${CMAKE_BINARY_DIR}/adrenotools)
# Keep adrenotools/linkernsbypass symbols hidden inside libmain.so and libhook_impl.so so
# -Wl,-z,global -Wl,-export-dynamic never exports linkernsbypass .bss pointers (e.g.
# android_get_exported_namespace) into the process global symbol group and hijacks system PLTs.
set_target_properties(adrenotools linkernsbypass PROPERTIES
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
if(TARGET hook_impl)
    target_link_options(hook_impl PRIVATE -Wl,--exclude-libs,liblinkernsbypass.a)
endif()

add_library(SDL3 SHARED IMPORTED)
set_target_properties(SDL3 PROPERTIES IMPORTED_LOCATION ${DEPS}/lib/libSDL3.so)
add_library(shaderc_shared SHARED IMPORTED)
set_target_properties(shaderc_shared PROPERTIES IMPORTED_LOCATION ${DEPS}/lib/libshaderc_shared.so)

file(GLOB RT_SRC CONFIGURE_DEPENDS ${R}/runtime/*.c ${R}/runtime/hle/*.c ${R}/runtime/services/*.c ${R}/runtime/nvn/*.c
     ${R}/runtime/nvn/maxwell/*.c)
# FFmpeg (android/deps/build_ffmpeg.sh): the game's movies and the start screen's background; without it, neither
set(HAVE_FFMPEG FALSE)
if(EXISTS ${DEPS}/lib/libavformat.so)
    set(HAVE_FFMPEG TRUE)
    foreach(l avcodec avformat avutil swscale)
        add_library(${l} SHARED IMPORTED)
        set_target_properties(${l} PROPERTIES IMPORTED_LOCATION ${DEPS}/lib/lib${l}.so)
    endforeach()
else()
    list(REMOVE_ITEM RT_SRC ${R}/runtime/hle/movie.c)
endif()
file(GLOB VK_SRC CONFIGURE_DEPENDS ${R}/runtime/nvn/vulkan/*.c ${R}/runtime/nvn/vulkan/*.cpp)
file(GLOB UI_SRC CONFIGURE_DEPENDS ${R}/runtime/ui/*.mm)
set(IMGUI ${R}/third_party/imgui)
set(IMGUI_SRC ${IMGUI}/imgui.cpp ${IMGUI}/imgui_draw.cpp ${IMGUI}/imgui_tables.cpp ${IMGUI}/imgui_widgets.cpp
    ${IMGUI}/backends/imgui_impl_sdl3.cpp ${IMGUI}/backends/imgui_impl_vulkan.cpp)

add_library(main SHARED ${RT_SRC} ${R}/runtime/platform/android.c ${VK_SRC} ${UI_SRC} ${IMGUI_SRC}
            ${DEPS}/volk/volk.c ${R}/android/common/guest_window.c ${R}/runtime/native_gate.S)
# the UI sources are plain C++ apart from __APPLE__ blocks (compiled as C++ on Android)
set_source_files_properties(${UI_SRC} PROPERTIES LANGUAGE CXX COMPILE_OPTIONS "-xc++")
set_source_files_properties(${R}/runtime/gamelib.c PROPERTIES COMPILE_DEFINITIONS "GAMELIB_ID=\"${GAMELIB_ID}\"")
set_source_files_properties(${R}/runtime/gamebuild.c PROPERTIES COMPILE_DEFINITIONS "GAMELIB_ID=\"${GAMELIB_ID}\"")
set_source_files_properties(${R}/runtime/android_libm_compat.c PROPERTIES COMPILE_OPTIONS "-fno-builtin")
set_source_files_properties(${DEPS}/volk/volk.c ${IMGUI_SRC} PROPERTIES COMPILE_OPTIONS "-fvisibility=hidden")

execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${R}
    OUTPUT_VARIABLE RYZA_GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT RYZA_GIT_COMMIT)
    set(RYZA_GIT_COMMIT "unknown")
endif()

file(READ "${R}/android/botwrecompandroid/app/build.gradle.kts" RYZA_GRADLE_CFG)
set(RYZA_VERSION_CODE "57")
set(RYZA_VERSION_NAME "0.3.2-exp-mhrise-4")
if(RYZA_GRADLE_CFG MATCHES "versionCode[ \t]*=[ \t]*([0-9]+)")
    set(RYZA_VERSION_CODE "${CMAKE_MATCH_1}")
endif()
if(RYZA_GRADLE_CFG MATCHES "versionName[ \t]*=[ \t]*\"([^\"]+)\"")
    set(RYZA_VERSION_NAME "${CMAKE_MATCH_1}")
endif()

target_include_directories(main PRIVATE ${R}/runtime ${R}/runtime/nvn ${IMGUI} ${IMGUI}/backends ${DEPS}/include
                           ${DEPS}/volk ${BOTW_GEN_DIR} ${R}/third_party/smaa/Textures ${R}/android/common)
target_compile_definitions(main PRIVATE
    RYZA_BUILD_COMMIT="${RYZA_GIT_COMMIT}"
    RYZA_VERSION_CODE="${RYZA_VERSION_CODE}"
    RYZA_VERSION_NAME="${RYZA_VERSION_NAME}"
    RYZA_BUILD_VARIANT="precomputed-render-pipeline"
    VK_NO_PROTOTYPES VK_USE_PLATFORM_ANDROID_KHR IMGUI_IMPL_VULKAN_USE_VOLK   # Vulkan from the loaded driver (volk)
    VMA_STATIC_VULKAN_FUNCTIONS=0 VMA_DYNAMIC_VULKAN_FUNCTIONS=1)
if(HAVE_FFMPEG)
    target_link_libraries(main PRIVATE avcodec avformat avutil swscale)
else()
    target_compile_definitions(main PRIVATE LARECOMP_NO_FFMPEG)
endif()
# -O2 -g whatever the Gradle build type (the Android runtime optimization flags): a debug build left the runtime at -O0, about half of the
# game thread's time
target_compile_options(main PRIVATE -O3 -fno-strict-aliasing -Wall -Wno-unused-function -Wno-format  # (as on Linux: %llx for uint64_t)
    $<$<COMPILE_LANGUAGE:CXX>:-std=c++17>)
target_link_libraries(main PRIVATE SDL3 shaderc_shared adrenotools android log dl m)
# (runtime/hle.h, ipc.h, main.c: tables in named sections found through __start_ / __stop_ symbols)
# -rdynamic / -Wl,-export-dynamic agar simbol-simbol runtime seperti g_fnprof, fnprof_hit, dll
# dapat di-resolve saat dlopen() botwgame.so
target_link_options(main PRIVATE -Wl,-z,nostart-stop-gc -Wl,-z,global -Wl,-export-dynamic -Wl,--exclude-libs,libadrenotools.a -Wl,--exclude-libs,liblinkernsbypass.a)
