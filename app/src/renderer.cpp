// Ogre Battle 64: Person of Lordly Caliber - RT64-based renderer.
//
// Replaces the bring-up null renderer. Wraps RT64's Application (Vulkan on
// Linux) behind ultramodern's RendererContext interface. Display lists are
// parsed by RT64's built-in GBI interpreters; OB64's ucode is F3DEX 2.08,
// which RT64 auto-detects from the OSTask's ucode data via loadUCodeGBI.
//
// Adapted from N64Recomp/RecompFrontend's rt64_render_context.cpp (minus the
// RecompFrontend UI / texture-pack / mod wiring).

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <memory>
#include <string>

#include "hle/rt64_application.h"
#include "shared/rt64_f3d_defines.h"

#include "ultramodern/renderer_context.hpp"
#include "ultramodern/ultramodern.hpp"

#include "renderer.hpp"

namespace ogre {

namespace {

// RSP DMEM/IMEM and the RDP FIFO registers RT64's Application::Core expects.
// The game's RSP task code is reimplemented by the runtime / RT64, so these
// are just static slots RT64 can read/write during display list processing.
uint8_t DMEM[0x1000];
uint8_t IMEM[0x1000];

uint32_t MI_INTR_REG = 0;
uint32_t DPC_START_REG = 0;
uint32_t DPC_END_REG = 0;
uint32_t DPC_CURRENT_REG = 0;
uint32_t DPC_STATUS_REG = 0;
uint32_t DPC_CLOCK_REG = 0;
uint32_t DPC_BUFBUSY_REG = 0;
uint32_t DPC_PIPEBUSY_REG = 0;
uint32_t DPC_TMEM_REG = 0;

uint8_t dummy_rom_header[0x40];

void dummy_check_interrupts() {}
// --- ultramodern GraphicsConfig -> RT64 config mappers -------------------

RT64::UserConfiguration::Antialiasing to_rt64(ultramodern::renderer::Antialiasing option) {
    switch (option) {
        case ultramodern::renderer::Antialiasing::None:   return RT64::UserConfiguration::Antialiasing::None;
        case ultramodern::renderer::Antialiasing::MSAA2X: return RT64::UserConfiguration::Antialiasing::MSAA2X;
        case ultramodern::renderer::Antialiasing::MSAA4X: return RT64::UserConfiguration::Antialiasing::MSAA4X;
        case ultramodern::renderer::Antialiasing::MSAA8X: return RT64::UserConfiguration::Antialiasing::MSAA8X;
        default: return RT64::UserConfiguration::Antialiasing::OptionCount;
    }
}

RT64::UserConfiguration::AspectRatio to_rt64(ultramodern::renderer::AspectRatio option) {
    switch (option) {
        case ultramodern::renderer::AspectRatio::Original: return RT64::UserConfiguration::AspectRatio::Original;
        case ultramodern::renderer::AspectRatio::Expand:   return RT64::UserConfiguration::AspectRatio::Expand;
        case ultramodern::renderer::AspectRatio::Manual:   return RT64::UserConfiguration::AspectRatio::Manual;
        default: return RT64::UserConfiguration::AspectRatio::OptionCount;
    }
}

RT64::UserConfiguration::RefreshRate to_rt64(ultramodern::renderer::RefreshRate option) {
    switch (option) {
        case ultramodern::renderer::RefreshRate::Original: return RT64::UserConfiguration::RefreshRate::Original;
        case ultramodern::renderer::RefreshRate::Display:  return RT64::UserConfiguration::RefreshRate::Display;
        case ultramodern::renderer::RefreshRate::Manual:   return RT64::UserConfiguration::RefreshRate::Manual;
        default: return RT64::UserConfiguration::RefreshRate::OptionCount;
    }
}

RT64::UserConfiguration::InternalColorFormat to_rt64(ultramodern::renderer::HighPrecisionFramebuffer option) {
    switch (option) {
        case ultramodern::renderer::HighPrecisionFramebuffer::Off:  return RT64::UserConfiguration::InternalColorFormat::Standard;
        case ultramodern::renderer::HighPrecisionFramebuffer::On:   return RT64::UserConfiguration::InternalColorFormat::High;
        case ultramodern::renderer::HighPrecisionFramebuffer::Auto: return RT64::UserConfiguration::InternalColorFormat::Automatic;
        default: return RT64::UserConfiguration::InternalColorFormat::OptionCount;
    }
}

RT64::EnhancementConfiguration::Presentation::Mode to_rt64(ultramodern::renderer::PresentationMode mode) {
    switch (mode) {
        case ultramodern::renderer::PresentationMode::Console:       return RT64::EnhancementConfiguration::Presentation::Mode::Console;
        case ultramodern::renderer::PresentationMode::SkipBuffering: return RT64::EnhancementConfiguration::Presentation::Mode::SkipBuffering;
        case ultramodern::renderer::PresentationMode::PresentEarly:  return RT64::EnhancementConfiguration::Presentation::Mode::PresentEarly;
    }
    return RT64::EnhancementConfiguration::Presentation::Mode::Console;
}



void set_application_user_config(RT64::Application* app, const ultramodern::renderer::GraphicsConfig& config) {
    switch (config.res_option) {
        default:
        case ultramodern::renderer::Resolution::Auto:
            app->userConfig.resolution = RT64::UserConfiguration::Resolution::WindowIntegerScale;
            app->userConfig.downsampleMultiplier = 1;
            break;
        case ultramodern::renderer::Resolution::Original:
            app->userConfig.resolution = RT64::UserConfiguration::Resolution::Original;
            app->userConfig.downsampleMultiplier = 1;
            break;
        case ultramodern::renderer::Resolution::Original2x:
            app->userConfig.resolution = RT64::UserConfiguration::Resolution::Manual;
            app->userConfig.resolutionMultiplier = 2.0 * std::max(config.ds_option, 1);
            app->userConfig.downsampleMultiplier = std::max(config.ds_option, 1);
            break;
    }

    app->userConfig.aspectRatio = to_rt64(config.ar_option);
    app->userConfig.antialiasing = to_rt64(config.msaa_option);
    app->userConfig.refreshRate = to_rt64(config.rr_option);
    app->userConfig.refreshRateTarget = config.rr_manual_value;
    app->userConfig.internalColorFormat = to_rt64(config.hpfb_option);
    app->userConfig.displayBuffering = RT64::UserConfiguration::DisplayBuffering::Triple;
}

ultramodern::renderer::SetupResult map_setup_result(RT64::Application::SetupResult result) {
    switch (result) {
        case RT64::Application::SetupResult::Success:                   return ultramodern::renderer::SetupResult::Success;
        case RT64::Application::SetupResult::DynamicLibrariesNotFound: return ultramodern::renderer::SetupResult::DynamicLibrariesNotFound;
        case RT64::Application::SetupResult::InvalidGraphicsAPI:       return ultramodern::renderer::SetupResult::InvalidGraphicsAPI;
        case RT64::Application::SetupResult::GraphicsAPINotFound:      return ultramodern::renderer::SetupResult::GraphicsAPINotFound;
        case RT64::Application::SetupResult::GraphicsDeviceNotFound:   return ultramodern::renderer::SetupResult::GraphicsDeviceNotFound;
    }
    fprintf(stderr, "Unhandled RT64::Application::SetupResult\n");
    return ultramodern::renderer::SetupResult::GraphicsAPINotFound;
}

ultramodern::renderer::GraphicsApi map_graphics_api(RT64::UserConfiguration::GraphicsAPI api) {
    switch (api) {
        case RT64::UserConfiguration::GraphicsAPI::D3D12:     return ultramodern::renderer::GraphicsApi::D3D12;
        case RT64::UserConfiguration::GraphicsAPI::Vulkan:    return ultramodern::renderer::GraphicsApi::Vulkan;
        case RT64::UserConfiguration::GraphicsAPI::Metal:     return ultramodern::renderer::GraphicsApi::Metal;
        case RT64::UserConfiguration::GraphicsAPI::Automatic: return ultramodern::renderer::GraphicsApi::Auto;
        default: break;
    }
    fprintf(stderr, "Unhandled RT64::UserConfiguration::GraphicsAPI\n");
    return ultramodern::renderer::GraphicsApi::Auto;
}

// --- RT64Renderer ---------------------------------------------------------

class RT64Renderer final : public ultramodern::renderer::RendererContext {
  public:
    RT64Renderer(uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode) {
        setup_result = ultramodern::renderer::SetupResult::Success;
        chosen_api = ultramodern::renderer::GraphicsApi::Auto;

        // Set up the RT64 application core fields.
        RT64::Application::Core app_core{};
#if defined(_WIN32)
        app_core.window = window_handle.window;
#elif defined(__linux__) || defined(__ANDROID__)
        app_core.window = window_handle;
#elif defined(__APPLE__)
        app_core.window.window = window_handle.window;
        app_core.window.view = window_handle.view;
#endif
        app_core.checkInterrupts = dummy_check_interrupts;

        app_core.HEADER = dummy_rom_header;
        app_core.RDRAM = rdram;
        app_core.DMEM = DMEM;
        app_core.IMEM = IMEM;

        app_core.MI_INTR_REG = &MI_INTR_REG;

        app_core.DPC_START_REG = &DPC_START_REG;
        app_core.DPC_END_REG = &DPC_END_REG;
        app_core.DPC_CURRENT_REG = &DPC_CURRENT_REG;
        app_core.DPC_STATUS_REG = &DPC_STATUS_REG;
        app_core.DPC_CLOCK_REG = &DPC_CLOCK_REG;
        app_core.DPC_BUFBUSY_REG = &DPC_BUFBUSY_REG;
        app_core.DPC_PIPEBUSY_REG = &DPC_PIPEBUSY_REG;
        app_core.DPC_TMEM_REG = &DPC_TMEM_REG;

        // The VI registers are owned by ultramodern (written by the recompiled
        // osVi* calls); point RT64 at them so it decodes the real VI mode.
        ultramodern::renderer::ViRegs* vi_regs = ultramodern::renderer::get_vi_regs();
        app_core.VI_STATUS_REG = &vi_regs->VI_STATUS_REG;
        app_core.VI_ORIGIN_REG = &vi_regs->VI_ORIGIN_REG;
        app_core.VI_WIDTH_REG = &vi_regs->VI_WIDTH_REG;
        app_core.VI_INTR_REG = &vi_regs->VI_INTR_REG;
        app_core.VI_V_CURRENT_LINE_REG = &vi_regs->VI_V_CURRENT_LINE_REG;
        app_core.VI_TIMING_REG = &vi_regs->VI_TIMING_REG;
        app_core.VI_V_SYNC_REG = &vi_regs->VI_V_SYNC_REG;
        app_core.VI_H_SYNC_REG = &vi_regs->VI_H_SYNC_REG;
        app_core.VI_LEAP_REG = &vi_regs->VI_LEAP_REG;
        app_core.VI_H_START_REG = &vi_regs->VI_H_START_REG;
        app_core.VI_V_START_REG = &vi_regs->VI_V_START_REG;
        app_core.VI_V_BURST_REG = &vi_regs->VI_V_BURST_REG;
        app_core.VI_X_SCALE_REG = &vi_regs->VI_X_SCALE_REG;
        app_core.VI_Y_SCALE_REG = &vi_regs->VI_Y_SCALE_REG;

        // RT64 writes its own configuration to the config path otherwise.
        RT64::ApplicationConfiguration app_config;
        app_config.appId = "ogrebattle64";
        app_config.useConfigurationFile = false;

        // Create the RT64 application.
        app_ = std::make_unique<RT64::Application>(app_core, app_config);

        // Initial user config from the current ultramodern settings.
        const auto& cur_config = ultramodern::renderer::get_graphics_config();
        set_application_user_config(app_.get(), cur_config);
        app_->userConfig.developerMode = developer_mode;
        // OB64's ucode is F3DEX 2.08; RT64's getGBIForUCode picks the right GBI
        // from the OSTask's ucode data on every send_dl, so no override needed.
        app_->userConfig.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::Automatic;

        // Set up the RT64 application (window surface, device, shaders, workers).
        uint32_t thread_id = 0;
#ifdef _WIN32
        thread_id = window_handle.thread_id;
#endif
        setup_result = map_setup_result(app_->setup(thread_id));
        chosen_api = map_graphics_api(app_->chosenGraphicsAPI);
        if (setup_result != ultramodern::renderer::SetupResult::Success) {
            fprintf(stderr, "[renderer] RT64 setup failed (rt64 api=%d)\n", static_cast<int>(app_->chosenGraphicsAPI));
            app_ = nullptr;
            return;
        }

        app_->setFullScreen(cur_config.wm_option == ultramodern::renderer::WindowMode::Fullscreen);
        printf("[renderer] RT64 renderer initialized (api=%d)\n", static_cast<int>(chosen_api));
    }

