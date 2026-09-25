// Player-editable controller bindings for the app's own UI and the game.
//
// The port has two host input sources (SDL's keyboard state and an opened
// SDL_GameController) and one guest consumer (the N64 controller state the
// runtime polls). Until now both hardcoded their mapping in sdl_platform.cpp.
// This module holds that mapping as data so the launcher's CONTROLS tab and the
// in-game overlay can show and edit it, and `sdl_platform.cpp` reads it back
// every poll.
//
// One row per N64 button holds:
//   * one keyboard scancode, or SDL_SCANCODE_UNKNOWN for unbound;
//   * one gamepad source: a button, a direction of the right stick, or none.
// The analog stick (left stick / no keyboard key) and the right stick's C-button
// layer are fixed, not bindings; see N64ButtonInfo::fixed for what the row shows.
//
// The map is saved as plain text at `<config>/controls.cfg`. Key names come from
// SDL_GetScancodeName / SDL_GetScancodeFromName, so the file is readable and
// survives SDL updates; gamepad names come from the table below, so it does not
// depend on SDL_GameControllerGetStringForButton being present.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include <SDL.h>

namespace ogre {

// N64 controller button bits, in the order libultra reports them in the
// osContPad struct (and therefore the order the game reads).
enum N64Button : uint16_t {
    N64_BTN_A = 0x8000,
    N64_BTN_B = 0x4000,
    N64_BTN_Z = 0x2000,
    N64_BTN_START = 0x1000,
    N64_BTN_UP = 0x0800,
    N64_BTN_DOWN = 0x0400,
    N64_BTN_LEFT = 0x0200,
    N64_BTN_RIGHT = 0x0100,
    N64_BTN_L = 0x0020,
    N64_BTN_R = 0x0010,
    // The four C bits are libultra's CONT_C_UP/DOWN/LEFT/RIGHT values, which are
    // also the values in the recomp frontend's input_types.h
    // (tools/RecompFrontend/recompinput/include/recompinput/input_types.h).
    N64_BTN_C_UP = 0x0008,
    N64_BTN_C_DOWN = 0x0004,
    N64_BTN_C_LEFT = 0x0002,
    N64_BTN_C_RIGHT = 0x0001,
};

// One row per bindable N64 input. `field` is what the button does on the field
// map, from the game's controls description; it is the CONTROLS tab's second
// column. `tag` is the short spelling used by `OGRE_TAP_BUTTON`, kept here so the
// tap parser and the UI share one table.
struct N64ButtonInfo {
    const char* name;   // display name, e.g. "A", "C-UP"
    const char* tag;    // OGRE_TAP_BUTTON spelling, e.g. "a", "cu"
    const char* field;  // what it does on the field map ("" = nothing known)
    uint16_t bit;
};

constexpr int kN64ButtonCount = 14;

// Rows in display order: A, B, Z, START, L, R, D-pad, C buttons.
const N64ButtonInfo& n64_button_info(int index);

// The gamepad side of one row.
struct PadBinding {
    enum class Source {
        None,
        Button,      // `code` is an SDL_GameControllerButton
        RightStick,  // `dir` is -1 or +1 on SDL_CONTROLLER_AXIS_RIGHTX/Y
    };
    Source source = Source::None;
    int code = -1;
    int dir = 0;
    int axis = -1;  // SDL_CONTROLLER_AXIS_RIGHTX or RIGHTY for RightStick
};

struct ButtonBinding {
    SDL_Scancode key = SDL_SCANCODE_UNKNOWN;
    PadBinding pad;
};

// The process-wide binding set. Reads happen on the game thread (every input
// poll) and writes on the main thread (the UI), so access is through a mutex and
// a small copy; see read_input_map()/update_input_map().
class InputMap {
public:
    InputMap();

    void reset_defaults();

    int size() const { return kN64ButtonCount; }
    const ButtonBinding& row(int index) const;
    const N64ButtonInfo& info(int index) const { return n64_button_info(index); }

    // Localised display text for the two slots of a row ("-" when unbound).
    std::string key_text(int index) const;
    std::string pad_text(int index) const;

    // Edits.
    void set_key(int index, SDL_Scancode code);
    void set_pad_button(int index, int pad_code);
    void clear_key(int index);
    void clear_pad(int index);

    // Text form. The key name is whatever SDL_GetScancodeName returns; a name
    // that does not round-trip is dropped on load rather than kept wrong.
    std::string serialize() const;
    bool parse(const std::string& text);

private:
    ButtonBinding rows_[kN64ButtonCount];
};

// The live map: copied under a lock for each reader.
InputMap read_input_map();
// Replaces the live map and persists it to `<config>/controls.cfg`.
void update_input_map(const InputMap& map);

// Loads `<config>/controls.cfg` if it exists. Call once at startup, after
// `resolve_pref_dir()`. Missing or unreadable files leave the defaults.
void load_input_map(const std::filesystem::path& pref_dir);

// The configuration file's path for a given config directory.
std::filesystem::path input_map_path(const std::filesystem::path& pref_dir);

// N64 button state from the two host sources, using the current map.
uint16_t keyboard_buttons_from_map(const InputMap& map);
uint16_t gamecontroller_buttons_from_map(const InputMap& map, SDL_GameController* controller);

// Short names for the gamepad table and for capture ("A", "D-Up", "R-Stick Up").
std::string pad_button_name(int code);
std::string pad_source_text(const PadBinding& binding);

// The first gamepad button held on any of `pads`, or -1. Used by the rebinding
// capture in the launcher and the overlay; it is an edge in practice because the
// caller only polls while a capture is armed.
int first_pressed_pad_button(SDL_GameController* const* pads, int count);

// True for the key SDL reports for Escape; reserved for the in-game overlay, so
// it is never captured as a binding.
bool is_reserved_key(SDL_Scancode code);

}  // namespace ogre
