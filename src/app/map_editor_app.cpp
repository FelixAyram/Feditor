#include "omsi/fs_cow.hpp"
#include "omsi/disk_stats.hpp"
#include "omsi/path_utf8.hpp"
#include "omsi/vfs_overlay.hpp"
#include "beamng/bng_assets.hpp"
#include "beamng/bng_roads.hpp"
#include "beamng/bng_session.hpp"
#include "gfx/dx12/dx12_texture.hpp"
#include "map_editor_app.hpp"
#include "app/map_editor_math.hpp"

#include "app/map_editor/editor_types.hpp"
#include "ui/i18n.hpp"

#include "editor/editor_ground.hpp"
#include "editor/terrain_polycut.hpp"
#include "editor/terrain_restamp.hpp"
#include "editor/editor_tree_billboard.hpp"
#include "gfx/dx12/dx12_texture.hpp"
#include "gfx/dx12/hello_o3d_renderer.hpp"
#include "input/luna_camera.hpp"
#include "omsi/cfg_document.hpp"
#include "omsi/logger.hpp"
#include "omsi/utf8_win32.hpp"
#include "app/seh_guard.hpp"
#include "app/crash_report.hpp"
#include "omsi/editor_version.hpp"
#include "omsi/coop_session.hpp"
#include <map>
#include <set>
#include "omsi/map_packager.hpp"
#include "omsi/updater.hpp"
#include "omsi/editor_version.hpp"
#include <thread>
#include "omsi/fps_detector.hpp"
#include "omsi/frame_budget.hpp"
#include "omsi/load_governor.hpp"
#include "omsi/visual_quality.hpp"
#include "omsi/editor_sfx.hpp"
#include "omsi/sound/world_sound.hpp"
#include "omsi/sound/world_sound_scene.hpp"
#include "omsi/sound/world_sound_window.hpp"
#include "omsi/editor_place_fx.hpp"
#include "omsi/hardware_profile.hpp"
#include "omsi/job_system.hpp"
#include "omsi/main_thread_watchdog.hpp"
#include "omsi/o3d_prepared_cache.hpp"
#include "omsi/editor_transform_soa.hpp"
#include "omsi/frustum.hpp"
#include "omsi/win32_shell_open.hpp"
#include "omsi/editor_session_log.hpp"
#include "omsi/editor_command.hpp"
#include "omsi/editor_fixpack.hpp"
#include "omsi/map_tile_layout.hpp"
#include "omsi/map_tile_editor.hpp"
#include "omsi/map_geo_tools.hpp"
#include "omsi/map_attachment.hpp"
#include "omsi/o3d_format.hpp"
#include "o3d/o3d_loader.hpp"
#include "omsi/sco_types.hpp"
#include "omsi/crossing_height_deform.hpp"
#include "omsi/model_cfg.hpp"
#include "omsi/spline_types.hpp"
#include "omsi/world_load_job.hpp"
#include "omsi/x_mesh.hpp"
#include "ui/editor_side_panel.hpp"
#include "ui/editor_prop_dialogs.hpp"
#include "ui/spline_creator_dialog.hpp"
#include "app/spline_creator_app.hpp"
#include "ui/editor_adjust_hud.hpp"
#include "ui/editor_map_text_editor.hpp"
#include "ui/imgui_overlay.hpp"
#include "ui/map_editor_ultralight_panel.hpp"
#include "ui/ultralight_overlay.hpp"
#include "ui/map_editor_imgui_panel.hpp"
#include "ui/editor_shortcuts.hpp"
#include "ui/launcher.hpp"
#include "omsi/editor_map_workspace.hpp"
#include "omsi/map_scenery_script.hpp"
#include "omsi/model/mesh_animator.hpp"
#include "omsi/editor_omsi_tools.hpp"
#include "omsi/tt_loader.hpp"
#include "omsi/tt_man.hpp"
#include "omsi/tt_editor_ui.hpp"

