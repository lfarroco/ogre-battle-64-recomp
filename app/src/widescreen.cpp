// Widescreen: the WIDESCREEN setting applied to RT64's aspect ratio.
//
// RT64 has three aspect-ratio modes (`AspectRatio::Original`, `Expand`,
// `Manual`). Expand is the one that widens the field of view instead of
// stretching the picture: `rt64_workload_queue.cpp:134-211` derives an
// `aspectRatioScale` from the window, and `rt64_projection_processor.cpp:101`
// counter-scales the game's projection matrices by `1/scale` for the
// projections that cover the screen and are wider than tall
// (`ProjectionProcessor::processScene`). The portable result is hor+: the same
// vertical framing with more world at the sides. The 2D draws that are not
// counter-scaled are stretched horizontally, which is why the default is Off
// and `Missions` is the mode the setting exists for.
//
// The value lives in `GraphicsConfig` and reaches RT64 through the runtime:
// `ultramodern::renderer::set_graphics_config` flags an `UpdateConfigAction`,
// the renderer thread picks it up in `events.cpp`, and `RT64Renderer::
// update_config` (`renderer.cpp:917`) forwards it to `app_->updateUserConfig`,
// which discards the framebuffers that a new aspect ratio invalidates.
//
// The port owns this value: nothing else in the app writes `ar_option`, and
// this file is the only writer. `RT64Renderer::update_config` also copies the
// whole config, so the previous value of the other options must survive; it
// does, because the starting point is the live config.

#include "widescreen.hpp"

#include <cmath>
#include <cstdio>

#include <SDL.h>
#include <ultramodern/config.hpp>
#include <ultramodern/ultramodern.hpp>

#include "bank_overlays.hpp"
#include "settings.hpp"

namespace ogre {
namespace {

// Scene `0x03` is the mission: the 3D field with the party, its intro, and the
// battles inside it. Descriptor `0x8018F350`; see docs/scenes.md. It is the
// scene the developer asked widescreen for, and the only one `Missions` turns
// the mode on for.
constexpr uint16_t kMissionScene = 0x0003;

// The VI framebuffer is 320x240, so 4:3 is the game's own shape. 16:9 is the
// shape a widescreen mode gives the window: `ProjectionProcessor` counter-scales
// the projections by the same scale the framebuffer renderer stretches by, so
// the window's shape is the target that scale is derived from.
constexpr float kOriginalAspect = 4.0f / 3.0f;
constexpr float kExpandAspect = 16.0f / 9.0f;

// The height a window opens at. The width follows the shape.
constexpr int kInitialHeight = 720;

// The mode as of the previous `widescreen_update` call, so a change of mode is
// what resizes the window and a scene change is not.
WidescreenMode g_last_mode = WidescreenMode::Off;
bool g_have_last_mode = false;

int width_for_height(int height, float aspect) {
    return static_cast<int>(std::lround(static_cast<double>(height) * aspect));
}

bool wants_expand() {
    switch (widescreen_mode()) {
        case WidescreenMode::Off:
            return false;
        case WidescreenMode::Always:
            return true;
        case WidescreenMode::Missions:
            return active_scene_id() == kMissionScene;
    }
    return false;
}

// The shape the *window* has, which is the mode's and not the scene's: any
// widescreen mode keeps the window wide for the whole run, and the 4:3 scenes
// are pillarboxed inside it.
float window_aspect() {
    return widescreen_mode() == WidescreenMode::Off ? kOriginalAspect : kExpandAspect;
}

const char* option_name(ultramodern::renderer::AspectRatio option) {
    switch (option) {
        case ultramodern::renderer::AspectRatio::Original:
            return "original";
        case ultramodern::renderer::AspectRatio::Expand:
            return "expand";
        case ultramodern::renderer::AspectRatio::Manual:
            return "manual";
        default:
            return "?";
    }
}

}  // namespace

bool widescreen_update() {
    // A mode change is the one thing that changes the window's shape. The
    // scene's own change must not: the callers use this to decide whether to
    // re-fit the window, and resizing on every scene transition is the
    // behaviour the developer rejected.
    bool mode_changed = false;
    const WidescreenMode mode = widescreen_mode();
    if (!g_have_last_mode || mode != g_last_mode) {
        g_last_mode = mode;
        g_have_last_mode = true;
        mode_changed = true;
    }

    const ultramodern::renderer::AspectRatio wanted =
        wants_expand() ? ultramodern::renderer::AspectRatio::Expand
                       : ultramodern::renderer::AspectRatio::Original;

    const ultramodern::renderer::GraphicsConfig config =
        ultramodern::renderer::get_graphics_config();
    if (config.ar_option == wanted) {
        return mode_changed;
    }

    ultramodern::renderer::GraphicsConfig next = config;
    next.ar_option = wanted;
    ultramodern::renderer::set_graphics_config(next);
    std::fprintf(stderr, "[widescreen] aspect ratio %s (scene 0x%04X, mode %s)\n",
                 option_name(wanted), (unsigned)active_scene_id(),
                 widescreen_mode_label(widescreen_mode_index()));
    return mode_changed;
}

void widescreen_fit_window(SDL_Window* window) {
    if (window == nullptr) {
        return;
    }
    int width = 0;
    int height = 0;
    SDL_GetWindowSize(window, &width, &height);
    if (width <= 0 || height <= 0) {
        return;
    }
    const int wanted = width_for_height(height, window_aspect());
    if (wanted == width) {
        return;
    }
    SDL_SetWindowSize(window, wanted, height);
    std::fprintf(stderr, "[widescreen] window %dx%d -> %dx%d (window %s, mode %s)\n", width, height,
                 wanted, height, widescreen_mode() == WidescreenMode::Off ? "4:3" : "16:9",
                 widescreen_mode_label(widescreen_mode_index()));
}

void widescreen_initial_window_size(int& width, int& height) {
    height = kInitialHeight;
    width = width_for_height(height, window_aspect());
}

}  // namespace ogre
