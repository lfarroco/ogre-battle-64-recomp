// Player-editable app settings (see settings.hpp).

#include "settings.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "ultramodern/ultramodern.hpp"

namespace ogre {
namespace {

// The GAME SPEED radio group. The values are a subset of what the port's guides
// use for `OGRE_SPEED`; 6 and 8 were dropped because the game's own cursor
// aiming cannot keep up at those speeds.
const int kGameSpeeds[kGameSpeedCount] = {1, 2, 4};

// The config directory the file is written to; empty until load_settings runs,
// in which case a change still applies to the runtime but is not persisted.
std::filesystem::path g_pref_dir;

// The live values. Only the main thread (the launcher loop and the overlay's
// update_gfx callback) reads and writes them, so they need no lock.
int g_game_speed = 1;
WidescreenMode g_widescreen = WidescreenMode::Off;

// `OGRE_SPEED`'s value, or 0 when unset or unparsable. The runtime reads the
// same variable itself for its initial value, so this is only used to decide
// precedence and to show the value the run started with.
int env_game_speed() {
    const char* env = std::getenv("OGRE_SPEED");
    if (env == nullptr || env[0] == '\0') {
        return 0;
    }
    char* end = nullptr;
    const long value = std::strtol(env, &end, 10);
    if (end == env || value < 1 || value > 64) {
        return 0;
    }
    return static_cast<int>(value);
}

std::string lowercase(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return text;
}

std::string trim(const std::string& text) {
    const size_t first = text.find_first_not_of(" \t\r");
    if (first == std::string::npos) {
        return {};
    }
    const size_t last = text.find_last_not_of(" \t\r");
    return text.substr(first, last - first + 1);
}

// The value of `wanted` in a `key = value` file: comments start at `#`, the key
// is matched case-insensitively after trimming, and the first match wins.
std::string setting_value(const std::string& text, const char* wanted) {
    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t end = text.find('\n', pos);
        std::string line = text.substr(pos, end == std::string::npos ? std::string::npos
                                                                    : end - pos);
        const size_t hash = line.find('#');
        if (hash != std::string::npos) {
            line = line.substr(0, hash);
        }
        const size_t equals = line.find('=');
        if (equals != std::string::npos &&
            lowercase(trim(line.substr(0, equals))) == wanted) {
            return trim(line.substr(equals + 1));
        }
        if (end == std::string::npos) {
            break;
        }
        pos = end + 1;
    }
    return {};
}

// `game_speed = <n>`; a bare integer on its own line is accepted too, so a file
// edited by hand before WIDESCREEN existed keeps working.
int parse_game_speed(const std::string& text) {
    std::string value = setting_value(text, "game_speed");
    if (value.empty()) {
        size_t pos = 0;
        while (pos <= text.size()) {
            const size_t end = text.find('\n', pos);
            std::string line = text.substr(pos, end == std::string::npos ? std::string::npos
                                                                        : end - pos);
            const size_t hash = line.find('#');
            if (hash != std::string::npos) {
                line = line.substr(0, hash);
            }
            value = trim(line);
            if (!value.empty() && value.find('=') == std::string::npos) {
                break;
            }
            if (end == std::string::npos) {
                return 1;
            }
            pos = end + 1;
        }
    }
    char* value_end = nullptr;
    const long parsed = std::strtol(value.c_str(), &value_end, 10);
    if (value_end != value.c_str() && parsed >= 1 && parsed <= 64) {
        return static_cast<int>(parsed);
    }
    return 1;
}

// `off` / `missions` / `always`, or 0 / 1 / 2.
WidescreenMode parse_widescreen(const std::string& text) {
    const std::string value = lowercase(setting_value(text, "widescreen"));
    if (value == "always" || value == "2") {
        return WidescreenMode::Always;
    }
    if (value == "missions" || value == "mission" || value == "1") {
        return WidescreenMode::Missions;
    }
    return WidescreenMode::Off;
}

std::string read_file(const std::filesystem::path& path, bool& ok) {
    FILE* file = std::fopen(path.string().c_str(), "rb");
    if (file == nullptr) {
        ok = false;
        return {};
    }
    std::string text;
    char buffer[512];
    size_t got = 0;
    while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
        text.append(buffer, got);
    }
    std::fclose(file);
    ok = true;
    return text;
}

