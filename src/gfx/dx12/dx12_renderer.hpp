#pragma once



#include <d3d12.h>

#include <dxgi1_6.h>

#include <wrl/client.h>



#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include <unordered_map>

#include <vector>



#include "gfx/common/rhi.hpp"

#include "o3d/o3d_loader.hpp"



namespace omsi::gfx::dx12 {



using Microsoft::WRL::ComPtr;



struct MaterialDrawBatch {
    std::uint32_t index_start = 0;
    std::uint32_t index_count = 0;
    D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu{};
    bool has_texture = false;
};

struct Dx12MeshGpu {
    ComPtr<ID3D12Resource> vertex_buffer;
    ComPtr<ID3D12Resource> index_buffer;
    D3D12_VERTEX_BUFFER_VIEW vbv{};
    D3D12_INDEX_BUFFER_VIEW ibv{};
    std::uint32_t index_count = 0;
    std::vector<MaterialDrawBatch> batches;
};



class Dx12Renderer final : public IRenderer {

public:

    explicit Dx12Renderer(const GfxConfig& config);

    ~Dx12Renderer() override;



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

    [[nodiscard]] std::size_t gpu_mesh_count() const override { return mesh_gpu_cache_.size(); }

    [[nodiscard]] std::size_t last_frame_draws() const { return last_frame_draws_; }

    void set_debug_origin(const f4f23ma::f4& origin) override;
    void draw_overlay() override;
    void set_pending_screenshot(const std::filesystem::path& path) override;

    [[nodiscard]] Dx12MeshGpu upload_mesh(const o3d::MappedO3D& source);

    [[nodiscard]] Dx12MeshGpu upload_buffer(const o3d::Vertex* vertices, std::uint32_t vertex_count,
                                            const std::uint32_t* indices, std::uint32_t index_count);



private:

    struct alignas(256) DrawCB {

        f4f23ma::ma4 view_proj;

        f4f23ma::ma4 object;

    };



    static constexpr std::uint32_t kDrawCbStride = 256;

    static constexpr std::uint32_t kMaxDrawsPerFrame = 8192;



    bool create_device();

    bool create_swapchain(HWND hwnd);

    bool create_render_targets();

    bool create_command_objects();

    bool create_root_signature_and_pso();

    bool create_srv_heap();

    bool create_overlay_geometry();

    bool create_depth_buffer();

    [[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE load_texture_srv(const std::filesystem::path& path);

    void bind_and_draw_batch(const Dx12MeshGpu& gpu, const f4f23ma::ma4& object, const MaterialDrawBatch& batch);

    void draw_ndc_overlay();

    void wait_gpu();

    void move_to_next_frame();

    void enqueue_screenshot_copy(ID3D12GraphicsCommandList* cmd, ID3D12Resource* back);
    bool finalize_screenshot_bmp(const std::filesystem::path& path);



    GfxConfig config_;

    HWND hwnd_ = nullptr;



    ComPtr<IDXGIFactory6> factory_;

    ComPtr<IDXGIAdapter1> adapter_;

    ComPtr<ID3D12Device> device_;

    ComPtr<ID3D12CommandQueue> queue_;

    ComPtr<IDXGISwapChain3> swapchain_;

    ComPtr<ID3D12DescriptorHeap> rtv_heap_;

    ComPtr<ID3D12RootSignature> root_signature_;

    ComPtr<ID3D12PipelineState> pso_;



    static constexpr std::uint32_t kFrameCount = 2;

    static constexpr std::uint32_t kRtvCount = 2;

    static constexpr std::uint32_t kMaxSrvDescriptors = 512;



    struct Frame {

        ComPtr<ID3D12CommandAllocator> allocator;

        ComPtr<ID3D12GraphicsCommandList> cmd_list;

        ComPtr<ID3D12Resource> render_target;

        D3D12_CPU_DESCRIPTOR_HANDLE rtv_cpu{};

        UINT64 fence_value = 0;

    };



    std::vector<Frame> frames_;

    ComPtr<ID3D12Fence> fence_;

    HANDLE fence_event_ = nullptr;

    std::uint32_t frame_index_ = 0;

    std::uint32_t rtv_stride_ = 0;

    UINT64 fence_counter_ = 0;



    f4f23ma::ma4 view_ = f4f23ma::ma4::identity();

    f4f23ma::ma4 proj_ = f4f23ma::ma4::identity();



    ComPtr<ID3D12DescriptorHeap> dsv_heap_;

    ComPtr<ID3D12DescriptorHeap> srv_heap_;

    ComPtr<ID3D12Resource> depth_buffer_;

    D3D12_CPU_DESCRIPTOR_HANDLE dsv_cpu_{};

    D3D12_GPU_DESCRIPTOR_HANDLE srv_heap_gpu_{};

    std::uint32_t srv_stride_ = 0;

    std::uint32_t srv_next_ = 0;

    ComPtr<ID3D12Resource> default_white_texture_;

    std::unordered_map<std::string, D3D12_GPU_DESCRIPTOR_HANDLE> texture_srv_cache_;

    std::vector<ComPtr<ID3D12Resource>> texture_resources_;

    std::vector<ComPtr<ID3D12Resource>> texture_staging_resources_;

    ComPtr<ID3D12Resource> draw_cb_upload_;

    DrawCB* draw_cb_cpu_ = nullptr;

    std::uint32_t draw_cb_count_ = 0;



    std::unordered_map<std::string, Dx12MeshGpu> mesh_gpu_cache_;

    Dx12MeshGpu ndc_overlay_{};

    bool ndc_overlay_ready_ = false;

    f4f23ma::f4 debug_origin_{};

    std::size_t last_frame_draws_ = 0;

    std::filesystem::path pending_screenshot_path_;
    ComPtr<ID3D12Resource> screenshot_readback_;
    std::uint32_t screenshot_w_ = 0;
    std::uint32_t screenshot_h_ = 0;
    std::uint32_t screenshot_row_pitch_ = 0;

};



} // namespace omsi::gfx::dx12

