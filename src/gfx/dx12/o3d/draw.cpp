#include <atomic>
#include <cctype>
#include "gfx/dx12/hello_o3d_renderer.hpp"

#include "gfx/dx12/common/dx12_common.hpp"
#include "gfx/dx12/o3d/internal.hpp"
#include "ui/imgui_overlay.hpp"
#include "ui/ultralight_overlay.hpp"
#include "gfx/dx12/dx12_helpers.hpp"
#include "gfx/dx12/dx12_texture.hpp"
#include "omsi/frustum.hpp"
#include "omsi/job_system.hpp"
#include <thread>
#include "omsi/logger.hpp"
#include "omsi/model_cfg.hpp"
#include "omsi/visible_runtime.hpp"
#include "omsi/o3d_format.hpp"
#include "omsi/visual_quality.hpp"
#include "o3d/o3d_loader.hpp"

#include <DirectXMath.h>

#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <objbase.h>
#include <wrl/client.h>

#include <condition_variable>
#include <mutex>
#include <fstream>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <array>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <sstream>
#include <iomanip>
#include <stdexcept>
#include <unordered_map>
#include <vector>
#include <unordered_set>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "ole32.lib")

namespace omsi::gfx::dx12 {
namespace {

// [PERF] Threads of the render's own for its parallel parts (frustum cull, opaque-pass recording). On the
// shared job pool they found every worker busy - streaming decode, or blocked on the disk's IO slots - and
// the caller ran every part itself (the parallel recording measured exactly as fast as the serial one).
// Above-normal priority: a frame's parts run ahead of background loading.
class RenderPool {
public:
    static constexpr int kHelpers = 3;
    static RenderPool& get() {
        static RenderPool pool;
        return pool;
    }
    // fn(part) for part in [0, parts): the caller runs part 0, the helpers the rest; returns when all are done
    void run(int parts, const std::function<void(int)>& fn) {
        if (parts <= 1) {
            if (parts == 1) fn(0);
            return;
        }
        parts = (std::min)(parts, kHelpers + 1);
        {
            std::lock_guard<std::mutex> lk(mu_);
            job_ = &fn;
            parts_ = parts;
            pending_ = parts - 1;
            ++gen_;
        }
        cv_.notify_all();
        try {
            fn(0);
        } catch (...) {
        }
        std::unique_lock<std::mutex> lk(mu_);
        done_cv_.wait(lk, [&] { return pending_ == 0; });
        job_ = nullptr;
    }
    ~RenderPool() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& t : threads_)
            if (t.joinable()) t.detach(); // (process exit: never wait on them)
    }

private:
    RenderPool() {
        for (int i = 0; i < kHelpers; ++i) threads_.emplace_back([this, i] { loop(i + 1); });
    }
    void loop(int part) {
#ifdef _WIN32
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
#endif
        std::uint64_t seen = 0;
        for (;;) {
            const std::function<void(int)>* job = nullptr;
            int parts = 0;
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait(lk, [&] { return stop_ || gen_ != seen; });
                if (stop_) return;
                seen = gen_;
                job = job_;
                parts = parts_;
            }
            if (part >= parts || !job) continue;
            try {
                (*job)(part);
            } catch (...) {
            }
            std::lock_guard<std::mutex> lk(mu_);
            if (--pending_ == 0) done_cv_.notify_all();
        }
    }
    std::mutex mu_;
    std::condition_variable cv_, done_cv_;
    const std::function<void(int)>* job_ = nullptr;
    int parts_ = 0;
    int pending_ = 0;
    std::uint64_t gen_ = 0;
    bool stop_ = false;
    std::vector<std::thread> threads_;
};

// [PERF] The shadow cascades' recording on a thread of its own (it overlaps the whole picture recording):
// queued on the job pool behind loading jobs it often had not started when the main thread was done
// (POPULATE_SLOW csm_wait 4-13 ms) and the main thread ran it itself.
class ShadowThread {
public:
    static ShadowThread& get() {
        static ShadowThread t;
        return t;
    }
    void submit(std::function<void()> job) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            job_ = std::move(job);
        }
        cv_.notify_one();
    }
    ~ShadowThread() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            stop_ = true;
        }
        cv_.notify_one();
        if (thread_.joinable()) thread_.detach();
    }

private:
    ShadowThread() : thread_([this] { loop(); }) {}
    void loop() {
#ifdef _WIN32
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
#endif
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait(lk, [&] { return stop_ || static_cast<bool>(job_); });
                if (stop_) return;
                job = std::move(job_);
                job_ = nullptr;
            }
            try {
                job();
            } catch (...) {
            }
        }
    }
    std::mutex mu_;
    std::condition_variable cv_;
    std::function<void()> job_;
    bool stop_ = false;
    std::thread thread_;
};

// OMSI_VEG_CUTOUT=1: render alpha-blend textured foliage with blend OFF + depth-write (cutout)
// so it stops overdrawing. Default OFF (keeps current soft-blend look).
inline bool veg_cutout_enabled() {
    static const bool on = [] {
        const char* e = std::getenv("OMSI_VEG_CUTOUT");
        return e && e[0] == '1';
    }();
    return on;
}

// EnvCB (root constants b2) is SPLIT so the per-frame global part is uploaded ONCE per frame and
// only the per-object slice changes per draw. Previously all 56 floats were re-uploaded for every
// draw call (13k+ draws) = pure CPU overhead. The HLSL cbuffer layout is UNCHANGED:
//   float idx 0..10 : per-object  (env_strength, material_mode, highlight, hl_rgb, uv, alpha, point, opacity)
//   float idx 11    : ambient_scale   (global)
//   float idx 12    : light_count     (global)
//   float idx 13    : alphaRef / 255  (per material)
//   float idx 14    : opacityMap bound (per material)
//   float idx 15    : detail distances far edge in m (per material, 0 = 80)
//   float idx 16..47: 4 lights        (global)
//   float idx 48..55: sun dir/shadow/color (global)
// idx 11/12 are global but are re-sent here (same values) so a single 16-float write covers the
// material block: that keeps one SetGraphicsRoot32BitConstants call per draw.
inline void pack_env_object(float out[16], float env_strength, float material_mode, float highlight,
                            float hl_r, float hl_g, float hl_b, float uv_off_x, float uv_off_y,
                            float alpha_mode_f, float point_sample, float opacity, float ambient_scale,
                            float light_count, float alpha_ref, float opacity_map_on, float detail_distance) {
    out[0] = env_strength;
    out[1] = material_mode;
    out[2] = highlight;
    out[3] = hl_r;
    out[4] = hl_g;
    out[5] = hl_b;
    out[6] = uv_off_x;
    out[7] = uv_off_y;
    out[8] = alpha_mode_f;
    out[9] = point_sample;
    out[10] = opacity;
    out[11] = ambient_scale;
    out[12] = light_count;
    out[13] = alpha_ref;
    out[14] = opacity_map_on;
    out[15] = detail_distance;
}

// 45 floats covering EnvCB indices 11..55 (out[i] maps to cbuffer index 11 + i).
inline void pack_env_globals(float out[49], float ambient_scale, int light_count, const float lights[4][8],
                             const float sun_dir[3], const float sun_color[3], float sun_shadow_enable,
                             float night_blend, float wind_x, float wind_y, float time_s) {
    for (int i = 0; i < 49; ++i) out[i] = 0.f;
    out[45] = night_blend; // idx 56
    out[46] = wind_x;      // idx 57
    out[47] = wind_y;      // idx 58
    out[48] = time_s;      // idx 59
    out[0] = ambient_scale;                   // idx 11
    out[1] = static_cast<float>(light_count);  // idx 12
    // out[2..4] = idx 13..15 pad
    for (int li = 0; li < 4; ++li) {
        const int a = 5 + li * 4;  // idx 16 + li*4
        out[a + 0] = lights[li][0];
        out[a + 1] = lights[li][1];
        out[a + 2] = lights[li][2];
        out[a + 3] = lights[li][3];
        const int b = 21 + li * 4; // idx 32 + li*4
        out[b + 0] = lights[li][4];
        out[b + 1] = lights[li][5];
        out[b + 2] = lights[li][6];
        out[b + 3] = lights[li][7];
    }
    out[37] = sun_dir[0];          // idx 48
    out[38] = sun_dir[1];
    out[39] = sun_dir[2];
    out[40] = sun_shadow_enable;
    out[41] = sun_color[0];
    out[42] = sun_color[1];
    out[43] = sun_color[2];
    out[44] = 0.f;
}

} // namespace

void HelloO3dRenderer::render() {
    if (!impl_) return;
    auto& d = *impl_;
    const auto frame_t0 = std::chrono::steady_clock::now();

    // Reset per-frame diagnostic fields. These are intentionally cheap enough to
    // leave enabled for the profiling run; only a compact report is emitted.
    ++d.variant_frame;
    if (d.vb_pool) {
        std::lock_guard<std::mutex> vlk(d.vb_pool->mu);
        d.vb_pool->frame_now = d.variant_frame;
    }
    (void)0; // (the [matl_change] / CTC variant upload budget: one texture a frame)
    d.perf_wait_texture_ms = 0.0;
    d.perf_wait_frame_ms = 0.0;
    d.perf_populate_ms = 0.0;
    d.perf_cull_ms = 0.0;
    d.perf_sort_ms = 0.0;
    d.perf_classify_ms = 0.0;
    d.perf_record_ms = 0.0;
    d.perf_submit_ms = 0.0;
    d.perf_present_ms = 0.0;
    d.perf_chunks_tested = 0;
    d.perf_chunks_visible = 0;
    d.perf_chunks_culled = 0;
    d.perf_tiles_seen = 0;
    d.perf_tiles_visible = 0;
    d.perf_vertices_visible = 0;
    d.perf_indices_visible = 0;
    d.perf_geometry_bytes_visible = 0;
    d.perf_material_batches = 0;
    d.perf_draws = 0;
    d.perf_vb_binds = 0;
    d.perf_instance_slots = 0;
    d.perf_instanced_draws = 0;
    d.perf_visible_objects_est = 0;
    d.perf_instance_cap = 0;
    d.perf_instance_overflow = 0;
    d.perf_cb_slots_used = 0;
    d.perf_cb_slots_cap = 0;

    // Triple-buffered FrameResources: wait only for the slot we are about to reuse.
    wait_for_previous_frame();
    last_drawn_frame_index_ = frame_index_;

    const auto populate_t0 = std::chrono::steady_clock::now();
    populate_command_list();
    d.perf_populate_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - populate_t0).count();

    auto& fr = d.frames[frame_index_];
    const auto submit_t0 = std::chrono::steady_clock::now();
    flush_proc_uploads();
    {
        // the shadow cascades (recorded on a worker) before the picture that samples them; then the picture's
        // head and the parallel-recorded opaque parts (when this frame had them), then its main list
        ID3D12CommandList* lists[2 + 1 + HelloO3dRenderer::Impl::kRecWorkers]{};
        UINT n = 0;
        if (d.csm_list_this_frame && d.csm_list[frame_index_]) lists[n++] = d.csm_list[frame_index_].Get();
        d.csm_list_this_frame = false;
        for (auto* l : d.rec_pre)
            if (n < 1 + 1 + HelloO3dRenderer::Impl::kRecWorkers) lists[n++] = l;
        d.rec_pre.clear();
        lists[n++] = fr.command_list.Get();
        d.queue->ExecuteCommandLists(n, lists);
    }
    const UINT64 draw_fence = d.fence_value++;
    throw_hr(d.queue->Signal(d.fence.Get(), draw_fence), "draw Signal");
    fr.fence_value = draw_fence;
    d.perf_submit_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - submit_t0).count();

    if (!d.pending_screenshot_path.empty()) {
        // Screenshots need the GPU finished; rare path — full wait is OK here.
        if (d.fence->GetCompletedValue() < draw_fence) {
            throw_hr(d.fence->SetEventOnCompletion(draw_fence, d.fence_event), "screenshot fence");
            WaitForSingleObject(d.fence_event, INFINITE);
        }
        const auto path = d.pending_screenshot_path;
        d.pending_screenshot_path.clear();
        save_screenshot_bmp(path);
    }

    // Runtime VSync: Present(1) waits for monitor refresh; Present(0)+ALLOW_TEARING for uncapped.
    // Default from FELIX_O3D_VSYNC=1 env, overridable via set_vsync().
    static bool env_inited = false;
    if (!env_inited) {
        const char* env = std::getenv("FELIX_O3D_VSYNC");
        // Only a default: a saved/explicit set_vsync() choice (called before the first frame) wins.
        if (env && env[0] == '1' && !d.vsync_explicit) d.present_sync_interval = 1u;
        env_inited = true;
    }
    const UINT sync = d.present_sync_interval ? 1u : 0u;
    const auto present_t0 = std::chrono::steady_clock::now();
    throw_hr(d.swapchain->Present(sync, (sync == 0 && d.allow_tearing) ? DXGI_PRESENT_ALLOW_TEARING : 0u),
             "Present");
    d.perf_present_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - present_t0).count();

    const double frame_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - frame_t0).count();

    ++d.perf_report_frames;
    d.perf_sum_frame_ms += frame_ms;
    d.perf_sum_gpu_ms += d.perf_last_gpu_ms;
    d.perf_sum_populate_ms += d.perf_populate_ms;
    d.perf_sum_cull_ms += d.perf_cull_ms;
    d.perf_sum_sort_ms += d.perf_sort_ms;
    d.perf_sum_classify_ms += d.perf_classify_ms;
    d.perf_sum_record_ms += d.perf_record_ms;
    d.perf_sum_submit_ms += d.perf_submit_ms;
    d.perf_sum_present_ms += d.perf_present_ms;
    d.perf_sum_wait_texture_ms += d.perf_wait_texture_ms;
    d.perf_sum_wait_frame_ms += d.perf_wait_frame_ms;
    d.perf_sum_tested += d.perf_chunks_tested;
    d.perf_sum_visible += d.perf_chunks_visible;
    d.perf_sum_culled += d.perf_chunks_culled;
    d.perf_sum_tiles += d.perf_tiles_seen;
    d.perf_sum_tiles_visible += d.perf_tiles_visible;
    d.perf_sum_vertices += d.perf_vertices_visible;
    d.perf_sum_indices += d.perf_indices_visible;
    d.perf_sum_geometry_bytes += d.perf_geometry_bytes_visible;
    d.perf_sum_batches += d.perf_material_batches;
    d.perf_sum_draws += d.perf_draws;
    d.perf_sum_vb_binds += d.perf_vb_binds;
    d.perf_sum_instance_slots += d.perf_instance_slots;
    d.perf_sum_instanced_draws += d.perf_instanced_draws;
    d.perf_max_frame_ms = (std::max)(d.perf_max_frame_ms, frame_ms);
    d.perf_max_gpu_ms = (std::max)(d.perf_max_gpu_ms, d.perf_last_gpu_ms);

    if (d.perf_report_frames >= 60) {
        const double n = static_cast<double>(d.perf_report_frames);
        std::ostringstream o;
        o << std::fixed << std::setprecision(3);
        o << "PERF60 "
          << "cpu_avg=" << (d.perf_sum_frame_ms / n)
          << " cpu_max=" << d.perf_max_frame_ms
          << " gpu_avg=" << (d.perf_sum_gpu_ms / n)
          << " gpu_max=" << d.perf_max_gpu_ms
          << " gpu_scene=" << d.perf_last_gpu_scene_ms
          << " gpu_ui=" << d.perf_last_gpu_ui_ms
          << " wait_tex=" << (d.perf_sum_wait_texture_ms / n)
          << " wait_fence=" << (d.perf_sum_wait_frame_ms / n)
          << " populate=" << (d.perf_sum_populate_ms / n)
          << " cull=" << (d.perf_sum_cull_ms / n)
          << " sort=" << (d.perf_sum_sort_ms / n)
          << " classify=" << (d.perf_sum_classify_ms / n)
          << " record=" << (d.perf_sum_record_ms / n)
          << " submit=" << (d.perf_sum_submit_ms / n)
          << " present=" << (d.perf_sum_present_ms / n)
          << " tested=" << (d.perf_sum_tested / d.perf_report_frames)
          << " visible=" << (d.perf_sum_visible / d.perf_report_frames)
          << " culled=" << (d.perf_sum_culled / d.perf_report_frames)
          << " tiles=" << (d.perf_sum_tiles / d.perf_report_frames)
          << " tile_visible=" << (d.perf_sum_tiles_visible / d.perf_report_frames)
          << " verts=" << (d.perf_sum_vertices / d.perf_report_frames)
          << " indices=" << (d.perf_sum_indices / d.perf_report_frames)
          << " geom_MB=" << (static_cast<double>(d.perf_sum_geometry_bytes) / n / (1024.0 * 1024.0))
          << " batches=" << (d.perf_sum_batches / d.perf_report_frames)
          << " draws=" << (d.perf_sum_draws / d.perf_report_frames)
          << " vb_binds=" << (d.perf_sum_vb_binds / d.perf_report_frames)
          << " inst_slots=" << (d.perf_sum_instance_slots / d.perf_report_frames)
          << " inst_draws=" << (d.perf_sum_instanced_draws / d.perf_report_frames);
        omsi::log::info(o.str());
        if (!d.perf_hotspots.empty()) omsi::log::info(d.perf_hotspots);

        d.perf_report_frames = 0;
        d.perf_sum_frame_ms = 0.0;
        d.perf_sum_gpu_ms = 0.0;
        d.perf_max_frame_ms = 0.0;
        d.perf_max_gpu_ms = 0.0;
        d.perf_sum_populate_ms = 0.0;
        d.perf_sum_cull_ms = 0.0;
        d.perf_sum_sort_ms = 0.0;
        d.perf_sum_classify_ms = 0.0;
        d.perf_sum_record_ms = 0.0;
        d.perf_sum_submit_ms = 0.0;
        d.perf_sum_present_ms = 0.0;
        d.perf_sum_wait_texture_ms = 0.0;
        d.perf_sum_wait_frame_ms = 0.0;
        d.perf_sum_tested = 0;
        d.perf_sum_visible = 0;
        d.perf_sum_culled = 0;
        d.perf_sum_tiles = 0;
        d.perf_sum_tiles_visible = 0;
        d.perf_sum_vertices = 0;
        d.perf_sum_indices = 0;
        d.perf_sum_geometry_bytes = 0;
        d.perf_sum_batches = 0;
        d.perf_sum_draws = 0;
        d.perf_sum_vb_binds = 0;
        d.perf_sum_instance_slots = 0;
        d.perf_sum_instanced_draws = 0;
    }
}

