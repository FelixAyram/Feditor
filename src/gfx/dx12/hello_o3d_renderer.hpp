#pragma once

#include <cstdint>
#include <filesystem>

#include "f4f23ma/math.hpp"
#include "omsi/model_cfg_types.hpp"
#include "omsi/o3d_format.hpp"
#include "omsi/visible_runtime.hpp"

#include <cstddef>
#include <array>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace omsi::o3d {
class MappedO3D;
}

struct HWND__;
struct ID3D12Device;
struct ID3D12CommandQueue;
struct ID3D12GraphicsCommandList;

namespace omsi::ui {
class ImGuiOverlay;
class UltralightOverlay;
}

namespace omsi::gfx::dx12 {

struct MaterialDrawBatch;
struct GpuMeshChunk;

struct LoadedMeshChunkInfo {
    std::string mesh_file;
    std::optional<std::string> mouseevent_var;
    std::optional<std::string> visible_var;
    int visible_required = 0;
    bool visible = true;
    float bbox_min[3]{};
    float bbox_max[3]{};
};

// OpenOMSI omsi-render RenderPhase::DRAW_ORDER.
enum class WorldRenderPhase : std::uint8_t {
    PreSurface = 0,
    Terrain = 1,
    Surface = 2,
    Spline = 3,
    OnSurface = 4,
    BeforeNormal = 5,
    Normal = 6,
    Layer1 = 7,
    Layer3 = 8,
    Layer4 = 9,
};

/// One material draw inside a merged procedural chunk (splines: N strips → 1 VB/IB).
struct ProcMeshSubrange {
    std::uint32_t index_start = 0;
    std::uint32_t index_count = 0;
    std::filesystem::path texture_path;
    std::uint8_t alpha_mode = 0;
    int text_texture_index = -1;
    // BeamNG materials (optional): extra maps + detail tiling; solid colour when there is no albedo map.
    std::filesystem::path normal_path;
    std::filesystem::path detail_path;
    std::filesystem::path detail_normal_path;
    float detail_scale = 0.f;
    float detail_scale_y = 0.f; // 0 = same as detail_scale
    bool has_solid = false;
    float solid_rgba[4]{1.f, 1.f, 1.f, 1.f};
    // Opacity-mask material (tyre marks / decals): t0 = mask texture, t1 = 1x1 solid tint, alpha = opacityFactor.
    // With a colorMap present, the mask is bound to t7 and multiplies the colour alpha instead.
    std::filesystem::path mask_path;
    float mask_tint[4]{1.f, 1.f, 1.f, 1.f};
    // Alias of mask_path (same value) for callers that fill it by another name.
    std::filesystem::path mask;
    // BeamNG stage0 params, same values as mask_tint rgb/a but named explicitly:
    // rgb = baseColorFactor (applied to the colorMap), opacity_factor = opacityFactor.
    float base_color[4]{1.f, 1.f, 1.f, 1.f};
    float opacity_factor = 1.f;
    // BeamNG alphaRef (0..255 in materials.json) → PS discard threshold (default 127 = 0.5).
    int alpha_ref = 127;
    // BeamNG doubleSided: draw with CULL_NONE PSOs (leaves, bus-stop glass, grass billboards).
    bool double_sided = false;
    // BeamNG detailDistances far edge in metres; 0 = keep the legacy 80 m fade.
    float detail_distance = 0.f;
};

// Luna-style DX12 renderer for a single .o3d mesh (same pipeline as HelloCubeRenderer).
class HelloO3dRenderer {
  public:
    static constexpr std::uint32_t kSwapchainBuffers = 3;

public:
    HelloO3dRenderer() = default;
    ~HelloO3dRenderer();

    HelloO3dRenderer(const HelloO3dRenderer&) = delete;
    HelloO3dRenderer& operator=(const HelloO3dRenderer&) = delete;