void write_settings() {
    if (g_pref_dir.empty()) {
        return;
    }
    const std::filesystem::path path = settings_path(g_pref_dir);
    FILE* file = std::fopen(path.string().c_str(), "wb");
    if (file == nullptr) {
        std::fprintf(stderr, "[settings] could not write %s\n", path.string().c_str());
        return;
    }
    const std::string text =
        "# Ogre Battle 64: Recomp settings.\n"
        "# Edited from the launcher's SETTINGS tab, or the in-game Esc overlay.\n"
        "# game_speed: the emulated clock's multiplier (1, 2 or 4).\n"
        "game_speed = " + std::to_string(g_game_speed) + "\n"
        "# widescreen: off, missions (only the mission scene) or always.\n"
        "widescreen = " + lowercase(widescreen_mode_label(static_cast<int>(g_widescreen))) + "\n";
    std::fwrite(text.data(), 1, text.size(), file);
    std::fclose(file);
}

}  // namespace

int game_speed_value(int index) {
    if (index < 0 || index >= kGameSpeedCount) {
        return kGameSpeeds[0];
    }
    return kGameSpeeds[index];
}

int game_speed_index(int value) {
    for (int i = 0; i < kGameSpeedCount; i++) {
        if (kGameSpeeds[i] == value) {
            return i;
        }
    }
    return -1;
}

int nearest_game_speed(int value) {
    int best = kGameSpeeds[0];
    int best_distance = -1;
    for (int i = 0; i < kGameSpeedCount; i++) {
        const int distance = kGameSpeeds[i] > value ? kGameSpeeds[i] - value : value - kGameSpeeds[i];
        if (best_distance < 0 || distance < best_distance) {
            best = kGameSpeeds[i];
            best_distance = distance;
        }
    }
    return best;
}

int game_speed() {
    return g_game_speed;
}

void set_game_speed(int multiplier) {
    if (multiplier < 1) {
        multiplier = 1;
    }
    if (multiplier > 64) {
        multiplier = 64;
    }
    if (multiplier == g_game_speed) {
        return;
    }
    g_game_speed = multiplier;
    ultramodern::set_speed_multiplier(static_cast<uint32_t>(multiplier));
    write_settings();
}

const char* widescreen_mode_label(int index) {
    switch (static_cast<WidescreenMode>(index)) {
        case WidescreenMode::Off:      return "OFF";
        case WidescreenMode::Missions: return "MISSIONS";
        case WidescreenMode::Always:   return "ALWAYS";
    }
    return "OFF";
}

WidescreenMode widescreen_mode() {
    return g_widescreen;
}

int widescreen_mode_index() {
    const int index = static_cast<int>(g_widescreen);
    return index >= 0 && index < kWidescreenModeCount ? index : -1;
}

void set_widescreen_mode(WidescreenMode mode) {
    if (mode == g_widescreen) {
        return;
    }
    g_widescreen = mode;
    write_settings();
}

std::string widescreen_mode_text() {
    std::string text;
    for (int i = 0; i < kWidescreenModeCount; i++) {
        if (!text.empty()) {
            text += "  ";
        }
        text += std::string(i == static_cast<int>(g_widescreen) ? "[x] " : "[ ] ") +
                widescreen_mode_label(i);
    }
    return text;
}

std::filesystem::path settings_path(const std::filesystem::path& pref_dir) {
    return pref_dir / "settings.cfg";
}

void load_settings(const std::filesystem::path& pref_dir) {
    g_pref_dir = pref_dir;
    int speed = 1;
    WidescreenMode widescreen = WidescreenMode::Off;
    bool have_file = false;
    const std::string text = read_file(settings_path(pref_dir), have_file);
    if (have_file) {
        // A file written by an older build can name a value the panel no longer
        // offers; the closest offered one keeps it selectable.
        speed = nearest_game_speed(parse_game_speed(text));
        widescreen = parse_widescreen(text);
    }
    if (const int env = env_game_speed(); env != 0) {
        // A developer run names its speed on the command line; the value is
        // shown by the SETTINGS tab but not written over the player's file.
        speed = env;
        std::fprintf(stderr, "[settings] OGRE_SPEED=%d overrides the saved game speed\n", env);
    }
    // OGRE_WIDESCREEN=off|missions|always: the same rule as OGRE_SPEED. It is
    // also how a scripted run reaches the setting without the UI.
    if (const char* env = std::getenv("OGRE_WIDESCREEN"); env != nullptr && env[0] != '\0') {
        widescreen = parse_widescreen(std::string("widescreen = ") + env);
        std::fprintf(stderr, "[settings] OGRE_WIDESCREEN=%s overrides the saved widescreen mode\n",
                     env);
    }
    g_game_speed = speed;
    g_widescreen = widescreen;
    ultramodern::set_speed_multiplier(static_cast<uint32_t>(speed));
    std::fprintf(stderr, "[settings] game speed x%d, widescreen %s\n", speed,
                 lowercase(widescreen_mode_label(static_cast<int>(widescreen))).c_str());
}

}  // namespace ogre
