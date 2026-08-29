#include "web_platform.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace ogre {

// ============================================================================
// Input — real keyboard/gamepad state (Milestone 5)
// ============================================================================
//
// The browser main thread captures keyboard/gamepad events and pushes the
// current state into the atomics below via the exported ogre_input_set()
// (declared in web_platform.hpp). The runtime's game thread reads the same
// atomics from the input callbacks (osContStartReadData/osContGetReadData),
// so no DOM/browser code ever runs on the game thread.
//
// Slot assignment (parity with the native SDL build): slot 0 is the keyboard
// (always connected), slots 1-3 are gamepads.

namespace {

struct alignas(8) ControllerState {
    std::atomic<uint16_t> buttons{0};
    std::atomic<float> stick_x{0.0f};
    std::atomic<float> stick_y{0.0f};
    std::atomic<int32_t> connected{0};
};

ControllerState g_controllers[4];

}  // namespace

extern "C" void ogre_input_set(int controller, uint16_t buttons, float x, float y, int connected) {
    if (controller < 0 || controller >= 4) {
        return;
    }
    ControllerState& c = g_controllers[controller];
    c.buttons.store(buttons, std::memory_order_relaxed);
    c.stick_x.store(x, std::memory_order_relaxed);
    c.stick_y.store(y, std::memory_order_relaxed);
    c.connected.store(connected, std::memory_order_relaxed);
}

// Debug helper used by the web shell/tests to confirm the JS -> wasm input
// path works (returns the current slot-0 button mask).
extern "C" uint16_t ogre_input_debug() {
    return g_controllers[0].buttons.load(std::memory_order_relaxed);
}

static void poll_input() {
    // Input state is event-driven (JS pushes it via ogre_input_set), so there
    // is nothing to poll on this side.
}

static bool get_input(int controller_num, uint16_t* buttons, float* x, float* y) {
    if (controller_num < 0 || controller_num >= 4) {
        return false;
    }
    ControllerState& c = g_controllers[controller_num];
    if (c.connected.load(std::memory_order_relaxed) == 0) {
        return false;
    }
    *buttons = c.buttons.load(std::memory_order_relaxed);
    *x = c.stick_x.load(std::memory_order_relaxed);
    *y = c.stick_y.load(std::memory_order_relaxed);
    return true;
}

static void set_rumble(int controller_num, bool rumble) {
    // The web build has no rumble support yet (browser vibration API is a
    // possible future addition); accept and ignore.
    (void)controller_num;
    (void)rumble;
}

static ultramodern::input::connected_device_info_t get_connected_device_info(int controller_num) {
    ultramodern::input::connected_device_info_t info{};
    info.connected_device = ultramodern::input::Device::None;
    info.connected_pak = ultramodern::input::Pak::None;

    if (controller_num == 0) {
        // Slot 0 is always the keyboard.
        info.connected_device = ultramodern::input::Device::Controller;
    } else if (controller_num >= 1 && controller_num < 4 &&
               g_controllers[controller_num].connected.load(std::memory_order_relaxed) != 0) {
        info.connected_device = ultramodern::input::Device::Controller;
    }
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

// ============================================================================
// Audio — ring buffer in wasm memory consumed by an AudioWorklet (M5)
// ============================================================================
//
// The game thread pushes interleaved stereo int16 samples via
// queue_audio_samples(); the browser AudioWorklet (app/web/audio-worklet.js)
// reads them straight out of wasm memory — a SharedArrayBuffer, kept at a
// fixed 1 GiB so the buffer never moves — using the state/ring pointers
// exported below, and resamples from the game rate to the AudioContext rate.
//
// head/tail are absolute uint32 counters that wrap naturally; the ring index
// is (counter & (ring_frames - 1)). ring_frames must stay a power of two.

namespace {

constexpr uint32_t kAudioRingFrames = 32768;  // ~0.74 s of stereo at 44.1 kHz

struct alignas(64) AudioRingState {
    std::atomic<uint32_t> head{0};        // producer cursor, in frames
    std::atomic<uint32_t> tail{0};        // consumer cursor, in frames
    std::atomic<uint32_t> rate{48000};    // current game sample rate (Hz)
    std::atomic<uint32_t> enabled{0};     // 1 once the game has queued audio
    std::atomic<uint32_t> ring_frames{kAudioRingFrames};
};

AudioRingState g_audio_state;
alignas(64) int16_t g_audio_ring[kAudioRingFrames * 2];

}  // namespace

// Byte offset of the ring state within linear memory (for the AudioWorklet).
extern "C" uint32_t ogre_audio_state_ptr() {
    return reinterpret_cast<uint32_t>(&g_audio_state);
}

// Byte offset of the sample ring within linear memory (for the AudioWorklet).
extern "C" uint32_t ogre_audio_ring_ptr() {
    return reinterpret_cast<uint32_t>(g_audio_ring);
}

// Number of stereo frames currently buffered (used by the web shell/test to
// confirm audio is flowing).
extern "C" uint32_t ogre_audio_frames_available() {
    return g_audio_state.head.load(std::memory_order_acquire) -
           g_audio_state.tail.load(std::memory_order_acquire);
}

static void queue_audio_samples(int16_t* samples, size_t count) {
    // count is the number of int16 samples (interleaved stereo), so the frame
    // count is count/2.
    const size_t frames = count / 2;
    if (frames == 0 || samples == nullptr) {
        return;
    }

    static bool first_queue_logged = false;
    if (!first_queue_logged) {
        fprintf(stderr, "[web:audio] game queued its first audio buffer (%zu frames)\n", frames);
        first_queue_logged = true;
    }

    AudioRingState& s = g_audio_state;
    const uint32_t ring_frames = s.ring_frames.load(std::memory_order_relaxed);
    const uint32_t mask = ring_frames - 1;

    const uint32_t head = s.head.load(std::memory_order_acquire);
    const uint32_t tail = s.tail.load(std::memory_order_acquire);
    const uint32_t avail = head - tail;

    // Drop-newest when full. The worklet drains at real time; if it is not
    // consuming yet (autoplay blocked / audio not started), the game's
    // osAiGetLength pacing keeps the ring full and we simply discard rather
    // than letting the backlog grow forever.
    if (avail + frames > ring_frames) {
        static bool warned = false;
        if (!warned) {
            fprintf(stderr, "[web:audio] ring full, dropping audio (consumer not draining?)\n");
            warned = true;
        }
        return;
    }

    uint32_t idx = head;
    for (size_t i = 0; i < frames; ++i, ++idx) {
        g_audio_ring[((idx & mask) << 1) + 0] = samples[i * 2 + 0];
        g_audio_ring[((idx & mask) << 1) + 1] = samples[i * 2 + 1];
    }
    // Publish the samples before advancing head (release pairs with the
    // worklet's Atomics.load).
    s.head.store(head + frames, std::memory_order_release);
    s.enabled.store(1, std::memory_order_release);
}

static size_t get_audio_frames_remaining() {
    AudioRingState& s = g_audio_state;
    return static_cast<size_t>(s.head.load(std::memory_order_acquire) -
                               s.tail.load(std::memory_order_acquire));
}

static void set_audio_frequency(uint32_t frequency) {
    if (frequency == 0) {
        return;
    }
    uint32_t prev = g_audio_state.rate.exchange(frequency, std::memory_order_relaxed);
    if (prev != frequency) {
        fprintf(stderr, "[web:audio] game sample rate set to %u Hz\n", frequency);
    }
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