    bool initialize(HWND__* hwnd, std::uint32_t width, std::uint32_t height, bool clip_x_mirrored = false);
    bool load_o3d(const omsi::o3d::MappedO3D& source,
                  const omsi::model_cfg::ModelCfgMeshEntry* cfg_mesh = nullptr);
    bool load_model_cfg(const omsi::model_cfg::ModelCfgDocument& cfg);
    void apply_mesh_visibility(
        const std::function<bool(const omsi::model_cfg::ModelCfgMeshEntry&)>& is_visible);
    void apply_visible_runtime(const omsi::model_cfg::ModelCfgDocument& cfg, const omsi::visible::VisibleRuntime& rt);
    [[nodiscard]] std::vector<LoadedMeshChunkInfo> chunk_info() const;
    // require_mouseevent: bus/viewer mesh clicks. Editor: false + optional allow-list (SCO/SLI chunks).
    [[nodiscard]] std::optional<std::size_t> pick_chunk(float mouse_x, float mouse_y, std::uint32_t fb_width,
                                                        std::uint32_t fb_height,
                                                        const float view_proj_transposed[16],
                                                        bool require_mouseevent = true,
                                                        const std::size_t* allowed_chunks = nullptr,
                                                        std::size_t allowed_count = 0,
                                                        float* out_t = nullptr) const;
    // Vertical/world ray vs visible mesh triangles. Returns world-space hit Z (nearest along ray).
    // exclude_chunks: skip draft object / markers while placing.
    [[nodiscard]] std::optional<float> pick_surface_hit_z(float ox, float oy, float oz, float dx, float dy,
                                                          float dz, const std::size_t* exclude_chunks = nullptr,
                                                          std::size_t exclude_count = 0, float* out_t = nullptr) const;
    void set_world_view_proj(const float world_view_proj_transposed[16]);
    void set_camera_world_pos(float x, float y, float z);
    // Render-only distance cull (metres; 0 = disabled). Terrain is never distance-culled.
    void set_cull_max_distance(float metres);
    /// Skip drawing chunks whose projected size is < pct % of screen height (0 = off). Cheap per-chunk test.
    void set_min_screen_size_pct(float pct);
    // Monitor vertical sync (DXGI Present sync interval 0/1).
    void set_vsync(bool enabled);
    [[nodiscard]] bool vsync() const;
    /// Terrain tile LOD: the tile under the camera keeps full quality, neighbouring heavy tiles switch to
    /// index-only clustered LODs (ring 1..4). Default on. Heights/pick/objects never see the LOD mesh.
    void set_terrain_lod_enabled(bool on);
    /// Look direction (unit) + vertical FOV / aspect for CSM cascade frustum fit.
    void set_camera_view(float fwd_x, float fwd_y, float fwd_z, float aspect, float fov_y_rad);
    /// PSSM cascade far (meters). Recomputes 3 splits with λ=0.5; default ~300 m when visual radius unset.
    void set_csm_far_distance(float far_m);
    // Scene fog strength (1 = the default distance fog, 0 = none): Vista > Graficos > Niebla.
    void set_fog_scale(float scale);
    void set_chunk_fixed_lit(std::size_t chunk_index, bool fixed_lit);
    void set_chunk_path_rail(std::size_t chunk_index, bool rail);
    void mark_texture_slot_ignore_season(std::uint32_t slot);
    // Coarse frustum bins: WC row size (not 300 m). Origin is editor-space SW of the cell at (0,0).
    void set_tile_cull_cell(float origin_x, float origin_y, float width_m, float height_m);
    void set_chunk_world_matrices(const std::vector<std::array<float, 16>>& matrices_transposed);
    void set_axis_gizmo_matrices(const float view_proj_transposed[16]);
    void set_pending_screenshot(const std::filesystem::path& path);
    bool upload_ground_plane(float center_x, float center_y, float z, float half_extent_x, float half_extent_y);
    // Blender-style tile mesh in local OMSI Z-up space (origin 0,0,0). Same draw path as ground plane / bus.
    bool upload_terrain_mesh(const omsi::o3d::Vertex* vertices, std::uint32_t vertex_count,
                             const std::uint32_t* indices, std::uint32_t index_count,
                             const std::filesystem::path& texture_path = {});
    // Append another .o3d without clearing existing chunks (map editor instances).
    // Optional cfg_mesh supplies [matl]/[matl_alpha] from .sco / model.cfg (decals, glass).
    bool append_o3d(const omsi::o3d::MappedO3D& source,
                    const omsi::model_cfg::ModelCfgMeshEntry* cfg_mesh = nullptr,
                    const std::vector<omsi::o3d::Vertex>* vertex_override = nullptr);
    // Procedural mesh chunk (tree billboards / splines / terrain) with optional OMSI alpha mode (0/1/2).
    bool append_textured_mesh(const omsi::o3d::Vertex* vertices, std::uint32_t vertex_count,
                              const std::uint32_t* indices, std::uint32_t index_count,
                              const std::filesystem::path& texture_path, std::uint8_t alpha_mode = 0,
                              int text_texture_index = -1, const float* solid_rgba = nullptr);
    // Several materials / index ranges sharing one VB+IB (stream spline upload).
    bool append_textured_mesh_merged(const omsi::o3d::Vertex* vertices, std::uint32_t vertex_count,
                                     const std::uint32_t* indices, std::uint32_t index_count,
                                     const std::vector<ProcMeshSubrange>& subranges);
    // Bind file textures into SRV cache (batched upload list). Call before bulk spline GPU upload.
    void ensure_texture_files_resident(const std::vector<std::filesystem::path>& paths);
    // Submit batched COPY-queue VB/IB uploads (safe to call outside render; no CPU wait on GPU).
    void flush_batched_mesh_uploads();
    // Rebuild VB/IB of an existing procedural chunk (spline remesh).
    // Empty texture_path → untextured preview mesh (does not keep prior textures).
    // wait_gpu=false for live editor preview (avoids fence hitch every mouse move).
    bool replace_textured_mesh(std::size_t chunk_index, const omsi::o3d::Vertex* vertices,
                               std::uint32_t vertex_count, const std::uint32_t* indices,
                               std::uint32_t index_count, const std::filesystem::path& texture_path,
                               std::uint8_t alpha_mode = 0, bool wait_gpu = true, bool wireframe = false,
                               bool force_tex = false);
    // Reset upload allocator if copy/tex fences are done (start of frame). Never wait here.
    void flush_deferred_upload_reset();
    // Copy a cached file albedo SRV into an ImGui descriptor. false = not bound yet.
    bool copy_file_texture_srv(const std::filesystem::path& path, std::uint64_t dst_cpu_ptr);
    // Share VB/IB/SRVs of an existing chunk (same .o3d GPU mesh, new world matrix).
    [[nodiscard]] std::optional<std::size_t> clone_chunk(std::size_t src_chunk_index);
    // VB+IB present and at least one draw batch (false after release_chunk_gpu).
    [[nodiscard]] bool chunk_has_gpu(std::size_t chunk_index) const;
    [[nodiscard]] std::string chunk_debug_line(std::size_t chunk_index) const;
    void clear_meshes();
    void clear_ground();
    [[nodiscard]] bool is_gpu_idle() const;
    void translate_chunk(std::size_t chunk_index, float dx, float dy, float dz);
    void set_chunk_world_matrix(std::size_t chunk_index, const float world_transposed[16]);
    // The chunk's current world matrix (transposed, as set_chunk_world_matrix takes it). False: no such chunk.
    [[nodiscard]] bool chunk_world_transposed(std::size_t chunk_index, float out[16]) const;
    void set_chunk_visible(std::size_t chunk_index, bool visible);
    // Vegetation (tree billboards): vertex wind sway + tilted, translucent lighting.
    void set_chunk_wind_sway(std::size_t chunk_index, bool on);
    // a mesh of the player's bus: its own headlight pools (SceneLight interior = -1) do not light it
    void set_chunk_vehicle_self(std::size_t chunk_index, bool on);
    // [smoothskin]: a skinned chunk's vertices as its bones hold them now (6 floats each: position, normal, in the
    // file's vertex order and the mesh's frame). The uvs stay. False when the chunk is not skinned or counts differ.
    bool update_chunk_skin(std::size_t chunk_index, const float* pos_nrm6, std::uint32_t count);
    // Both sides of every batch (vehicle meshes: some .o3d are wound inside out - openOMSI turns those round -
    // and drawn one-sided their front showed nothing: AI car bodies, parts of buses).
    void set_chunk_double_sided(std::size_t chunk_index, bool on);
    // A vehicle's [matl_alpha] 2 materials (glass in the body texture): the opaque texels drawn with depth in
    // the opaque phase, only the translucent ones blended (all blended without depth, bodies looked like glass).
    void set_chunk_split_blend(std::size_t chunk_index, bool on);
    // entrypoint / carpark helper boxes: shown only while set_show_editor_helpers(true) (Vista > Show carpark)
    void set_chunk_editor_helper(std::size_t chunk_index, bool on);
    void set_show_editor_helpers(bool on);
    // Per frame: night blend 0..1 (OMSI nightmaps fade in) and wind vector in m/s (world XY).
    void set_scene_night_wind(float night_blend, float wind_x_ms, float wind_y_ms);
    // Mark/unmark a chunk as terrain: terrain chunks are never cut by the render-distance cull
    // (meta.mesh_file = "__oo_terrain__") and use the terrain pipeline bias.
    void set_chunk_terrain(std::size_t chunk_index, bool on);
    // Override the terrain-LOD (vertex clustering) candidate flag decided at upload time.
    // BeamNG TSStatic prototypes call this with `false`: a big city / road-system mesh is not a survey tile.
    void set_chunk_lod_capable(std::size_t chunk_index, bool capable);
    /// Terrain tile detail: attach a second (cut) mesh to a chunk whose own mesh is the cheap uncut grid. The detail is
    /// drawn only while the chunk's tile is within `ring_max` tiles (Chebyshev) of the camera, and the base mesh is
    /// then drawn behind it as an underlay. Reuses the chunk's texture/material. Returns false on upload failure.
    bool set_chunk_detail_mesh(std::size_t chunk_index, const omsi::o3d::Vertex* vertices, std::uint32_t vertex_count,
                               const std::uint32_t* indices, std::uint32_t index_count, int ring_max);
    void clear_chunk_detail(std::size_t chunk_index);
    [[nodiscard]] bool chunk_has_detail(std::size_t chunk_index) const;
    /// Chebyshev tile distance from the camera to the chunk's tile (300 m tiles); -1 if unknown.
    [[nodiscard]] int chunk_camera_ring(std::size_t chunk_index) const;
    // Exempt a chunk from the projected-size cull (set_min_screen_size_pct) in the draw pass: used by the
    // BeamNG backend for TSStatic / forest / ground-cover chunks, whose visibility is decided app side by the
    // load radius. Clones (clone_chunk) inherit the flag. Does NOT enable the cluster LOD.
    void set_chunk_no_size_cull(std::size_t chunk_index, bool exempt);
    // Multiplies the projected-size threshold for one chunk (> 1: hidden while still bigger on screen).
    void set_chunk_size_cull_scale(std::size_t chunk_index, float scale);
    /// Free VB/IB GPU memory; hide chunk. Safe on already-released indices.
    void release_chunk_gpu(std::size_t chunk_index);
    /// Drain graphics + copy queues before reusing upload allocators.
    void wait_gpu_idle_for_upload(const char* reason = "unspecified");
    // [rendertype] presurface: TMap pass 0 (before terrain). Normal Z, hole writes depth.
    void set_chunk_presurface(std::size_t chunk_index, bool enabled);
    void set_chunk_render_phase(std::size_t chunk_index, WorldRenderPhase phase, bool surface_bias,
                                bool decal);
    // Map editor: skip OMSI shadow-decal pass (huge draw-call win for scenery).
    void set_draw_shadow_decals(bool enabled);
    // Hover/select tint: amount 0..1, RGB target (default light yellow).
    void set_chunk_highlight(std::size_t chunk_index, float amount, float r = 1.f, float g = 0.95f,
                             float b = 0.55f);
    // Per-chunk opacity 0..1 (editor mesh opacity slider). Forces alpha blend when < 1.
    void set_chunk_opacity(std::size_t chunk_index, float opacity);
    void set_chunk_invert_color(std::size_t chunk_index, bool invert);
    void set_chunk_wireframe(std::size_t chunk_index, bool wireframe);
    [[nodiscard]] bool chunk_has_uv_vars(std::size_t chunk_index) const;
    void apply_chunk_uv_vars(std::size_t chunk_index, const std::function<float(const std::string&)>& get);
    // Replace albedo of [useTextTexture] batches (unique SRV per instance). RGBA8, top-down.
    bool replace_chunk_albedo_rgba(std::size_t chunk_index, const std::uint8_t* rgba, std::uint32_t width,
                                   std::uint32_t height, int text_texture_index = -1);
    // [matl_freetex]: swap instance albedo (unique SRV). texture_key empty = all freetex batches.
    bool replace_chunk_albedo_file(std::size_t chunk_index, const std::filesystem::path& path,
                                   std::string_view texture_key = {});
    // Embedded O3D albedo for a [matl_change] freetex key (`mc:<id>`).
    [[nodiscard]] std::string chunk_freetex_original(std::size_t chunk_index,
                                                     std::string_view texture_key) const;
    // [matl_change] on one chunk only (clones share the source's texture slots): the slot `texture_key`
    // shows `path` (empty: its own texture again). Each (slot, texture) table is uploaded once and reused,
    // so lamps that blink cost a pointer swap, and one AI car's brake lights leave the others alone.
    // target: 0 = the albedo (t0); 2 = a [matl_item] night map at the night slot, glowing while set (by day
    // too); 3 = a [matl_item] light map there, fully lit while set. An empty path restores the slot's own.
    bool set_chunk_freetex_variant(std::size_t chunk_index, std::string_view texture_key,
                                   const std::filesystem::path& path, int target = 0);
    // whether that variant's table is up already (setting it is then only a pointer swap)
    [[nodiscard]] bool chunk_freetex_variant_ready(std::size_t chunk_index, std::string_view texture_key,
                                                   const std::filesystem::path& path, int target = 0) const;
    // Re-upload file-backed albedo/env (same SRV slots) after set_texture_max_edge. Skips OFT text.
    bool reload_file_textures();
    /// Queue all file-backed slots for season/night path rebind (no sync GPU walk).
    void begin_season_texture_rebind();
    /// Re-resolve + upload up to max_n slots this frame. Returns how many were touched.
    int pump_season_texture_rebinds(int max_n = 3);
    // Textures of meshes created while every texture upload list was in flight (drawn white until then):
    // loaded into their descriptor slot later, a few a frame. Returns how many went up.
    int pump_late_textures(int max_n = 4);
    // Drop GPU+CPU cache entries so an overwritten file (e.g. ground_paint.bmp) reloads.
    void invalidate_texture_path(const std::filesystem::path& path);
    void clear_chunk_highlights();

