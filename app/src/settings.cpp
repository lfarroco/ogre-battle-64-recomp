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

// The live value. Only the main thread (the launcher loop and the overlay's
// update_gfx callback) reads and writes it, so it needs no lock.
int g_game_speed = 1;

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

// `game_speed = <n>`; a bare integer on its own line is accepted too, so a file
// edited by hand keeps working.
int parse_game_speed(const std::string& text) {
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
        std::string value = equals == std::string::npos ? line : line.substr(equals + 1);
        const size_t first = value.find_first_not_of(" \t\r");
        const size_t last = value.find_last_not_of(" \t\r");
        value = first == std::string::npos ? std::string{} : value.substr(first, last - first + 1);
        if (!value.empty()) {
            char* value_end = nullptr;
            const long parsed = std::strtol(value.c_str(), &value_end, 10);
            if (value_end != value.c_str() && parsed >= 1 && parsed <= 64) {
                return static_cast<int>(parsed);
            }
        }
        if (end == std::string::npos) {
            break;
        }
        pos = end + 1;
    }
    return 1;
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
        "game_speed = " + std::to_string(g_game_speed) + "\n";
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

std::filesystem::path settings_path(const std::filesystem::path& pref_dir) {
    return pref_dir / "settings.cfg";
}

void load_settings(const std::filesystem::path& pref_dir) {
    g_pref_dir = pref_dir;
    int speed = 1;
    bool have_file = false;
    const std::string text = read_file(settings_path(pref_dir), have_file);
    if (have_file) {
        // A file written by an older build can name a value the panel no longer
        // offers; the closest offered one keeps it selectable.
        speed = nearest_game_speed(parse_game_speed(text));
    }
    if (const int env = env_game_speed(); env != 0) {
        // A developer run names its speed on the command line; the value is
        // shown by the SETTINGS tab but not written over the player's file.
        speed = env;
        std::fprintf(stderr, "[settings] OGRE_SPEED=%d overrides the saved game speed\n", env);
    }
    g_game_speed = speed;
    ultramodern::set_speed_multiplier(static_cast<uint32_t>(speed));
    std::fprintf(stderr, "[settings] game speed x%d\n", speed);
}

}  // namespace ogre
