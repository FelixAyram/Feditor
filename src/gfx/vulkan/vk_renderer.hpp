#pragma once

#include <vulkan/vulkan.h>

#include <vector>

#include "gfx/common/rhi.hpp"

namespace omsi::gfx::vulkan {

class VkRenderer final : public IRenderer {
public:
    explicit VkRenderer(const GfxConfig& config);
    ~VkRenderer() override;

    bool initialize(void* native_window) override;
    void resize(std::uint32_t width, std::uint32_t height) override;
    void begin_frame(const f4f23ma::ma4& view, const f4f23ma::ma4& proj) override;
    void prepare_mesh(const GpuMesh& mesh, const o3d::MappedO3D& source) override;
    void draw_mesh(const GpuMesh& mesh, const o3d::MappedO3D& source) override;
    void prepare_buffer_mesh(std::string_view key, const o3d::Vertex* vertices, std::uint32_t vertex_count,
                             const std::uint32_t* indices, std::uint32_t index_count,
                             const std::filesystem::path& texture_path = {}) override;
    void draw_buffer_mesh(std::string_view key, const f4f23ma::ma4& object) override;
    void end_frame() override;
    void wait_for_gpu() override;
    void shutdown() override;
    [[nodiscard]] std::size_t gpu_mesh_count() const override { return 0; }

private:
    GfxConfig config_;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue graphics_queue_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    std::vector<VkImage> swap_images_;
    std::vector<VkImageView> swap_views_;
    VkRenderPass render_pass_ = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> framebuffers_;
    VkCommandPool cmd_pool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> cmd_bufs_;
    std::vector<VkSemaphore> image_available_;
    std::vector<VkSemaphore> render_finished_;
    std::vector<VkFence> in_flight_;
    std::uint32_t frame_i_ = 0;
    f4f23ma::ma4 view_ = f4f23ma::ma4::identity();
    f4f23ma::ma4 proj_ = f4f23ma::ma4::identity();
};

} // namespace omsi::gfx::vulkan