#include "omsi/editor_sli_bake.hpp"
#include "omsi/editor_tile_bake.hpp"
#include "omsi/o3d_writer.hpp"
#include "omsi/bus_catalog.hpp"
#include "omsi/bus.hpp"
#include "omsi/cti.hpp"
#include "omsi/model_cfg.hpp"
#include "omsi/model/mesh_animator.hpp"
#include "omsi/script/vehicle_runtime.hpp"
#include "omsi/vehicle/bus_body_physics.hpp"
#include "omsi/vehicle/oo_vehicle.h"
#include <queue>
#include <random>
#include "omsi/sound/app_session_volume.hpp"
#include "omsi/vehicle_input_bindings.hpp"
#include "o3d/o3d_loader.hpp"
#include "omsi/world_height.hpp"
#include "omsi/omsi_sky.hpp"
#include "omsi/omsi_wind.hpp"
#include "omsi/texture_season.hpp"

#include <imgui.h>
#include <imgui_internal.h>
#include <d3d12.h>

#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

#include <DirectXMath.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#include <psapi.h>
#pragma comment(lib, "Psapi.lib")
#include <shellapi.h>
#include <commdlg.h>
#include <shobjidl.h>
#include <wincodec.h>
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "winmm.lib")

#include <array>
#include <cwctype>
#include <fstream>
#include <algorithm>
#include <execution>
#include <atomic>
#include <thread>
#include <future>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cassert>
#include <cstdio>
#include <ctime>
#include <cstring>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <deque>
#include <limits>
#include <random>
#include <queue>
#include <optional>
#include <functional>
#include <queue>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace omsi::app {

// TT / station-link editor modes (referenced via extern from run_loop_*.inc.cpp).
int g_omsi_tt_mode = 0; // 0=Add Trackentry 1=Add Station 2=Show TT
int g_omsi_sl_mode = 0; // 0=SetStart 1=SetEnd 2=AddPathSeg

namespace {
using namespace DirectX;
using namespace map_editor_math;

// Bridge profiler spikes → main_hang.log (set in map editor loop).
inline omsi::MainThreadWatchdog* g_main_watchdog = nullptr;

struct MapEditorFrameProfiler {
    using Clock = std::chrono::steady_clock;
    enum Section : std::size_t {
        UndoJobs = 0,
        ScriptTick,
        ScoVisibility,
        VisualRadius,
        PathRails,
        DistanceCull,
        UiPumpPoll,   // glfwPollEvents (+ light host)
        UiPumpPanel,  // panel / text editor / adjust HUD
        StreamApply,
        TerrainPrecache,
        SceneryAttachSpawn,
        BusTick,     // player bus (openOMSI step + ground / obstacles)
        AiTraffic,   // AI traffic (openOMSI step, lanes, car meshes)
        ParkedCars,  // parked cars
        EditorLogic,
        ImGuiBuild,
        SplineHandles,
        HudBuild,
        Render,
        PostRender,
        SectionCount
    };

    struct Scope {
        MapEditorFrameProfiler* owner = nullptr;
        Section section{};
        Clock::time_point begin{};
        ~Scope() {
            if (!owner) return;
            owner->samples_ms[section] += std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
        }
    };

    FpsDetector fps;
    std::ofstream file;
    std::array<double, SectionCount> samples_ms{};
    std::array<double, SectionCount> max_ms{};
    std::array<double, 120> total_history{};
    std::array<std::array<double, 120>, SectionCount> section_history{};
    std::size_t history_count = 0;
    std::size_t history_cursor = 0;
    std::uint64_t frame = 0;
    Clock::time_point frame_begin{};
    bool active = false;
    bool segment_active = false;
    Clock::time_point segment_begin{};
    std::filesystem::path path;
    // Hitch histogram: SPIKING = active_ms over budget; THROTTLED = present/vsync bound.
    std::uint64_t hist_ok = 0;
    std::uint64_t hist_spiking = 0;
    std::uint64_t hist_throttled = 0;