    ~RT64Renderer() override = default;


    bool valid() override { return app_ != nullptr; }

    bool update_config(const ultramodern::renderer::GraphicsConfig& old_config,
                       const ultramodern::renderer::GraphicsConfig& new_config) override {
        if (app_ == nullptr || old_config == new_config) {
            return false;
        }

        if (new_config.wm_option != old_config.wm_option) {
            app_->setFullScreen(new_config.wm_option == ultramodern::renderer::WindowMode::Fullscreen);
        }

        set_application_user_config(app_.get(), new_config);

        // Only discard framebuffers when a resolution-affecting option changed.
        const bool resolution_changed = new_config.res_option != old_config.res_option;
        const bool aspect_ratio_changed = new_config.ar_option != old_config.ar_option;
        const bool downsampling_changed = new_config.ds_option != old_config.ds_option;
        const bool msaa_changed = new_config.msaa_option != old_config.msaa_option;
        app_->updateUserConfig(resolution_changed || aspect_ratio_changed || downsampling_changed || msaa_changed);

        if (msaa_changed) {
            app_->updateMultisampling();
        }
        return true;
    }

    void enable_instant_present() override {
        if (app_ == nullptr) {
            return;
        }
        app_->enhancementConfig.presentation.mode = RT64::EnhancementConfiguration::Presentation::Mode::PresentEarly;
        app_->updateEnhancementConfig();
    }

