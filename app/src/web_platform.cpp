#include "web_platform.hpp"

#include <cstdio>
#include <string>

namespace ogre {

// --- Input: no-op (Milestone 5) --------------------------------------------
static void poll_input() {}

static bool get_input(int controller_num, uint16_t* buttons, float* x, float* y) {
    (void)controller_num;
    (void)buttons;
    (void)x;
    (void)y;
    return false;
}

static void set_rumble(int controller_num, bool rumble) {
    (void)controller_num;
    (void)rumble;
}

static ultramodern::input::connected_device_info_t get_connected_device_info(int controller_num) {
    ultramodern::input::connected_device_info_t info{};
    info.connected_device = ultramodern::input::Device::None;
    info.connected_pak = ultramodern::input::Pak::None;
    (void)controller_num;
    return info;
}

ultramodern::input::callbacks_t make_input_callbacks() {
    return {
        .poll_input = poll_input,
        .get_input = get_input,
        .set_rumble = set_rumble,
        .get_connected_device_info = get_connected_device_info,
    };
}

// --- Audio: no-op (Milestone 5) ---------------------------------------------
static void queue_audio_samples(int16_t* samples, size_t count) {
    (void)samples;
    (void)count;
}

static size_t get_audio_frames_remaining() {
    return 0;
}

static void set_audio_frequency(uint32_t frequency) {
    (void)frequency;
}

ultramodern::audio_callbacks_t make_audio_callbacks() {
    return {
        .queue_samples = queue_audio_samples,
        .get_frames_remaining = get_audio_frames_remaining,
        .set_frequency = set_audio_frequency,
    };
}

// --- Error handling: log to the console -------------------------------------
static void show_message_box(const char* msg) {
    fprintf(stderr, "[web:error] %s\n", msg);
}

ultramodern::error_handling::callbacks_t make_error_handling_callbacks() {
    return {.message_box = show_message_box};
}

// --- Events: none ------------------------------------------------------------
ultramodern::events::callbacks_t make_events_callbacks() {
    return {
        .vi_callback = nullptr,
        .gfx_init_callback = nullptr,
    };
}

// --- Thread names ------------------------------------------------------------
static std::string get_game_thread_name(const OSThread* thread) {
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "N64 Thread %d", thread->id);
    return std::string(buffer);
}

ultramodern::threads::callbacks_t make_threads_callbacks() {
    return {.get_game_thread_name = get_game_thread_name};
}

}  // namespace ogre