    // OMSI light_enh / maplight editor preview (up to 4 nearest lights).
    struct SceneLight {
        float x = 0.f, y = 0.f, z = 0.f;
        float range = 12.f;
        float r = 1.f, g = 1.f, b = 1.f;
        float intensity = 1.f;
        // 1 = a vehicle's [interiorlight]: lights only the meshes with an [illumination_interior] (its saloon),
        // never the world outside nor the vehicle's own body (it shone through the roof onto the street).
        // -1 = the player's bus's own headlight pool: lights the world, never the bus itself (nor any saloon)
        float interior = 0.f;
    };
    void set_scene_lights(const SceneLight* lights, int count, float ambient_scale = 1.f);
    /// Vehicle lamp coronas ([light_enh] / [light_enh_2]) drawn this frame: 10 floats each as the FFI gives them
    /// (x y z, axis x y z - 0: all round -, r g b, size), times `strength`. Glows only: they light nothing.
    void set_vehicle_coronas(const float* lamps10, int count, float strength);
    /// Directional sun (OMSI Z-up) + optional cascaded shadow maps (staggered, toggleable).
    void set_sun_lighting(float dir_x, float dir_y, float dir_z, float color_r, float color_g, float color_b,
                          bool enable_shadows, std::uint32_t shadow_map_size = 1024);
    void set_clear_color(float r, float g, float b);
    /// OpenOMSI/OMSI sky dome (Z-up gradient + sun disk). Call each frame with sky::evaluate().
    void set_sky_visuals(float zenith_r, float zenith_g, float zenith_b, float horizon_r, float horizon_g,
                         float horizon_b, float sun_dir_x, float sun_dir_y, float sun_dir_z, float sun_r,
                         float sun_g, float sun_b, float sun_intensity, bool enabled = true);