    static const char* name(Section s) {
        switch (s) {
        case UndoJobs: return "undo_jobs";
        case ScriptTick: return "scripts";
        case ScoVisibility: return "sco_visibility";
        case VisualRadius: return "visual_radius";
        case PathRails: return "path_rails";
        case DistanceCull: return "distance_cull";
        case UiPumpPoll: return "ui_pump_poll";
        case UiPumpPanel: return "ui_pump_panel";
        case StreamApply: return "stream_apply";
        case TerrainPrecache: return "terrain_precache";
        case SceneryAttachSpawn: return "sa_spawn";
        case BusTick: return "bus_tick";
        case AiTraffic: return "ai_traffic";
        case ParkedCars: return "parked_cars";
        case EditorLogic: return "editor_logic";
        case ImGuiBuild: return "imgui";
        case SplineHandles: return "spline_handles";
        case HudBuild: return "hud_build";
        case Render: return "render_cpu";
        case PostRender: return "post_render";
        default: return "unknown";
        }
    }

    static std::filesystem::path default_path() {
#ifdef _WIN32
        std::array<wchar_t, 32768> buf{};
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n > 0 && n < buf.size()) {
            std::filesystem::path exe(std::wstring(buf.data(), n));
            const auto dir = exe.parent_path();
            if (!dir.empty()) return dir / "frame_profile.log";
        }
#endif
        std::error_code ec;
        const auto cwd = std::filesystem::current_path(ec);
        return (ec ? std::filesystem::path(".") : cwd) / "frame_profile.log";
    }

    void open() {
        path = default_path();
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        file.open(path, std::ios::out | std::ios::trunc);
        if (file.is_open()) {
            file << "Feditor frame profiler\n";
            file << "version=motorv68\n";
            file << "file=" << path.string() << "\n";
            file << "format=MAPPERF60; averages/maxima over 60 frames; active_ms=total-present\n";
            file << "hitch_tags=SPIKING|THROTTLED|LOAD_SLOW create|upload|io|apply\n";
            file.flush();
        }
        active = file.is_open();
    }

    Scope scope(Section s) { return Scope{this, s, Clock::now()}; }

    void begin_frame() {
        if (!active) return;
        frame_begin = Clock::now();
        samples_ms.fill(0.0);
        segment_active = false;
    }

    void begin_segment() {
        if (!active) return;
        segment_begin = Clock::now();
        segment_active = true;
    }

    void end_segment(Section s) {
        if (!active || !segment_active) return;
        const double elapsed = std::chrono::duration<double, std::milli>(Clock::now() - segment_begin).count();
        if (s == EditorLogic) {
            const double nested = samples_ms[StreamApply] + samples_ms[TerrainPrecache] +
                                  samples_ms[SceneryAttachSpawn] + samples_ms[BusTick] + samples_ms[AiTraffic] +
                                  samples_ms[ParkedCars];
            samples_ms[s] += (std::max)(0.0, elapsed - nested);
        } else if (s == ImGuiBuild) {
            samples_ms[s] += (std::max)(0.0, elapsed - samples_ms[SplineHandles] - samples_ms[HudBuild]);
        } else {
            samples_ms[s] += elapsed;
        }
        segment_active = false;
    }

