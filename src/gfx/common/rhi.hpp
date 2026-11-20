#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "f4f23ma/math.hpp"
#include "o3d/o3d_loader.hpp"

namespace omsi::gfx {

enum class GfxApi { Dx12, Vulkan };

struct GfxConfig {
    GfxApi api = GfxApi::Dx12;
    std::uint32_t width = 1920;
    std::uint32_t height = 1080;
    bool vsync = true;
    std::string title = "Feditor";
};

struct GpuMesh {
    std::uint32_t vertex_count = 0;
    std::uint32_t index_count = 0;
    bool index_32 = false;
    f4f23ma::ma4 object_matrix = f4f23ma::ma4::identity();
    std::vector<o3d::Material> materials;
};

class IRenderer {
public:
    virtual ~IRenderer() = default;

    virtual bool initialize(void* native_window) = 0;
    virtual void resize(std::uint32_t width, std::uint32_t height) = 0;
    virtual void begin_frame(const f4f23ma::ma4& view, const f4f23ma::ma4& proj) = 0;
    virtual void prepare_mesh(const GpuMesh& mesh, const o3d::MappedO3D& source) = 0;
    virtual void draw_mesh(const GpuMesh& mesh, const o3d::MappedO3D& source) = 0;
    virtual void prepare_buffer_mesh(std::string_view key, const o3d::Vertex* vertices,
                                     std::uint32_t vertex_count, const std::uint32_t* indices,
                                     std::uint32_t index_count,
                                     const std::filesystem::path& texture_path = {}) = 0;
    virtual void draw_buffer_mesh(std::string_view key, const f4f23ma::ma4& object) = 0;
    virtual void end_frame() = 0;
    virtual void wait_for_gpu() = 0;
    virtual void shutdown() = 0;
    [[nodiscard]] virtual std::size_t gpu_mesh_count() const = 0;
    [[nodiscard]] virtual std::size_t last_frame_draws() const { return 0; }
    virtual void set_debug_origin(const f4f23ma::f4& origin) {}
    virtual void draw_overlay() {}
    virtual void set_pending_screenshot(const std::filesystem::path& path) { (void)path; }
};

[[nodiscard]] std::unique_ptr<IRenderer> create_renderer(const GfxConfig& config);

} // namespace omsi::gfx
