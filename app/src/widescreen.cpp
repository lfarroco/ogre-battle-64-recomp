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
// and Expand is limited to the mission scene the setting exists for.
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

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include <SDL.h>
#include <ultramodern/config.hpp>
#include <ultramodern/ultramodern.hpp>

#include "bank_overlays.hpp"
#include "hd_backgrounds.hpp"
#include "settings.hpp"

namespace ogre {
namespace {

// Scene `0x03` is the mission: the 3D field with the party, its intro, and the
// battles inside it. Descriptor `0x8018F350`; see docs/scenes.md. It is the
// scene the developer asked widescreen for.
constexpr uint16_t kMissionScene = 0x0003;

// Scene `0x0D` is the New Game dialogue/cutscene scene. Its backdrop is a
// 496x384 canvas and the 4:3 view crops it, so Expand reveals the sides of the
// game's own art (verified with the HD-background pack off: the wide capture
// shows statues and columns the 4:3 capture does not). It expands whether or not
// the pack is installed.
constexpr uint16_t kDialogueScene = 0x000D;

// The scenes Expand is used for by default. `OGRE_WS_SCENES=<hex>[,<hex>...]`
// replaces the set for a developer run. The set is parsed once. A scene the
// HD-background pack covers is added by `scene_expands` on top of this.
const std::vector<uint16_t>& expand_scenes() {
    static const std::vector<uint16_t> scenes = [] {
        std::vector<uint16_t> out{kMissionScene, kDialogueScene};
        const char* spec = std::getenv("OGRE_WS_SCENES");
        if (spec == nullptr || spec[0] == '\0') {
            return out;
        }
        out.clear();
        const char* p = spec;
        while (*p != '\0') {
            char* end = nullptr;
            const unsigned long id = std::strtoul(p, &end, 0);
            if (end == p) {
                break;
            }
            out.push_back(uint16_t(id & 0xFFFFu));
            p = end;
            while (*p == ',' || *p == ' ' || *p == '+') {
                ++p;
            }
        }
        if (out.empty()) {
            out.push_back(kMissionScene);
        }
        return out;
    }();
    return scenes;
}

bool scene_expands(uint16_t scene) {
    // A scene the pack covers expands too, so the replacement fills the wide
    // window instead of being pillarboxed.
    if (hd_backgrounds_covers_scene(scene)) {
        return true;
    }
    for (uint16_t id : expand_scenes()) {
        if (id == scene) {
            return true;
        }
    }
    return false;
}

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

// Set by the renderer when it has left fullscreen, consumed by the next
// `widescreen_update`. Written from the renderer thread.
std::atomic<bool> g_refit_requested{false};

int width_for_height(int height, float aspect) {
    return static_cast<int>(std::lround(static_cast<double>(height) * aspect));
}

// Whether Expand is on this frame: the toggle is on and the dispatcher is
// running a scene in the expand set.
bool wants_expand() {
    return widescreen_mode() != WidescreenMode::Off && scene_expands(active_scene_id());
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
    if (!g_have_last_mode) {
        // The first call is the frame the game starts on, and the window already
        // has this mode's shape: `create_window` calls `widescreen_fit_window`
        // right after creating it, with the mode `load_settings` left in place.
        // Reporting a change here would re-fit the window on the first frame,
        // which is wrong for a run that starts fullscreen: on Windows RT64's
        // fullscreen path is a raw `SetWindowPos` to the monitor rect, so SDL
        // still has the size it created the window at, and `SDL_SetWindowSize`
        // then shrank the fullscreen window to the 4:3 shape in the display's
        // top-left corner, with the desktop around it (developer report,
        // session 121).
        g_last_mode = mode;
        g_have_last_mode = true;
    }
    else if (mode != g_last_mode) {
        g_last_mode = mode;
        mode_changed = true;
    }

    // The renderer has left fullscreen, which puts the window back under SDL's
    // control; the shape is re-derived from the mode now, after the exit rather
    // than in the same frame as the request (the renderer applies the change on
    // its own thread).
    if (g_refit_requested.exchange(false)) {
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
    // Fullscreen owns the window's geometry: RT64 sets it with a raw
    // `SetWindowPos` on Windows and `[NSWindow toggleFullScreen:]` on macOS, and
    // neither updates SDL's idea of the window's size. Reshaping the window here
    // would shrink the picture into a corner of the display. The port knows it
    // asked for fullscreen because it wrote `wm_option` itself, which is more
    // reliable than SDL's flags: `SDL_WINDOW_FULLSCREEN` is not set on the
    // Windows path, because SDL did not perform the transition.
    if (ultramodern::renderer::get_graphics_config().wm_option ==
        ultramodern::renderer::WindowMode::Fullscreen) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            std::fprintf(stderr,
                         "[widescreen] fullscreen: the renderer owns the window's shape\n");
        }
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

void widescreen_request_refit() {
    g_refit_requested.store(true);
}

void widescreen_initial_window_size(int& width, int& height) {
    height = kInitialHeight;
    width = width_for_height(height, window_aspect());
}

}  // namespace ogre