    void end_frame(std::size_t edit_objects, std::size_t scene_places, std::size_t spline_gpu,
                   std::size_t gpu_chunks, std::size_t tiles, const gfx::dx12::HelloO3dRenderer::GpuTiming& gpu) {
        if (!active) return;
        const double total = std::chrono::duration<double, std::milli>(Clock::now() - frame_begin).count();
        fps.push(total, gpu.gpu_total_ms, gpu.gpu_scene_ms, gpu.gpu_ui_ms, gpu.cpu_wait_fence_ms,
                 gpu.cpu_present_ms, gpu.cpu_populate_ms);
        // Present/vsync is not a stream hitch — active work excludes present wait.
        const double active_ms = (std::max)(0.0, total - gpu.cpu_present_ms);
        const auto idx = history_cursor;
        total_history[idx] = total;
        for (std::size_t i = 0; i < SectionCount; ++i) {
            section_history[i][idx] = samples_ms[i];
            max_ms[i] = (std::max)(max_ms[i], samples_ms[i]);
        }
        history_cursor = (history_cursor + 1) % total_history.size();
        history_count = (std::min)(history_count + 1, total_history.size());
        ++frame;

        const char* bound = fps.bound();
        const bool present_bound = std::strcmp(bound, "present_vsync") == 0;
        if (active_ms >= 16.67 && !present_bound)
            ++hist_spiking;
        else if (present_bound && total >= 16.67)
            ++hist_throttled;
        else
            ++hist_ok;

        std::array<std::pair<double, std::size_t>, SectionCount> rank{};
        for (std::size_t i = 0; i < SectionCount; ++i) rank[i] = {samples_ms[i], i};
        std::sort(rank.begin(), rank.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });

        if (g_main_watchdog && (active_ms >= 16.67 || total >= 22.0)) {
            g_main_watchdog->note_budget(
                frame, total, active_ms, gpu.cpu_present_ms, bound,
                name(static_cast<Section>(rank[0].second)), rank[0].first,
                name(static_cast<Section>(rank[1].second)), rank[1].first,
                name(static_cast<Section>(rank[2].second)), rank[2].first);
        }

        if (total >= 16.67 || active_ms >= 16.67) {
            const char* hitch =
                present_bound ? "THROTTLED" : (active_ms >= 16.67 ? "SPIKING" : "OK");
            char spike[768]{};
            std::snprintf(
                spike, sizeof(spike),
                "MAPPERF_SPIKE hitch=%s frame=%llu total_ms=%.2f active_ms=%.2f gpu_ms=%.2f "
                "gpu_scene=%.2f gpu_ui=%.2f wait_fence=%.2f present=%.2f populate=%.2f bound=%s "
                "top1=%s:%.2f top2=%s:%.2f top3=%s:%.2f",
                hitch, static_cast<unsigned long long>(frame), total, active_ms, gpu.gpu_total_ms,
                gpu.gpu_scene_ms, gpu.gpu_ui_ms, gpu.cpu_wait_fence_ms, gpu.cpu_present_ms,
                gpu.cpu_populate_ms, bound, name(static_cast<Section>(rank[0].second)), rank[0].first,
                name(static_cast<Section>(rank[1].second)), rank[1].first,
                name(static_cast<Section>(rank[2].second)), rank[2].first);
            file << spike << '\n';
            if (g_main_watchdog) g_main_watchdog->note_line(spike);
            // Slow CPUs spike every frame: at most 2 spike lines/s in logfile.log.
            static auto s_spike_log_t = Clock::time_point{};
            if (active_ms >= 22.0 && Clock::now() - s_spike_log_t >= std::chrono::milliseconds(500)) {
                s_spike_log_t = Clock::now();
                log::info(spike);
            }
        }
        // editor_logic catch-all: surface which nested scopes ate the frame when >50 ms.
        if (samples_ms[EditorLogic] > 50.0) {
            char el[384]{};
            std::snprintf(el, sizeof(el),
                          "EDITOR_LOGIC_SPIKE frame=%llu editor_logic=%.2f path_rails=%.2f "
                          "visual_radius=%.2f stream_apply=%.2f scripts=%.2f ui_pump_poll=%.2f "
                          "sa_spawn=%.2f bus_tick=%.2f ai_traffic=%.2f parked_cars=%.2f",
                          static_cast<unsigned long long>(frame), samples_ms[EditorLogic],
                          samples_ms[PathRails], samples_ms[VisualRadius], samples_ms[StreamApply],
                          samples_ms[ScriptTick], samples_ms[UiPumpPoll], samples_ms[SceneryAttachSpawn],
                          samples_ms[BusTick], samples_ms[AiTraffic], samples_ms[ParkedCars]);
            file << el << '\n';
            if (g_main_watchdog) g_main_watchdog->note_line(el);
            static auto s_el_log_t = Clock::time_point{};
            if (Clock::now() - s_el_log_t >= std::chrono::milliseconds(500)) {
                s_el_log_t = Clock::now();
                log::warn(el);
            }
        }