    /// Full OMSI environment for one frame: sky textures (himmel day/dusk/night), clouds, sun/moon, stars,
    /// weather (cloud cover/type, precipitation, fog/haze, wind). Cheap per frame (constant data only); the
    /// textures are loaded once on the first call (decoded on worker threads).
    struct SkyEnvironment {
        std::filesystem::path asset_root;   // OMSI install root (Texture\, helper\)
        float sun_dir[3] = {0.4f, 0.22f, 0.88f};
        float sun_color[3] = {1.f, 0.95f, 0.75f};
        float sun_intensity = 1.f;          // 0 when the sun is below the horizon
        float moon_dir[3] = {-0.4f, -0.22f, 0.5f};
        float moon_intensity = 0.f;
        float day_w = 1.f, dusk_w = 0.f, night_w = 0.f; // himmel texture blend (OMSI sky_w_*)
        float sun_alt_deg = 45.f;
        float star_brightness = 0.f;        // 0 day … 1 night (OMSI night_blend)
        float lst_rad = 0.f, lat_rad = 0.9f;
        float cloud_cover = 0.f;            // 0..1
        int cloud_type = 0;                 // omsi::sky::CloudType
        float cloud_base_m = 0.f;
        float cloud_wind_to_rad = 0.f;      // direction the clouds move toward (azimuth from north)
        float cloud_wind_speed = 0.f;       // m/s
        float fog_rgb[3] = {0.7f, 0.77f, 0.87f};
        float fog_density = 0.f;            // 1/m (exp fog for water)
        float haze = 0.f;                   // 0..1 horizon haze
        int precip_type = 0;                // 0 none, 1 rain, 2 snow
        float precip_intensity = 0.f;       // 0..1
        float cloud_brightness = 1.f;
        float god_ray_strength = 0.f;       // 0 = off; light shafts need sun shadows (CSM) enabled
        bool shaft_plus = false;            // false = Normal (8 steps), true = Plus (32 steps, stronger scatter)
        float ground_z = -1.0e9f;           // ground height for the light shafts (downward rays end there)
        bool shaft_debug = false;           // OMSI_SHAFT_DEBUG=1: output raw shadow visibility instead of scatter
    };
    void set_sky_environment(const SkyEnvironment& env);

