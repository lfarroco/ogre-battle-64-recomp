// Ogre Battle 64: Person of Lordly Caliber - null renderer.
//
// Implements ultramodern::renderer::RendererContext without a GPU. Used by the
// native build when RT64 is disabled (-DOGRE_USE_RT64=OFF) and by the
// WebAssembly build (docs/WEB-PORT.md §8).
//
// It accepts display-list submissions, tracks basic statistics (plan §14) and
// logs boot milestones (plan §13). It never creates a graphics context and
// never links against RT64.
//
// Since milestone 6 (plan §15) it also runs the F3DEX2 workload analyzer
// (gbi.hpp) over every display list, so the browser-renderer decision can be
// made from real data about what Ogre Battle 64 actually submits. The
// aggregate stats are exported to the web page via ogre_gfx_stats().

#include "renderer.hpp"

#include <atomic>
#include <cstdio>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "ultramodern/renderer_context.hpp"
#include "ultramodern/ultramodern.hpp"

#include "gbi.hpp"
#include "milestones.hpp"

namespace ogre {

namespace {

// Aggregate analyzer state. send_dl runs on the runtime's gfx thread; the web
// page reads it from the JS main thread via ogre_gfx_stats(), so both the
// stats and the snapshot buffer are mutex-guarded.
gbi::WorkloadStats g_workload;
std::mutex g_stats_mutex;
char g_stats_snapshot[4096];

void refresh_stats_snapshot() {
    std::string summary = gbi::format_summary(g_workload);
    snprintf(g_stats_snapshot, sizeof(g_stats_snapshot), "%s", summary.c_str());
}

class NullRenderer final : public ultramodern::renderer::RendererContext {
  public:
    NullRenderer() {
        setup_result = ultramodern::renderer::SetupResult::Success;
        chosen_api = ultramodern::renderer::GraphicsApi::Auto;
        OGRE_MILESTONE("RENDERER", "null renderer initialized (no GPU)");
    }

    bool valid() override { return true; }

    bool update_config(const ultramodern::renderer::GraphicsConfig& old_config,
                       const ultramodern::renderer::GraphicsConfig& new_config) override {
        return false;
    }

    void enable_instant_present() override {}

    void send_dl(const OSTask* task) override {
        const unsigned frame = ++display_list_count_;

        // Run the F3DEX2 workload analyzer (plan §15, milestone 6).
        if (rdram_ != nullptr) {
            const uint32_t dl_offset = task->t.data_ptr & 0x3FFFFFF;
            {
                std::lock_guard<std::mutex> lock(g_stats_mutex);
                gbi::analyze_dl(rdram_, dl_offset, task->t.data_size, g_workload);
            }
        }

        // Instrumentation (plan §14): first 16 DLs + every 300th, plus a
        // lightweight per-task summary. OB64's gfx ucode is F3DEX 2.08.
        if (frame <= 16 || (frame % 300) == 0) {
            printf("[renderer] send_dl frame=%u type=%u ucode=0x%08X ucode_data=0x%08X "
                   "data_ptr=0x%08X data_size=0x%X\n",
                   frame, static_cast<unsigned>(task->t.type), static_cast<unsigned>(task->t.ucode),
                   static_cast<unsigned>(task->t.ucode_data), static_cast<unsigned>(task->t.data_ptr),
                   static_cast<unsigned>(task->t.data_size));
            OGRE_MILESTONE("RSP", "display list submitted (frame %u, type %u, ucode 0x%08X, data_ptr 0x%08X)",
                           frame, static_cast<unsigned>(task->t.type),
                           static_cast<unsigned>(task->t.ucode), static_cast<unsigned>(task->t.data_ptr));
        }

        // Refresh the exported stats snapshot (cheap: only every task; a page
        // poll just reads the last snapshot).
        {
            std::lock_guard<std::mutex> lock(g_stats_mutex);
            refresh_stats_snapshot();
        }

        // Periodic workload summary (plan §15): first task + every 50th, so a
        // long-running native session shows what the game actually submits.
        if (frame == 1 || (frame % 50) == 0) {
            std::lock_guard<std::mutex> lock(g_stats_mutex);
            std::string summary = gbi::format_summary(g_workload);
            OGRE_MILESTONE("GFX-WORKLOAD", "%s", summary.c_str());
        }
    }

    void send_dummy_workload(uint32_t fb_address) override {
        OGRE_MILESTONE("VI", "dummy workload fb=0x%08X", fb_address);
    }

    void update_screen() override {
        const unsigned frame = ++vi_swap_count_;
        if (frame <= 16 || (frame % 300) == 0) {
            printf("[renderer] update_screen (VI swap) count=%u\n", frame);
            OGRE_MILESTONE("VI", "update_screen (VI swap) count=%u", frame);
        }
    }

    void shutdown() override {
        std::lock_guard<std::mutex> lock(g_stats_mutex);
        refresh_stats_snapshot();
        OGRE_MILESTONE("RENDERER", "null renderer shut down (%u DLs, %u VI swaps)",
                       display_list_count_.load(), vi_swap_count_.load());
        OGRE_MILESTONE("GFX-WORKLOAD", "%s", g_stats_snapshot);
    }

    uint32_t get_display_framerate() const override { return 60; }

    float get_resolution_scale() const override { return 1.0f; }

    void set_rdram(uint8_t* rdram) { rdram_ = rdram; }

  private:
    std::atomic<unsigned int> vi_swap_count_{0};
    std::atomic<unsigned int> display_list_count_{0};
    uint8_t* rdram_ = nullptr;
};

NullRenderer* g_active_renderer = nullptr;

}  // namespace

std::unique_ptr<ultramodern::renderer::RendererContext> create_renderer(
    uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode) {
    (void)window_handle;
    (void)developer_mode;
    auto renderer = std::make_unique<NullRenderer>();
    renderer->set_rdram(rdram);
    g_active_renderer = renderer.get();
    return renderer;
}

}  // namespace ogre

extern "C" {

// Returns the accumulated graphics-workload summary (milestone 6). The buffer
// is valid until the next call; the web shell copies it with UTF8ToString.
const char* ogre_gfx_stats() {
    std::lock_guard<std::mutex> lock(ogre::g_stats_mutex);
    return ogre::g_stats_snapshot;
}

}  // extern "C"
