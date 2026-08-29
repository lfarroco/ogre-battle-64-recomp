// Ogre Battle 64: Person of Lordly Caliber - null renderer.
//
// Implements ultramodern::renderer::RendererContext without a GPU. Used by the
// native build when RT64 is disabled (-DOGRE_USE_RT64=OFF) and by the
// WebAssembly build (docs/WEB-PORT.md §8).
//
// It accepts display-list submissions, tracks basic statistics (plan §14) and
// logs boot milestones (plan §13). It never creates a graphics context and
// never links against RT64.

#include "renderer.hpp"

#include <atomic>
#include <cstdio>
#include <cstdint>
#include <memory>

#include "ultramodern/renderer_context.hpp"
#include "ultramodern/ultramodern.hpp"

#include "milestones.hpp"

namespace ogre {

namespace {

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
        OGRE_MILESTONE("RENDERER", "null renderer shut down (%u DLs, %u VI swaps)",
                       display_list_count_.load(), vi_swap_count_.load());
    }

    uint32_t get_display_framerate() const override { return 60; }

    float get_resolution_scale() const override { return 1.0f; }

  private:
    std::atomic<unsigned int> vi_swap_count_{0};
    std::atomic<unsigned int> display_list_count_{0};
};

}  // namespace

std::unique_ptr<ultramodern::renderer::RendererContext> create_renderer(
    uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode) {
    (void)rdram;
    (void)window_handle;
    (void)developer_mode;
    return std::make_unique<NullRenderer>();
}

}  // namespace ogre