    /// Water tiles (OMSI TFileWater): one quad per tile, world-space origin and size, corner heights in
    /// file order (x0,y0)(x1,y0)(x0,y1)(x1,y1). Rebuilt only when called (call on change).
    struct WaterQuad {
        float origin_x = 0.f, origin_y = 0.f, size_m = 0.f;
        float h[4]{0.f, 0.f, 0.f, 0.f};
    };
    void set_water_quads(const WaterQuad* quads, std::uint32_t count);
    // Screen-space NDC overlay (DX12 flip model: GDI cannot draw over the swapchain).
    void set_screen_rect_overlay(float fb_x0, float fb_y0, float fb_x1, float fb_y1, float fb_w, float fb_h,
                                 bool enabled);
    [[nodiscard]] std::size_t chunk_count() const;
    // CPU-side resource footprint for MEM60 diagnostics (pick buffers, SRV slots, caches).
    struct ResourceDiag {
        std::size_t chunks = 0;
        std::size_t chunks_with_gpu = 0;
        std::size_t srv_next = 0;
        std::size_t texture_cache = 0;
        std::size_t pick_bytes = 0;
        std::size_t tex_slots = 0;
        std::size_t upload_scratch = 0;
    };
    [[nodiscard]] ResourceDiag resource_diag() const;

