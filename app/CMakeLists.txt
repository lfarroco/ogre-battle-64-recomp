# Ogre Battle 64: Person of Lordly Caliber - PC port app.
#
# Builds the recompiled game code (RecompiledFuncs/) against N64ModernRuntime
# (ultramodern + librecomp) and links the RT64 renderer. See
# docs/guides/app-build.md for instructions.
#
#   cmake -S app -B build-app -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-app -j

cmake_minimum_required(VERSION 3.20)

if (APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum OS X deployment version")
endif()

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

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# Root of the third-party tree (sibling of app/).
set(OGRE_TOOLS "${CMAKE_CURRENT_SOURCE_DIR}/../tools")

# --- RT64 renderer -----------------------------------------------------------
# Static build, expecting an SDL window outside of Windows (same options as
# Zelda64Recomp). On macOS RT64 selects its Metal backend automatically.
set(RT64_STATIC TRUE)
set(RT64_SDL_WINDOW_VULKAN TRUE)
add_compile_definitions(HLSL_CPU)

# --- Runtime: ultramodern + librecomp (+ its bundled N64Recomp) --------------
add_subdirectory(${OGRE_TOOLS}/N64ModernRuntime ${CMAKE_BINARY_DIR}/N64ModernRuntime)

# --- Renderer ----------------------------------------------------------------
add_subdirectory(${OGRE_TOOLS}/RT64 ${CMAKE_BINARY_DIR}/RT64)
target_include_directories(rt64 PRIVATE ${CMAKE_BINARY_DIR}/RT64/src)

# --- SDL2 (window, input, audio) ---------------------------------------------
find_package(SDL2 REQUIRED)

# --- Recompiled game code ----------------------------------------------------
# The main-segment recompilation output (3659 functions). Compiled as a static
# lib so the app can link it alongside the runtime.
file(GLOB RECOMP_C_SOURCES   ${CMAKE_CURRENT_SOURCE_DIR}/../RecompiledFuncs/*.c)
file(GLOB RECOMP_CPP_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/../RecompiledFuncs/*.cpp)

add_library(ogrebattle64_recomp STATIC ${RECOMP_C_SOURCES} ${RECOMP_CPP_SOURCES})

target_include_directories(ogrebattle64_recomp PRIVATE
    ${OGRE_TOOLS}/N64ModernRuntime/N64Recomp/include
    ${OGRE_TOOLS}/N64ModernRuntime/ultramodern/include
    ${OGRE_TOOLS}/N64ModernRuntime/librecomp/include
)

target_compile_options(ogrebattle64_recomp PRIVATE
    -fno-strict-aliasing
    -Wno-unused-variable
    -Wno-unused-parameter
    -Wno-unused-function
    -Wno-unused-but-set-variable
    -Wno-implicit-function-declaration
)

# --- The app --------------------------------------------------------------
add_executable(ogrebattle64
    src/main.cpp
    src/sdl_platform.cpp
    src/renderer.cpp
    src/rsp.cpp
    src/overlays.cpp
)

target_include_directories(ogrebattle64 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../RecompiledFuncs
    ${OGRE_TOOLS}/N64Recomp/include
    ${OGRE_TOOLS}/N64ModernRuntime/N64Recomp/include
    ${OGRE_TOOLS}/N64ModernRuntime/ultramodern/include
    ${OGRE_TOOLS}/N64ModernRuntime/librecomp/include
    ${OGRE_TOOLS}/N64ModernRuntime/thirdparty
)

target_link_libraries(ogrebattle64 PRIVATE
    ogrebattle64_recomp
    librecomp
    ultramodern
    rt64
    SDL2::SDL2
)

if (APPLE)
    target_link_libraries(ogrebattle64 PRIVATE
        "-framework Cocoa"
        "-framework Metal"
        "-framework QuartzCore"
    )
elseif (UNIX AND NOT APPLE)
    find_package(Threads REQUIRED)
    target_link_libraries(ogrebattle64 PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
endif()
