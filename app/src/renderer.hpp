#pragma once

#include <cstdint>
#include <memory>

#include "ultramodern/renderer_context.hpp"
#include "ultramodern/ultramodern.hpp"

namespace ogre {

// Creates the renderer context handed to ultramodern's gfx thread.
std::unique_ptr<ultramodern::renderer::RendererContext> create_renderer(
    uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode);

}  // namespace ogre