        {
            static bool s_main_set = false;
            if (!s_main_set) {
                s_main_set = true;
                omsi::disk_stats::set_main_thread();
            }
            if (const auto disk = omsi::disk_stats::maybe_log(5.0); !disk.empty()) {
                file << disk << '\n';
                log::info(disk);
            }
        }
        if ((frame % 60u) != 0u) return;

        auto average = [&](std::size_t section) {
            double sum = 0.0;
            for (std::size_t i = 0; i < history_count; ++i) sum += section_history[section][i];
            return sum / static_cast<double>(history_count);
        };
        double total_sum = 0.0;
        double total_max = 0.0;
        for (std::size_t i = 0; i < history_count; ++i) {
            total_sum += total_history[i];
            total_max = (std::max)(total_max, total_history[i]);
        }
        const double total_avg = history_count ? total_sum / static_cast<double>(history_count) : 0.0;
        const double real_fps = total_avg > 0.001 ? 1000.0 / total_avg : 0.0;

        file << "MAPPERF60 frame=" << frame << " real_fps=" << real_fps
             << " avg_total_ms=" << total_avg << " max_total_ms=" << total_max
             << " objects=" << edit_objects << " scene_places=" << scene_places
             << " spline_gpu=" << spline_gpu << " gpu_chunks=" << gpu_chunks << " tiles=" << tiles;
        double measured_sum = 0.0;
        std::array<std::pair<double, std::size_t>, SectionCount> avg_rank{};
        for (std::size_t i = 0; i < SectionCount; ++i) {
            const double avg = average(i);
            measured_sum += avg;
            avg_rank[i] = {avg, i};
            file << ' ' << name(static_cast<Section>(i)) << "_avg=" << avg
                 << ' ' << name(static_cast<Section>(i)) << "_max=" << max_ms[i];
            max_ms[i] = 0.0;
        }
        std::sort(avg_rank.begin(), avg_rank.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });
        file << " unaccounted_ms=" << (total_avg - measured_sum)
             << " hottest=" << name(static_cast<Section>(avg_rank[0].second)) << ":" << avg_rank[0].first
             << " fps_ewma=" << fps.smoothed_fps() << " fps_1pct=" << fps.fps_1pct_low()
             << " gpu_avg=" << fps.avg_gpu_ms() << " bound=" << fps.bound()
             << " hist_ok=" << hist_ok << " hist_SPIKING=" << hist_spiking
             << " hist_THROTTLED=" << hist_throttled << '\n';
        file.flush();
        {
            char hist[192]{};
            std::snprintf(hist, sizeof(hist),
                          "HITCH_HIST frame=%llu ok=%llu SPIKING=%llu THROTTLED=%llu",
                          static_cast<unsigned long long>(frame),
                          static_cast<unsigned long long>(hist_ok),
                          static_cast<unsigned long long>(hist_spiking),
                          static_cast<unsigned long long>(hist_throttled));
            if (g_main_watchdog) g_main_watchdog->note_line(hist);
        }
        hist_ok = hist_spiking = hist_throttled = 0;
        history_count = 0;
        history_cursor = 0;
    }

    ~MapEditorFrameProfiler() {
        if (file.is_open()) {
            file.flush();
            file.close();
        }
    }
};

// Boot + hitch log: MAINLOAD is main-thread setup (tile parse wait is labeled separately).
// MAPPERF_SPIKE is a single editor frame that blew the 16.7 ms budget.
struct MainLoadTrace {
    using Clock = std::chrono::steady_clock;
    struct Hit {
        const char* name = "";
        double ms = 0.0;
    };
    std::vector<Hit> hits;
    Clock::time_point t0{};
    Clock::time_point last{};
    std::ofstream* file = nullptr;
    bool started = false;