    void send_dl(const OSTask* task) override {
        if (app_ == nullptr) {
            return;
        }
        app_->state->rsp->reset();
        app_->interpreter->loadUCodeGBI(task->t.ucode & 0x3FFFFFF, task->t.ucode_data & 0x3FFFFFF, true);
        app_->processDisplayLists(app_->core.RDRAM, task->t.data_ptr & 0x3FFFFFF, 0, true);
    }

    void send_dummy_workload(uint32_t fb_address) override {
        if (app_ == nullptr) {
            return;
        }
        app_->state->listProcessBegin();
        app_->state->rdp->setColorImage(G_IM_FMT_RGBA, G_IM_SIZ_16b, 320, fb_address);
        // G_AD_DISABLE | G_CD_MAGICSQ | G_CK_NONE | G_TC_FILT | G_TF_BILERP | G_TT_NONE | G_TL_TILE | G_TD_CLAMP | G_TP_PERSP | G_CYC_FILL | G_PM_NPRIMITIVE
        // G_AC_NONE | G_ZS_PIXEL | G_RM_NOOP | G_RM_NOOP2
        app_->state->rdp->setOtherMode(0x382C30, 0);
        app_->state->rdp->fillRect(0, 0, 320 << 2, 240 << 2);
        app_->state->fullSync();
        app_->state->listProcessEnd();
    }

