#include "renderer.hpp"

#include <cstdio>
#include <mutex>

// -----------------------------------------------------------------------------
// Null renderer for the bring-up phase.
//
// Ogre Battle 64 renders everything through the RDP, and RDP commands only
// exist after the game's RSP microcode is recompiled. Until then there is
// nothing meaningful to draw, so this context acknowledges VI updates and
// display-list submissions while logging boot progress.
//
// When the RSP microcode is wired up, this file is replaced by the RT64-based
// renderer (tools/RT64) via the same `ultramodern::renderer::RendererContext`
// interface.
// -----------------------------------------------------------------------------

namespace ogre {

class NullRenderer final : public ultramodern::renderer::RendererContext {
  public:
    NullRenderer() {
        setup_result = ultramodern::renderer::SetupResult::Success;
        chosen_api = ultramodern::renderer::GraphicsApi::Auto;
    }

    bool valid() override { return true; }

    bool update_config(const ultramodern::renderer::GraphicsConfig& old_config,
                       const ultramodern::renderer::GraphicsConfig& new_config) override {
        return false;
    }

    void enable_instant_present() override {}

    void send_dl(const OSTask* task) override {
        unsigned frame = ++display_list_count_;
        if (frame <= 16 || (frame % 300) == 0) {
            printf("[renderer] send_dl frame=%u type=%u\n", frame, static_cast<unsigned>(task->t.type));
        }
    }

    void send_dummy_workload(uint32_t fb_address) override {
        printf("[renderer] send_dummy_workload fb=0x%08X\n", fb_address);
    }

    void update_screen() override {
        unsigned frame = ++vi_swap_count_;
        if (frame <= 16 || (frame % 300) == 0) {
            printf("[renderer] update_screen (VI swap) count=%u\n", frame);
        }
    }

    void shutdown() override {}

    uint32_t get_display_framerate() const override { return 60; }

    float get_resolution_scale() const override { return 1.0f; }

  private:
    unsigned int vi_swap_count_ = 0;
    unsigned int display_list_count_ = 0;
};

std::unique_ptr<ultramodern::renderer::RendererContext> create_renderer(
    uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode) {
    return std::make_unique<NullRenderer>();
}

}  // namespace ogre
