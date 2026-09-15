# Ogre Battle 64: Person of Lordly Caliber - PC port app.
#
# Builds the recompiled game code (RecompiledFuncs/) against N64ModernRuntime
# (ultramodern + librecomp) and links the RT64 renderer. See
# docs/guides/app-build.md for instructions, and docs/WEB-PORT.md for the
# WebAssembly port plan.
#
#   cmake -S app -B build-app -DCMAKE_BUILD_TYPE=Release          # RT64 (default)
#   cmake -S app -B build-null -DOGRE_USE_RT64=OFF                # null renderer
#   emcmake cmake -S app -B build-wasm                            # Emscripten
#   cmake --build <dir> -j

cmake_minimum_required(VERSION 3.20)

project(ogrebattle64 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if (WIN32)
    # Opt out of constexpr mutex constructor on windows to prevent vcredist issues
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR")
endif()

# Emscripten: pthreads (SharedArrayBuffer) must be enabled for EVERY target —
# the runtime (ultramodern/librecomp) uses std::thread/atomics, and wasm-ld
# rejects non-pthread objects in a --shared-memory link. Placed before the
# runtime subdirectory so the flags inherit.
if (EMSCRIPTEN)
    add_compile_options(-pthread -fexceptions)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# Root of the third-party tree (sibling of app/).
set(OGRE_TOOLS "${CMAKE_CURRENT_SOURCE_DIR}/../tools")

# --- Renderer selection -----------------------------------------------------
# RT64 is the primary native renderer (Vulkan on Linux, Metal on macOS,
# D3D12/Vulkan on Windows). It can be disabled for a GPU-free null-renderer
# build (useful for bring-up and CI), and it is always disabled on Emscripten,
# where the wasm build uses the null renderer until a WebGL/WebGPU renderer
# exists (see docs/WEB-PORT.md).
option(OGRE_USE_RT64 "Build with the RT64 renderer (native builds only)" ON)

if (EMSCRIPTEN)
    set(OGRE_USE_RT64 OFF)
endif()

# --- Runtime: ultramodern + librecomp (+ its bundled N64Recomp) --------------
add_subdirectory(${OGRE_TOOLS}/N64ModernRuntime ${CMAKE_BINARY_DIR}/N64ModernRuntime)

# Frame pointers in the runtime too, not just in the app: a crash inside
# runtime code reached through a bridge (the message-queue math, for example)
# has an empty shadow *guest* chain, and without frame pointers lldb prints only
# the faulting frame and nobody can see who called it. RT64 keeps its own flags
# (hot path). See docs/guides/app-build.md -> "Diagnostics toolkit".
foreach(OGRE_FP_TARGET librecomp ultramodern)
    if (TARGET ${OGRE_FP_TARGET})
        target_compile_options(${OGRE_FP_TARGET} PRIVATE -fno-omit-frame-pointer)
    endif()
endforeach()

# --- Renderer ----------------------------------------------------------------
if (OGRE_USE_RT64)
    # Static build, expecting an SDL window outside of Windows (same options as
    # Zelda64Recomp). On Linux RT64_SDL_WINDOW_VULKAN creates a Vulkan surface from
    # the SDL window (the primary, most-tested backend path).
    set(RT64_STATIC TRUE)
    set(RT64_SDL_WINDOW_VULKAN TRUE)
    add_compile_definitions(HLSL_CPU)
    add_subdirectory(${OGRE_TOOLS}/RT64 ${CMAKE_BINARY_DIR}/RT64)
    target_include_directories(rt64 PRIVATE ${CMAKE_BINARY_DIR}/RT64/src)
    set(OGRE_RENDERER_SOURCES src/renderer.cpp)
elseif (EMSCRIPTEN)
    # Milestone 7: WebGL2 browser renderer (docs/WEB-PORT.md §16 Option A).
    set(OGRE_RENDERER_SOURCES src/web_renderer.cpp)
else()
    set(OGRE_RENDERER_SOURCES src/null_renderer.cpp)
endif()

# --- SDL2 (window, input, audio; native only) --------------------------------
# On Emscripten SDL2 comes from the emscripten ports (-sUSE_SDL=2), and the
# initial web build does not use SDL at all (no-op callbacks instead).
if (NOT EMSCRIPTEN)
    find_package(SDL2 REQUIRED)
endif()

# --- Recompiled game code ----------------------------------------------------
# The main-segment recompilation output (807 functions). Compiled as a static
# lib so the app can link it alongside the runtime. CONFIGURE_DEPENDS re-globs
# when `make recomp` adds/removes files.
file(GLOB RECOMP_C_SOURCES   CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../RecompiledFuncs/*.c)
file(GLOB RECOMP_CPP_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../RecompiledFuncs/*.cpp)

add_library(ogrebattle64_recomp STATIC ${RECOMP_C_SOURCES} ${RECOMP_CPP_SOURCES})

target_include_directories(ogrebattle64_recomp PRIVATE
    ${OGRE_TOOLS}/N64ModernRuntime/N64Recomp/include
    ${OGRE_TOOLS}/N64ModernRuntime/ultramodern/include
    ${OGRE_TOOLS}/N64ModernRuntime/librecomp/include
)

target_compile_options(ogrebattle64_recomp PRIVATE
    -fno-strict-aliasing
    # Keep frame pointers so a host backtrace actually unwinds at a crash: the
    # runtime's shadow guest chain is empty when the fault is in runtime code
    # reached through a bridge (see tools/watch.sh and docs/guides/app-build.md).
    -fno-omit-frame-pointer
    -Wno-unused-variable
    -Wno-unused-parameter
    -Wno-unused-function
    -Wno-unused-but-set-variable
    -Wno-implicit-function-declaration
)

# --- Recompiled RSP microcode -----------------------------------------------
# Non-graphics RSP tasks are run through recompiled microcode (src/rsp.cpp).
# Today that is the game's Nintendo-JPEG decoder (M_NJPEGTASK, type 4) used for
# the New Game opening's full-screen backgrounds; regenerate with
# `make rsp-recomp` (rsp-njpeg.toml -> RspFuncs/njpeg_ucode.cpp). The generated
# code is C++ (it uses librecomp's RSP VU implementation).
file(GLOB RSP_RECOMP_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../RspFuncs/*.cpp)

if(RSP_RECOMP_SOURCES)
    add_library(ogrebattle64_rsp STATIC ${RSP_RECOMP_SOURCES})

    target_include_directories(ogrebattle64_rsp PRIVATE
        ${OGRE_TOOLS}/N64ModernRuntime/N64Recomp/include
        ${OGRE_TOOLS}/N64ModernRuntime/ultramodern/include
        ${OGRE_TOOLS}/N64ModernRuntime/librecomp/include
    )

    target_compile_options(ogrebattle64_rsp PRIVATE
        -fno-strict-aliasing
        -fno-omit-frame-pointer
        -Wno-unused-variable
        -Wno-unused-parameter
        -Wno-unused-function
        -Wno-unused-but-set-variable
        -Wno-implicit-function-declaration
    )
endif()

# --- Streamed-overlay bank units (Phase 4) ----------------------------------
# Independent recompilations (config-bank<U>.yaml / .toml, built by
# `make bank-recomp`) for the streamed overlay records the game loads into RAM
# that overlay C also uses. They cannot share overlay C's VMAs in one ELF, so
# each unit namespaces its symbols (`ovl<U>_`) and they are registered at runtime
# by src/bank_overlays.cpp. One static library per unit: each unit's sources must
# include their own funcs.h.
file(GLOB BANK_UNIT_DIRS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../Bank*Funcs)

set(OGRE_BANK_LIBS)
foreach(BANK_UNIT_DIR ${BANK_UNIT_DIRS})
    get_filename_component(BANK_UNIT_LIB ${BANK_UNIT_DIR} NAME)
    file(GLOB BANK_UNIT_SOURCES CONFIGURE_DEPENDS ${BANK_UNIT_DIR}/*.c)

    add_library(${BANK_UNIT_LIB} STATIC ${BANK_UNIT_SOURCES})
    target_include_directories(${BANK_UNIT_LIB} PRIVATE
        ${BANK_UNIT_DIR}
        ${OGRE_TOOLS}/N64ModernRuntime/N64Recomp/include
        ${OGRE_TOOLS}/N64ModernRuntime/ultramodern/include
        ${OGRE_TOOLS}/N64ModernRuntime/librecomp/include
    )
    target_compile_options(${BANK_UNIT_LIB} PRIVATE
        -fno-strict-aliasing
        -fno-omit-frame-pointer
        -Wno-unused-variable
        -Wno-unused-parameter
        -Wno-unused-function
        -Wno-unused-but-set-variable
        -Wno-implicit-function-declaration
    )
    list(APPEND OGRE_BANK_LIBS ${BANK_UNIT_LIB})
endforeach()

# --- The app --------------------------------------------------------------
# Native: SDL-based entry + platform callbacks. Web: no-SDL entry + no-op
# callbacks; the browser main thread stays responsive while the runtime runs
# on pthreads (docs/WEB-PORT.md §11-12).
# gbi.cpp (F3DEX2 DL parser + workload analyzer, plan §15) is shared by both.
if (EMSCRIPTEN)
    set(OGRE_APP_SOURCES
        src/main_web.cpp
        src/web_platform.cpp
        src/rsp.cpp
        src/overlays.cpp
        src/bank_overlays.cpp
        src/gbi.cpp
    )
else()
    set(OGRE_APP_SOURCES
        src/main.cpp
        src/sdl_platform.cpp
        src/synth_frame.cpp
        src/rsp.cpp
        src/overlays.cpp
        src/bank_overlays.cpp
        src/gbi.cpp
    )
endif()

add_executable(ogrebattle64 ${OGRE_APP_SOURCES} ${OGRE_RENDERER_SOURCES})

# Keep frame pointers in the app + recompiled code so lldb can unwind a host
# backtrace at a crash. The runtime keeps a shadow *guest* call chain, but it is
# empty when the fault is inside runtime code reached through a bridge (a
# SIGFPE in the message-queue math, for example), and without frame pointers
# `bt` prints only the faulting frame. RT64 is left alone (hot path).
target_compile_options(ogrebattle64 PRIVATE -fno-omit-frame-pointer)

target_include_directories(ogrebattle64 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../RecompiledFuncs
    ${OGRE_TOOLS}/N64Recomp/include
    ${OGRE_TOOLS}/N64ModernRuntime/N64Recomp/include
    ${OGRE_TOOLS}/N64ModernRuntime/ultramodern/include
    ${OGRE_TOOLS}/N64ModernRuntime/librecomp/include
    ${OGRE_TOOLS}/N64ModernRuntime/thirdparty
)

if (OGRE_USE_RT64)
    # RT64 headers. RT64's own include_directories() only apply inside the RT64
    # subdirectory, so repeat them here for the app's renderer.cpp.
    target_include_directories(ogrebattle64 PRIVATE
        ${OGRE_TOOLS}/RT64/src
        ${OGRE_TOOLS}/RT64/src/contrib
        ${OGRE_TOOLS}/RT64/src/contrib/plume
        ${OGRE_TOOLS}/RT64/src/contrib/imgui
        ${OGRE_TOOLS}/RT64/src/contrib/hlslpp/include
        ${OGRE_TOOLS}/RT64/src/contrib/mupen64plus-core/src/api
        ${OGRE_TOOLS}/RT64/src/contrib/nativefiledialog-extended/src/include
    )
endif()

target_link_libraries(ogrebattle64 PRIVATE
    ogrebattle64_recomp
    ${OGRE_BANK_LIBS}
    librecomp
    ultramodern
)

if(RSP_RECOMP_SOURCES)
    target_link_libraries(ogrebattle64 PRIVATE ogrebattle64_rsp)
endif()

if (OGRE_USE_RT64)
    target_link_libraries(ogrebattle64 PRIVATE rt64)
endif()

if (NOT EMSCRIPTEN)
    target_link_libraries(ogrebattle64 PRIVATE SDL2::SDL2)
    if (UNIX AND NOT APPLE)
        find_package(Threads REQUIRED)
        target_link_libraries(ogrebattle64 PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
    endif()
endif()

# --- Emscripten-specific flags ----------------------------------------------
# pthreads (SharedArrayBuffer), a fixed 1 GiB heap (rdram needs >= 512 MiB;
# fixed — no ALLOW_MEMORY_GROWTH — so the SharedArrayBuffer handed to the
# AudioWorklet never moves), no runtime teardown after main() returns, the
# IDBFS filesystem for save persistence, and exports for the web shell.
# (Compile flags are set globally above; the remaining flags apply at link.)
if (EMSCRIPTEN)
    target_link_options(ogrebattle64 PRIVATE
        -pthread
        -fexceptions
        "-sPTHREAD_POOL_SIZE=16"
        "-sINITIAL_MEMORY=1073741824"
        "-sEXIT_RUNTIME=0"
        "-sSTACK_SIZE=1048576"
        "-sUSE_WEBGL2=1"
        "-sMAX_WEBGL_VERSION=2"
        
        "-sEXPORTED_FUNCTIONS=_ogre_start_boot,_ogre_poll_milestones,_ogre_input_set,_ogre_input_debug,_ogre_audio_state_ptr,_ogre_audio_ring_ptr,_ogre_audio_frames_available,_ogre_gfx_stats,_ogre_gfx_create_context,_ogre_gfx_set_canvas,_ogre_gfx_flush,_ogre_gfx_test_draw,_ogre_gfx_debug_tex,_ogre_gfx_debug_tex_count,_ogre_gfx_debug_last_tex,_ogre_gfx_debug_flags,_ogre_set_trace_enabled,_ogre_send_prnmi,_malloc,_free,_main"
        # Session-16 diagnostic: keep C function names in the wasm so worker
        # crash stacks identify the faulting game/runtime function. Remove
        # before any release (costs size).
        "--profiling-funcs"
        "-sEXPORTED_RUNTIME_METHODS=FS,IDBFS,ccall,cwrap,UTF8ToString,writeArrayToMemory,HEAPU8"
        "-lidbfs.js"
    )
endif()
