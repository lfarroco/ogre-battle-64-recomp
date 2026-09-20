// Player-editable controller bindings (see input_map.hpp).

#include "input_map.hpp"

#include <cstdio>
#include <mutex>
#include <sstream>
#include <vector>

namespace ogre {
namespace {

const N64ButtonInfo kButtons[kN64ButtonCount] = {
    {"A", "a", "Selects the highlighted object.", N64_BTN_A},
    {"B", "b", "Cancels a selection.", N64_BTN_B},
    {"Z", "z", "No field action listed.", N64_BTN_Z},
    {"START", "start", "Pauses the game.", N64_BTN_START},
    {"L", "l", "No field action listed.", N64_BTN_L},
    {"R", "r", "Brings up the action menu.", N64_BTN_R},
    {"D-UP", "up", "Moves the cursor.", N64_BTN_UP},
    {"D-DOWN", "down", "Moves the cursor.", N64_BTN_DOWN},
    {"D-LEFT", "left", "Moves the cursor.", N64_BTN_LEFT},
    {"D-RIGHT", "right", "Moves the cursor.", N64_BTN_RIGHT},
    {"C-UP", "cu", "Raises the camera angle.", N64_BTN_C_UP},
    {"C-DOWN", "cd", "Lowers the camera angle.", N64_BTN_C_DOWN},
    {"C-LEFT", "cl", "Toggles the overview map.", N64_BTN_C_LEFT},
    {"C-RIGHT", "cr", "Toggles scroll modes, and pauses the game while held.",
     N64_BTN_C_RIGHT},
};

// Gamepad button names. The tags are the file/parse spellings; the labels are
// what the CONTROLS tab shows. SDL's own names are close, but its availability
// and exact spelling have changed across versions, so the table is local.
struct PadName {
    int code;
    const char* tag;
    const char* label;
};

const PadName kPadNames[] = {
    {SDL_CONTROLLER_BUTTON_A, "a", "A"},
    {SDL_CONTROLLER_BUTTON_B, "b", "B"},
    {SDL_CONTROLLER_BUTTON_X, "x", "X"},
    {SDL_CONTROLLER_BUTTON_Y, "y", "Y"},
    {SDL_CONTROLLER_BUTTON_BACK, "back", "Back"},
    {SDL_CONTROLLER_BUTTON_GUIDE, "guide", "Guide"},
    {SDL_CONTROLLER_BUTTON_START, "start", "Start"},
    {SDL_CONTROLLER_BUTTON_LEFTSTICK, "lstick", "L-Stick"},
    {SDL_CONTROLLER_BUTTON_RIGHTSTICK, "rstick", "R-Stick"},
    {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, "lb", "LB"},
    {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, "rb", "RB"},
    {SDL_CONTROLLER_BUTTON_DPAD_UP, "dup", "D-Up"},
    {SDL_CONTROLLER_BUTTON_DPAD_DOWN, "ddown", "D-Down"},
    {SDL_CONTROLLER_BUTTON_DPAD_LEFT, "dleft", "D-Left"},
    {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, "dright", "D-Right"},
};

// The right-stick directions the C buttons can bind to. `axis` is the SDL axis
// the sign of `dir` is read on.
struct StickDir {
    const char* tag;
    const char* label;
    int axis;
    int dir;
};

const StickDir kStickDirs[] = {
    {"rstick-up", "R-Stick Up", SDL_CONTROLLER_AXIS_RIGHTY, -1},
    {"rstick-down", "R-Stick Down", SDL_CONTROLLER_AXIS_RIGHTY, 1},
    {"rstick-left", "R-Stick Left", SDL_CONTROLLER_AXIS_RIGHTX, -1},
    {"rstick-right", "R-Stick Right", SDL_CONTROLLER_AXIS_RIGHTX, 1},
};

// The keyboard defaults match the mapping sdl_platform.cpp hardcoded before the
// bindings were editable: X/Z/C for A/B/Z, Enter for Start, Q/E for L/R, the
// arrow keys for the D-pad and I/J/K/L for the C buttons.
const SDL_Scancode kDefaultKeys[kN64ButtonCount] = {
    SDL_SCANCODE_X,     SDL_SCANCODE_Z,    SDL_SCANCODE_C,     SDL_SCANCODE_RETURN,
    SDL_SCANCODE_Q,     SDL_SCANCODE_E,    SDL_SCANCODE_UP,    SDL_SCANCODE_DOWN,
    SDL_SCANCODE_LEFT,  SDL_SCANCODE_RIGHT, SDL_SCANCODE_I,    SDL_SCANCODE_K,
    SDL_SCANCODE_J,     SDL_SCANCODE_L,
};

// The gamepad defaults match the old hardcoded map. C-LEFT and C-RIGHT had no
// button (the fixed right stick covered them) and stay unbound here; the right
// stick layer is still always on, so the defaults reproduce the old behaviour
// with the C buttons also reachable from the stick.
struct PadDefault {
    PadBinding::Source source;
    int code;
    int dir;
    int axis;
};

const PadDefault kDefaultPads[kN64ButtonCount] = {
    {PadBinding::Source::Button, SDL_CONTROLLER_BUTTON_A, 0, -1},
    {PadBinding::Source::Button, SDL_CONTROLLER_BUTTON_B, 0, -1},
    {PadBinding::Source::Button, SDL_CONTROLLER_BUTTON_LEFTSTICK, 0, -1},
    {PadBinding::Source::Button, SDL_CONTROLLER_BUTTON_START, 0, -1},
    {PadBinding::Source::Button, SDL_CONTROLLER_BUTTON_LEFTSHOULDER, 0, -1},
    {PadBinding::Source::Button, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, 0, -1},
    {PadBinding::Source::Button, SDL_CONTROLLER_BUTTON_DPAD_UP, 0, -1},
    {PadBinding::Source::Button, SDL_CONTROLLER_BUTTON_DPAD_DOWN, 0, -1},
    {PadBinding::Source::Button, SDL_CONTROLLER_BUTTON_DPAD_LEFT, 0, -1},
    {PadBinding::Source::Button, SDL_CONTROLLER_BUTTON_DPAD_RIGHT, 0, -1},
    {PadBinding::Source::Button, SDL_CONTROLLER_BUTTON_X, 0, -1},
    {PadBinding::Source::Button, SDL_CONTROLLER_BUTTON_Y, 0, -1},
    {PadBinding::Source::None, -1, 0, -1},
    {PadBinding::Source::None, -1, 0, -1},
};

const PadName* find_pad_name(const std::string& tag) {
    for (const PadName& name : kPadNames) {
        if (tag == name.tag) {
            return &name;
        }
    }
    return nullptr;
}

const StickDir* find_stick_dir(const std::string& tag) {
    for (const StickDir& dir : kStickDirs) {
        if (tag == dir.tag) {
            return &dir;
        }
    }
    return nullptr;
}

std::string trim(const std::string& text) {
    const size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

std::mutex g_map_mutex;
InputMap g_map;             // guarded by g_map_mutex
std::filesystem::path g_pref_dir;

}  // namespace

const N64ButtonInfo& n64_button_info(int index) {
    if (index < 0 || index >= kN64ButtonCount) {
        return kButtons[0];
    }
    return kButtons[index];
}

InputMap::InputMap() {
    reset_defaults();
}

void InputMap::reset_defaults() {
    for (int i = 0; i < kN64ButtonCount; i++) {
        rows_[i].key = kDefaultKeys[i];
        rows_[i].pad.source = kDefaultPads[i].source;
        rows_[i].pad.code = kDefaultPads[i].code;
        rows_[i].pad.dir = kDefaultPads[i].dir;
        rows_[i].pad.axis = kDefaultPads[i].axis;
    }
}

const ButtonBinding& InputMap::row(int index) const {
    if (index < 0 || index >= kN64ButtonCount) {
        return rows_[0];
    }
    return rows_[index];
}

std::string InputMap::key_text(int index) const {
    const SDL_Scancode code = row(index).key;
    if (code == SDL_SCANCODE_UNKNOWN) {
        return "-";
    }
    const char* name = SDL_GetScancodeName(code);
    return (name != nullptr && name[0] != '\0') ? name : "-";
}

std::string InputMap::pad_text(int index) const {
    return pad_source_text(row(index).pad);
}

void InputMap::set_key(int index, SDL_Scancode code) {
    if (index >= 0 && index < kN64ButtonCount) {
        rows_[index].key = code;
    }
}

void InputMap::set_pad_button(int index, int pad_code) {
    if (index < 0 || index >= kN64ButtonCount) {
        return;
    }
    rows_[index].pad.source = PadBinding::Source::Button;
    rows_[index].pad.code = pad_code;
    rows_[index].pad.dir = 0;
    rows_[index].pad.axis = -1;
}

void InputMap::clear_key(int index) {
    if (index >= 0 && index < kN64ButtonCount) {
        rows_[index].key = SDL_SCANCODE_UNKNOWN;
    }
}

void InputMap::clear_pad(int index) {
    if (index >= 0 && index < kN64ButtonCount) {
        rows_[index].pad = PadBinding{};
    }
}

std::string InputMap::serialize() const {
    std::string out =
        "# Ogre Battle 64: Recomp controller bindings.\n"
        "# <n64 button>.<key|pad> = <value>; `-` and `none` are unbound.\n"
        "# Edited from the launcher's CONTROLS tab, or the in-game Esc overlay.\n";
    for (int i = 0; i < kN64ButtonCount; i++) {
        const N64ButtonInfo& info = kButtons[i];
        out += std::string(info.tag) + ".key = " + key_text(i) + "\n";
        out += std::string(info.tag) + ".pad = ";
        const PadBinding& pad = rows_[i].pad;
        switch (pad.source) {
            case PadBinding::Source::Button: {
                std::string tag = "none";
                for (const PadName& name : kPadNames) {
                    if (name.code == pad.code) {
                        tag = name.tag;
                        break;
                    }
                }
                out += "button:" + tag;
                break;
            }
            case PadBinding::Source::RightStick: {
                std::string tag = "rstick-up";
                for (const StickDir& dir : kStickDirs) {
                    if (dir.axis == pad.axis && dir.dir == pad.dir) {
                        tag = dir.tag;
                        break;
                    }
                }
                out += tag;
                break;
            }
            case PadBinding::Source::None:
            default:
                out += "none";
                break;
        }
        out += "\n";
    }
    return out;
}

bool InputMap::parse(const std::string& text) {
    InputMap parsed;  // defaults for anything the file does not mention
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        const size_t hash = line.find('#');
        if (hash != std::string::npos) {
            line = line.substr(0, hash);
        }
        const size_t equals = line.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        const std::string field = trim(line.substr(0, equals));
        const std::string value = trim(line.substr(equals + 1));
        const size_t dot = field.rfind('.');
        if (dot == std::string::npos) {
            continue;
        }
        const std::string tag = field.substr(0, dot);
        const std::string slot = field.substr(dot + 1);
        int index = -1;
        for (int i = 0; i < kN64ButtonCount; i++) {
            if (tag == kButtons[i].tag) {
                index = i;
                break;
            }
        }
        if (index < 0) {
            continue;
        }
        if (slot == "key") {
            if (value.empty() || value == "-") {
                parsed.rows_[index].key = SDL_SCANCODE_UNKNOWN;
            }
            else {
                const SDL_Scancode code = SDL_GetScancodeFromName(value.c_str());
                parsed.rows_[index].key = code;
            }
        }
        else if (slot == "pad") {
            if (value.empty() || value == "none") {
                parsed.rows_[index].pad = PadBinding{};
            }
            else if (value.rfind("button:", 0) == 0) {
                const PadName* name = find_pad_name(value.substr(7));
                if (name != nullptr) {
                    parsed.set_pad_button(index, name->code);
                }
            }
            else {
                const StickDir* dir = find_stick_dir(value);
                if (dir != nullptr) {
                    parsed.rows_[index].pad.source = PadBinding::Source::RightStick;
                    parsed.rows_[index].pad.dir = dir->dir;
                    parsed.rows_[index].pad.axis = dir->axis;
                    parsed.rows_[index].pad.code = -1;
                }
            }
        }
    }
    *this = parsed;
    return true;
}

std::string pad_button_name(int code) {
    for (const PadName& name : kPadNames) {
        if (name.code == code) {
            return name.label;
        }
    }
    return "?";
}

std::string pad_source_text(const PadBinding& binding) {
    switch (binding.source) {
        case PadBinding::Source::Button:
            return pad_button_name(binding.code);
        case PadBinding::Source::RightStick:
            for (const StickDir& dir : kStickDirs) {
                if (dir.axis == binding.axis && dir.dir == binding.dir) {
                    return dir.label;
                }
            }
            return "R-Stick";
        case PadBinding::Source::None:
        default:
            return "-";
    }
}

bool is_reserved_key(SDL_Scancode code) {
    // Escape opens and closes the in-game overlay; binding it to a game button
    // would make the two actions fight.
    return code == SDL_SCANCODE_ESCAPE;
}

int first_pressed_pad_button(SDL_GameController* const* pads, int count) {
    for (int i = 0; i < count; i++) {
        SDL_GameController* pad = pads[i];
        if (pad == nullptr) {
            continue;
        }
        for (const PadName& name : kPadNames) {
            if (SDL_GameControllerGetButton(
                    pad, static_cast<SDL_GameControllerButton>(name.code)) != 0) {
                return name.code;
            }
        }
    }
    return -1;
}

// --- the live map -------------------------------------------------------------

InputMap read_input_map() {
    std::lock_guard<std::mutex> lock(g_map_mutex);
    return g_map;
}

void update_input_map(const InputMap& map) {
    {
        std::lock_guard<std::mutex> lock(g_map_mutex);
        g_map = map;
    }
    if (!g_pref_dir.empty()) {
        const std::filesystem::path path = input_map_path(g_pref_dir);
        if (FILE* file = std::fopen(path.string().c_str(), "wb")) {
            const std::string text = map.serialize();
            std::fwrite(text.data(), 1, text.size(), file);
            std::fclose(file);
        }
        else {
            std::fprintf(stderr, "[input] could not write %s\n", path.string().c_str());
        }
    }
}

std::filesystem::path input_map_path(const std::filesystem::path& pref_dir) {
    return pref_dir / "controls.cfg";
}

void load_input_map(const std::filesystem::path& pref_dir) {
    g_pref_dir = pref_dir;
    const std::filesystem::path path = input_map_path(pref_dir);
    FILE* file = std::fopen(path.string().c_str(), "rb");
    if (file == nullptr) {
        return;  // defaults
    }
    std::string text;
    char buffer[512];
    size_t got = 0;
    while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
        text.append(buffer, got);
    }
    std::fclose(file);
    InputMap loaded;
    loaded.parse(text);
    {
        std::lock_guard<std::mutex> lock(g_map_mutex);
        g_map = loaded;
    }
    std::fprintf(stderr, "[input] loaded %s\n", path.string().c_str());
}

