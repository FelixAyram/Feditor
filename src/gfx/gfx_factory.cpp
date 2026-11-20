#include "gfx/common/rhi.hpp"

#ifdef OMSI_GFX_VULKAN
#include "gfx/vulkan/vk_renderer.hpp"
#endif
#include "gfx/dx12/dx12_renderer.hpp"

namespace omsi::gfx {

std::unique_ptr<IRenderer> create_renderer(const GfxConfig& config) {
    switch (config.api) {
#ifdef OMSI_GFX_VULKAN
    case GfxApi::Vulkan:
        return std::make_unique<vulkan::VkRenderer>(config);
#endif
    case GfxApi::Dx12:
    default:
        return std::make_unique<dx12::Dx12Renderer>(config);
    }
}

} // namespace omsi::gfx