    // GPU timestamps (EndQuery, resolved after the slot's fence — no extra CPU stall).
    // Stamps: 0=scene start, 1=after geometry, 2=after ImGui/Ultralight (before Present).
    static constexpr std::uint32_t kGpuStampCount = 3;
    struct GpuTiming {
        double gpu_scene_ms = 0.0;
        double gpu_ui_ms = 0.0;
        double gpu_total_ms = 0.0;
        double cpu_wait_fence_ms = 0.0;
        double cpu_wait_tex_ms = 0.0;
        double cpu_populate_ms = 0.0;
        double cpu_cull_ms = 0.0;
        double cpu_record_ms = 0.0;
        double cpu_submit_ms = 0.0;
        double cpu_present_ms = 0.0;
        std::uint64_t draws = 0;
        std::uint64_t visible_chunks = 0;
        std::uint64_t culled_chunks = 0;
        const char* bound = "unknown";
    };
    [[nodiscard]] GpuTiming last_gpu_timing() const;
    void resize(std::uint32_t width, std::uint32_t height);
    void render();
    bool save_screenshot_bmp(const std::filesystem::path& path);
    void shutdown();

    // Dear ImGui overlay (optional). Set before render(); cleared on shutdown().
    void set_imgui_overlay(omsi::ui::ImGuiOverlay* overlay);
    void set_ultralight_overlay(omsi::ui::UltralightOverlay* overlay);
    [[nodiscard]] ID3D12Device* dx12_device() const;
    [[nodiscard]] ID3D12CommandQueue* dx12_command_queue() const;
    [[nodiscard]] std::uint32_t dx12_num_frames_in_flight() const { return kFrameCount; }