uint16_t keyboard_buttons_from_map(const InputMap& map) {
    const Uint8* keys = SDL_GetKeyboardState(nullptr);
    uint16_t buttons = 0;
    for (int i = 0; i < map.size(); i++) {
        const SDL_Scancode code = map.row(i).key;
        if (code != SDL_SCANCODE_UNKNOWN && keys[code] != 0) {
            buttons |= map.info(i).bit;
        }
    }
    return buttons;
}

uint16_t gamecontroller_buttons_from_map(const InputMap& map, SDL_GameController* controller) {
    if (controller == nullptr) {
        return 0;
    }
    uint16_t buttons = 0;
    for (int i = 0; i < map.size(); i++) {
        const PadBinding& pad = map.row(i).pad;
        switch (pad.source) {
            case PadBinding::Source::Button:
                if (pad.code >= 0 &&
                    SDL_GameControllerGetButton(controller,
                                                static_cast<SDL_GameControllerButton>(pad.code)) != 0) {
                    buttons |= map.info(i).bit;
                }
                break;
            case PadBinding::Source::RightStick: {
                const Sint16 value = SDL_GameControllerGetAxis(
                    controller, static_cast<SDL_GameControllerAxis>(pad.axis));
                if (pad.dir < 0 ? value < -8000 : value > 8000) {
                    buttons |= map.info(i).bit;
                }
                break;
            }
            case PadBinding::Source::None:
            default:
                break;
        }
    }
    return buttons;
}

}  // namespace ogre