    void update_screen() override {
        if (app_ == nullptr) {
            return;
        }
        app_->updateScreen();
    }

    void shutdown() override {
        if (app_ != nullptr) {
            app_->end();
            app_ = nullptr;
        }
    }

    uint32_t get_display_framerate() const override {
        if (app_ == nullptr || app_->presentQueue == nullptr || app_->presentQueue->ext.sharedResources == nullptr) {
            return 60;
        }
        return app_->presentQueue->ext.sharedResources->swapChainRate;
    }

    float get_resolution_scale() const override {
        if (app_ == nullptr || app_->presentQueue == nullptr || app_->presentQueue->ext.sharedResources == nullptr) {
            return 1.0f;
        }
        constexpr int kReferenceHeight = 240;
        switch (app_->userConfig.resolution) {
            case RT64::UserConfiguration::Resolution::WindowIntegerScale:
                if (app_->presentQueue->ext.sharedResources->swapChainHeight > 0) {
                    return std::max(float((app_->presentQueue->ext.sharedResources->swapChainHeight + kReferenceHeight - 1) / kReferenceHeight), 1.0f);
                }
                return 1.0f;
            case RT64::UserConfiguration::Resolution::Manual:
                return float(app_->userConfig.resolutionMultiplier);
            case RT64::UserConfiguration::Resolution::Original:
            default:
                return 1.0f;
        }
    }

  private:
    std::unique_ptr<RT64::Application> app_;
};

}  // namespace

std::unique_ptr<ultramodern::renderer::RendererContext> create_renderer(
    uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode) {
    return std::make_unique<RT64Renderer>(rdram, window_handle, developer_mode);
}

}  // namespace ogre