    // Public so o3d translation units (CSM, draw helpers) can name the type.
    struct Impl;

private:
    static constexpr std::uint32_t kFrameCount = kSwapchainBuffers;

    void wait_for_previous_frame();
    void wait_pending_texture_uploads();
    bool try_reset_upload_allocator();
    bool begin_proc_upload();
    void flush_proc_uploads();
    void populate_command_list();
    bool create_device_and_swapchain(HWND__* hwnd);
    bool create_pipeline();
    bool create_axis_pipeline();
    bool create_constant_buffer();
    void create_depth_buffer(std::uint32_t width, std::uint32_t height);
    void create_scene_color_buffers(std::uint32_t width, std::uint32_t height);
    void create_msaa_color_buffer(std::uint32_t width, std::uint32_t height);
    void pick_msaa_quality();
    bool create_fxaa_pipeline();
    void record_fxaa_pass(ID3D12GraphicsCommandList* cmd, std::uint32_t frame_index);
    void record_msaa_resolve(ID3D12GraphicsCommandList* cmd, ID3D12Resource* dest);
    [[nodiscard]] bool scene_postfx_active() const;
    [[nodiscard]] bool scene_msaa_active() const;
    bool upload_axis_gpu();
    bool upload_mesh_gpu(const omsi::o3d::MappedO3D& source,
                         const omsi::model_cfg::ModelCfgMeshEntry* cfg_mesh,
                         const std::vector<omsi::o3d::Vertex>* vertex_override = nullptr);
    void bind_proc_texture(GpuMeshChunk& chunk, MaterialDrawBatch& batch, const std::filesystem::path& texture_path,
                           std::uint8_t alpha_mode, int text_texture_index, const float* solid_rgba);
    void bind_pbr_textures(MaterialDrawBatch& batch, const ProcMeshSubrange& sub);

    HWND__* hwnd_ = nullptr;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::uint32_t frame_index_ = 0;
    std::uint32_t last_drawn_frame_index_ = 0;
    std::uint32_t rtv_descriptor_size_ = 0;

    Impl* impl_ = nullptr;
    omsi::ui::ImGuiOverlay* imgui_overlay_ = nullptr;
    omsi::ui::UltralightOverlay* ultralight_overlay_ = nullptr;
};

} // namespace omsi::gfx::dx12