void HelloO3dRenderer::populate_command_list() {
    auto& d = *impl_;
    const auto pop_t0 = std::chrono::steady_clock::now();
    flush_deferred_upload_reset();
    wait_pending_texture_uploads();
    auto& fr = d.frames[frame_index_];
    // This FrameResource is free (wait_for_previous_frame already waited its fence).
    throw_hr(fr.allocator->Reset(), "allocator reset");
    throw_hr(fr.command_list->Reset(fr.allocator.Get(), d.pipeline_state.Get()), "cmd reset");
    d.instance_write_cursor = 0; // P6: restart instance region for this frame

    D3D12_VIEWPORT scene_vp{};
    D3D12_RECT scene_sc{};
    scene_viewport_rect(width_, height_, scene_vp, scene_sc);

    ID3D12DescriptorHeap* heaps[] = {d.srv_heap.Get()};

    // ---- Scene cache: same view + same scene epoch -> copy the last image, draw only the UI -------
    if (d.pending_tex_upload_fence != d.scene_last_tex_fence) {
        d.scene_last_tex_fence = d.pending_tex_upload_fence; // a texture finished/started uploading
        scene_dirty(d, "texture upload");
    }
    {
        // Highlights are cleared and set again every frame by the editor: compare the final set.
        std::uint64_t h = 1469598103934665603ull ^ d.highlighted_chunks.size();
        for (const std::size_t ci : d.highlighted_chunks) {
            if (ci >= d.mesh_chunks.size()) continue;
            const auto& c = d.mesh_chunks[ci];
            const float v[5] = {static_cast<float>(ci), c.highlight, c.highlight_r, c.highlight_g, c.highlight_b};
            h = scene_hash_bytes(v, sizeof v, h);
        }
        static std::uint64_t s_hl = 0;
        scene_dirty_if(d, s_hl, h, "highlight");
    }
    {
        static const bool trace_vis = std::getenv("OMSI_TRACE_SCENE_CACHE") != nullptr;
        static std::uint64_t s_net = 0;
        for (const auto& [ci, before] : d.vis_before) {
            if (ci >= d.mesh_chunks.size() || d.mesh_chunks[ci].meta.visible == before) continue;
            scene_dirty(d, "visible");
            if (trace_vis && (++s_net % 100u) == 1u)
                omsi::log::info("scene dirty visible chunk=" + std::to_string(ci) + " -> " +
                                (d.mesh_chunks[ci].meta.visible ? "1 " : "0 ") + d.mesh_chunks[ci].meta.mesh_file +
                                " (net changes " + std::to_string(s_net) + ")");
            if (!trace_vis) break;
        }
        d.vis_before.clear();
        d.vis_touched.clear();
    }
    const std::int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch())
                                    .count();
    ID3D12Resource* backbuffer = d.render_targets[frame_index_].Get();
    const D3D12_RESOURCE_DESC bb_desc = backbuffer->GetDesc();
    static const bool cache_off = [] {
        const char* e = std::getenv("OMSI_SCENE_CACHE");
        return e && e[0] == '0';
    }();
    static const bool trace_cache = std::getenv("OMSI_TRACE_SCENE_CACHE") != nullptr;
    const bool same_view = std::memcmp(&d.scene_cache_vp, &d.view_proj_transposed_, sizeof(XMFLOAT4X4)) == 0 &&
                           std::memcmp(d.scene_cache_cam, d.camera_world_pos, sizeof d.scene_cache_cam) == 0;
    bool reuse = false;
    if (!cache_off && d.scene_cache_valid && d.scene_cache && d.pending_screenshot_path.empty() &&
        d.scene_cache_w == static_cast<UINT>(bb_desc.Width) && d.scene_cache_h == bb_desc.Height &&
        d.scene_cache_epoch == d.scene_epoch && same_view) {
        // Time-animated content keeps a reduced redraw rate; a static scene only the 500 ms refresh.
        const bool wind = d.frame_drew_sway && (d.wind_x * d.wind_x + d.wind_y * d.wind_y) > 0.0025f;
        const bool precip = d.sw.env_set && d.sw.env.precip_type > 0 && d.sw.env.precip_intensity > 0.f;
        const bool fast_anim = wind || d.frame_drew_water || precip;
        const bool clouds = d.sw.env_set && d.sw.env.cloud_type >= 1 && d.sw.env.cloud_cover > 0.01f &&
                            d.sw.env.cloud_wind_speed > 0.01f;
        const double max_age_ms = fast_anim ? 66.0 : (clouds ? 125.0 : 500.0); // 15 Hz sway/water, 8 Hz clouds
        reuse = static_cast<double>(now_ns - d.scene_last_full_ns) / 1e6 < max_age_ms;
    }
    if (trace_cache && (d.scene_reused_frames + d.scene_full_frames) % 300u == 0u)
        omsi::log::info("scene cache reused=" + std::to_string(d.scene_reused_frames) + " full=" +
                        std::to_string(d.scene_full_frames) + " last_dirty=" + d.scene_dirty_why +
                        " same_view=" + std::to_string(same_view ? 1 : 0) + " sway=" +
                        std::to_string(d.frame_drew_sway ? 1 : 0) + " water=" + std::to_string(d.frame_drew_water ? 1 : 0));
    if (reuse) {
        ++d.scene_reused_frames;
        d.perf_chunks_visible = d.perf_chunks_tested = d.perf_chunks_culled = 0;
        D3D12_RESOURCE_BARRIER b[2]{};
        UINT nb = 0;
        b[nb++] = dx12_transition_barrier(backbuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
        if (d.scene_cache_state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
            b[nb++] = dx12_transition_barrier(d.scene_cache.Get(), d.scene_cache_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
            d.scene_cache_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
        }
        fr.command_list->ResourceBarrier(nb, b);
        const UINT q0 = frame_index_ * HelloO3dRenderer::kGpuStampCount;
        if (d.timestamp_query_heap) fr.command_list->EndQuery(d.timestamp_query_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, q0);
        fr.command_list->CopyResource(backbuffer, d.scene_cache.Get());
        b[0] = dx12_transition_barrier(backbuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
        fr.command_list->ResourceBarrier(1, b);
        if (d.timestamp_query_heap)
            fr.command_list->EndQuery(d.timestamp_query_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, q0 + 1u);
        const D3D12_CPU_DESCRIPTOR_HANDLE back_rtv =
            offset_cpu_handle(d.rtv_heap->GetCPUDescriptorHandleForHeapStart(), rtv_descriptor_size_, frame_index_);
        fr.command_list->SetDescriptorHeaps(1, heaps);
        fr.command_list->OMSetRenderTargets(1, &back_rtv, FALSE, nullptr);
        fr.command_list->RSSetViewports(1, &scene_vp);
        fr.command_list->RSSetScissorRects(1, &scene_sc);
        if (ultralight_overlay_) ultralight_overlay_->upload_texture(fr.command_list.Get());
        if (imgui_overlay_) imgui_overlay_->render_dx12(fr.command_list.Get());
        if (d.timestamp_query_heap && fr.timestamp_readback) {
            fr.command_list->EndQuery(d.timestamp_query_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, q0 + 2u);
            fr.command_list->ResolveQueryData(d.timestamp_query_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, q0,
                                              HelloO3dRenderer::kGpuStampCount, fr.timestamp_readback.Get(), 0);
        }
        b[0] = dx12_transition_barrier(backbuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        fr.command_list->ResourceBarrier(1, b);
        throw_hr(fr.command_list->Close(), "cmd close (scene cache)");
        return;
    }
    ++d.scene_full_frames;
    // Save this frame's image when the view and scene were already the same as the last full frame
    // (the next frames are likely to reuse it); while moving there is nothing to keep.
    const bool save_cache = !cache_off && same_view && d.scene_prev_epoch == d.scene_epoch &&
                            std::memcmp(&d.scene_prev_vp, &d.view_proj_transposed_, sizeof(XMFLOAT4X4)) == 0;
    d.scene_prev_epoch = d.scene_epoch;
    d.scene_prev_vp = d.view_proj_transposed_;
    if (!same_view || d.scene_cache_epoch != d.scene_epoch) d.scene_cache_valid = false;
    std::memcpy(&d.scene_cache_vp, &d.view_proj_transposed_, sizeof(XMFLOAT4X4));
    std::memcpy(d.scene_cache_cam, d.camera_world_pos, sizeof d.scene_cache_cam);
    const std::uint64_t frame_epoch = d.scene_epoch;
    d.scene_last_full_ns = now_ns;
    d.frame_drew_sway = false;
    d.frame_drew_water = d.sw.water_index_count > 0;
    // CSM depth pass before color (uses upper half of instance ring).
    int csm_cascade_now = -1, csm_cascade_also = -1;
    bool csm_pending = false;
    d.csm_list_this_frame = false;
    (void)ensure_csm_resources(d);
    if (d.shadow_cb_base_cpu) {
        // this frame's shadow CB, starting from the last frame's (the cascades not redrawn keep their matrices)
        std::uint8_t* const prev = d.shadow_cb_cpu;
        d.shadow_cb_cpu = d.shadow_cb_base_cpu + static_cast<std::size_t>(frame_index_) * HelloO3dRenderer::Impl::kShadowCbFrameBytes;
        d.shadow_cb_gpu = d.shadow_cb_base_gpu + static_cast<UINT64>(frame_index_) * HelloO3dRenderer::Impl::kShadowCbFrameBytes;
        if (prev && prev != d.shadow_cb_cpu) std::memcpy(d.shadow_cb_cpu, prev, 272);
    }
    if (d.csm_ready && d.sun_shadow_enable > 0.5f) {
        // Re-render shadow cascades every OTHER frame. The shadow map + light matrices stay
        // consistent between frames (both skipped together), so sampling is valid; shadows lag
        // one frame, which is imperceptible while moving but halves the shadow GPU cost — the
        // single largest GPU cost at high refresh.
        static std::uint32_t s_csm_tick = 0;
        // One cascade a frame: the near one every second frame, the middle and far ones by turns on the
        // others - the same refresh for the near shadows, and no frame carries all three (with 400 AI cars
        // moving every cascade was stale: 13 ms more on every second frame, the 1 % lows at 16 fps)
        const std::uint32_t t = ++s_csm_tick;
        // near every 2nd frame, middle every 4th, far (the most casters: 4-10 ms waited for on the main thread
        // with 4 tiles streamed) every 8th; one frame in 8 draws no cascade. Each cascade's matrices change only
        // with its own map, so a cascade not drawn this frame stays consistent.
        if (d.shadow_dir_dirty) {
            // the sun stepped (Impl::shadow_sun_dir): all cascades this frame, so no cascade samples a map
            // drawn for the old direction with a matrix fitted to the new one
            d.shadow_dir_dirty = false;
            csm_cascade_now = -1;
            csm_pending = true;
        } else {
            // The near cascade every frame (redrawn only when stale - something in it moved): drawn every second
            // frame, the shadows of the moving AI cars stood still one frame in two and jumped - they flickered
            // and popped over the cars while they drove. The middle and far ones keep their turns.
            // (the middle one every second frame: every fourth, the shadows of the cars 18-80 m off stood still
            //  three frames and jumped - they flickered behind the cars)
            csm_cascade_now = 0;
            if ((t & 1u) == 1u) csm_cascade_also = 1;
            else if ((t & 7u) == 2u) csm_cascade_also = 2;
            csm_pending = true; // recorded on a worker below, once the terrain LOD is decided
        }
    } else if (d.csm_ready) {
        update_csm_matrices(d);
    }

    fr.command_list->SetGraphicsRootSignature(d.root_signature.Get());
    fr.command_list->SetDescriptorHeaps(1, heaps);
    // Partition CB ring per in-flight frame so frame N cannot stomp frame N-1 matrices/textures.
    {
        const std::uint32_t slots_per_frame = kSceneCbSlotCount / kFrameCount;
        d.scene_cb_slot = frame_index_ * slots_per_frame;
    }
    fr.command_list->RSSetViewports(1, &scene_vp);
    fr.command_list->RSSetScissorRects(1, &scene_sc);

    const bool use_msaa = scene_msaa_active();
    const bool use_scene_rt = scene_postfx_active();
    ID3D12Resource* color_target = use_msaa ? d.msaa_color.Get()
                                 : use_scene_rt ? d.scene_color[frame_index_].Get()
                                                : d.render_targets[frame_index_].Get();

    D3D12_RESOURCE_BARRIER barrier{};
    if (!use_msaa && !use_scene_rt) {
        barrier = dx12_transition_barrier(color_target, D3D12_RESOURCE_STATE_PRESENT,
                                          D3D12_RESOURCE_STATE_RENDER_TARGET);
        fr.command_list->ResourceBarrier(1, &barrier);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle =
        use_msaa ? d.msaa_rtv_cpu
                 : use_scene_rt ? d.scene_rtv_cpu[frame_index_]
                                : offset_cpu_handle(d.rtv_heap->GetCPUDescriptorHandleForHeapStart(),
                                                    rtv_descriptor_size_, frame_index_);
    fr.command_list->OMSetRenderTargets(1, &rtv_handle, FALSE, &d.dsv_cpu);
    d.main_rtv_cpu = rtv_handle; // light shafts rebind this RTV with the read-only depth view

    const float clear_color[] = {d.clear_rgb[0], d.clear_rgb[1], d.clear_rgb[2], 1.f};
    fr.command_list->ClearRenderTargetView(rtv_handle, clear_color, 0, nullptr);
    // Reverse-Z: clear to far (0). Scene PSOs use GREATER_EQUAL (near≈1).
    fr.command_list->ClearDepthStencilView(d.dsv_cpu, D3D12_CLEAR_FLAG_DEPTH, 0.f, 0, 0, nullptr);

    // OpenOMSI-style sky dome (after clear, before scene geometry).
    draw_sky_pass(d, fr.command_list.Get(), frame_index_, static_cast<float>(scene_vp.Width),
                  static_cast<float>(scene_vp.Height));

    if (d.csm_ready && d.shadow_cb_gpu) {
        fr.command_list->SetGraphicsRootConstantBufferView(4, d.shadow_cb_gpu);
        fr.command_list->SetGraphicsRootDescriptorTable(5, d.csm_srv_gpu);
    }

    float env_globals_frame[45]{}; // (the same state is set again on the parallel / tail lists)
    // EnvCB global half (sun/lights/ambient) — set ONCE per frame. Root signature is the scene
    // signature here (sky restored it). Per draw we only write [0..10].
    {
        static const auto s_t0 = std::chrono::steady_clock::now();
        const float time_s = std::chrono::duration<float>(std::chrono::steady_clock::now() - s_t0).count();
        float env_globals[49];
        pack_env_globals(env_globals, d.ambient_scale, d.scene_light_count, d.scene_lights, d.sun_dir,
                         d.sun_color, d.sun_shadow_enable, d.night_blend, d.wind_x, d.wind_y, time_s);
        fr.command_list->SetGraphicsRoot32BitConstants(2, 45, env_globals, 11);
        std::memcpy(env_globals_frame, env_globals, sizeof(env_globals_frame));
        // night/wind/time live in ShadowCB.gEnvExtra (byte 256): root constants are at the 64-DWORD limit.
        if (d.shadow_cb_cpu) std::memcpy(d.shadow_cb_cpu + 256, env_globals + 45, 4 * sizeof(float));
        if (d.shadow_cb_cpu) std::memcpy(d.shadow_cb_cpu + 252, &d.fog_scale, sizeof(float)); // gCamForward.w
        if (d.shadow_cb_cpu && (d.xlight_dirty || d.xlight_cb_written != d.shadow_cb_cpu)) {
            if (d.xlight_blob.empty()) {
                const float info[4] = {0.f, 0.f, 0.f, 1.f};
                std::memcpy(d.shadow_cb_cpu + 272, info, sizeof(info));
            } else {
                std::memcpy(d.shadow_cb_cpu + 272, d.xlight_blob.data(), d.xlight_blob.size());
            }
            d.xlight_dirty = false;
            d.xlight_cb_written = d.shadow_cb_cpu;
        }
    }

    fr.command_list->SetPipelineState(d.pipeline_state.Get());
    fr.command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // GPU timestamps (bottom-of-pipe EndQuery). 0=after clear, 1=after scene, 2=after UI.
    if (d.timestamp_query_heap) {
        const UINT q0 = frame_index_ * HelloO3dRenderer::kGpuStampCount;
        fr.command_list->EndQuery(d.timestamp_query_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, q0);
    }

    DirectX::XMMATRIX cull_wvp = DirectX::XMMatrixIdentity();
    bool have_frustum = false;
    omsi::Frustum cull_frustum{};
    if (d.has_view_proj_) {
        const DirectX::XMMATRIX vp_t = DirectX::XMLoadFloat4x4(&d.view_proj_transposed_);
        static bool s_vp_raw = false;
        if (!s_vp_raw) {
            s_vp_raw = true;
            const auto& vp = d.view_proj_transposed_;
            omsi::log::info("VP_RAW row0=(" + std::to_string(vp._11) + "," + std::to_string(vp._12) + "," +
                            std::to_string(vp._13) + "," + std::to_string(vp._14) + ") row3=(" +
                            std::to_string(vp._41) + "," + std::to_string(vp._42) + "," +
                            std::to_string(vp._43) + "," + std::to_string(vp._44) + ")");
        }
        auto self_test = [](const omsi::Frustum& fr, DirectX::FXMMATRIX vp, DirectX::XMFLOAT3* out_n,
                            DirectX::XMFLOAT3* out_p) -> bool {
            DirectX::XMVECTOR det{};
            const DirectX::XMMATRIX inv = DirectX::XMMatrixInverse(&det, vp);
            DirectX::XMFLOAT3 n{}, f{};
            DirectX::XMStoreFloat3(&n, DirectX::XMVector3TransformCoord(DirectX::XMVectorSet(0.f, 0.f, 0.f, 1.f), inv));
            DirectX::XMStoreFloat3(&f, DirectX::XMVector3TransformCoord(DirectX::XMVectorSet(0.f, 0.f, 1.f, 1.f), inv));
            const float fx = f.x - n.x, fy = f.y - n.y, fz = f.z - n.z;
            const float fl = std::sqrt(fx * fx + fy * fy + fz * fz);
            if (fl <= 1.e-6f) return false;
            const float px = n.x + 10.f * fx / fl;
            const float py = n.y + 10.f * fy / fl;
            const float pz = n.z + 10.f * fz / fl;
            if (out_n) *out_n = n;
            if (out_p) *out_p = {px, py, pz};
            return omsi::point_in_frustum(fr, px, py, pz);
        };
        static int s_vp_mode = -1; // 0 = vp_t as-is, 1 = extra transpose
        const DirectX::XMMATRIX vp_x = DirectX::XMMatrixTranspose(vp_t);
        if (s_vp_mode < 0) {
            const omsi::Frustum f0 = omsi::extract_frustum_planes_dx(vp_t);
            const omsi::Frustum f1 = omsi::extract_frustum_planes_dx(vp_x);
            if (self_test(f0, vp_t, nullptr, nullptr)) s_vp_mode = 0;
            else if (self_test(f1, vp_x, nullptr, nullptr)) s_vp_mode = 1;
            else s_vp_mode = 0;
        }
        cull_wvp = (s_vp_mode == 0) ? vp_t : vp_x;
        cull_frustum = omsi::extract_frustum_planes_dx(cull_wvp);
        have_frustum = true;
        static unsigned gh_self_frames = 0;
        if ((++gh_self_frames % 120u) == 1u) {
            DirectX::XMFLOAT3 cam{}, test{};
            const bool inside = self_test(cull_frustum, cull_wvp, &cam, &test);
            omsi::log::info(std::string("frustum GH self-test ") + (inside ? "yes" : "NO") + " mode=" +
                            std::to_string(s_vp_mode) + " test=(" + std::to_string(test.x) + "," +
                            std::to_string(test.y) + "," + std::to_string(test.z) + ") cam=(" +
                            std::to_string(cam.x) + "," + std::to_string(cam.y) + "," + std::to_string(cam.z) +
                            ")");
        }
    }

    // Gribb–Hartmann + p-vertex (DirectX NDC z∈[0,1], Near=row3). Terrain / flat tiles stay visible.
    // Screen-size cull: projected diameter / visible height = r / (dist * tan(fov/2)). Compared in squared form
    // (r^2 < k^2 * dist^2), so there is no sqrt and no extra transform - it reuses the world AABB below.
    // NOTE: path rails / decals / surfaces are size-exempt below, so .sco paths never vanish because of it.
    const float size_k = (d.min_screen_size_pct > 0.f)
                             ? d.min_screen_size_pct * 0.01f * std::tan(d.camera_fov_y * 0.5f)
                             : 0.f;
    const float size_k2 = size_k * size_k;
    // Row-vector view-projection (same convention as the WVP written in draw_chunk_batches) used to project
    // world AABB corners for the size test below.
    const DirectX::XMMATRIX size_vp = d.has_view_proj_
                                          ? DirectX::XMMatrixTranspose(DirectX::XMLoadFloat4x4(&d.view_proj_transposed_))
                                          : DirectX::XMMatrixIdentity();
    const float size_aspect = d.camera_aspect > 0.1f ? d.camera_aspect : 1.6f;
    const float size_frac = d.min_screen_size_pct * 0.01f;
    auto chunk_in_frustum = [&](const GpuMeshChunk& chunk) -> bool {
        if (chunk.screen_space) return true;
        if (!have_frustum) return true;
        {
            if (chunk.meta.mesh_file == "__terrain_tile__") return true;
        }
        float wminx, wminy, wminz, wmaxx, wmaxy, wmaxz;
        if (chunk.aabb_cache_valid &&
            std::memcmp(&chunk.aabb_cache_matrix, &chunk.local_world_transposed,
                        sizeof(DirectX::XMFLOAT4X4)) == 0) {
            // Same world matrix as last frame: reuse the cached world AABB (static chunks).
            wminx = chunk.aabb_cache_min[0];
            wminy = chunk.aabb_cache_min[1];
            wminz = chunk.aabb_cache_min[2];
            wmaxx = chunk.aabb_cache_max[0];
            wmaxy = chunk.aabb_cache_max[1];
            wmaxz = chunk.aabb_cache_max[2];
        } else {
            const DirectX::XMMATRIX world_t = DirectX::XMLoadFloat4x4(&chunk.local_world_transposed);
            const DirectX::XMMATRIX world = DirectX::XMMatrixTranspose(world_t);
            const float x0 = chunk.meta.bbox_min[0], y0 = chunk.meta.bbox_min[1], z0 = chunk.meta.bbox_min[2];
            const float x1 = chunk.meta.bbox_max[0], y1 = chunk.meta.bbox_max[1], z1 = chunk.meta.bbox_max[2];
            const float xs[2] = {x0, x1}, ys[2] = {y0, y1}, zs[2] = {z0, z1};
            wminx = 1.e30f, wminy = 1.e30f, wminz = 1.e30f;
            wmaxx = -1.e30f, wmaxy = -1.e30f, wmaxz = -1.e30f;
            for (float x : xs)
                for (float y : ys)
                    for (float z : zs) {
                        DirectX::XMFLOAT3 p{};
                        DirectX::XMStoreFloat3(&p,
                                               DirectX::XMVector3Transform(DirectX::XMVectorSet(x, y, z, 1.f), world));
                        wminx = (std::min)(wminx, p.x);
                        wminy = (std::min)(wminy, p.y);
                        wminz = (std::min)(wminz, p.z);
                        wmaxx = (std::max)(wmaxx, p.x);
                        wmaxy = (std::max)(wmaxy, p.y);
                        wmaxz = (std::max)(wmaxz, p.z);
                    }
            chunk.aabb_cache_matrix = chunk.local_world_transposed;
            chunk.aabb_cache_min[0] = wminx;
            chunk.aabb_cache_min[1] = wminy;
            chunk.aabb_cache_min[2] = wminz;
            chunk.aabb_cache_max[0] = wmaxx;
            chunk.aabb_cache_max[1] = wmaxy;
            chunk.aabb_cache_max[2] = wmaxz;
            chunk.aabb_cache_valid = true;
        }
        const bool huge_aabb =
            (std::max)({(wmaxx - wminx), (wmaxy - wminy), (wmaxz - wminz)}) > 300.f;
        // Render distance cull using the (cached) world AABB centre. Applies to scenery and
        // splines but NOT terrain (would leave holes at the horizon). Surface-bias chunks (path
        // rails) are also exempt: otherwise raising/tilting the camera culls them by 3D distance
        // even though the app-side visual radius keeps them "visible".
        // Huge meshes (>300 m on any axis: city blocks, road systems, one-piece TStatic sceneries)
        // are exempt too: their centre can be far while a big part of the mesh is still in view.
        if (d.cull_max_distance > 0.f && !huge_aabb && !chunk.is_terrain &&
            chunk.meta.mesh_file != "__oo_terrain__" && !chunk.surface_bias &&
            chunk.render_phase != WorldRenderPhase::Spline &&
            chunk.render_phase != WorldRenderPhase::Surface) {
            const float cx = 0.5f * (wminx + wmaxx) - d.camera_world_pos[0];
            const float cy = 0.5f * (wminy + wmaxy) - d.camera_world_pos[1];
            const float cz = 0.5f * (wminz + wmaxz) - d.camera_world_pos[2];
            const float ex = wmaxx - wminx, ey = wmaxy - wminy, ez = wmaxz - wminz;
            const float r2 = 0.25f * (ex * ex + ey * ey + ez * ez);
            const float lim = d.cull_max_distance * 1.1f + std::sqrt(r2) + 10.f; // margin: no edge popping
            if (cx * cx + cy * cy + cz * cz > lim * lim) return false;
        }
        // Surface-bias chunks (path rails) and spline surfaces skip the frustum test: their AABB
        // is the whole road and rotating the camera could clip them out. Distance-cull is
        // already exempted for surface_bias above.
        if (!chunk.surface_bias && chunk.render_phase != WorldRenderPhase::Spline) {
            // Margin around the box: edges of objects popped out at the screen border (bbox from the
            // rest pose, animated parts / thin overhangs outside it, cached AABB of a moved chunk).
            const float ext = (std::max)({wmaxx - wminx, wmaxy - wminy, wmaxz - wminz});
            const float m = (std::min)(2.f + 0.15f * ext, 30.f);
            if (!omsi::aabb_in_frustum(cull_frustum, wminx - m, wminy - m, wminz - m, wmaxx + m, wmaxy + m,
                                       wmaxz + m))
                return false;
        }
        if (size_k2 > 0.f && !huge_aabb) {
            const std::string& mf = chunk.meta.mesh_file;
            // Never size-culled: terrain tiles, splines/roads/decals (long + flat), path rails and every
            // surface layer. Those are limited ONLY by the visual radius (app side), as before.
            // chunk.no_size_cull: BeamNG TSStatic / forest / ground-cover chunks (set_chunk_no_size_cull).
            const bool size_exempt =
                chunk.is_terrain || chunk.no_size_cull || chunk.fixed_lit || chunk.decal || chunk.surface_bias ||
                chunk.lod_capable ||
                chunk.render_phase == WorldRenderPhase::Terrain || chunk.render_phase == WorldRenderPhase::Spline ||
                chunk.render_phase == WorldRenderPhase::Surface || chunk.render_phase == WorldRenderPhase::OnSurface ||
                chunk.render_phase == WorldRenderPhase::PreSurface || mf == "__oo_terrain__" ||
                mf == "__terrain_tile__" || mf == "__spline__" || mf == "__spline_merged__";
            if (!size_exempt) {
                const float ex = wmaxx - wminx, ey = wmaxy - wminy, ez = wmaxz - wminz;
                const float r2 = 0.25f * (ex * ex + ey * ey + ez * ez); // bounding-sphere radius^2
                const float cx = 0.5f * (wminx + wmaxx) - d.camera_world_pos[0];
                const float cy = 0.5f * (wminy + wmaxy) - d.camera_world_pos[1];
                const float cz = 0.5f * (wminz + wmaxz) - d.camera_world_pos[2];
                const float dist2 = cx * cx + cy * cy + cz * cz;
                const float sm = chunk.size_cull_scale;
                if (dist2 > r2 && r2 < size_k2 * sm * sm * dist2) {
                    // Sphere says "small". Confirm with the real projected AABB: an object is only hidden when
                    // its LARGEST on-screen extent (height or width) is under the threshold. A long/flat thing
                    // (fence, wall, platform edge) keeps one long dimension and stays visible.
                    bool any_behind = false;
                    float nx0 = 1.e30f, nx1 = -1.e30f, ny0 = 1.e30f, ny1 = -1.e30f;
                    for (int k = 0; k < 8 && !any_behind; ++k) {
                        const DirectX::XMVECTOR clip = DirectX::XMVector4Transform(
                            DirectX::XMVectorSet((k & 1) ? wmaxx : wminx, (k & 2) ? wmaxy : wminy,
                                                 (k & 4) ? wmaxz : wminz, 1.f),
                            size_vp);
                        const float w = DirectX::XMVectorGetW(clip);
                        if (w <= 1.e-3f) {
                            any_behind = true;
                            break;
                        }
                        const float nx = DirectX::XMVectorGetX(clip) / w;
                        const float ny = DirectX::XMVectorGetY(clip) / w;
                        nx0 = (std::min)(nx0, nx);
                        nx1 = (std::max)(nx1, nx);
                        ny0 = (std::min)(ny0, ny);
                        ny1 = (std::max)(ny1, ny);
                    }
                    if (!any_behind) {
                        const float ext_h = (ny1 - ny0) * 0.5f;                  // fraction of screen height
                        const float ext_w = (nx1 - nx0) * 0.5f * size_aspect;     // same unit, from width
                        if ((std::max)(ext_h, ext_w) < size_frac * 0.6f * sm) return false; // margin
                    }
                    // behind/straddling the camera plane: keep (cannot be judged safely)
                }
            }
        }
        return true;
    };

    // Auto-instancing support: clone_chunk already shares VB/IB (same GPU buffers, unique
    // world matrix + chunk index for picks/IDs). Below we (1) skip redundant VB/IB binds when
    // consecutive draws share the same buffers, and (2) optionally sort opaque draws by
    // shared mesh pointer so clones of the same .o3d cluster together → fewer state changes.
    // Object/spline map IDs are untouched; we never merge logical instances into one mesh.
    // (VB/IB keyed by GPU address, not by resource: meshes share the 64 MB arenas of VbArenaPool)
    struct DrawBindCache {
        D3D12_GPU_VIRTUAL_ADDRESS vb = 0;
        D3D12_GPU_VIRTUAL_ADDRESS ib = 0;
        ID3D12PipelineState* pso = nullptr;
        D3D12_GPU_DESCRIPTOR_HANDLE srv{};
        bool srv_valid = false;
        std::uint32_t draws = 0;
        std::uint32_t vb_binds = 0;
    };
    // Where the draw lambdas record: the frame's list, or (parallel opaque pass) a worker's own list and bind
    // cache (thread_local: the job pool runs the parts on any thread, the caller included).
    struct RecCtx {
        ID3D12GraphicsCommandList* cl = nullptr;
        DrawBindCache bc{};
    };
    RecCtx main_rc{fr.command_list.Get(), {}};
    static thread_local RecCtx* t_rc = nullptr;
    t_rc = &main_rc;
    DrawBindCache& bind_cache = main_rc.bc;
    std::uint32_t worker_draws = 0, worker_vb_binds = 0;

    auto is_terrain_chunk = [](const GpuMeshChunk& chunk) {
        if (chunk.is_terrain) return true; // editor/BeamNG terrain tiles (set_chunk_terrain)
        const auto& fn = chunk.meta.mesh_file;
        return fn == "__terrain_tile__" || fn == "__oo_terrain__";
    };

    // BeamNG doubleSided: swap in the CULL_NONE variant of the PSO the phase/alpha logic picked.
    // content meshes (.o3d / .x): Direct3D culls their back faces and models rely on it (glass and decals
    // are modelled once per side) - openOMSI culls_back_faces. Vegetation / double-sided keep both sides.
    auto cull_content = [&](ID3D12PipelineState* pso, const GpuMeshChunk& ch, const MaterialDrawBatch& batch,
                            bool special_pass) -> ID3D12PipelineState* {
        // (text planes keep both sides: a sign read from behind beats a blank sign; the editor shows them
        //  from any side while placing)
        if (special_pass || batch.double_sided || ch.wind_sway || batch.is_text_texture) return pso;
        if (ch.content_mesh < 0) {
            const auto& mf = ch.meta.mesh_file;
            auto ends_ci = [&](const char* suf, std::size_t n) {
                if (mf.size() < n) return false;
                for (std::size_t k = 0; k < n; ++k)
                    if (std::tolower(static_cast<unsigned char>(mf[mf.size() - n + k])) != suf[k]) return false;
                return true;
            };
            ch.content_mesh = (ends_ci(".o3d", 4) || ends_ci(".x", 2)) ? 1 : 0;
        }
        if (ch.content_mesh == 0) return pso;
        if (pso == d.pipeline_state_alpha.Get() && d.pipeline_state_alpha_cull) return d.pipeline_state_alpha_cull.Get();
        if (pso == d.pipeline_state_alpha_nozwrite.Get() && d.pipeline_state_alpha_nozwrite_cull)
            return d.pipeline_state_alpha_nozwrite_cull.Get();
        if (pso == d.pipeline_state_surface_bias.Get() && d.pipeline_state_surface_bias_cull)
            return d.pipeline_state_surface_bias_cull.Get();
        if (pso == d.pipeline_state_ghost_surface.Get() && d.pipeline_state_ghost_surface_cull)
            return d.pipeline_state_ghost_surface_cull.Get();
        if (pso == d.pipeline_state_on_surface.Get() && d.pipeline_state_on_surface_cull)
            return d.pipeline_state_on_surface_cull.Get();
        return pso;
    };
    auto apply_double_sided = [&](ID3D12PipelineState* pso, const MaterialDrawBatch& batch) {
        if (!batch.double_sided) return pso;
        if (pso == d.pipeline_state.Get() && d.pipeline_state_double_sided)
            return d.pipeline_state_double_sided.Get();
        if (d.pipeline_state_double_sided_cutout && pso == d.pipeline_state_veg_cutout.Get())
            return d.pipeline_state_double_sided_cutout.Get();
        return pso; // alpha/blend/surface PSOs already use CULL_NONE
    };

    // Ground layers (roads / .sli = 1, [surface] objects = 2). Their [matl_alpha] 2 materials are drawn in
    // two parts: the opaque CORE (texels with a >= 0.98) in the opaque phase with the ground PSO, WRITING
    // DEPTH - so between overlapping roads, sidewalks and markings the triangle that is really higher at
    // that pixel wins, vertex by vertex, whatever the draw order - then the translucent REMAINDER (a < 0.98)
    // blended over that true depth. (Omsi.exe keeps z-write on for alpha 2 unless [matl_noZwrite]; drawing
    // the whole material blended WITHOUT depth made the order decide: a flat road drawn after a sidewalk
    // covered the sidewalk although every vertex of the road is lower.)
    auto ground_kind = [&](const GpuMeshChunk& c) -> int {
        if (c.render_phase == WorldRenderPhase::Spline || c.meta.mesh_file == "__spline_merged__" ||
            c.meta.mesh_file == "__spline__")
            return 1;
        if (c.surface_bias || c.render_phase == WorldRenderPhase::Surface) return 2;
        return 0;
    };
    static const bool s_ground_split = std::getenv("OMSI_NO_GROUND_SPLIT") == nullptr; // (A/B test switch)
    bool ground_core_pass = false;    // drawing only the opaque cores of ground alpha-2 batches
    bool vehicle_core_pass = false;   // the same for vehicles' alpha-2 batches (split_blend chunks)
    bool ground_split_active = false; // the cores were drawn: the alpha phase draws only the remainders
    // surface alpha phase: 1 = leave out the depth-written [matl_alpha] 2 batches (drawn after, in file order),
    // 2 = only those; the alpha-test parts of the same chunks stay instanced
    int blend_filter = 0;
    auto draw_chunk_batches = [&](const GpuMeshChunk& chunk, const std::vector<MaterialDrawBatch>& batches,
                                  bool shadow_pass, bool invert_pass = false, bool wireframe_pass = false,
                                  bool detail_pass = false) {
        if (batches.empty()) return;
        // One CB slot per chunk (not per material) — avoids matrix stomping when batch count > ring.
        const std::uint32_t slot = std::atomic_ref<std::uint32_t>(d.scene_cb_slot).fetch_add(1u);
        {
            const std::uint32_t slots_per_frame = kSceneCbSlotCount / kFrameCount;
            const std::uint32_t slot_end = (frame_index_ + 1u) * slots_per_frame;
            d.perf_cb_slots_cap = slots_per_frame;
            if (slot >= slot_end) {
                if ((++d.cb_overflow_frames % 120u) == 1u)
                    omsi::log::warn("SCENE_CB_RING_FULL frame=" + std::to_string(frame_index_) +
                                    " cap=" + std::to_string(slots_per_frame) +
                                    " draws=" + std::to_string(d.perf_draws) +
                                    " -> draws skipped; raise kSceneCbSlotCount");
                return;
            }
        }
        d.perf_cb_slots_used = slot - frame_index_ * (kSceneCbSlotCount / kFrameCount) + 1u;
        auto* cb = reinterpret_cast<SceneConstantBuffer*>(d.cb_ring_cpu + slot * kConstantBufferSize);
        if (d.has_view_proj_) {
            if (chunk.screen_space) {
                DirectX::XMStoreFloat4x4(&cb->world_view_proj, DirectX::XMMatrixIdentity());
            } else {
                const DirectX::XMMATRIX world_t = DirectX::XMLoadFloat4x4(&chunk.local_world_transposed);
                const DirectX::XMMATRIX vp_t = DirectX::XMLoadFloat4x4(&d.view_proj_transposed_);
                const DirectX::XMMATRIX world = DirectX::XMMatrixTranspose(world_t);
                const DirectX::XMMATRIX view_proj = DirectX::XMMatrixTranspose(vp_t);
                DirectX::XMStoreFloat4x4(&cb->world_view_proj, DirectX::XMMatrixTranspose(world * view_proj));
            }
        } else {
            DirectX::XMStoreFloat4x4(&cb->world_view_proj, DirectX::XMMatrixIdentity());
        }
        const D3D12_GPU_VIRTUAL_ADDRESS cbv =
            d.scene_cb_gpu_base + static_cast<D3D12_GPU_VIRTUAL_ADDRESS>(slot) * kConstantBufferSize;

        // clone_chunk shares VB/IB: only rebind when the underlying resource changes.
        {
            // detail_pass: the terrain tile's cut mesh (its own VB/IB, never LOD'd).
            const D3D12_GPU_VIRTUAL_ADDRESS vb_ptr =
                detail_pass ? chunk.detail_vbv.BufferLocation : chunk.vertex_buffer_view.BufferLocation;
            const bool use_lod =
                !detail_pass && chunk.lod_level > 0 && chunk.lod_ib[chunk.lod_level - 1].buffer;
            const D3D12_GPU_VIRTUAL_ADDRESS ib_ptr = detail_pass ? chunk.detail_ibv.BufferLocation
                                                     : use_lod   ? chunk.lod_ib[chunk.lod_level - 1].view.BufferLocation
                                                                 : chunk.index_buffer_view.BufferLocation;
            if (t_rc->bc.vb != vb_ptr || t_rc->bc.ib != ib_ptr) {
                t_rc->cl->IASetVertexBuffers(0, 1, detail_pass ? &chunk.detail_vbv : &chunk.vertex_buffer_view);
                t_rc->cl->IASetIndexBuffer(detail_pass ? &chunk.detail_ibv
                                                  : use_lod   ? &chunk.lod_ib[chunk.lod_level - 1].view
                                                              : &chunk.index_buffer_view);
                t_rc->bc.vb = vb_ptr;
                t_rc->bc.ib = ib_ptr;
                ++t_rc->bc.vb_binds;
            }
            ++t_rc->bc.draws;
        }
        t_rc->cl->SetGraphicsRootConstantBufferView(0, cbv);
        // v22.09.34 unified: write WVP into StructuredBuffer + bind SRV root[3]
        // (same as .17 — without this, VS reads zeros → nothing on screen).
        if (!d.instance_buffer || !d.instance_buffer_cpu) {
            omsi::log::error("instance_buffer not created — geometry will be invisible");
        } else {
            const std::uint32_t base = frame_index_ * HelloO3dRenderer::Impl::kMaxInstancesPerDraw;
            // Lower half for color; upper half reserved for CSM casters when shadows on.
            const std::uint32_t inst_cap = (d.csm_ready && d.sun_shadow_enable > 0.5f)
                                              ? (HelloO3dRenderer::Impl::kMaxInstancesPerDraw / 2)
                                              : HelloO3dRenderer::Impl::kMaxInstancesPerDraw;
            d.perf_instance_cap = inst_cap;
            // NEVER wrap mid-frame — that overwrites WVP of draws already recorded → ghosts / scale jump.
            const std::uint32_t cur_i = std::atomic_ref<std::uint32_t>(d.instance_write_cursor).fetch_add(1u);
            if (cur_i < inst_cap) {
                const std::uint32_t inst_slot = base + cur_i;
                auto* dst = reinterpret_cast<HelloO3dRenderer::Impl::InstanceMatrices*>(
                    d.instance_buffer_cpu + inst_slot * HelloO3dRenderer::Impl::kInstanceStride);
                dst->wvp = cb->world_view_proj;
                dst->world = chunk.local_world_transposed;
                const D3D12_GPU_VIRTUAL_ADDRESS inst_gpu =
                    d.instance_buffer->GetGPUVirtualAddress() +
                    static_cast<UINT64>(inst_slot) * HelloO3dRenderer::Impl::kInstanceStride;
                t_rc->cl->SetGraphicsRootShaderResourceView(3, inst_gpu);
            } else {
                // Overflow: SKIP this draw instead of reusing the last bound matrix.
                // Reusing it drew many objects at one position/scale (huge / shifted / not
                // clickable). Skipping only omits an object for this frame — never corrupts others.
                ++d.perf_instance_overflow;
                static std::uint32_t s_ovf = 0;
                if ((++s_ovf % 120u) == 1u)
                    omsi::log::warn("INSTANCE_RING_OVERFLOW frame=" + std::to_string(frame_index_) +
                                    " cap=" + std::to_string(inst_cap) +
                                    " shadows=" + (d.csm_ready && d.sun_shadow_enable > 0.5f ? "on" : "off") +
                                    " cursor=" + std::to_string(d.instance_write_cursor) +
                                    " draws=" + std::to_string(d.perf_draws) +
                                    " -> draw skipped (no corruption). Raise kMaxInstancesPerDraw.");
                return;
            }
        }

        // Per-chunk string compares hoisted out of the per-batch loop (100-material meshes paid them 100x).
        const bool chunk_is_terrain = is_terrain_chunk(chunk);
        const bool chunk_is_spline_merged = chunk.meta.mesh_file == "__spline_merged__";
        const bool chunk_is_spline_name = chunk.meta.mesh_file == "__spline__";
        const int chunk_ground = ground_kind(chunk);
        for (const auto& batch : batches) {
            if (batch.index_count == 0) continue;
            if (blend_filter != 0 && chunk_ground == 0 &&
                ((batch.alpha_mode == MatlAlphaMode::alpha_blend) != (blend_filter == 2)))
                continue;

            ID3D12PipelineState* pso = d.pipeline_state.Get();
            const bool ghost_opacity = chunk.opacity < 0.995f;
            const bool ground_split = chunk_ground != 0 && batch.alpha_mode == MatlAlphaMode::alpha_blend &&
                                      !batch.no_zwrite && !batch.no_zcheck && !batch.is_on_surface &&
                                      !batch.is_shadow && !batch.is_text_texture && !chunk.decal &&
                                      !ghost_opacity && !shadow_pass && !wireframe_pass && !invert_pass &&
                                      !detail_pass && s_ground_split && d.pipeline_state_spline_bias && d.pipeline_state_surface_bias;
            if (ground_core_pass && !ground_split) continue;
            const bool veh_split = chunk.split_blend && batch.alpha_mode == MatlAlphaMode::alpha_blend &&
                                   !batch.is_text_texture && !batch.is_shadow && !ghost_opacity && !shadow_pass &&
                                   !wireframe_pass && !invert_pass && !detail_pass;
            if (vehicle_core_pass && !veh_split) continue;
            float alpha_mode_override = -1.f;
            if (detail_pass && d.pipeline_state_terrain_bias) {
                pso = d.pipeline_state_terrain_bias.Get();
            } else if (chunk.detail_ready && chunk.detail_active && !wireframe_pass && !invert_pass && !shadow_pass &&
                       d.pipeline_state_terrain_underlay) {
                pso = d.pipeline_state_terrain_underlay.Get(); // uncut base mesh behind the cut detail
            } else if (chunk_is_terrain && d.pipeline_state_terrain_bias) {
                pso = d.pipeline_state_terrain_bias.Get();
            } else if (chunk.screen_space && d.pipeline_state_alpha_nozcheck) {
                pso = d.pipeline_state_alpha_nozcheck.Get();
            } else if (wireframe_pass) {
                pso = d.pipeline_state_wireframe.Get();
            } else if (invert_pass) {
                pso = d.pipeline_state_invert.Get();
            } else if (shadow_pass || batch.is_shadow) {
                pso = d.pipeline_state_alpha_shadow.Get();
            } else if (ghost_opacity && !chunk_is_terrain) {
                // Editor opacity slider: checked before the spline / surface / on_surface pipelines,
                // which ignored it (splines and [surface] .sco stayed opaque). Blend, depth test, no
                // depth write; ground layers keep their pull over the terrain.
                const bool ground = chunk.render_phase == WorldRenderPhase::Spline || chunk_is_spline_merged ||
                                    chunk_is_spline_name || chunk.surface_bias ||
                                    chunk.render_phase == WorldRenderPhase::Surface ||
                                    chunk.render_phase == WorldRenderPhase::OnSurface || batch.is_on_surface ||
                                    chunk.decal;
                pso = (ground && d.pipeline_state_ghost_surface) ? d.pipeline_state_ghost_surface.Get()
                                                                 : d.pipeline_state_alpha_nozwrite.Get();
            } else if (batch.alpha_mode == MatlAlphaMode::alpha_blend && d.pipeline_state_ghost_surface &&
                       (chunk.render_phase == WorldRenderPhase::Spline || chunk_is_spline_merged ||
                        chunk_is_spline_name || chunk.surface_bias ||
                        chunk.render_phase == WorldRenderPhase::Surface)) {
                // [matl_alpha] 2 on roads / [surface] objects: core (opaque texels, depth written) in the
                // opaque phase, the translucent remainder here (see ground_kind above)
                if (ground_core_pass) {
                    pso = chunk_ground == 1 ? d.pipeline_state_spline_bias.Get() : d.pipeline_state_surface_bias.Get();
                    alpha_mode_override = 3.f; // ps: keep a >= 0.98 only
                } else if (ground_split && ground_split_active) {
                    pso = (chunk_ground == 1 && d.pipeline_state_ghost_spline) ? d.pipeline_state_ghost_spline.Get()
                                                                               : d.pipeline_state_ghost_surface.Get();
                    alpha_mode_override = 2.2f; // ps_alpha: drop the texels the core already drew
                } else {
                    pso = d.pipeline_state_ghost_surface.Get();
                }
            } else if (batch.is_on_surface || chunk.decal ||
                       chunk.render_phase == WorldRenderPhase::OnSurface) {
                pso = d.pipeline_state_on_surface ? d.pipeline_state_on_surface.Get()
                                                  : d.pipeline_state_alpha_nozwrite.Get();
            } else if ((chunk.render_phase == WorldRenderPhase::Spline || chunk_is_spline_merged ||
                        chunk_is_spline_name) &&
                       d.pipeline_state_spline_bias) {
                // every road the same pull (openOMSI code 1.0), merged or single: a single spline on the
                // surface pull came through the merged roads it overlaps
                // Road/spline surfaces: depth-biased over terrain, but back-face culled (top only).
                pso = d.pipeline_state_spline_bias.Get();
            } else if ((chunk.surface_bias || chunk.render_phase == WorldRenderPhase::Surface) &&
                       d.pipeline_state_surface_bias) {
                pso = d.pipeline_state_surface_bias.Get();
            } else if (chunk.path_rail && d.pipeline_state_path_rail && !ghost_opacity) {
                pso = d.pipeline_state_path_rail.Get(); // over the ground, own height (see vs_rail)
            } else if (batch.is_text_texture && d.pipeline_state_ghost_surface && !ghost_opacity) {
                pso = d.pipeline_state_ghost_surface_cull ? d.pipeline_state_ghost_surface_cull.Get() : d.pipeline_state_ghost_surface.Get(); // text over its board, one-sided as Omsi.exe draws it (see mesh.cpp)
            } else if (batch.alpha_mode == MatlAlphaMode::alpha_blend || ghost_opacity) {
                // [matl_alpha] 2 or editor opacity < 1: SRC_ALPHA blend.
                // Ghost uses nozwrite so semi-transparent meshes don't occlude each other wrongly.
                if (batch.is_presurface && d.pipeline_state_presurface_alpha && !ghost_opacity)
                    pso = d.pipeline_state_presurface_alpha.Get();
                else if (batch.no_zcheck)
                    pso = d.pipeline_state_alpha_nozcheck.Get();
                else if (batch.no_zwrite || ghost_opacity)
                    pso = d.pipeline_state_alpha_nozwrite.Get();
                else if (veg_cutout_enabled() && d.pipeline_state_veg_cutout && batch.has_texture &&
                         !batch.is_text_texture)
                    pso = d.pipeline_state_veg_cutout.Get(); // cutout: no blend, writes depth
                else
                    pso = d.pipeline_state_alpha.Get();
            } else if (batch.alpha_mode == MatlAlphaMode::alpha_test) {
                // [matl_alpha] 1: 1-bit punch, no blend (opaque PSO + discard in shader).
                pso = d.pipeline_state.Get();
            }
            if (vehicle_core_pass) {
                pso = d.pipeline_state.Get(); // solid, depth written (CULL_NONE for a double-sided batch below)
                alpha_mode_override = 3.f;
            } else if (veh_split) {
                alpha_mode_override = 2.2f; // only the translucent texels (the core is drawn)
            }
            pso = cull_content(pso, chunk, batch, wireframe_pass || invert_pass || shadow_pass);
            pso = apply_double_sided(pso, batch);
            if (t_rc->bc.pso != pso) {
                t_rc->cl->SetPipelineState(pso);
                t_rc->bc.pso = pso;
            }

            float material_mode =
                wireframe_pass ? 3.f
                               : (invert_pass ? 2.f
                                              : ((shadow_pass || batch.is_shadow)
                                                     ? 1.f
                                                     : (chunk.fixed_lit ? 4.f : 0.f)));
            const bool pbr_draw = batch.pbr && material_mode == 0.f;
            if (pbr_draw) material_mode = 5.f;
            const float env_strength = pbr_draw ? batch.pbr_detail_scale : (batch.has_envmap ? batch.envmap_strength : 0.f);
            const float alpha_mode_f = alpha_mode_override >= 0.f ? alpha_mode_override
                                                                  : static_cast<float>(static_cast<int>(batch.alpha_mode));
            const float point_sample = pbr_draw ? batch.pbr_flags : (batch.is_text_texture ? 1.f : 0.f);
            if (chunk.wind_sway) d.frame_drew_sway = true;
            float obj_consts[16];
            pack_env_object(obj_consts, env_strength, material_mode, chunk.highlight, chunk.highlight_r,
                            chunk.highlight_g, chunk.highlight_b, batch.uv_off_x, batch.uv_off_y,
                            alpha_mode_f, point_sample, chunk.opacity * batch.alpha_scale, d.ambient_scale,
                            static_cast<float>(d.scene_light_count), batch.alpha_ref,
                            (batch.pbr_opacity_map ? 1.f : 0.f) + (batch.pbr_tint_rgb ? 2.f : 0.f) + (chunk.wind_sway ? 8.f : 0.f) + ((!pbr_draw && batch.has_nightmap) ? 4.f : 0.f) + ((!pbr_draw && batch.has_nightmap && batch.lightmap_mode) ? 16.f : 0.f) + ((!pbr_draw && batch.env_alpha_mask) ? 32.f : 0.f) + ((!pbr_draw && batch.has_transmap) ? 32768.f : 0.f) + ((!pbr_draw && batch.has_nightmap && batch.night_always) ? 65536.f : 0.f) + (chunk.interior ? 131072.f : 0.f) + (chunk.vehicle_self ? 262144.f : 0.f) + ((!pbr_draw && batch.has_nightmap && batch.lightmap_mode && batch.lightmap_scale >= 0.f) ? 64.f * (1.f + std::round(std::clamp(batch.lightmap_scale, 0.f, 1.f) * 255.f)) : 0.f), pbr_draw ? batch.pbr_detail_distance : batch.emissive_packed);
            t_rc->cl->SetGraphicsRoot32BitConstants(2, 16, obj_consts, 0);

            if (batch.has_texture) t_rc->cl->SetGraphicsRootDescriptorTable(1, batch.srv_gpu);
            else t_rc->cl->SetGraphicsRootDescriptorTable(1, d.srv_heap_gpu);
            auto issue = [&]() {
                if (!detail_pass && chunk.lod_level > 0 && chunk.lod_ib[chunk.lod_level - 1].buffer &&
                    batch.index_start == 0)
                    t_rc->cl->DrawIndexedInstanced(chunk.lod_ib[chunk.lod_level - 1].index_count, 1, 0, 0, 0);
                else
                    t_rc->cl->DrawIndexedInstanced(batch.index_count, 1, batch.index_start, 0, 0);
            };
            issue();
        }
    };

    auto draw_invert_chunk = [&](const GpuMeshChunk& chunk) {
        if (!chunk.opaque_batches.empty()) draw_chunk_batches(chunk, chunk.opaque_batches, false, true);
        if (!chunk.alpha_batches.empty()) draw_chunk_batches(chunk, chunk.alpha_batches, false, true);
    };

    // P6: real GPU instancing for consecutive clones sharing VB/IB (trees, repeated SCOs).
    // Writes N float4x4 into the instance ring and issues one DrawIndexedInstanced.
    auto draw_list_instanced = [&](const std::vector<std::size_t>& indices,
                                   auto batch_getter, bool shadow_pass = false, bool invert_pass = false,
                                   bool wireframe_pass = false) {
        if (indices.empty()) return;
        // Instanced draws take root constants (highlight/opacity) from the group head only.
        auto per_chunk_state = [&](const GpuMeshChunk& c) {
            return c.highlight > 0.001f || c.opacity < 0.995f || ground_kind(c) != 0 || c.split_blend;
        };
        std::size_t i = 0;
        while (i < indices.size()) {
            const auto& head = d.mesh_chunks[indices[i]];
            const D3D12_GPU_VIRTUAL_ADDRESS vb = head.vertex_buffer_view.BufferLocation;
            if (per_chunk_state(head)) {
                draw_chunk_batches(head, batch_getter(head), shadow_pass, invert_pass, wireframe_pass);
                ++i;
                continue;
            }
            // An instanced group draws every member with the head's batches: only clones whose batches show the
            // same textures (srv tables) and UV / alpha state may join. Each AI car has its own paint scheme and
            // its own lamp textures ([matl_change] variants): grouped on the vertex buffer alone, every car of a
            // type took the paint and the lamps of whichever clone headed the group - cars changed their skin in
            // front of the camera and never showed their own headlights / blinkers.
            const auto& head_batches = batch_getter(head);
            auto same_look = [&](const GpuMeshChunk& c) {
                if (shadow_pass) return true; // (depth only)
                const auto& cb = batch_getter(c);
                if (cb.size() != head_batches.size()) return false;
                for (std::size_t q = 0; q < cb.size(); ++q) {
                    const auto& a = cb[q];
                    const auto& b = head_batches[q];
                    if (a.srv_gpu.ptr != b.srv_gpu.ptr || a.uv_off_x != b.uv_off_x || a.uv_off_y != b.uv_off_y ||
                        a.alpha_scale != b.alpha_scale)
                        return false;
                }
                return true;
            };
            std::size_t j = i + 1;
            while (j < indices.size() && d.mesh_chunks[indices[j]].vertex_buffer_view.BufferLocation == vb &&
                   !per_chunk_state(d.mesh_chunks[indices[j]]) && same_look(d.mesh_chunks[indices[j]]))
                ++j;
            const std::size_t group_n = j - i;
            if (group_n < 2) {
                draw_chunk_batches(head, batch_getter(head), shadow_pass, invert_pass, wireframe_pass);
                i = j;
                continue;
            }
            // Instance all that share this VB (up to kMaxInstancesPerDraw per submit).
            const auto& batches = batch_getter(head);
            if (batches.empty()) {
                i = j;
                continue;
            }
            // Bind VB/IB once
            {
                const D3D12_GPU_VIRTUAL_ADDRESS ib_ptr = head.index_buffer_view.BufferLocation;
                if (t_rc->bc.vb != vb || t_rc->bc.ib != ib_ptr) {
                    t_rc->cl->IASetVertexBuffers(0, 1, &head.vertex_buffer_view);
                    t_rc->cl->IASetIndexBuffer(&head.index_buffer_view);
                    t_rc->bc.vb = vb;
                    t_rc->bc.ib = ib_ptr;
                    ++t_rc->bc.vb_binds;
                }
            }
            const std::uint32_t inst_cap = (d.csm_ready && d.sun_shadow_enable > 0.5f)
                                              ? (HelloO3dRenderer::Impl::kMaxInstancesPerDraw / 2)
                                              : HelloO3dRenderer::Impl::kMaxInstancesPerDraw;
            d.perf_instance_cap = inst_cap;
            std::size_t base_i = i;
            while (base_i < j) {
                const std::size_t n = (std::min)(j - base_i, static_cast<std::size_t>(inst_cap));
                const std::uint32_t region = frame_index_ * HelloO3dRenderer::Impl::kMaxInstancesPerDraw;
                const std::uint32_t got =
                    std::atomic_ref<std::uint32_t>(d.instance_write_cursor).fetch_add(static_cast<std::uint32_t>(n));
                if (got + static_cast<std::uint32_t>(n) > inst_cap) {
                    // Do not spin forever when one VB group exceeds the per-frame
                    // instance ring. Fall back to the normal per-chunk path for the
                    // remainder of this group; correctness is more important than
                    // instancing in this overflow case.
                    for (std::size_t k = base_i; k < j; ++k)
                        draw_chunk_batches(d.mesh_chunks[indices[k]], batch_getter(d.mesh_chunks[indices[k]]),
                                           shadow_pass, invert_pass, wireframe_pass);
                    base_i = j;
                    continue;
                }
                const std::uint32_t inst0 = region + got;
                for (std::size_t k = 0; k < n; ++k) {
                    const auto& ch = d.mesh_chunks[indices[base_i + k]];
                    HelloO3dRenderer::Impl::InstanceMatrices mats{};
                    if (d.has_view_proj_ && !ch.screen_space) {
                        const DirectX::XMMATRIX world_t = DirectX::XMLoadFloat4x4(&ch.local_world_transposed);
                        const DirectX::XMMATRIX vp_t = DirectX::XMLoadFloat4x4(&d.view_proj_transposed_);
                        const DirectX::XMMATRIX world = DirectX::XMMatrixTranspose(world_t);
                        const DirectX::XMMATRIX view_proj = DirectX::XMMatrixTranspose(vp_t);
                        DirectX::XMStoreFloat4x4(&mats.wvp, DirectX::XMMatrixTranspose(world * view_proj));
                    } else {
                        DirectX::XMStoreFloat4x4(&mats.wvp, DirectX::XMMatrixIdentity());
                    }
                    mats.world = ch.local_world_transposed;
                    auto* dst = reinterpret_cast<HelloO3dRenderer::Impl::InstanceMatrices*>(
                        d.instance_buffer_cpu +
                        (inst0 + static_cast<std::uint32_t>(k)) * HelloO3dRenderer::Impl::kInstanceStride);
                    *dst = mats;
                }
                const D3D12_GPU_VIRTUAL_ADDRESS inst_gpu =
                    d.instance_buffer->GetGPUVirtualAddress() +
                    static_cast<UINT64>(inst0) * HelloO3dRenderer::Impl::kInstanceStride;
                t_rc->cl->SetGraphicsRootShaderResourceView(3, inst_gpu);

                // Use first chunk of group for material/env (same prototype for clones).
                const auto& proto = d.mesh_chunks[indices[base_i]];
                for (const auto& batch : batches) {
                    if (batch.index_count == 0) continue;
                    if (blend_filter != 0 && ((batch.alpha_mode == MatlAlphaMode::alpha_blend) != (blend_filter == 2)))
                        continue;
                    ID3D12PipelineState* pso = d.pipeline_state.Get();
                    const bool ghost_opacity = proto.opacity < 0.995f;
                    if (wireframe_pass) pso = d.pipeline_state_wireframe.Get();
                    else if (invert_pass) pso = d.pipeline_state_invert.Get();
                    else if (shadow_pass || batch.is_shadow) pso = d.pipeline_state_alpha_shadow.Get();
                    else if (batch.is_on_surface)
                        pso = d.pipeline_state_on_surface ? d.pipeline_state_on_surface.Get()
                                                         : d.pipeline_state_alpha_nozwrite.Get();
                    else if (batch.alpha_mode == MatlAlphaMode::alpha_blend || ghost_opacity) {
                        if (batch.no_zwrite) pso = d.pipeline_state_alpha_nozwrite.Get();
                        else if (veg_cutout_enabled() && d.pipeline_state_veg_cutout &&
                                 batch.has_texture && !batch.is_text_texture)
                            pso = d.pipeline_state_veg_cutout.Get(); // cutout: no blend, writes depth
                        else pso = d.pipeline_state_alpha.Get();
                    } else if (batch.alpha_mode == MatlAlphaMode::alpha_test) {
                        pso = d.pipeline_state_alpha.Get();
                    }
                    pso = cull_content(pso, proto, batch, wireframe_pass || invert_pass || shadow_pass);
                    pso = apply_double_sided(pso, batch);
                    if (t_rc->bc.pso != pso) {
                        t_rc->cl->SetPipelineState(pso);
                        t_rc->bc.pso = pso;
                    }
                    float material_mode =
                        wireframe_pass ? 3.f
                                       : (invert_pass ? 2.f
                                                      : ((shadow_pass || batch.is_shadow)
                                                             ? 1.f
                                                             : (proto.fixed_lit ? 4.f : 0.f)));
                    const bool pbr_draw = batch.pbr && material_mode == 0.f;
                    if (pbr_draw) material_mode = 5.f;
                    const float env_strength = pbr_draw ? batch.pbr_detail_scale : batch.envmap_strength;
                    if (proto.wind_sway) d.frame_drew_sway = true;
                    float obj_consts[16];
                    pack_env_object(obj_consts, env_strength, material_mode, proto.highlight, proto.highlight_r,
                                    proto.highlight_g, proto.highlight_b, batch.uv_off_x, batch.uv_off_y,
                                    static_cast<float>(static_cast<int>(batch.alpha_mode)),
                                    pbr_draw ? batch.pbr_flags : (batch.is_text_texture ? 1.f : 0.f), proto.opacity * batch.alpha_scale,
                                    d.ambient_scale, static_cast<float>(d.scene_light_count), batch.alpha_ref,
                                    (batch.pbr_opacity_map ? 1.f : 0.f) + (batch.pbr_tint_rgb ? 2.f : 0.f) + (proto.wind_sway ? 8.f : 0.f) + ((!pbr_draw && batch.has_nightmap) ? 4.f : 0.f) + ((!pbr_draw && batch.has_nightmap && batch.lightmap_mode) ? 16.f : 0.f) + ((!pbr_draw && batch.env_alpha_mask) ? 32.f : 0.f) + ((!pbr_draw && batch.has_transmap) ? 32768.f : 0.f) + ((!pbr_draw && batch.has_nightmap && batch.night_always) ? 65536.f : 0.f) + (proto.interior ? 131072.f : 0.f) + (proto.vehicle_self ? 262144.f : 0.f) + ((!pbr_draw && batch.has_nightmap && batch.lightmap_mode && batch.lightmap_scale >= 0.f) ? 64.f * (1.f + std::round(std::clamp(batch.lightmap_scale, 0.f, 1.f) * 255.f)) : 0.f), pbr_draw ? batch.pbr_detail_distance : batch.emissive_packed);
                    t_rc->cl->SetGraphicsRoot32BitConstants(2, 16, obj_consts, 0);
                    if (batch.has_texture) t_rc->cl->SetGraphicsRootDescriptorTable(1, batch.srv_gpu);
                    else t_rc->cl->SetGraphicsRootDescriptorTable(1, d.srv_heap_gpu);
                    t_rc->cl->DrawIndexedInstanced(batch.index_count, static_cast<UINT>(n),
                                                         batch.index_start, 0, 0);
                    ++t_rc->bc.draws;
                    if (n > 1) std::atomic_ref<std::uint64_t>(d.perf_instanced_draws).fetch_add(1u);
                }
                base_i += n;
            }
            i = j;
        }
    };

    // Only [rendertype] presurface (sco priority) draws before terrain — not matl.presurface on mesh upload.
    auto skip_presurface = [](const GpuMeshChunk& chunk) {
        return chunk.render_phase == WorldRenderPhase::PreSurface;
    };

    // ---- P3/P4/P5: one frustum pass + tile coarse cull + single VB sort ----
    // Cell size is the active map row (300 m classic, ~380 m WC at Vaals). Origin is
    // editor-space SW of the (0,0) bin — set_tile_cull_cell from the editor.
    const float kTileW = d.tile_cull_w > 1.f ? d.tile_cull_w : 300.f;
    const float kTileH = d.tile_cull_h > 1.f ? d.tile_cull_h : 300.f;
    const float kTileOx = d.tile_cull_ox;
    const float kTileOy = d.tile_cull_oy;
    struct TileKey {
        int x = 0, y = 0;
        bool operator==(const TileKey& o) const { return x == o.x && y == o.y; }
    };
    struct TileKeyHash {
        std::size_t operator()(const TileKey& k) const {
            return (static_cast<std::size_t>(k.x) * 73856093u) ^
                   (static_cast<std::size_t>(k.y) * 19349663u);
        }
    };

    // World-space center of chunk AABB (for tile binning).
    auto chunk_world_center = [](const GpuMeshChunk& chunk) -> DirectX::XMFLOAT3 {
        const float mx = 0.5f * (chunk.meta.bbox_min[0] + chunk.meta.bbox_max[0]);
        const float my = 0.5f * (chunk.meta.bbox_min[1] + chunk.meta.bbox_max[1]);
        const float mz = 0.5f * (chunk.meta.bbox_min[2] + chunk.meta.bbox_max[2]);
        const DirectX::XMMATRIX world_t = DirectX::XMLoadFloat4x4(&chunk.local_world_transposed);
        const DirectX::XMMATRIX world = DirectX::XMMatrixTranspose(world_t);
        DirectX::XMVECTOR c = DirectX::XMVector3TransformCoord(
            DirectX::XMVectorSet(mx, my, mz, 1.f), world);
        DirectX::XMFLOAT3 out{};
        DirectX::XMStoreFloat3(&out, c);
        return out;
    };
    auto tile_key_from_world = [&](float wx, float wy) -> TileKey {
        return TileKey{static_cast<int>(std::floor((wx - kTileOx) / kTileW)),
                       static_cast<int>(std::floor((wy - kTileOy) / kTileH))};
    };
    // Frustum test a whole tile as an AABB in world XY (generous Z).
    auto tile_in_frustum = [&](TileKey tk) -> bool {
        if (!have_frustum) return true;
        const float x0 = kTileOx + static_cast<float>(tk.x) * kTileW;
        const float y0 = kTileOy + static_cast<float>(tk.y) * kTileH;
        const float x1 = x0 + kTileW;
        const float y1 = y0 + kTileH;
        const float z0 = -50.f, z1 = 80.f;
        const float pad = 0.15f;
        const float xs[2] = {x0, x1}, ys[2] = {y0, y1}, zs[2] = {z0, z1};
        bool any = false;
        bool all_neg = true, all_pos = true;
        for (int i = 0; i < 2; ++i)
            for (int j = 0; j < 2; ++j)
                for (int k = 0; k < 2; ++k) {
                    DirectX::XMVECTOR p = DirectX::XMVectorSet(xs[i], ys[j], zs[k], 1.f);
                    DirectX::XMVECTOR clip = DirectX::XMVector4Transform(p, cull_wvp);
                    float w = DirectX::XMVectorGetW(clip);
                    if (w < 0.f) all_pos = false;
                    else all_neg = false;
                    float invw = (std::abs(w) > 1.e-6f) ? (1.f / w) : 0.f;
                    float ndc_x = DirectX::XMVectorGetX(clip) * invw;
                    float ndc_y = DirectX::XMVectorGetY(clip) * invw;
                    float ndc_z = DirectX::XMVectorGetZ(clip) * invw;
                    if (ndc_x >= -1.f - pad && ndc_x <= 1.f + pad && ndc_y >= -1.f - pad &&
                        ndc_y <= 1.f + pad && ndc_z >= -pad && ndc_z <= 1.f + pad)
                        any = true;
                }
        if (all_neg) return false;
        return any || !all_pos; // straddling near plane → keep
    };

    // P5: which tiles survive coarse frustum.
    std::unordered_map<TileKey, bool, TileKeyHash> tile_visible;
    tile_visible.reserve(64);
    auto ensure_tile_visible = [&](TileKey tk) -> bool {
        auto it = tile_visible.find(tk);
        if (it != tile_visible.end()) return it->second;
        const bool ok = tile_in_frustum(tk);
        tile_visible.emplace(tk, ok);
        return ok;
    };

    // ---- Terrain tile LOD: pick a level per heavy tile by ring distance to the camera tile -------------
    // ring 0 (tile under the camera) = original mesh, ring 1 = level 2, ring 2 = level 3, ring >= 3 = level 4.
    // Heights (heightfield / object placement) never read this mesh, so lowering it changes only the picture.
    if (d.terrain_lod_enabled && !d.lod_chunks.empty()) {
        ++d.lod_frame_counter;
        // free replaced index buffers once the GPU is certainly done with them
        for (std::size_t g = 0; g < d.lod_graveyard.size();) {
            if (d.lod_frame_counter - d.lod_graveyard[g].first > 8u) {
                d.lod_graveyard[g] = std::move(d.lod_graveyard.back());
                d.lod_graveyard.pop_back();
            } else ++g;
        }
        // 1) adopt ONE finished job per frame (creates tiny upload-heap index buffers)
        HelloO3dRenderer::Impl::LodJobResult done;
        bool have_done = false;
        {
            std::lock_guard<std::mutex> lk(d.lod_shared->mu);
            if (!d.lod_shared->ready.empty()) {
                done = std::move(d.lod_shared->ready.back());
                d.lod_shared->ready.pop_back();
                have_done = true;
            }
        }
        if (have_done && done.chunk < d.mesh_chunks.size()) {
            auto& ch = d.mesh_chunks[done.chunk];
            if (ch.lod_capable && ch.lod_gen == done.gen) {
                for (int L = 0; L < GpuMeshChunk::kLodLevels; ++L) {
                    auto& dst = ch.lod_ib[L];
                    if (dst.buffer) d.lod_graveyard.emplace_back(d.lod_frame_counter, std::move(dst.buffer));
                    dst = GpuMeshChunk::LodIndexBuffer{};
                    const auto& src = done.idx[L];
                    if (src.empty()) continue;
                    const UINT64 bytes = static_cast<UINT64>(src.size()) * sizeof(std::uint32_t);
                    D3D12_HEAP_PROPERTIES hp{};
                    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
                    D3D12_RESOURCE_DESC rd{};
                    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                    rd.Width = bytes;
                    rd.Height = 1;
                    rd.DepthOrArraySize = 1;
                    rd.MipLevels = 1;
                    rd.SampleDesc.Count = 1;
                    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                    if (FAILED(d.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                                 IID_PPV_ARGS(dst.buffer.ReleaseAndGetAddressOf()))))
                        continue;
                    void* mapped = nullptr;
                    D3D12_RANGE rr{0, 0};
                    if (SUCCEEDED(dst.buffer->Map(0, &rr, &mapped)) && mapped) {
                        std::memcpy(mapped, src.data(), static_cast<std::size_t>(bytes));
                        dst.buffer->Unmap(0, nullptr);
                        dst.view.BufferLocation = dst.buffer->GetGPUVirtualAddress();
                        dst.view.SizeInBytes = static_cast<UINT>(bytes);
                        dst.view.Format = DXGI_FORMAT_R32_UINT;
                        dst.index_count = static_cast<std::uint32_t>(src.size());
                    } else dst.buffer.Reset();
                }
            }
            ch.lod_job_submitted = false;
        }
        // 2) submit at most one build job per frame (closest heavy tile first is not needed: one per frame)
        if (d.lod_shared->inflight.load() < 2) {
            for (const std::size_t ci : d.lod_chunks) {
                if (ci >= d.mesh_chunks.size()) continue;
                auto& ch = d.mesh_chunks[ci];
                if (!ch.lod_capable || ch.lod_job_submitted || ch.pick_positions.empty() || ch.pick_indices.empty())
                    continue;
                bool has_any = false;
                for (int L = 0; L < GpuMeshChunk::kLodLevels; ++L) has_any = has_any || ch.lod_ib[L].buffer;
                if (has_any) continue;
                ch.lod_job_submitted = true;
                auto shared = d.lod_shared;
                shared->inflight.fetch_add(1);
                const std::uint32_t gen = ch.lod_gen;
                const float bx0 = ch.meta.bbox_min[0], by0 = ch.meta.bbox_min[1];
                const float bx1 = ch.meta.bbox_max[0], by1 = ch.meta.bbox_max[1];
                auto pos = std::make_shared<std::vector<DirectX::XMFLOAT3>>(ch.pick_positions);
                auto idx = std::make_shared<std::vector<std::uint32_t>>(ch.pick_indices);
                omsi::JobSystem::global().submit_background([shared, ci, gen, pos, idx, bx0, by0, bx1, by1] {
                    HelloO3dRenderer::Impl::LodJobResult r;
                    r.chunk = ci;
                    r.gen = gen;
                    try {
                        omsi::gfx::dx12::terrain_lod::build_all_levels(*pos, *idx, bx0, by0, bx1, by1, r.idx);
                    } catch (...) {
                        for (auto& v : r.idx) v.clear();
                    }
                    {
                        std::lock_guard<std::mutex> lk(shared->mu);
                        shared->ready.push_back(std::move(r));
                    }
                    shared->inflight.fetch_sub(1);
                });
                break;
            }
        }
        // 3) level per chunk this frame, from the camera's XY distance to the tile's rectangle (not the
        // tile ring: standing by a tile border the neighbour, metres away, dropped to level 2 and its
        // simplified surface rose through the roads and crossings next to the camera). Full detail
        // within one tile width; a 10 % margin keeps a level until the camera is clearly past it.
        const float lod_tile = (std::max)(kTileW, kTileH);
        for (const std::size_t ci : d.lod_chunks) {
            if (ci >= d.mesh_chunks.size()) continue;
            auto& ch = d.mesh_chunks[ci];
            const int prev = ch.lod_level;
            ch.lod_level = 0;
            if (!ch.lod_capable) continue;
            const auto c = chunk_world_center(ch);
            const float hx = 0.5f * (ch.meta.bbox_max[0] - ch.meta.bbox_min[0]);
            const float hy = 0.5f * (ch.meta.bbox_max[1] - ch.meta.bbox_min[1]);
            const float dx = (std::max)(0.f, std::fabs(d.camera_world_pos[0] - c.x) - hx);
            const float dy = (std::max)(0.f, std::fabs(d.camera_world_pos[1] - c.y) - hy);
            const float dist = std::sqrt(dx * dx + dy * dy);
            const auto level_at = [&](float m) {
                return m < lod_tile ? 0 : m < 2.f * lod_tile ? 2 : m < 3.f * lod_tile ? 3 : 4;
            };
            int want = level_at(dist);
            if (want > prev) want = (std::max)(prev, level_at(dist - 0.1f * lod_tile)); // coarser: with margin
            while (want > 0 && !ch.lod_ib[want - 1].buffer) --want;
            ch.lod_level = static_cast<std::uint8_t>(want);
        }
    }

    // Terrain detail (cut) meshes: active while the chunk's tile is within detail_ring_max tiles of the camera.
    // One extra ring of hysteresis once active so a camera standing on the border does not flicker.
    if (!d.detail_chunks.empty()) {
        const int dcam_tx = static_cast<int>(std::floor((d.camera_world_pos[0] - kTileOx) / kTileW));
        const int dcam_ty = static_cast<int>(std::floor((d.camera_world_pos[1] - kTileOy) / kTileH));
        for (const std::size_t ci : d.detail_chunks) {
            if (ci >= d.mesh_chunks.size()) continue;
            auto& ch = d.mesh_chunks[ci];
            if (!ch.detail_ready || !ch.meta.visible) {
                ch.detail_active = false;
                continue;
            }
            const auto c = chunk_world_center(ch);
            const TileKey tk = tile_key_from_world(c.x, c.y);
            const int ring = (std::max)(std::abs(tk.x - dcam_tx), std::abs(tk.y - dcam_ty));
            ch.detail_active = ring <= ch.detail_ring_max + (ch.detail_active ? 1 : 0);
        }
    }

    // The shadow cascades: recorded on a worker into their own command list while this thread records the
    // picture (they only read the chunks - the terrain LOD and detail meshes are decided above - and write the
    // upper half of the instance ring). Submitted before the main list (see the frame's ExecuteCommandLists).
    // On the main thread they were 8-13 ms of every cascade frame with the AI traffic moving.
    if (csm_pending) {
        const std::uint32_t fi = frame_index_;
        bool ready = true;
        if (!d.csm_alloc[fi]) {
            ready = SUCCEEDED(d.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                               IID_PPV_ARGS(d.csm_alloc[fi].GetAddressOf()))) &&
                    SUCCEEDED(d.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, d.csm_alloc[fi].Get(), nullptr,
                                                          IID_PPV_ARGS(d.csm_list[fi].GetAddressOf()))) &&
                    SUCCEEDED(d.csm_list[fi]->Close());
        }
        if (ready) {
            auto job = std::make_shared<std::atomic<int>>(0);
            d.csm_job = job;
            auto* dd = &d;
            const int ci = csm_cascade_now, ci2 = csm_cascade_also;
            auto body = [dd, fi, ci, ci2, job]() {
                int expect = 0;
                if (!job->compare_exchange_strong(expect, 3)) return; // (the other side has it)
                int result = 2;
                try {
                    auto& D = *dd;
                    throw_hr(D.csm_alloc[fi]->Reset(), "csm alloc reset");
                    throw_hr(D.csm_list[fi]->Reset(D.csm_alloc[fi].Get(), nullptr), "csm list reset");
                    render_csm_pass(D, D.csm_list[fi].Get(), fi, ci, false, ci2); // (its caster pass in parallel: the caller helps, no wait-lock)
                    throw_hr(D.csm_list[fi]->Close(), "csm list close");
                    result = 1;
                } catch (...) {
                }
                job->store(result, std::memory_order_release);
            };
            d.csm_job_body = body;
            ShadowThread::get().submit(body);
        } else {
            render_csm_pass(d, fr.command_list.Get(), frame_index_, csm_cascade_now, false, csm_cascade_also); // (no list: as before)
        }
    }

    // P3: single candidate list after tile + chunk frustum (once).
    std::vector<std::size_t> visible_chunks;
    visible_chunks.reserve(d.mesh_chunks.size() / 4 + 16);

    const auto cull_t0 = std::chrono::steady_clock::now();
    std::unordered_set<TileKey, TileKeyHash> tiles_tested;
    std::unordered_set<TileKey, TileKeyHash> tiles_visible;
    tiles_tested.reserve(128);
    tiles_visible.reserve(128);

    // Diagnostic-only per-tile/geometry stats are expensive: one full world-matrix transform
    // (chunk_world_center) + hash inserts PER VISIBLE CHUNK, EVERY FRAME. They are consumed only
    // by the 60-frame report, so compute them on that frame only. On very large chunk counts this
    // was dominating CPU regardless of how much was actually drawn, which is why lowering the
    // visual/screen cull did not raise FPS (the cull work still ran, only draws were skipped).
    const bool cull_report_frame = (d.perf_report_frames == 59u);

    // Frustum/distance/size tests are independent per chunk (chunk_in_frustum only writes that chunk's own
    // mutable AABB cache), so on large scenes run them across the job pool and keep the original order in the
    // serial pass below. 0 = skipped (not visible/no VB), 1 = tested and culled, 2 = tested and kept.
    static thread_local std::vector<std::uint8_t> cull_state;
    const std::size_t cull_n = d.mesh_chunks.size();
    const bool cull_parallel = cull_n >= 4096;
    if (cull_parallel) {
        cull_state.assign(cull_n, 0);
        std::uint8_t* const st = cull_state.data(); // plain pointer: cull_state is thread_local
        // (on the render's own threads: the job pool's workers were busy loading, the main thread culled alone)
        {
            constexpr int kParts = RenderPool::kHelpers + 1;
            RenderPool::get().run(kParts, [&, st](int part) {
                const std::size_t a = cull_n * static_cast<std::size_t>(part) / kParts;
                const std::size_t b = cull_n * static_cast<std::size_t>(part + 1) / kParts;
                for (std::size_t i = a; i < b; ++i) {
                    const auto& c = d.mesh_chunks[i];
                    if (!c.meta.visible || !c.vertex_buffer) continue;
                    st[i] = (c.screen_space || chunk_in_frustum(c)) ? 2 : 1;
                }
            });
        }
    }

    // (parallel: the state array says it all - the serial pass only reads one byte per chunk instead of
    //  walking ~90k chunk structs again, 3-4 ms of cache misses a frame)
    if (cull_parallel && !cull_report_frame) {
        const std::uint8_t* const st = cull_state.data();
        for (std::size_t i = 0; i < cull_n; ++i) {
            if (st[i] == 0) continue;
            ++d.perf_chunks_tested;
            if (st[i] == 2) visible_chunks.push_back(i);
            else ++d.perf_chunks_culled;
        }
    }
    for (std::size_t i = 0; i < d.mesh_chunks.size() && !(cull_parallel && !cull_report_frame); ++i) {
        const auto& chunk = d.mesh_chunks[i];
        if (!chunk.meta.visible || !chunk.vertex_buffer) continue;
        ++d.perf_chunks_tested;

        if (chunk.screen_space) {
            visible_chunks.push_back(i);
            if (cull_report_frame) {
                const auto center = chunk_world_center(chunk);
                const TileKey tk = tile_key_from_world(center.x, center.y);
                tiles_tested.insert(tk);
                tiles_visible.insert(tk);
            }
            continue;
        }

        // Tile pre-cull is advisory; keep the existing correctness behavior.
        if (cull_parallel ? cull_state[i] != 2 : !chunk_in_frustum(chunk)) {
            ++d.perf_chunks_culled;
            continue;
        }
        visible_chunks.push_back(i);
        if (cull_report_frame) {
            const auto center = chunk_world_center(chunk);
            const TileKey tk = tile_key_from_world(center.x, center.y);
            tiles_tested.insert(tk);
            tiles_visible.insert(tk);
        }
    }
    d.perf_chunks_visible = visible_chunks.size();
    d.perf_visible_objects_est = visible_chunks.size();
    d.perf_tiles_seen = tiles_tested.size();
    d.perf_tiles_visible = tiles_visible.size();

    d.perf_cull_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - cull_t0).count();

    // Count the amount of geometry/material work represented by visible chunks.
    // Report frame only (see above) — this loops every visible chunk.
    if (cull_report_frame) {
        for (const std::size_t i : visible_chunks) {
            const auto& c = d.mesh_chunks[i];
            const std::uint64_t vb = c.vertex_buffer_view.SizeInBytes;
            const std::uint64_t ib = c.index_buffer_view.SizeInBytes;
            d.perf_geometry_bytes_visible += vb + ib;
            d.perf_vertices_visible += c.vertex_buffer_view.StrideInBytes
                                            ? vb / c.vertex_buffer_view.StrideInBytes
                                            : 0;
            const std::uint64_t index_stride =
                c.index_buffer_view.Format == DXGI_FORMAT_R16_UINT ? 2ull : 4ull;
            d.perf_indices_visible += ib / index_stride;
            d.perf_material_batches += c.opaque_batches.size() + c.alpha_batches.size() +
                                       c.alpha_nozwrite_batches.size() + c.shadow_batches.size();
        }
    }

    // On the report frame only, identify the spatial/asset hotspots. This is the
    // "exact zone" diagnostic: 300 m buckets approximate the map tile grid, and
    // mesh_file identifies the individual O3D/SCO prototype contributing work.
    d.perf_hotspots.clear();
    if (d.perf_report_frames == 59u) {
        struct Hot {
            std::uint64_t bytes = 0;
            std::uint64_t chunks = 0;
            std::uint64_t batches = 0;
        };
        std::unordered_map<TileKey, Hot, TileKeyHash> hot_tiles;
        std::unordered_map<std::string, Hot> hot_meshes;
        hot_tiles.reserve(128);
        hot_meshes.reserve(256);

        for (const std::size_t i : visible_chunks) {
            const auto& c = d.mesh_chunks[i];
            const std::uint64_t bytes =
                static_cast<std::uint64_t>(c.vertex_buffer_view.SizeInBytes) +
                static_cast<std::uint64_t>(c.index_buffer_view.SizeInBytes);
            const auto center = chunk_world_center(c);
            auto& ht = hot_tiles[tile_key_from_world(center.x, center.y)];
            ht.bytes += bytes;
            ++ht.chunks;
            ht.batches += c.opaque_batches.size() + c.alpha_batches.size() +
                          c.alpha_nozwrite_batches.size() + c.shadow_batches.size();

            auto& hm = hot_meshes[c.meta.mesh_file];
            hm.bytes += bytes;
            ++hm.chunks;
            hm.batches += c.opaque_batches.size() + c.alpha_batches.size() +
                          c.alpha_nozwrite_batches.size() + c.shadow_batches.size();
        }

        std::vector<std::pair<TileKey, Hot>> tiles(hot_tiles.begin(), hot_tiles.end());
        std::sort(tiles.begin(), tiles.end(), [](const auto& a, const auto& b) {
            return a.second.bytes > b.second.bytes;
        });
        std::vector<std::pair<std::string, Hot>> meshes(hot_meshes.begin(), hot_meshes.end());
        std::sort(meshes.begin(), meshes.end(), [](const auto& a, const auto& b) {
            return a.second.bytes > b.second.bytes;
        });

        std::ostringstream hs;
        hs << "HOTSPOTS";
        const std::size_t nt = (std::min)(tiles.size(), std::size_t{8});
        for (std::size_t i = 0; i < nt; ++i) {
            const auto& [tk, h] = tiles[i];
            hs << " tile" << i << "=(" << tk.x << "," << tk.y << ")"
               << ":chunks=" << h.chunks
               << ",MB=" << (static_cast<double>(h.bytes) / (1024.0 * 1024.0))
               << ",batches=" << h.batches;
        }
        const std::size_t nm = (std::min)(meshes.size(), std::size_t{8});
        for (std::size_t i = 0; i < nm; ++i) {
            const auto& [name, h] = meshes[i];
            hs << " mesh" << i << "=" << name
               << ":chunks=" << h.chunks
               << ",MB=" << (static_cast<double>(h.bytes) / (1024.0 * 1024.0))
               << ",batches=" << h.batches;
        }
        d.perf_hotspots = hs.str();
    }

    // distance to an object's origin (its chunks share the world matrix, so they tie and keep file order);
    // chunks baked in world space (identity matrix) use their box centre
    auto object_dist2 = [&](const GpuMeshChunk& c) -> float {
        const auto& m = c.local_world_transposed;
        float x = m._14, y = m._24, z = m._34;
        if (x == 0.f && y == 0.f && z == 0.f) {
            const float* mn = c.aabb_cache_valid ? c.aabb_cache_min : c.meta.bbox_min;
            const float* mx = c.aabb_cache_valid ? c.aabb_cache_max : c.meta.bbox_max;
            x = 0.5f * (mn[0] + mx[0]); y = 0.5f * (mn[1] + mx[1]); z = 0.5f * (mn[2] + mx[2]);
        }
        const float dx = x - d.camera_world_pos[0], dy = y - d.camera_world_pos[1], dz = z - d.camera_world_pos[2];
        return dx * dx + dy * dy + dz * dz;
    };
    // P4: sort once by shared VB for bind sharing, then classify into draw lists.
    const auto sort_t0 = std::chrono::steady_clock::now();
    std::stable_sort(visible_chunks.begin(), visible_chunks.end(), [&](std::size_t a, std::size_t b) {
        const auto va = d.mesh_chunks[a].vertex_buffer_view.BufferLocation;
        const auto vb = d.mesh_chunks[b].vertex_buffer_view.BufferLocation;
        if (va != vb) return va < vb;
        // (clones that look the same side by side: an instanced group takes only those - see draw_list_instanced)
        auto look = [&](std::size_t c) -> std::uint64_t {
            const auto& ch = d.mesh_chunks[c];
            if (!ch.opaque_batches.empty()) return ch.opaque_batches.front().srv_gpu.ptr;
            if (!ch.alpha_batches.empty()) return ch.alpha_batches.front().srv_gpu.ptr;
            return 0;
        };
        const auto la = look(a), lb = look(b);
        if (la != lb) return la < lb;
        return a < b;
    });
    d.perf_sort_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - sort_t0).count();

    struct VisibleDrawLists {
        std::vector<std::size_t> presurface_opaque;
        std::vector<std::size_t> presurface_alpha;
        std::vector<std::size_t> terrain;
        std::vector<std::size_t> surface_opaque;
        std::vector<std::size_t> surface_alpha;
        std::vector<std::size_t> shadow;
        std::vector<std::size_t> wireframe;
        std::vector<std::size_t> alpha_nozwrite;
        std::vector<std::size_t> invert;
        std::vector<std::size_t> ghost; // editor opacity < 1: drawn last, far to near
        std::vector<std::size_t> rails; // editor path rails: after the scene, over the ground
        std::vector<std::size_t> ground_cores;  // surface chunks with ground alpha-2 batches (their opaque cores)
        std::vector<std::size_t> vehicle_cores; // split_blend chunks (vehicles' alpha-2 cores)
    } lists;
    lists.presurface_opaque.reserve(visible_chunks.size() / 8 + 4);
    lists.surface_opaque.reserve(visible_chunks.size() / 2 + 4);

    const auto classify_t0 = std::chrono::steady_clock::now();
    for (std::size_t i : visible_chunks) {
        const auto& chunk = d.mesh_chunks[i];
        if (chunk.invert_color) {
            lists.invert.push_back(i);
            continue;
        }
        if (chunk.wireframe) {
            lists.wireframe.push_back(i);
            continue;
        }
        if (is_terrain_chunk(chunk)) {
            lists.terrain.push_back(i);
            continue;
        }
        if (chunk.path_rail && d.pipeline_state_path_rail) {
            lists.rails.push_back(i);
            continue;
        }
        if (chunk.opacity < 0.995f) {
            // A see-through chunk drawn in its own phase was overwritten by everything behind it drawn
            // later (it writes no depth): it vanished or showed the wrong things. After the scene.
            lists.ghost.push_back(i);
            continue;
        }
        if (skip_presurface(chunk)) {
            lists.presurface_opaque.push_back(i);
            lists.presurface_alpha.push_back(i);
            continue;
        }
        // (each pass's list only holds the chunks with batches for it: every pass walked every visible chunk -
        //  9-10k - and the per-chunk call returned at once for most of them)
        if (!chunk.opaque_batches.empty()) lists.surface_opaque.push_back(i);
        if (!chunk.alpha_batches.empty()) {
            lists.surface_alpha.push_back(i);
            if (ground_kind(chunk) != 0) lists.ground_cores.push_back(i);
        }
        if (d.draw_shadow_decals && !chunk.shadow_batches.empty()) lists.shadow.push_back(i);
        if (!chunk.alpha_nozwrite_batches.empty()) lists.alpha_nozwrite.push_back(i);
        if (chunk.split_blend && (!chunk.alpha_batches.empty() || !chunk.alpha_nozwrite_batches.empty()))
            lists.vehicle_cores.push_back(i);
    }
    d.perf_classify_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - classify_t0).count();

    // OMSI TMap.Render order: PreSurfaces → ground → terrain → surface opaque → surface alpha
    // → shadow decals → wireframe → alpha_nozwrite → invert.
    const auto record_t0 = std::chrono::steady_clock::now();
    // (REC_PASS: ms per pass of the record, logged with the draw stats - where the 10 ms go)
    double rec_ms[8]{};
    std::size_t rec_n[8]{};
    auto rec_lap_t = record_t0;
    auto rec_lap = [&](int k, std::size_t n) {
        const auto t = std::chrono::steady_clock::now();
        rec_ms[k] += std::chrono::duration<double, std::milli>(t - rec_lap_t).count();
        rec_n[k] += n;
        rec_lap_t = t;
    };
    draw_list_instanced(lists.presurface_opaque, [](const GpuMeshChunk& c) -> const std::vector<MaterialDrawBatch>& {
        return c.opaque_batches;
    });
    draw_list_instanced(lists.presurface_alpha, [](const GpuMeshChunk& c) -> const std::vector<MaterialDrawBatch>& {
        return c.alpha_batches;
    });
    for (std::size_t i : lists.presurface_alpha)
        draw_chunk_batches(d.mesh_chunks[i], d.mesh_chunks[i].alpha_nozwrite_batches, false);
    if (d.has_ground_plane) draw_chunk_batches(d.ground_chunk, d.ground_chunk.opaque_batches, false);
    for (std::size_t i : lists.terrain) {
        draw_chunk_batches(d.mesh_chunks[i], d.mesh_chunks[i].opaque_batches, false);
        draw_chunk_batches(d.mesh_chunks[i], d.mesh_chunks[i].alpha_batches, false);
    }
    // Cut detail meshes of nearby terrain tiles (their uncut base is drawn behind them as an underlay).
    for (std::size_t i : visible_chunks) {
        const auto& dc = d.mesh_chunks[i];
        if (dc.detail_ready && dc.detail_active && !dc.detail_batches.empty())
            draw_chunk_batches(dc, dc.detail_batches, false, false, false, true);
    }
    rec_lap(0, lists.presurface_opaque.size() + lists.presurface_alpha.size() + lists.terrain.size());
    // Trees + repeated SCOs (same VB after sort) → one DrawIndexedInstanced.
    {
        auto opaque_getter = [](const GpuMeshChunk& c) -> const std::vector<MaterialDrawBatch>& {
            return c.opaque_batches;
        };
        // (the pass's state on a fresh list: what the frame set up before recording)
        auto set_scene_state = [&](ID3D12GraphicsCommandList* cl) {
            cl->SetGraphicsRootSignature(d.root_signature.Get());
            cl->SetDescriptorHeaps(1, heaps);
            cl->RSSetViewports(1, &scene_vp);
            cl->RSSetScissorRects(1, &scene_sc);
            cl->OMSetRenderTargets(1, &rtv_handle, FALSE, &d.dsv_cpu);
            if (d.csm_ready && d.shadow_cb_gpu) {
                cl->SetGraphicsRootConstantBufferView(4, d.shadow_cb_gpu);
                cl->SetGraphicsRootDescriptorTable(5, d.csm_srv_gpu);
            }
            cl->SetGraphicsRoot32BitConstants(2, 45, env_globals_frame, 11);
            cl->SetPipelineState(d.pipeline_state.Get());
            cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        };
        static const bool s_mt_off = std::getenv("OMSI_NO_MT_RECORD") != nullptr; // (A/B test switch)
        const auto& ops = lists.surface_opaque;
        constexpr int kW = HelloO3dRenderer::Impl::kRecWorkers;
        bool parallel = !s_mt_off && ops.size() >= 1200;
        const std::uint32_t fi = frame_index_;
        // worker lists / allocators and the tail list (made once)
        for (int w = 0; parallel && w < kW; ++w) {
            if (!d.rec_alloc[fi][w] &&
                FAILED(d.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                        IID_PPV_ARGS(d.rec_alloc[fi][w].GetAddressOf()))))
                parallel = false;
            if (parallel && !d.rec_list[w]) {
                if (FAILED(d.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, d.rec_alloc[fi][w].Get(),
                                                       nullptr, IID_PPV_ARGS(d.rec_list[w].GetAddressOf()))) ||
                    FAILED(d.rec_list[w]->Close()))
                    parallel = false;
            }
        }
        if (parallel && !d.rec_tail[fi]) {
            if (FAILED(d.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, fr.allocator.Get(), nullptr,
                                                   IID_PPV_ARGS(d.rec_tail[fi].GetAddressOf()))) ||
                FAILED(d.rec_tail[fi]->Close()))
                parallel = false;
        }
        if (!parallel) {
            draw_list_instanced(ops, opaque_getter);
        } else {
            // parts cut where the vertex buffer changes (an instanced group stays whole)
            std::size_t cuts[kW + 1]{};
            cuts[kW] = ops.size();
            for (int w = 1; w < kW; ++w) {
                std::size_t k = (std::max)(cuts[w - 1], ops.size() * static_cast<std::size_t>(w) / kW);
                while (k > 0 && k < ops.size() &&
                       d.mesh_chunks[ops[k]].vertex_buffer_view.BufferLocation ==
                           d.mesh_chunks[ops[k - 1]].vertex_buffer_view.BufferLocation)
                    ++k;
                cuts[w] = k;
            }
            throw_hr(fr.command_list->Close(), "head close");
            std::vector<std::size_t> parts[kW];
            for (int w = 0; w < kW; ++w) parts[w].assign(ops.begin() + static_cast<std::ptrdiff_t>(cuts[w]),
                                                          ops.begin() + static_cast<std::ptrdiff_t>(cuts[w + 1]));
            std::uint32_t pdraws[kW]{}, pbinds[kW]{};
            bool pok[kW]{};
            RenderPool::get().run(
                kW,
                [&](int w) {
                    try {
                        auto* cl = d.rec_list[w].Get();
                        throw_hr(d.rec_alloc[fi][w]->Reset(), "rec alloc reset");
                        throw_hr(cl->Reset(d.rec_alloc[fi][w].Get(), d.pipeline_state.Get()), "rec list reset");
                        set_scene_state(cl);
                        RecCtx ctx{cl, {}};
                        RecCtx* prev = t_rc;
                        t_rc = &ctx;
                        draw_list_instanced(parts[w], opaque_getter);
                        t_rc = prev;
                        throw_hr(cl->Close(), "rec list close");
                        pdraws[w] = ctx.bc.draws;
                        pbinds[w] = ctx.bc.vb_binds;
                        pok[w] = true;
                    } catch (...) {
                    }
                });
            // the tail: the rest of the frame, on the frame's allocator (the head is closed)
            throw_hr(d.rec_tail[fi]->Reset(fr.allocator.Get(), d.pipeline_state.Get()), "tail reset");
            std::swap(fr.command_list, d.rec_tail[fi]); // (fr.command_list: the tail; rec_tail: this frame's head)
            set_scene_state(fr.command_list.Get());
            d.rec_pre.clear();
            d.rec_pre.push_back(d.rec_tail[fi].Get());
            for (int w = 0; w < kW; ++w) {
                if (pok[w]) d.rec_pre.push_back(d.rec_list[w].Get());
                worker_draws += pdraws[w];
                worker_vb_binds += pbinds[w];
            }
            main_rc.cl = fr.command_list.Get();
            main_rc.bc.vb = 0;
            main_rc.bc.ib = 0;
            main_rc.bc.pso = nullptr;
            main_rc.bc.srv_valid = false;
        }
    }
    rec_lap(1, lists.surface_opaque.size());
    // opaque cores of the ground layers' alpha-2 materials, with depth (see ground_kind)
    ground_core_pass = true;
    for (std::size_t i : lists.ground_cores) {
        const auto& c = d.mesh_chunks[i];
        draw_chunk_batches(c, c.alpha_batches, false);
    }
    ground_core_pass = false;
    vehicle_core_pass = true;
    for (std::size_t i : lists.vehicle_cores) {
        const auto& c = d.mesh_chunks[i];
        if (!c.alpha_batches.empty()) draw_chunk_batches(c, c.alpha_batches, false);
        if (!c.alpha_nozwrite_batches.empty()) draw_chunk_batches(c, c.alpha_nozwrite_batches, false);
    }
    vehicle_core_pass = false;
    // [matl_alpha] 2 materials that write depth (Omsi.exe keeps ZWRITEENABLE on unless [matl_noZwrite]) are
    // drawn as Omsi.exe draws a model: mesh by mesh in the order of its file (OMSI SDK: glass meshes go last
    // in model.cfg / .sco so the interior behind them is drawn first). Instanced in VB order, a bus body
    // could be drawn before its interior and hide it with its window depth. Objects far to near.
    rec_lap(2, 0);
    std::vector<std::size_t> zblend_chunks;
    for (std::size_t i : lists.surface_alpha) {
        const auto& c = d.mesh_chunks[i];
        if (ground_kind(c) != 0) continue;
        for (const auto& b : c.alpha_batches)
            if (b.alpha_mode == MatlAlphaMode::alpha_blend) {
                zblend_chunks.push_back(i);
                break;
            }
    }
    ground_split_active = true;
    blend_filter = zblend_chunks.empty() ? 0 : 1;
    draw_list_instanced(lists.surface_alpha, [](const GpuMeshChunk& c) -> const std::vector<MaterialDrawBatch>& {
        return c.alpha_batches;
    });
    blend_filter = 0;
    ground_split_active = false;
    rec_lap(3, lists.surface_alpha.size());
    if (!zblend_chunks.empty()) {
        std::vector<std::pair<float, std::size_t>> keyed;
        keyed.reserve(zblend_chunks.size());
        for (std::size_t i : zblend_chunks) keyed.emplace_back(object_dist2(d.mesh_chunks[i]), i);
        std::sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) {
            if (a.first != b.first) return a.first > b.first;
            return a.second < b.second; // same object: file order
        });
        blend_filter = 2;
        for (const auto& [k, i] : keyed) draw_chunk_batches(d.mesh_chunks[i], d.mesh_chunks[i].alpha_batches, false);
        blend_filter = 0;
    }
    rec_lap(4, zblend_chunks.size());
    if (d.draw_shadow_decals) {
        for (std::size_t i : lists.shadow)
            draw_chunk_batches(d.mesh_chunks[i], d.mesh_chunks[i].shadow_batches, true);
    }
    for (std::size_t i : lists.wireframe) {
        const auto& chunk = d.mesh_chunks[i];
        if (!chunk.opaque_batches.empty())
            draw_chunk_batches(chunk, chunk.opaque_batches, false, false, true);
        if (!chunk.alpha_batches.empty())
            draw_chunk_batches(chunk, chunk.alpha_batches, false, false, true);
    }
    for (std::size_t i : lists.alpha_nozwrite)
        draw_chunk_batches(d.mesh_chunks[i], d.mesh_chunks[i].alpha_nozwrite_batches, false);
    for (std::size_t i : lists.rails) {
        const auto& c = d.mesh_chunks[i];
        if (!c.opaque_batches.empty()) draw_chunk_batches(c, c.opaque_batches, false);
        if (!c.alpha_batches.empty()) draw_chunk_batches(c, c.alpha_batches, false);
        if (!c.alpha_nozwrite_batches.empty()) draw_chunk_batches(c, c.alpha_nozwrite_batches, false);
    }
    if (!lists.ghost.empty()) {
        const float cx = d.camera_world_pos[0], cy = d.camera_world_pos[1], cz = d.camera_world_pos[2];
        auto dist2 = [&](std::size_t i) {
            const auto& c = d.mesh_chunks[i];
            const float* mn = c.aabb_cache_valid ? c.aabb_cache_min : c.meta.bbox_min;
            const float* mx = c.aabb_cache_valid ? c.aabb_cache_max : c.meta.bbox_max;
            const float dx = 0.5f * (mn[0] + mx[0]) - cx, dy = 0.5f * (mn[1] + mx[1]) - cy,
                        dz = 0.5f * (mn[2] + mx[2]) - cz;
            return dx * dx + dy * dy + dz * dz;
        };
        (void)dist2;
        std::sort(lists.ghost.begin(), lists.ghost.end(), [&](std::size_t a, std::size_t b) {
            const float da = object_dist2(d.mesh_chunks[a]), db = object_dist2(d.mesh_chunks[b]);
            if (da != db) return da > db;
            return a < b; // the meshes of one object in the order of its file
        });
        for (std::size_t i : lists.ghost) {
            const auto& c = d.mesh_chunks[i];
            if (!c.opaque_batches.empty()) draw_chunk_batches(c, c.opaque_batches, false);
            if (!c.alpha_batches.empty()) draw_chunk_batches(c, c.alpha_batches, false);
            if (!c.alpha_nozwrite_batches.empty()) draw_chunk_batches(c, c.alpha_nozwrite_batches, false);
        }
    }
    rec_lap(5, lists.shadow.size() + lists.wireframe.size() + lists.alpha_nozwrite.size() + lists.rails.size() +
                   lists.ghost.size());
    for (std::size_t i : lists.invert)
        draw_invert_chunk(d.mesh_chunks[i]);
    // OMSI water tiles (fresnel + animated normals) and weather particles (rain / snow), blended over the
    // opaque scene with the scene depth (reverse-Z, GREATER_EQUAL, no depth write).
    draw_water_pass(d, fr.command_list.Get(), frame_index_, static_cast<float>(scene_vp.Width),
                    static_cast<float>(scene_vp.Height));
    draw_precip_pass(d, fr.command_list.Get(), frame_index_, static_cast<float>(scene_vp.Width),
                     static_cast<float>(scene_vp.Height));
    draw_corona_pass(d, fr.command_list.Get(), frame_index_, static_cast<float>(scene_vp.Width),
                     static_cast<float>(scene_vp.Height));
    // Volumetric light shafts: raymarch the CSM from the camera (additive, after the scene).
    draw_shaft_pass(d, fr.command_list.Get(), frame_index_, static_cast<float>(scene_vp.Width),
                    static_cast<float>(scene_vp.Height));

    rec_lap(6, 0);
    d.perf_record_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - record_t0).count();
    {
        static double s_ms[8]{};
        static std::size_t s_n[8]{};
        static int s_frames = 0;
        for (int k = 0; k < 8; ++k) {
            s_ms[k] += rec_ms[k];
            s_n[k] += rec_n[k];
        }
        if (++s_frames >= 300) {
            char b[400];
            std::snprintf(b, sizeof(b),
                          "REC_PASS avg_ms pre+terrain=%.2f(%zu) opaque=%.2f(%zu) cores=%.2f alpha=%.2f(%zu) zblend=%.2f(%zu) "
                          "decal/rails/ghost=%.2f(%zu) water/fx=%.2f",
                          s_ms[0] / s_frames, s_n[0] / s_frames, s_ms[1] / s_frames, s_n[1] / s_frames, s_ms[2] / s_frames,
                          s_ms[3] / s_frames, s_n[3] / s_frames, s_ms[4] / s_frames, s_n[4] / s_frames, s_ms[5] / s_frames,
                          s_n[5] / s_frames, s_ms[6] / s_frames);
            omsi::log::info(b);
            for (int k = 0; k < 8; ++k) {
                s_ms[k] = 0.0;
                s_n[k] = 0;
            }
            s_frames = 0;
        }
    }
    d.perf_draws = bind_cache.draws + worker_draws;
    d.perf_vb_binds = bind_cache.vb_binds + worker_vb_binds;
    d.perf_instance_slots = d.instance_write_cursor;

    if (d.axis_vertex_count > 0 && d.axis_pipeline_state && d.axis_vertex_buffer) {
        const float gizmo_size = axis_gizmo_viewport_size(static_cast<std::uint32_t>(scene_vp.Width),
                                                          static_cast<std::uint32_t>(scene_vp.Height));
        const float gizmo_margin = axis_gizmo_margin(static_cast<std::uint32_t>(scene_vp.Width),
                                                     static_cast<std::uint32_t>(scene_vp.Height));
        const float gizmo_x = (std::max)(scene_vp.TopLeftX, scene_vp.TopLeftX + scene_vp.Width - gizmo_size - gizmo_margin);
        D3D12_VIEWPORT gizmo_vp{};
        gizmo_vp.TopLeftX = gizmo_x;
        gizmo_vp.TopLeftY = gizmo_margin;
        gizmo_vp.Width = gizmo_size;
        gizmo_vp.Height = gizmo_size;
        gizmo_vp.MinDepth = 0.f;
        gizmo_vp.MaxDepth = 1.f;

        const D3D12_RECT gizmo_scissor = {
            static_cast<LONG>(gizmo_x),
            static_cast<LONG>(gizmo_margin),
            static_cast<LONG>(gizmo_x + gizmo_size),
            static_cast<LONG>(gizmo_margin + gizmo_size),
        };

        fr.command_list->RSSetViewports(1, &gizmo_vp);
        fr.command_list->RSSetScissorRects(1, &gizmo_scissor);
        fr.command_list->SetPipelineState(d.axis_pipeline_state.Get());
        fr.command_list->SetGraphicsRootSignature(d.axis_root_signature.Get());
        fr.command_list->SetGraphicsRootDescriptorTable(0, d.axis_cbv_gpu);
        fr.command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
        fr.command_list->IASetVertexBuffers(0, 1, &d.axis_vertex_buffer_view);
        fr.command_list->DrawInstanced(d.axis_vertex_count, 1, 0, 0);
    }

    fr.command_list->SetGraphicsRootSignature(d.root_signature.Get());
    if (d.timestamp_query_heap) {
        const UINT q1 = frame_index_ * HelloO3dRenderer::kGpuStampCount + 1u;
        fr.command_list->EndQuery(d.timestamp_query_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, q1);
    }
    // Flip swapchain is 1×: resolve MSAA → scene (SMAA 1x) or backbuffer, then UI on backbuffer.
    if (use_msaa) {
        ID3D12Resource* resolve_dest =
            use_scene_rt ? d.scene_color[frame_index_].Get() : d.render_targets[frame_index_].Get();
        record_msaa_resolve(fr.command_list.Get(), resolve_dest);
        if (use_scene_rt) {
            record_fxaa_pass(fr.command_list.Get(), frame_index_);
        } else {
            barrier = dx12_transition_barrier(d.render_targets[frame_index_].Get(),
                                              D3D12_RESOURCE_STATE_RESOLVE_DEST,
                                              D3D12_RESOURCE_STATE_RENDER_TARGET);
            fr.command_list->ResourceBarrier(1, &barrier);
            D3D12_CPU_DESCRIPTOR_HANDLE back_rtv =
                offset_cpu_handle(d.rtv_heap->GetCPUDescriptorHandleForHeapStart(), rtv_descriptor_size_,
                                  frame_index_);
            fr.command_list->OMSetRenderTargets(1, &back_rtv, FALSE, nullptr);
            fr.command_list->RSSetViewports(1, &scene_vp);
            fr.command_list->RSSetScissorRects(1, &scene_sc);
        }
    } else if (use_scene_rt) {
        record_fxaa_pass(fr.command_list.Get(), frame_index_);
    }

    // Scene cache: keep the finished 3D image (before the UI) for the frames that do not change.
    if (save_cache) {
        ID3D12Resource* bb = d.render_targets[frame_index_].Get();
        if (!d.scene_cache || d.scene_cache_w != static_cast<UINT>(bb_desc.Width) || d.scene_cache_h != bb_desc.Height) {
            d.scene_cache.Reset();
            D3D12_RESOURCE_DESC cd = bb_desc;
            cd.Flags = D3D12_RESOURCE_FLAG_NONE;
            const CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);
            if (SUCCEEDED(d.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &cd,
                                                            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                            IID_PPV_ARGS(d.scene_cache.GetAddressOf())))) {
                d.scene_cache_state = D3D12_RESOURCE_STATE_COPY_DEST;
                d.scene_cache_w = static_cast<UINT>(bb_desc.Width);
                d.scene_cache_h = bb_desc.Height;
            }
        }
        if (d.scene_cache) {
            D3D12_RESOURCE_BARRIER b[2]{};
            UINT nb = 0;
            b[nb++] = dx12_transition_barrier(bb, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
            if (d.scene_cache_state != D3D12_RESOURCE_STATE_COPY_DEST)
                b[nb++] = dx12_transition_barrier(d.scene_cache.Get(), d.scene_cache_state, D3D12_RESOURCE_STATE_COPY_DEST);
            fr.command_list->ResourceBarrier(nb, b);
            fr.command_list->CopyResource(d.scene_cache.Get(), bb);
            b[0] = dx12_transition_barrier(bb, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            b[1] = dx12_transition_barrier(d.scene_cache.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
            fr.command_list->ResourceBarrier(2, b);
            d.scene_cache_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
            d.scene_cache_valid = true;
            d.scene_cache_epoch = frame_epoch;
        }
    }

    if (ultralight_overlay_) ultralight_overlay_->upload_texture(fr.command_list.Get());
    if (imgui_overlay_) imgui_overlay_->render_dx12(fr.command_list.Get());

    if (d.timestamp_query_heap && fr.timestamp_readback) {
        const UINT q0 = frame_index_ * HelloO3dRenderer::kGpuStampCount;
        fr.command_list->EndQuery(d.timestamp_query_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, q0 + 2u);
        fr.command_list->ResolveQueryData(d.timestamp_query_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, q0,
                                          HelloO3dRenderer::kGpuStampCount, fr.timestamp_readback.Get(), 0);
    }

    csm_end_frame_barrier(d, fr.command_list.Get());

    barrier = dx12_transition_barrier(d.render_targets[frame_index_].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                                      D3D12_RESOURCE_STATE_PRESENT);
    fr.command_list->ResourceBarrier(1, &barrier);
    static std::uint32_t s_draw_log_frames = 0;
    if ((++s_draw_log_frames % 300u) == 1u && bind_cache.draws > 0) {
        omsi::log::info("draw stats draws=" + std::to_string(bind_cache.draws) +
                        " vb_binds=" + std::to_string(bind_cache.vb_binds) +
                        " visible_chunks=" + std::to_string(d.perf_chunks_visible) +
                        " tested=" + std::to_string(d.perf_chunks_tested) +
                        " inst_used=" + std::to_string(d.instance_write_cursor) + "/" +
                        std::to_string(d.perf_instance_cap) +
                        " inst_ovf=" + std::to_string(d.perf_instance_overflow) +
                        " cb_used=" + std::to_string(d.perf_cb_slots_used) + "/" +
                        std::to_string(d.perf_cb_slots_cap) +
                        " shadows=" + std::string((d.csm_ready && d.sun_shadow_enable > 0.5f) ? "on" : "off") +
                        " culled=" + std::to_string(d.perf_chunks_culled) +
                        " cull_ms=" + std::to_string(d.perf_cull_ms) +
                        " record_ms=" + std::to_string(d.perf_record_ms) +
                        " sortcls_ms=" + std::to_string(d.perf_sort_ms + d.perf_classify_ms) +
                        " (split: cull vs record vs sort)");
    }
    // the shadow list recorded on the worker: wait for it (it ran alongside all of the above)
    double csm_wait_ms = 0.0;
    int csm_inline = 0;
    if (d.csm_job) {
        const auto csm_w0 = std::chrono::steady_clock::now();
        csm_inline = d.csm_job->load(std::memory_order_acquire) == 0 ? 1 : 0;
        // not started yet (every pool thread busy - streaming reads off a slow disk held it 0.5 s): record it here
        if (d.csm_job->load(std::memory_order_acquire) == 0 && d.csm_job_body) d.csm_job_body();
        while (d.csm_job->load(std::memory_order_acquire) == 3) std::this_thread::yield();
        csm_wait_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - csm_w0).count();
        d.csm_job_body = nullptr;
        d.csm_list_this_frame = d.csm_job->load(std::memory_order_acquire) == 1;
        if (!d.csm_list_this_frame) {
            // (it failed: the shadow map still goes to the pixel-shader state the frame-end barrier expects)
            const D3D12_RESOURCE_BARRIER b = dx12_transition_barrier(
                d.csm_depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            fr.command_list->ResourceBarrier(1, &b);
        }
        d.csm_job.reset();
    }
    throw_hr(fr.command_list->Close(), "cmd close");
    const double pop_total =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - pop_t0).count();
    if (pop_total > 20.0) {
        const double setup = pop_total - d.perf_cull_ms - d.perf_sort_ms - d.perf_classify_ms - d.perf_record_ms;
        omsi::log::warn("POPULATE_SLOW total=" + std::to_string(pop_total) + " setup=" + std::to_string(setup) +
                        " cull=" + std::to_string(d.perf_cull_ms) + " sortcls=" +
                        std::to_string(d.perf_sort_ms + d.perf_classify_ms) + " record=" +
                        std::to_string(d.perf_record_ms) + " visible=" + std::to_string(d.perf_chunks_visible) +
                        " chunks=" + std::to_string(d.mesh_chunks.size()) + " csm_wait=" +
                        std::to_string(csm_wait_ms) + " csm_inline=" + std::to_string(csm_inline));
    }
}

} // namespace omsi::gfx::dx12