    void start(std::ofstream* f) {
        file = f;
        t0 = last = Clock::now();
        started = true;
        line("MAINLOAD begin (phases = work on the editor thread)");
    }

    void mark(const char* name) {
        if (!started) return;
        const auto now = Clock::now();
        const double ms = std::chrono::duration<double, std::milli>(now - last).count();
        last = now;
        hits.push_back({name, ms});
        char buf[256]{};
        std::snprintf(buf, sizeof(buf), "MAINLOAD phase=%s ms=%.2f", name, ms);
        line(buf);
    }

    void finish() {
        if (!started) return;
        const double total = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        auto ranked = hits;
        std::sort(ranked.begin(), ranked.end(), [](const Hit& a, const Hit& b) { return a.ms > b.ms; });
        char head[192]{};
        std::snprintf(head, sizeof(head), "MAINLOAD_TOP total_ms=%.1f ranked=", total);
        std::string msg = head;
        const std::size_t n = (std::min)(ranked.size(), static_cast<std::size_t>(8));
        for (std::size_t i = 0; i < n; ++i) {
            char bit[96]{};
            std::snprintf(bit, sizeof(bit), "%s:%.1f%s", ranked[i].name, ranked[i].ms, i + 1 < n ? " " : "");
            msg += bit;
        }
        line(msg.c_str());
        started = false;
    }

    void line(const char* s) {
        log::info(s);
        if (file && file->is_open()) {
            *file << s << '\n';
            file->flush();
        }
    }
};

#include "detail/map_editor_primitives.inc.cpp"
} // namespace

world::LoadOptions make_editor_load_opts(const options::OmsiOptions& opts,
                                         const std::filesystem::path& omsi_root,
                                         const std::int32_t tile_radius) {
    world::LoadOptions load{};
    load.omsi_root = omsi_root;
    // Editor UI owns the ring: Chebyshev tile_radius only (OMSI Neighbour=1 → R=1 → 9 tiles).
    // Never override with HardwareProfile metre windows — those ignored tile_radius in tiles_near_entry.
    load.tile_radius = tile_radius < 1 ? 1 : (tile_radius > 20 ? 20 : tile_radius);
    load.load_radius_m = 0.f;
    load.unload_radius_m = 0.f;
    load.max_object_dist = 300.f;
    load.load_splines = true;
    load.load_terrain = false;
    (void)opts;
    return load;
}

int run_map_editor_app(MapEditorConfig cfg, world::World world) {
    using namespace DirectX;
    using namespace map_editor_math;
    using map_editor::AutofillAttach;
    using map_editor::EditMode;
    using map_editor::EndMarker;
    using map_editor::MarqueeIntent;
    using map_editor::MarqueePhase;
    using map_editor::PathEndpointPick;
    using map_editor::PathRail;
    std::function<void(map::MapObjectInst&, bool lock_z)> apply_object_surface_follow_fn;
    bool spawn_align_pitch_bank = false;
    MapEditorFrameProfiler frame_profiler{};
    frame_profiler.open();
    omsi::LoadGovernor::global().target_frame_ms = 1000.0 / 60.0;
    log::info("frame profiler log: " + frame_profiler.path.string());
    MainLoadTrace boot_trace;
    boot_trace.start(frame_profiler.file.is_open() ? &frame_profiler.file : nullptr);
#include "detail/map_editor_run_setup_01.inc.cpp"
#include "detail/map_editor_run_setup_02.inc.cpp"
#include "detail/map_editor_run_setup_03.inc.cpp"
#include "detail/map_editor_run_setup_04.inc.cpp"
#include "detail/map_editor_run_setup_05.inc.cpp"
#include "detail/map_editor_run_setup_06.inc.cpp"
#include "detail/map_editor_run_loop_01.inc.cpp"
#include "detail/map_editor_run_loop_02.inc.cpp"
#include "detail/map_editor_run_loop_03.inc.cpp"
#include "detail/map_editor_run_loop_04.inc.cpp"
}

} // namespace omsi::app
