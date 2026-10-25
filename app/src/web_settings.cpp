// Browser bridge for the settings the native Esc overlay owns (see
// web_settings.hpp).

#include "web_settings.hpp"

#include <cstdint>
#include <cstdlib>
#include <vector>

#include <ultramodern/ultramodern.hpp>

#include "bank_overlays.hpp"
#include "game.hpp"
#include "hd_backgrounds.hpp"
#include "map_cursor.hpp"
#include "mission_cursor.hpp"
#include "settings.hpp"

namespace ogre {
namespace {

// The scenes the wide view is used for. Mirrors the native `expand_scenes()` in
// `widescreen.cpp`; that file includes SDL and is not part of the wasm target,
// so the set lives here for the browser. `OGRE_WS_SCENES` replaces it for a
// developer run.
const std::vector<uint16_t>& expand_scenes() {
    static const std::vector<uint16_t> scenes = [] {
        std::vector<uint16_t> out{0x0003, 0x000D};
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
            out.push_back(static_cast<uint16_t>(id & 0xFFFFu));
            p = end;
            while (*p == ',' || *p == ' ' || *p == '+') {
                ++p;
            }
        }
        if (out.empty()) {
            out.push_back(0x0003);
        }
        return out;
    }();
    return scenes;
}

int guest_byte(uint32_t address) {
    uint8_t* rdram = ultramodern::get_rdram_base();
    if (rdram == nullptr) {
        return -1;
    }
    return rdram[(address & 0x1FFFFFFFu) ^ 3u];
}

}  // namespace

void web_settings_load() {
    load_settings("/ogre");
}

int chaos_frame() {
    return guest_byte(CHAOS_FRAME_ADDRESS);
}

bool web_widescreen_expand() {
    if (widescreen_mode() == WidescreenMode::Off) {
        return false;
    }
    const uint16_t scene = active_scene_id();
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

}  // namespace ogre

extern "C" void ogre_web_settings_load() {
    ogre::web_settings_load();
}

extern "C" int ogre_web_settings_game_speed() {
    return ogre::game_speed();
}

extern "C" void ogre_web_settings_set_game_speed(int multiplier) {
    ogre::set_game_speed(multiplier);
}

extern "C" int ogre_web_settings_widescreen() {
    return ogre::widescreen_mode() == ogre::WidescreenMode::On ? 1 : 0;
}

extern "C" void ogre_web_settings_set_widescreen(int on) {
    ogre::set_widescreen_mode(on != 0 ? ogre::WidescreenMode::On : ogre::WidescreenMode::Off);
}

extern "C" int ogre_web_settings_mouse() {
    return ogre::mouse_mode() == ogre::MouseMode::On ? 1 : 0;
}

extern "C" void ogre_web_settings_set_mouse(int on) {
    ogre::set_mouse_mode(on != 0 ? ogre::MouseMode::On : ogre::MouseMode::Off);
}

extern "C" int ogre_web_settings_display_experiments() {
    return ogre::display_experiments_enabled() ? 1 : 0;
}

extern "C" int ogre_web_chaos_frame() {
    return ogre::chaos_frame();
}

extern "C" int ogre_web_active_scene() {
    return static_cast<int>(ogre::active_scene_id());
}

extern "C" int ogre_web_widescreen_expand() {
    return ogre::web_widescreen_expand() ? 1 : 0;
}

extern "C" void ogre_web_mouse_move(int x, int y, int win_w, int win_h, int draw_w, int draw_h) {
    ogre_map_cursor_note_mouse(x, y, win_w, win_h, draw_w, draw_h);
    ogre_mission_cursor_note_mouse(x, y, win_w, win_h, draw_w, draw_h);
}

extern "C" void ogre_web_mouse_click(int x, int y, int win_w, int win_h, int draw_w, int draw_h) {
    ogre_map_cursor_click(x, y, win_w, win_h, draw_w, draw_h);
    ogre_mission_cursor_click(x, y, win_w, win_h, draw_w, draw_h);
}

extern "C" void ogre_web_mouse_leave() {
    ogre_map_cursor_clear();
    ogre_mission_cursor_clear();
}
