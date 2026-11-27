#include "omsi/logger.hpp"
#include <chrono>
#include "omsi/path_utf8.hpp"
#include "ui/map_editor_imgui_panel.hpp"

#include "gfx/dx12/dx12_texture.hpp"
#include "omsi/tt_man.hpp"
#include "omsi/tt_editor_ui.hpp"
#include "omsi/map_geo_tools.hpp"
#include "omsi/omsi_sky.hpp"
#include "o3d/o3d_loader.hpp"
#include "omsi/sco_types.hpp"
#include "omsi/x_mesh.hpp"
#include "omsi/options.hpp"
#include "omsi/spline_types.hpp"
#include "ui/editor_shortcuts.hpp"
#include "ui/i18n.hpp"
#include "ui/imgui_overlay.hpp"
#include "ui/theme.hpp"
#include "omsi/main_thread_watchdog.hpp"

#include <imgui.h>
#include <Windows.h>
#include <commdlg.h>
#include "editor_map_text_editor.hpp"


#include "omsi/utf8_win32.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

std::unordered_map<std::string, std::vector<ImU32>> g_thumb_pix;
std::unordered_map<std::string, omsi::gfx::dx12::CpuRgbaImage> g_thumb_tex;

std::string path_utf8(const std::filesystem::path& p) {
    try {
        const auto u8 = p.u8string();
        return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
    } catch (const std::filesystem::filesystem_error&) {
        try {
            return omsi::wide_to_utf8(p.wstring());
        } catch (...) {
            return {};
        }
    }
}

std::string path_name_lower(const std::filesystem::path& p) {
    auto n = path_utf8(p.filename());
    for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return n;
}

bool native_is_under(const std::wstring& child, const std::wstring& root) {
    if (root.empty() || child.size() < root.size()) return false;
    if (child.compare(0, root.size(), root) != 0) return false;
    if (child.size() == root.size()) return true;
    const wchar_t sep = child[root.size()];
    return sep == L'\\' || sep == L'/';
}

struct BrowserIdxEnt {
    std::wstring full;
    std::string name_lower;
    bool is_dir = false;
};

struct BrowserIndex {
    std::wstring root_native;
    std::chrono::steady_clock::time_point built_at{};
    std::vector<BrowserIdxEnt> ents;
    std::atomic<bool> ready{false};
};

struct BrowserIndexHub {
    std::mutex mu;
    std::shared_ptr<BrowserIndex> current;
    std::filesystem::path pending;
    std::atomic<bool> building{false};
};

BrowserIndexHub g_browser_idx;

void browser_index_build(const std::filesystem::path& root, const std::shared_ptr<BrowserIndex>& out) {
    std::error_code ec;
    const auto opts = std::filesystem::directory_options::skip_permission_denied;
    std::filesystem::recursive_directory_iterator it(root, opts, ec);
    const std::filesystem::recursive_directory_iterator end;
    if (ec) return;
    out->ents.reserve(4096);
    for (; it != end; it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        try {
            BrowserIdxEnt e;
            e.full = it->path().native();
            e.name_lower = path_name_lower(it->path());
            std::error_code iec;
            e.is_dir = it->is_directory(iec) && !iec;
            out->ents.push_back(std::move(e));
        } catch (const std::filesystem::filesystem_error&) {
            continue;
        }
    }
}

std::filesystem::path catalog_thumbs_dir() {
    return omsi::options::editor_ui_prefs_path().parent_path() / "catalog_thumbs";
}

std::filesystem::path catalog_thumb_tga(const std::filesystem::path& src, int view = 0) {
    const auto n = omsi::path_text(src.lexically_normal());
    const auto h = std::hash<std::string>{}(n);
    char buf[40]{};
    std::snprintf(buf, sizeof(buf), "%016llx_v%d.tga", static_cast<unsigned long long>(h), view);
    return catalog_thumbs_dir() / buf;
}

void catalog_thumbs_clear_mem() {
    g_thumb_pix.clear();
    g_thumb_tex.clear();
}

bool write_thumb_tga(const std::filesystem::path& p, const std::vector<ImU32>& px, int k) {
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary);
    if (!out) return false;
    const std::uint8_t hdr[18] = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                  static_cast<std::uint8_t>(k & 255), static_cast<std::uint8_t>((k >> 8) & 255),
                                  static_cast<std::uint8_t>(k & 255), static_cast<std::uint8_t>((k >> 8) & 255), 32,
                                  32};
    out.write(reinterpret_cast<const char*>(hdr), 18);
    for (int i = 0; i < k * k; ++i) {
        const ImU32 c = px[static_cast<std::size_t>(i)];
        const std::uint8_t bgra[4] = {static_cast<std::uint8_t>(c >> IM_COL32_B_SHIFT),
                                      static_cast<std::uint8_t>(c >> IM_COL32_G_SHIFT),
                                      static_cast<std::uint8_t>(c >> IM_COL32_R_SHIFT), 255};
        out.write(reinterpret_cast<const char*>(bgra), 4);
    }
    return static_cast<bool>(out);
}

bool load_thumb_tga(const std::filesystem::path& src, int k, std::vector<ImU32>& px, int view = 0) {
    const auto tga = catalog_thumb_tga(src, view);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(tga, ec)) return false;
    const auto src_t = std::filesystem::last_write_time(src, ec);
    const auto tga_t = std::filesystem::last_write_time(tga, ec);
    if (!ec && src_t > tga_t) return false;
    auto img = omsi::gfx::dx12::load_image_cpu_rgba(tga);
    if (!img || static_cast<int>(img->width) != k || static_cast<int>(img->height) != k) return false;
    px.resize(static_cast<std::size_t>(k * k));
    for (int i = 0; i < k * k; ++i) {
        const std::size_t o = static_cast<std::size_t>(i) * 4;
        px[static_cast<std::size_t>(i)] =
            IM_COL32(img->rgba[o], img->rgba[o + 1], img->rgba[o + 2], 255);
    }
    return true;
}

// Thumbnails as real GPU textures (one quad) instead of one AddRectFilled per pixel (3136 rects per
// 56 px thumb — hundreds of thousands per frame with catalog + favourites + prefabs visible).
std::function<std::uint64_t(const std::filesystem::path&)> g_thumb_tex_provider;
std::function<void(const std::filesystem::path&)> g_thumb_srv_evict;
std::function<bool()> g_mesh_tab_active;
std::function<void()> g_mesh_tab_draw;
std::function<void()> g_coop_tab_draw;
std::unordered_map<std::uint64_t, std::filesystem::path> g_thumb_px_tga;

bool write_tga_wh(const std::filesystem::path& p, const std::vector<ImU32>& px, int w, int h) {
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary);
    if (!out) return false;
    const std::uint8_t hdr[18] = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                  static_cast<std::uint8_t>(w & 255), static_cast<std::uint8_t>((w >> 8) & 255),
                                  static_cast<std::uint8_t>(h & 255), static_cast<std::uint8_t>((h >> 8) & 255), 32,
                                  32};
    out.write(reinterpret_cast<const char*>(hdr), 18);
    for (std::size_t i = 0; i < px.size(); ++i) {
        const ImU32 c = px[i];
        const std::uint8_t bgra[4] = {static_cast<std::uint8_t>(c >> IM_COL32_B_SHIFT),
                                      static_cast<std::uint8_t>(c >> IM_COL32_G_SHIFT),
                                      static_cast<std::uint8_t>(c >> IM_COL32_R_SHIFT),
                                      static_cast<std::uint8_t>(c >> IM_COL32_A_SHIFT)};
        out.write(reinterpret_cast<const char*>(bgra), 4);
    }
    return static_cast<bool>(out);
}

bool blit_thumb_gpu(ImDrawList* dl, const ImVec2& p0, const std::vector<ImU32>& px, int w, int h) {
    if (!g_thumb_tex_provider || px.size() != static_cast<std::size_t>(w * h) || px.empty()) return false;
    std::uint64_t hs = 1469598103934665603ull ^ static_cast<std::uint64_t>(w * 131 + h);
    for (const ImU32 c : px) {
        hs ^= c;
        hs *= 1099511628211ull;
    }
    auto it = g_thumb_px_tga.find(hs);
    if (it == g_thumb_px_tga.end()) {
        char buf[48]{};
        std::snprintf(buf, sizeof(buf), "px_%016llx.tga", static_cast<unsigned long long>(hs));
        const auto p = catalog_thumbs_dir() / buf;
        std::error_code ec;
        if (!std::filesystem::is_regular_file(p, ec) && !write_tga_wh(p, px, w, h)) return false;
        it = g_thumb_px_tga.emplace(hs, p).first;
    }
    const std::uint64_t id = g_thumb_tex_provider(it->second);
    if (id == 0) return false;
    dl->AddImage(static_cast<ImTextureID>(id), p0,
                 ImVec2(p0.x + static_cast<float>(w), p0.y + static_cast<float>(h)));
    return true;
}

void blit_thumb(ImDrawList* dl, const ImVec2& p0, const std::vector<ImU32>& px, int k) {
    if (blit_thumb_gpu(dl, p0, px, k, k)) return;
    for (int y = 0; y < k; ++y) {
        for (int x = 0; x < k; ++x) {
            const ImU32 col = px[static_cast<std::size_t>(y * k + x)];
            dl->AddRectFilled(ImVec2(p0.x + static_cast<float>(x), p0.y + static_cast<float>(y)),
                              ImVec2(p0.x + static_cast<float>(x) + 1.f, p0.y + static_cast<float>(y) + 1.f), col);
        }
    }
}

void blit_thumb_size(ImDrawList* dl, const ImVec2& p0, const std::vector<ImU32>& px, int w, int h) {
    if (blit_thumb_gpu(dl, p0, px, w, h)) return;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const ImU32 col = px[static_cast<std::size_t>(y * w + x)];
            dl->AddRectFilled(ImVec2(p0.x + static_cast<float>(x), p0.y + static_cast<float>(y)),
                              ImVec2(p0.x + static_cast<float>(x) + 1.f, p0.y + static_cast<float>(y) + 1.f), col);
        }
    }
}

} // namespace

namespace omsi::ui {
namespace {

const char* kToolKeys[] = {"tool.tile",    "tool.object", "tool.spline", "tool.terrain", "tool.traffic",
                           "tool.tracks",  "tool.station", "tool.splineexp", "tool.signals"};
constexpr int kToolCount = static_cast<int>(sizeof(kToolKeys) / sizeof(kToolKeys[0]));

// Aerial: full provider set (v326 UI defaults Google MT). DEM: v326 keyless chain.
const char* kSatProviders[] = {"Esri World",       "EOX Sentinel-2", "OMSI Link",      "MapTiler",
                               "Google Tiles",     "OpenStreetMap",  "Bing Aerial",    "Google Static",
                               "Geoportal PL",     "Geoportal PL HR", "Custom URL",    "GCBA 2017",
                               "IGN Argentina",    "GCMods Bing",    "GCMods Google",  "Google MT (exp)"};
const char* kDtmProviders[] = {"FABDEM V1-2 ★", "Copernicus GLO-30", "Open-Meteo GLO-90", "OpenTopo ASTER",
                               "NASA SRTM30", "Terrarium XYZ", "Open-Elevation", "Mapterhorn Terrarium"};

const char* kTrafficRules[] = {"Velocidad max",     "No adelantar", "No programado", "Densidad muy baja",
                               "Densidad baja",     "Densidad media-baja", "Densidad normal", "Densidad alta",
                               "Prioridad alta",    "Prioridad baja", "Prohibido coches", "Camiones permitidos",
                               "Autobuses/taxis"};
constexpr int kTrafficRuleCount = static_cast<int>(sizeof(kTrafficRules) / sizeof(kTrafficRules[0]));

bool item_matches_filter(const char* filter, const EditorListItem& it) {
    if (!filter || filter[0] == '\0') return true;
    std::string hay = it.label + " " + omsi::path_text(it.path);
    std::string needle(filter);
    const auto lower = [](std::string& s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    };
    lower(hay);
    lower(needle);
    return hay.find(needle) != std::string::npos;
}

void fire(EditorSidePanel& panel, EditorSidePanelState ev) { panel.imgui_merge_pending(ev); }

void draw_hover_catalog_preview(EditorSidePanelImGuiState& st, const std::filesystem::path& p, bool spline,
                                const std::filesystem::path& omsi_root);

// Icon button in the openOMSI style (omsi-ui: Material Symbols Rounded on a rounded box with a soft
// hover tint): vector star, outline when not a favourite, filled gold when it is, press = darker.
bool fav_star_button(const char* id, bool active = false) {
    const float sz = ImGui::GetFrameHeight();
    ImGui::PushID(id);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const bool hit = ImGui::InvisibleButton("star", ImVec2(sz, sz));
    const bool hov = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p1(p0.x + sz, p0.y + sz);
    if (hov || held) {
        dl->AddRectFilled(ImVec2(p0.x + 1.f, p0.y + 2.f), ImVec2(p1.x + 1.f, p1.y + 2.f), IM_COL32(0, 0, 0, 60),
                          sz * 0.32f);
        dl->AddRectFilled(p0, p1, held ? IM_COL32(255, 196, 40, 80) : IM_COL32(255, 196, 40, 38), sz * 0.32f);
    }
    const ImVec2 c(p0.x + sz * 0.5f, p0.y + sz * 0.53f);
    const float ro = sz * 0.36f, ri = ro * 0.46f;
    ImVec2 pts[10];
    for (int i = 0; i < 10; ++i) {
        const float a = -1.5707963f + static_cast<float>(i) * 0.62831853f;
        const float r = (i & 1) ? ri : ro;
        pts[i] = ImVec2(c.x + std::cos(a) * r, c.y + std::sin(a) * r);
    }
    const ImU32 gold = held ? IM_COL32(214, 150, 20, 255) : IM_COL32(255, 196, 40, 255);
    if (active || hov) {
        for (int i = 0; i < 10; ++i) dl->AddTriangleFilled(c, pts[i], pts[(i + 1) % 10], active ? gold : IM_COL32(255, 196, 40, 150));
    }
    dl->AddPolyline(pts, 10, active || hov ? gold : IM_COL32(150, 156, 168, 255), ImDrawFlags_Closed, 1.6f);
    ImGui::PopID();
    return hit;
}

float& field_step(EditorSidePanelImGuiState& st, const char* key, float def) {
    auto it = st.field_steps.find(key);
    if (it == st.field_steps.end()) it = st.field_steps.emplace(key, def).first;
    return it->second;
}

// DragFloat (mouse scrub) + ▲/▼ + ⚙ step. Returns true if value changed.
static bool g_spin_gesture_began = false;
static bool g_spin_gesture_ended = false;

bool spin_float(EditorSidePanelImGuiState& st, const char* label, const char* key, float* v, float def_step,
                float v_min = 0.f, float v_max = 0.f, const char* fmt = "%.3f", bool hide_label = false,
                float extra_right = 0.f, float label_col = 118.f, bool show_step = true) {
    ImGui::PushID(key);
    float& step = field_step(st, key, def_step);
    bool changed = false;
    bool step_changed = false;

    if (!hide_label) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        if (label_col > 8.f) ImGui::SameLine(label_col);
        else ImGui::SameLine(0.f, 4.f);
    }
    const float gear_w = ImGui::GetFrameHeight();
    const float spin_w = gear_w + 2.f;
    const float avail = ImGui::GetContentRegionAvail().x;
    const float controls_w = show_step ? (spin_w + gear_w) : spin_w;
    ImGui::SetNextItemWidth((std::max)(40.f, avail - controls_w - 6.f - extra_right));
    // Drag horizontally; Ctrl+click to type.
    // One undo for the whole mouse-hold scrub (Activated → Deactivated), not per step.
    if (ImGui::DragFloat("##val", v, step, v_min, v_max, fmt)) changed = true;
    if (ImGui::IsItemActivated()) g_spin_gesture_began = true;
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        changed = true;
        g_spin_gesture_ended = true;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        ImGui::SetTooltip("Arrastrar = scrub (step %.4g) · Ctrl+click = escribir", step);

    ImGui::SameLine(0.f, 2.f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.f, 0.f));
    ImGui::PushStyleColor(ImGuiCol_Button, theme::rgb(theme::surface));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::rgb(theme::surface_hi));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::rgb(theme::accent_lo));
    ImGui::BeginGroup();
    const ImVec2 btn(spin_w, ImGui::GetFrameHeight() * 0.5f - 1.f);
    if (ImGui::Button("##up", btn)) {
        *v += step;
        changed = true;
        g_spin_gesture_began = true;
        g_spin_gesture_ended = true;
    }
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 a = ImGui::GetItemRectMin();
        const ImVec2 b = ImGui::GetItemRectMax();
        const ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
        dl->AddTriangleFilled(ImVec2(c.x, c.y - 3.f), ImVec2(c.x - 4.f, c.y + 2.f), ImVec2(c.x + 4.f, c.y + 2.f),
                              IM_COL32(200, 200, 200, 255));
    }
    if (ImGui::Button("##dn", btn)) {
        *v -= step;
        changed = true;
        g_spin_gesture_began = true;
        g_spin_gesture_ended = true;
    }
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 a = ImGui::GetItemRectMin();
        const ImVec2 b = ImGui::GetItemRectMax();
        const ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
        dl->AddTriangleFilled(ImVec2(c.x, c.y + 3.f), ImVec2(c.x - 4.f, c.y - 2.f), ImVec2(c.x + 4.f, c.y - 2.f),
                              IM_COL32(200, 200, 200, 255));
    }
    ImGui::EndGroup();
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar();

    if (show_step) {
        ImGui::SameLine(0.f, 2.f);
        if (ImGui::Button("*", ImVec2(gear_w, ImGui::GetFrameHeight()))) ImGui::OpenPopup("step_popup");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
            ImGui::SetTooltip("Step drag/flecha (%.4g) — click para editar", step);
        if (ImGui::BeginPopup("step_popup")) {
            ImGui::Text("Step: %s", label);
            ImGui::SetNextItemWidth(120.f);
            if (ImGui::DragFloat("##step", &step, (std::max)(step * 0.05f, 0.0001f), 0.0001f, 1.0e6f, "%.5g"))
                step_changed = true;
            if (ImGui::SmallButton("0.01")) {
                step = 0.01f;
                step_changed = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("0.1")) {
                step = 0.1f;
                step_changed = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("1")) {
                step = 1.f;
                step_changed = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("5")) {
                step = 5.f;
                step_changed = true;
            }
            ImGui::EndPopup();
        }
    }
    if (step_changed) st.field_steps_dirty = true;

    if (v_max > v_min) *v = std::clamp(*v, v_min, v_max);
    ImGui::PopID();
    return changed;
}

bool spin_float_xyz(EditorSidePanelImGuiState& st, const char* label, const char* key, float* v, float def_step,
                    const char* fmt = "%.3f", float v_min = 0.f, float v_max = 0.f) {
    ImGui::PushID(key);
    float& step = field_step(st, key, def_step);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    const bool changed = ImGui::DragFloat("##val", v, step, v_min, v_max, fmt);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        ImGui::SetTooltip("Arrastrar = scrub (step %.4g) · Ctrl+click = escribir", step);
    ImGui::PopID();
    return changed;
}

void fire_apply_spline(EditorSidePanel& panel, bool arm_undo = true) {
    if (arm_undo) panel.arm_spline_grad_undo();
    EditorSidePanelState ev{};
    ev.apply_spline = true;
    fire(panel, ev);
}

void fire_apply_object(EditorSidePanel& panel, bool arm_undo = true) {
    if (arm_undo) panel.arm_object_edit_undo();
    EditorSidePanelState ev{};
    ev.apply_object = true;
    fire(panel, ev);
}

void draw_sli_topdown_preview(const std::filesystem::path& sli, float length, float radius, bool missing,
                             const std::filesystem::path& omsi_root, bool paint = true) {
    constexpr int kPx = 56;
    constexpr float k = static_cast<float>(kPx);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImDrawList* dl = paint ? ImGui::GetWindowDrawList() : nullptr;
    if (paint) {
        ImGui::Dummy(ImVec2(k, k));
        const ImVec2 p1(p0.x + k, p0.y + k);
        dl->AddRectFilled(p0, p1, IM_COL32(18, 20, 22, 255), 3.f);
        dl->AddRect(p0, p1, IM_COL32(70, 74, 82, 255), 3.f);
        if (missing || sli.empty()) {
            dl->AddLine(ImVec2(p0.x + 8.f, p0.y + 8.f), ImVec2(p1.x - 8.f, p1.y - 8.f), IM_COL32(220, 70, 70, 255), 2.f);
            dl->AddLine(ImVec2(p1.x - 8.f, p0.y + 8.f), ImVec2(p0.x + 8.f, p1.y - 8.f), IM_COL32(220, 70, 70, 255), 2.f);
            return;
        }
    } else if (missing || sli.empty()) {
        return;
    }
    const std::string key = omsi::path_text(sli) + "|" + std::to_string(length) + "|" + std::to_string(radius);
    auto sample_tex = [](const gfx::dx12::CpuRgbaImage& im, float u, float v) -> ImU32 {
        if (im.width == 0 || im.height == 0 || im.rgba.empty()) return IM_COL32(90, 90, 90, 255);
        u = u - std::floor(u);
        v = v - std::floor(v);
        if (u < 0.f) u += 1.f;
        if (v < 0.f) v += 1.f;
        const float fx = u * static_cast<float>(im.width - 1);
        const float fy = v * static_cast<float>(im.height - 1);
        const int x0 = static_cast<int>(fx);
        const int y0 = static_cast<int>(fy);
        const int x1 = (std::min)(x0 + 1, static_cast<int>(im.width - 1));
        const int y1 = (std::min)(y0 + 1, static_cast<int>(im.height - 1));
        const float tx = fx - static_cast<float>(x0);
        const float ty = fy - static_cast<float>(y0);
        auto pix = [&](int x, int y) {
            const std::size_t i = (static_cast<std::size_t>(y) * im.width + static_cast<std::size_t>(x)) * 4;
            return ImVec4(im.rgba[i] / 255.f, im.rgba[i + 1] / 255.f, im.rgba[i + 2] / 255.f, im.rgba[i + 3] / 255.f);
        };
        const ImVec4 a = pix(x0, y0), b = pix(x1, y0), c = pix(x0, y1), d = pix(x1, y1);
        const ImVec4 e = ImVec4(a.x + (b.x - a.x) * tx, a.y + (b.y - a.y) * tx, a.z + (b.z - a.z) * tx,
                                a.w + (b.w - a.w) * tx);
        const ImVec4 f = ImVec4(c.x + (d.x - c.x) * tx, c.y + (d.y - c.y) * tx, c.z + (d.z - c.z) * tx,
                                c.w + (d.w - c.w) * tx);
        const ImVec4 g = ImVec4(e.x + (f.x - e.x) * ty, e.y + (f.y - e.y) * ty, e.z + (f.z - e.z) * ty,
                                e.w + (f.w - e.w) * ty);
        if (g.w < 0.08f) return 0;
        return IM_COL32(static_cast<int>(g.x * 255.f + 0.5f), static_cast<int>(g.y * 255.f + 0.5f),
                        static_cast<int>(g.z * 255.f + 0.5f), 255);
    };
    auto ensure_tex = [&](const std::filesystem::path& tp) -> const gfx::dx12::CpuRgbaImage* {
        auto resolved = gfx::dx12::resolve_texture_path_day(sli, omsi::path_text(tp), omsi_root);
        if (!resolved && !tp.empty())
            resolved = gfx::dx12::resolve_texture_path_day(sli, omsi::path_text(tp.filename()), omsi_root);
        if (!resolved) return nullptr;
        const std::string tk = resolved->string();
        if (const auto it = g_thumb_tex.find(tk); it != g_thumb_tex.end()) return &it->second;
        auto img = gfx::dx12::load_image_cpu_rgba(*resolved);
        if (!img) return nullptr;
        const auto [ins, ok] = g_thumb_tex.emplace(tk, std::move(*img));
        return &ins->second;
    };
    if (g_thumb_pix.find(key) == g_thumb_pix.end()) {
        std::vector<ImU32> px;
        if (load_thumb_tga(sli, kPx, px)) {
            g_thumb_pix.emplace(key, std::move(px));
        } else {
        std::vector<ImU32> gen(static_cast<std::size_t>(kPx * kPx), IM_COL32(22, 24, 28, 255));
        std::vector<float> zbuf(static_cast<std::size_t>(kPx * kPx), -1.e9f);
        const auto def = spline::load_sli(sli);
        if (def) {
            map::SplineInst sp{};
            sp.sli_path = sli;
            sp.length = (std::max)(0.5f, length);
            sp.radius = radius;
            const auto meshes = spline::build_spline_meshes(sp, *def, omsi_root, 0.f);
            float xmin = 1.e9f, ymin = 1.e9f, xmax = -1.e9f, ymax = -1.e9f;
            for (const auto& m : meshes) {
                for (const auto& v : m.vertices) {
                    xmin = (std::min)(xmin, v.position[0]);
                    ymin = (std::min)(ymin, v.position[1]);
                    xmax = (std::max)(xmax, v.position[0]);
                    ymax = (std::max)(ymax, v.position[1]);
                }
            }
            const float span = (std::max)((std::max)(xmax - xmin, ymax - ymin), 0.05f);
            const float pad = span * 0.08f;
            xmin -= pad;
            ymin -= pad;
            const float den = span + pad * 2.f;
            auto projectf = [&](float wx, float wy) {
                return ImVec2(((wx - xmin) / den) * static_cast<float>(kPx - 1),
                              (1.f - (wy - ymin) / den) * static_cast<float>(kPx - 1));
            };
            for (const auto& m : meshes) {
                const gfx::dx12::CpuRgbaImage* tex = m.texture_path.empty() ? nullptr : ensure_tex(m.texture_path);
                const auto& idx = m.indices;
                for (std::size_t t = 0; t + 2 < idx.size(); t += 3) {
                    const auto& va = m.vertices[idx[t]];
                    const auto& vb = m.vertices[idx[t + 1]];
                    const auto& vc = m.vertices[idx[t + 2]];
                    const ImVec2 a = projectf(va.position[0], va.position[1]);
                    const ImVec2 b = projectf(vb.position[0], vb.position[1]);
                    const ImVec2 c = projectf(vc.position[0], vc.position[1]);
                    const float area = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
                    if (std::fabs(area) < 1.e-4f) continue;
                    const int minx = (std::max)(0, static_cast<int>(std::floor((std::min)(a.x, (std::min)(b.x, c.x)))));
                    const int maxx = (std::min)(kPx - 1, static_cast<int>(std::ceil((std::max)(a.x, (std::max)(b.x, c.x)))));
                    const int miny = (std::max)(0, static_cast<int>(std::floor((std::min)(a.y, (std::min)(b.y, c.y)))));
                    const int maxy = (std::min)(kPx - 1, static_cast<int>(std::ceil((std::max)(a.y, (std::max)(b.y, c.y)))));
                    for (int y = miny; y <= maxy; ++y) {
                        for (int x = minx; x <= maxx; ++x) {
                            const float px_x = static_cast<float>(x) + 0.5f;
                            const float px_y = static_cast<float>(y) + 0.5f;
                            const float w0 = ((b.x - px_x) * (c.y - px_y) - (c.x - px_x) * (b.y - px_y)) / area;
                            const float w1 = ((c.x - px_x) * (a.y - px_y) - (a.x - px_x) * (c.y - px_y)) / area;
                            const float w2 = 1.f - w0 - w1;
                            if (w0 < -0.001f || w1 < -0.001f || w2 < -0.001f) continue;
                            const float zz = w0 * va.position[2] + w1 * vb.position[2] + w2 * vc.position[2];
                            const std::size_t pi = static_cast<std::size_t>(y * kPx + x);
                            if (zz < zbuf[pi]) continue;
                            zbuf[pi] = zz;
                            const float uu = w0 * va.uv[0] + w1 * vb.uv[0] + w2 * vc.uv[0];
                            const float vv = w0 * va.uv[1] + w1 * vb.uv[1] + w2 * vc.uv[1];
                            const ImU32 col = tex ? sample_tex(*tex, uu, vv) : IM_COL32(110, 110, 110, 255);
                            if (col != 0) gen[pi] = col;
                        }
                    }
                }
            }
        }
        write_thumb_tga(catalog_thumb_tga(sli), gen, kPx);
        if (g_thumb_pix.size() > 2048) g_thumb_pix.clear();
        g_thumb_pix.emplace(key, std::move(gen));
        }
    }
    const auto& px = g_thumb_pix[key];
    if (paint) {
        blit_thumb(dl, p0, px, kPx);
        dl->AddRect(p0, ImVec2(p0.x + k, p0.y + k), IM_COL32(70, 74, 82, 255), 3.f);
    }
}

const gfx::dx12::CpuRgbaImage* thumb_load_tex(const std::filesystem::path& from_file, const std::string& tex,
                                              const std::filesystem::path& sco_path,
                                              const std::filesystem::path& omsi_root) {
    if (tex.empty()) return nullptr;
    auto resolved = gfx::dx12::resolve_texture_path_day(from_file, tex, omsi_root);
    if (!resolved) resolved = gfx::dx12::resolve_texture_path_day(sco_path, tex, omsi_root);
    if (!resolved) {
        std::error_code ec;
        const auto beside = sco_path.parent_path() / std::filesystem::path(tex);
        if (std::filesystem::is_regular_file(beside, ec)) resolved = beside;
    }
    if (!resolved) return nullptr;
    const std::string tk = resolved->string();
    if (const auto it = g_thumb_tex.find(tk); it != g_thumb_tex.end()) return &it->second;
    if (auto img = gfx::dx12::load_image_cpu_rgba(*resolved)) {
        const auto [ins, ok] = g_thumb_tex.emplace(tk, std::move(*img));
        return &ins->second;
    }
    return nullptr;
}

void sample_tex_rgba(const gfx::dx12::CpuRgbaImage& im, float u, float v, ImU32& col, int& a) {
    if (im.width == 0 || im.height == 0 || im.rgba.empty()) {
        col = IM_COL32(140, 140, 140, 255);
        a = 255;
        return;
    }
    u = u - std::floor(u);
    v = v - std::floor(v);
    if (u < 0.f) u += 1.f;
    if (v < 0.f) v += 1.f;
    const int x = (std::min)(static_cast<int>(u * static_cast<float>(im.width - 1)), static_cast<int>(im.width - 1));
    const int y = (std::min)(static_cast<int>(v * static_cast<float>(im.height - 1)), static_cast<int>(im.height - 1));
    const std::size_t i = (static_cast<std::size_t>(y) * im.width + static_cast<std::size_t>(x)) * 4;
    a = im.rgba[i + 3];
    col = IM_COL32(im.rgba[i], im.rgba[i + 1], im.rgba[i + 2], 255);
}

void raster_iso_tris(std::vector<ImU32>& px, std::vector<float>& zbuf, int kPx, const std::vector<o3d::Vertex>& verts,
                     const std::vector<std::uint32_t>& i0, const std::vector<std::uint32_t>& i1,
                     const std::vector<std::uint32_t>& i2, const std::vector<std::uint16_t>& mats,
                     const std::vector<const gfx::dx12::CpuRgbaImage*>& mat_tex, float yaw) {
    if (verts.empty() || i0.empty()) return;
    const float c = std::cos(yaw), s = std::sin(yaw);
    float xmin = 1.e9f, ymin = 1.e9f, zmin = 1.e9f, xmax = -1.e9f, ymax = -1.e9f, zmax = -1.e9f;
    for (const auto& v : verts) {
        xmin = (std::min)(xmin, v.position[0]);
        xmax = (std::max)(xmax, v.position[0]);
        ymin = (std::min)(ymin, v.position[1]);
        ymax = (std::max)(ymax, v.position[1]);
        zmin = (std::min)(zmin, v.position[2]);
        zmax = (std::max)(zmax, v.position[2]);
    }
    const float cx = 0.5f * (xmin + xmax), cy = 0.5f * (ymin + ymax), cz = 0.5f * (zmin + zmax);
    auto project = [&](const o3d::Vertex& v, float& sx, float& sy, float& depth) {
        const float x = v.position[0] - cx;
        const float y = v.position[1] - cy;
        const float z = v.position[2] - cz;
        const float rx = x * c - y * s;
        const float ry = x * s + y * c;
        sx = (rx - ry) * 0.55f;
        sy = z * 0.72f - (rx + ry) * 0.28f;
        depth = rx + ry + z;
    };
    float pminx = 1.e9f, pminy = 1.e9f, pmaxx = -1.e9f, pmaxy = -1.e9f;
    for (const auto& v : verts) {
        float sx, sy, d;
        project(v, sx, sy, d);
        pminx = (std::min)(pminx, sx);
        pmaxx = (std::max)(pmaxx, sx);
        pminy = (std::min)(pminy, sy);
        pmaxy = (std::max)(pmaxy, sy);
    }
    const float span = (std::max)((std::max)(pmaxx - pminx, pmaxy - pminy), 0.05f);
    const float pad = span * 0.08f;
    pminx -= pad;
    pminy -= pad;
    const float den = span + pad * 2.f;
    auto to_px = [&](float sx, float sy) {
        return ImVec2(((sx - pminx) / den) * static_cast<float>(kPx - 1),
                      (1.f - (sy - pminy) / den) * static_cast<float>(kPx - 1));
    };
    int ntri = 0;
    for (std::size_t t = 0; t < i0.size(); ++t) {
        if (ntri++ > 8000) break;
        if (i0[t] >= verts.size() || i1[t] >= verts.size() || i2[t] >= verts.size()) continue;
        const auto& va = verts[i0[t]];
        const auto& vb = verts[i1[t]];
        const auto& vc = verts[i2[t]];
        const gfx::dx12::CpuRgbaImage* tex =
            t < mats.size() && mats[t] < mat_tex.size() ? mat_tex[mats[t]] : nullptr;
        float ax, ay, d0, bx, by, d1, cx2, cy2, d2;
        project(va, ax, ay, d0);
        project(vb, bx, by, d1);
        project(vc, cx2, cy2, d2);
        const ImVec2 a = to_px(ax, ay), b = to_px(bx, by), cpt = to_px(cx2, cy2);
        const float area = (b.x - a.x) * (cpt.y - a.y) - (cpt.x - a.x) * (b.y - a.y);
        if (std::fabs(area) < 1.e-4f) continue;
        const int minx = (std::max)(0, static_cast<int>(std::floor((std::min)(a.x, (std::min)(b.x, cpt.x)))));
        const int maxx = (std::min)(kPx - 1, static_cast<int>(std::ceil((std::max)(a.x, (std::max)(b.x, cpt.x)))));
        const int miny = (std::max)(0, static_cast<int>(std::floor((std::min)(a.y, (std::min)(b.y, cpt.y)))));
        const int maxy = (std::min)(kPx - 1, static_cast<int>(std::ceil((std::max)(a.y, (std::max)(b.y, cpt.y)))));
        for (int y = miny; y <= maxy; ++y) {
            for (int x = minx; x <= maxx; ++x) {
                const float px_x = static_cast<float>(x) + 0.5f;
                const float px_y = static_cast<float>(y) + 0.5f;
                const float w0 = ((b.x - px_x) * (cpt.y - px_y) - (cpt.x - px_x) * (b.y - px_y)) / area;
                const float w1 = ((cpt.x - px_x) * (a.y - px_y) - (a.x - px_x) * (cpt.y - px_y)) / area;
                const float w2 = 1.f - w0 - w1;
                if (w0 < -0.001f || w1 < -0.001f || w2 < -0.001f) continue;
                const float zz = w0 * d0 + w1 * d1 + w2 * d2;
                const std::size_t pi = static_cast<std::size_t>(y * kPx + x);
                if (zz < zbuf[pi]) continue;
                ImU32 col;
                int al = 255;
                if (tex) {
                    const float uu = w0 * va.uv[0] + w1 * vb.uv[0] + w2 * vc.uv[0];
                    const float vv = w0 * va.uv[1] + w1 * vb.uv[1] + w2 * vc.uv[1];
                    sample_tex_rgba(*tex, uu, vv, col, al);
                    if (al < 16) continue;
                } else {
                    const float lx = 0.45f, ly = -0.55f, lz = 0.7f;
                    const float nx = w0 * va.normal[0] + w1 * vb.normal[0] + w2 * vc.normal[0];
                    const float ny = w0 * va.normal[1] + w1 * vb.normal[1] + w2 * vc.normal[1];
                    const float nz = w0 * va.normal[2] + w1 * vb.normal[2] + w2 * vc.normal[2];
                    float nd = nx * lx + ny * ly + nz * lz;
                    if (nd < 0.f) nd = -nd;
                    nd = 0.35f + 0.65f * (std::min)(1.f, nd);
                    const int g = static_cast<int>(nd * 180.f);
                    col = IM_COL32(g, g, g, 255);
                }
                zbuf[pi] = zz;
                px[pi] = col;
            }
        }
    }
}

void blit_billboard_tex(std::vector<ImU32>& px, int kPx, const gfx::dx12::CpuRgbaImage& im, bool flip_u) {
    for (int y = 0; y < kPx; ++y) {
        for (int x = 0; x < kPx; ++x) {
            float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(kPx);
            const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(kPx);
            if (flip_u) u = 1.f - u;
            ImU32 col;
            int a = 255;
            sample_tex_rgba(im, u, v, col, a);
            if (a < 16) {
                const int r = (col >> IM_COL32_R_SHIFT) & 255;
                const int g = (col >> IM_COL32_G_SHIFT) & 255;
                const int b = (col >> IM_COL32_B_SHIFT) & 255;
                if (r + g + b < 24) continue;
            }
            px[static_cast<std::size_t>(y * kPx + x)] = col;
        }
    }
}

void fill_sco_thumb_view(const std::filesystem::path& sco_path, const std::filesystem::path& omsi_root, int view) {
    constexpr int kPx = 56;
    const std::string key = "sco4|" + omsi::path_text(sco_path) + "|" + std::to_string(view);
    if (g_thumb_pix.find(key) != g_thumb_pix.end()) return;
    std::vector<ImU32> px;
    px.assign(static_cast<std::size_t>(kPx * kPx), IM_COL32(22, 24, 28, 255));
    std::vector<float> zbuf(static_cast<std::size_t>(kPx * kPx), -1.e9f);
    const auto* def = ::omsi::sco::load_sco(sco_path);
    if (def && def->tree.valid && !def->tree.texture.empty() && def->tree.texture.front() == '{') {
        ::omsi::sco::invalidate_sco_cache(sco_path);
        def = ::omsi::sco::load_sco(sco_path);
    }
    const bool is_tree = def && def->tree.valid && !def->tree.texture.empty() && def->tree.texture.front() != '{';
    // .sco that only use .x meshes: older runs cached a blank thumbnail, so never trust the disk cache for them.
    bool x_only = false;
    if (def && !is_tree) {
        bool has_o3d = false, has_x = false;
        for (const auto& m : def->meshes) {
            if (m.mesh_file.empty()) continue;
            (m.is_o3d ? has_o3d : has_x) = true;
        }
        x_only = has_x && !has_o3d;
    }
    if (!is_tree && !x_only && load_thumb_tga(sco_path, kPx, px, view)) {
        // a blank thumbnail cached by an older run (mixed .o3d/.x objects) is made again
        bool blank = true;
        for (const ImU32 cpx : px)
            if (cpx != IM_COL32(22, 24, 28, 255)) { blank = false; break; }
        if (!blank) {
            g_thumb_pix.emplace(key, std::move(px));
            return;
        }
        px.assign(static_cast<std::size_t>(kPx * kPx), IM_COL32(22, 24, 28, 255));
    }
    const float yaw = view == 0 ? 0.f : 3.14159265f;
    bool done = false;
    if (is_tree) {
        if (const auto* tex = thumb_load_tex(sco_path, def->tree.texture, sco_path, omsi_root)) {
            blit_billboard_tex(px, kPx, *tex, view != 0);
            done = true;
        }
    }
    // ([onlyeditor] objects too: entrypoints, help arrows - the editor shows them, so the catalog does)
    if (!done && def && !is_tree) {
        // Every mesh of the most detailed [LOD] band, .o3d and .x together in ONE raster (one bounding box):
        // a .sco mixing both drew only its first .o3d - often an empty LOD stub - and the thumbnail was blank.
        int best_lod = 1 << 30;
        for (const auto& m : def->meshes)
            if (!m.mesh_file.empty()) best_lod = (std::min)(best_lod, m.lod_index);
        std::vector<o3d::Vertex> verts;
        std::vector<std::uint32_t> a, b, c;
        std::vector<std::uint16_t> mt;
        std::vector<const gfx::dx12::CpuRgbaImage*> mat_tex;
        for (const auto& m : def->meshes) {
            if (m.mesh_file.empty() || m.lod_index != best_lod) continue;
            const auto stem = m.mesh_file.stem().wstring();
            if (_wcsicmp(stem.c_str(), L"empty") == 0 || _wcsicmp(stem.c_str(), L"empty_lod") == 0) continue;
            if (m.is_o3d) {
                const auto mapped = o3d::load_mapped_shared(m.mesh_file);
                if (!mapped || !mapped->valid()) continue;
                const auto& mesh = mapped->mesh();
                auto mv = o3d::decrypt_o3d_vertices(mesh);
                for (auto& v : mv) v = o3d::transform_vertex_to_omsi_world(v, mesh);
                const auto mats = o3d::parse_materials_from_section(mesh.materials_raw, mesh.material_count);
                const std::uint32_t base_v = static_cast<std::uint32_t>(verts.size());
                const std::uint16_t base_m = static_cast<std::uint16_t>(mat_tex.size());
                verts.insert(verts.end(), mv.begin(), mv.end());
                for (std::size_t mi = 0; mi < mats.size(); ++mi)
                    mat_tex.push_back(thumb_load_tex(m.mesh_file, mats[mi].texture, sco_path, omsi_root));
                if (mats.empty()) mat_tex.push_back(nullptr);
                auto push = [&](std::uint32_t i0, std::uint32_t i1, std::uint32_t i2, std::uint16_t mat) {
                    if (i0 >= mv.size() || i1 >= mv.size() || i2 >= mv.size()) return;
                    a.push_back(base_v + i0);
                    b.push_back(base_v + i1);
                    c.push_back(base_v + i2);
                    mt.push_back(static_cast<std::uint16_t>(base_m + (mats.empty() ? 0 : (std::min<std::size_t>)(mat, mats.size() - 1))));
                };
                if (mesh.long_triangle_indices) {
                    for (const auto& t : mesh.triangle_u32_span()) push(t.i0, t.i1, t.i2, t.material);
                } else {
                    for (const auto& t : mesh.triangle_u16_span()) push(t.i0, t.i1, t.i2, t.material);
                }
            } else {
                // same lookup order as the spawn path: the .sco folder, its model/ folder, Generic/model
                std::filesystem::path x_path = m.mesh_file;
                auto xf = xfile::load_x_file(x_path);
                if (!xf || xf->parts.empty()) {
                    const auto base = sco_path.parent_path();
                    for (const auto& cand : {base / "model" / x_path.filename(), base / x_path.filename(),
                                             omsi_root / "Sceneryobjects" / "Generic" / "model" / x_path.filename()}) {
                        std::error_code xec;
                        if (!std::filesystem::exists(cand, xec)) continue;
                        xf = xfile::load_x_file(cand);
                        if (xf && !xf->parts.empty()) { x_path = cand; break; }
                    }
                }
                if (!xf) continue;
                for (const auto& part : xf->parts) {
                    if (part.vertices.empty() || part.indices.size() < 3) continue;
                    const std::uint32_t base_v = static_cast<std::uint32_t>(verts.size());
                    const std::uint16_t mat = static_cast<std::uint16_t>(mat_tex.size());
                    verts.insert(verts.end(), part.vertices.begin(), part.vertices.end());
                    mat_tex.push_back(thumb_load_tex(x_path, part.texture_filename, sco_path, omsi_root));
                    for (std::size_t k = 0; k + 2 < part.indices.size(); k += 3) {
                        a.push_back(base_v + part.indices[k]);
                        b.push_back(base_v + part.indices[k + 1]);
                        c.push_back(base_v + part.indices[k + 2]);
                        mt.push_back(mat);
                    }
                }
            }
        }
        if (!a.empty()) {
            // textured first; textures without real alpha reject every texel: then shaded, untextured
            const ImU32 bg = IM_COL32(22, 24, 28, 255);
            raster_iso_tris(px, zbuf, kPx, verts, a, b, c, mt, mat_tex, yaw);
            bool any_paint = false;
            for (const ImU32 cpx : px)
                if (cpx != bg) { any_paint = true; break; }
            if (!any_paint) {
                std::fill(px.begin(), px.end(), bg);
                std::fill(zbuf.begin(), zbuf.end(), -1.e9f);
                const std::vector<const gfx::dx12::CpuRgbaImage*> no_tex(mat_tex.size(), nullptr);
                raster_iso_tris(px, zbuf, kPx, verts, a, b, c, mt, no_tex, yaw);
            }
            done = true;
        }
    }
    if (!(is_tree && !done)) write_thumb_tga(catalog_thumb_tga(sco_path, view), px, kPx);
    if (g_thumb_pix.size() > 2048) g_thumb_pix.clear();
    g_thumb_pix.emplace(key, std::move(px));
}

void draw_sco_iso_preview(const std::filesystem::path& sco_path, bool missing, const std::filesystem::path& omsi_root) {
    constexpr int kPx = 56;
    constexpr float k = static_cast<float>(kPx);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(k * 2.f + 4.f, k));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (missing || sco_path.empty()) {
        dl->AddRectFilled(p0, ImVec2(p0.x + k * 2.f + 4.f, p0.y + k), IM_COL32(18, 20, 22, 255), 3.f);
        dl->AddLine(ImVec2(p0.x + 8.f, p0.y + 8.f), ImVec2(p0.x + k - 8.f, p0.y + k - 8.f), IM_COL32(220, 70, 70, 255),
                    2.f);
        return;
    }
    fill_sco_thumb_view(sco_path, omsi_root, 0);
    fill_sco_thumb_view(sco_path, omsi_root, 1);
    const std::string k0 = "sco4|" + omsi::path_text(sco_path) + "|0";
    const std::string k1 = "sco4|" + omsi::path_text(sco_path) + "|1";
    // find(), not operator[]: a cache clear between the two fills must not insert an empty (blank) thumb.
    if (const auto it0 = g_thumb_pix.find(k0); it0 != g_thumb_pix.end()) blit_thumb(dl, p0, it0->second, kPx);
    if (const auto it1 = g_thumb_pix.find(k1); it1 != g_thumb_pix.end())
        blit_thumb(dl, ImVec2(p0.x + k + 4.f, p0.y), it1->second, kPx);
    dl->AddRect(p0, ImVec2(p0.x + k, p0.y + k), IM_COL32(70, 74, 82, 255), 3.f);
    dl->AddRect(ImVec2(p0.x + k + 4.f, p0.y), ImVec2(p0.x + k * 2.f + 4.f, p0.y + k), IM_COL32(70, 74, 82, 255), 3.f);
}


void draw_catalog_list(EditorSidePanel& panel, const std::vector<EditorListItem>& items, const bool spline_tool,
                       int& last_sel, const std::filesystem::path& omsi_root) {
    auto& st = panel.imgui_state();
    if (ImGui::Button(st.catalog_groups_tree ? i18n::tr_lit("Lista A-Z") : i18n::tr_lit("Agrupados por groups"), ImVec2(-FLT_MIN, 0.f)))
        st.catalog_groups_tree = !st.catalog_groups_tree;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(i18n::tr_lit("Lista entera A-Z, o la misma lista agrupada por [groups]."));
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##cat_f", i18n::tr_lit("Filtrar catalogo..."), st.catalog_filter,
                             IM_ARRAYSIZE(st.catalog_filter));
    const int cur_sel = spline_tool ? panel.catalog_spline_sel() : panel.catalog_object_sel();
    std::vector<int> visible;
    visible.reserve(items.size());
    for (int i = 0; i < static_cast<int>(items.size()); ++i) {
        if (item_matches_filter(st.catalog_filter, items[static_cast<std::size_t>(i)])) visible.push_back(i);
    }
    auto item_group_name = [&](const EditorListItem& it) -> std::string {
        if (!it.group.empty()) return it.group;
        const auto parent = omsi::path_text(it.path.parent_path().filename());
        return parent.empty() ? "(raiz)" : parent;
    };
    if (st.catalog_groups_tree) {
        std::map<std::string, std::vector<int>> by_group;
        for (int i : visible) by_group[item_group_name(items[static_cast<std::size_t>(i)])].push_back(i);
        for (auto& kv : by_group) {
            std::sort(kv.second.begin(), kv.second.end(), [&](int a, int b) {
                return items[static_cast<std::size_t>(a)].label < items[static_cast<std::size_t>(b)].label;
            });
        }
        // Groups: text-only + clipper (no thumbs — avoids freeze after folder scan).
        ImGui::BeginChild("##cat_groups", ImVec2(-FLT_MIN, 140.f), true);
        for (auto& kv : by_group) {
            ImGui::PushID(kv.first.c_str());
            if (ImGui::TreeNodeEx(kv.first.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth)) {
                ImGuiListClipper clip;
                clip.Begin(static_cast<int>(kv.second.size()));
                while (clip.Step()) {
                    for (int row = clip.DisplayStart; row < clip.DisplayEnd; ++row) {
                        const int i = kv.second[static_cast<std::size_t>(row)];
                        const auto& it = items[static_cast<std::size_t>(i)];
                        ImGui::PushID(i);
                        const bool selected = (i == cur_sel);
                        if (ImGui::Selectable(it.label.c_str(), selected)) {
                            panel.imgui_set_catalog_sel(spline_tool ? -1 : i, spline_tool ? i : -1);
                            last_sel = i;
                            EditorSidePanelState ev{};
                            if (spline_tool)
                                ev.select_catalog_spline = i;
                            else
                                ev.select_catalog_object = i;
                            fire(panel, ev);
                        }
                        ImGui::PopID();
                    }
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        ImGui::EndChild();
        (void)omsi_root;
        return;
    }
    const ImGuiTableFlags tflags = ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                   ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("##catalog", 3, tflags, ImVec2(-1.f, 140.f))) return;
    ImGui::TableSetupColumn("prev", ImGuiTableColumnFlags_WidthFixed, spline_tool ? 58.f : 120.f);
    ImGui::TableSetupColumn("Item", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("fav", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight() + 4.f);
    ImGui::TableHeadersRow();
    if (cur_sel != st.catalog_list_scrolled_sel) {
        int vis_row = -1;
        for (int r = 0; r < static_cast<int>(visible.size()); ++r) {
            if (visible[static_cast<std::size_t>(r)] == cur_sel) {
                vis_row = r;
                break;
            }
        }
        if (vis_row >= 0) {
            const float item_h = 56.f + ImGui::GetStyle().CellPadding.y * 2.f;
            ImGui::SetScrollY((std::max)(0.f, vis_row * item_h - ImGui::GetWindowHeight() * 0.35f));
        }
        st.catalog_list_scrolled_sel = cur_sel;
    }
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(visible.size()));
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            const int i = visible[static_cast<std::size_t>(row)];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            const auto& it = items[static_cast<std::size_t>(i)];
            const bool selected = (i == cur_sel);
            if (spline_tool)
                draw_sli_topdown_preview(it.path, it.preview_length, it.preview_radius, it.missing, omsi_root);
            else
                draw_sco_iso_preview(it.path, it.missing, omsi_root);
            ImGui::TableSetColumnIndex(1);
            ImGui::PushID(i);
            if (ImGui::Selectable(it.label.c_str(), selected, 0, ImVec2(0.f, 56.f))) {
                panel.imgui_set_catalog_sel(spline_tool ? -1 : i, spline_tool ? i : -1);
                last_sel = i;
                EditorSidePanelState ev{};
                if (spline_tool)
                    ev.select_catalog_spline = i;
                else
                    ev.select_catalog_object = i;
                fire(panel, ev);
            }
            ImGui::TableSetColumnIndex(2);
            bool is_fav = false;
            for (const auto& cat : st.fav_cats)
                for (const auto& fi : cat.items)
                    if (fi.spline == spline_tool && fi.path == it.path) is_fav = true;
            if (fav_star_button("fav", is_fav)) {
                if (is_fav) {
                    for (auto& cat : st.fav_cats)
                        cat.items.erase(std::remove_if(cat.items.begin(), cat.items.end(),
                                                       [&](const EditorSidePanelImGuiState::CatalogFavItem& fi) {
                                                           return fi.spline == spline_tool && fi.path == it.path;
                                                       }),
                                        cat.items.end());
                } else {
                    if (st.fav_cats.empty()) {
                        EditorSidePanelImGuiState::CatalogFavCat c;
                        c.name = "General";
                        st.fav_cats.push_back(std::move(c));
                        st.fav_active_cat = 0;
                    }
                    const int ci = (std::clamp)(st.fav_active_cat, 0, static_cast<int>(st.fav_cats.size()) - 1);
                    EditorSidePanelImGuiState::CatalogFavItem fi;
                    fi.spline = spline_tool;
                    fi.path = it.path;
                    fi.label = it.label;
                    st.fav_cats[static_cast<std::size_t>(ci)].items.push_back(std::move(fi));
                }
                save_editor_docks(st);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(is_fav ? i18n::tr_lit("Quitar de favoritos") : i18n::tr_lit("Agregar a favoritos (seccion activa)"));
            ImGui::PopID();
        }
    }
    ImGui::EndTable();
}

void draw_instance_list(EditorSidePanel& panel, const std::vector<EditorListItem>& items, const bool spline_tool,
                        const int selected_map_index, int& last_sel, const std::filesystem::path& omsi_root) {
    auto& st = panel.imgui_state();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##inst_f", i18n::tr_lit("Buscar instancia por nombre/SCO/SLI…"), st.instance_filter, IM_ARRAYSIZE(st.instance_filter));
    const auto visible_for=[&](){ std::vector<int> v; for(int i=0;i<(int)items.size();++i) if(item_matches_filter(st.instance_filter,items[(size_t)i])) v.push_back(i); return v; };
    auto visible=visible_for();
    const std::int32_t selected_id = spline_tool ? (panel.imgui_spline_edit().valid ? panel.imgui_spline_edit().id : 0) :
        (panel.imgui_object_edit().valid ? panel.imgui_object_edit().id : 0);
    ImGuiListClipper clipper; clipper.Begin((int)visible.size());
    if(ImGui::BeginTable("##instances",1,ImGuiTableFlags_BordersInnerV|ImGuiTableFlags_RowBg|ImGuiTableFlags_ScrollY,ImVec2(-1.f,120.f))){
        ImGui::TableSetupColumn(i18n::tr_lit("Instancia")); ImGui::TableHeadersRow();
        while(clipper.Step()) for(int row=clipper.DisplayStart;row<clipper.DisplayEnd;++row){int i=visible[(size_t)row]; const auto& it=items[(size_t)i]; bool sel=(selected_id>0?it.id==selected_id:it.map_index==selected_map_index); ImGui::TableNextRow();ImGui::TableSetColumnIndex(0);std::string lab=it.label; if((spline_tool?st.hidden_spline_ids.count(it.id):st.hidden_object_ids.count(it.id)))lab+=" (oculto)"; ImGui::PushID(i); if(ImGui::Selectable(lab.c_str(),sel,0,ImVec2(0,0))){last_sel=i;EditorSidePanelState ev{};if(spline_tool){ev.select_spline_index=it.map_index;ev.select_spline_id=it.id;}else{ev.select_object_index=it.map_index;ev.select_object_id=it.id;}fire(panel,ev);} if(!spline_tool&&it.path.extension()==".sco"&&ImGui::BeginDragDropSource()){auto u=omsi::path_text(it.path);ImGui::SetDragDropPayload("SCO_PATH",u.c_str(),u.size()+1);ImGui::TextUnformatted(it.label.c_str());ImGui::EndDragDropSource();} if(ImGui::IsItemHovered())draw_hover_catalog_preview(st,it.path,spline_tool,omsi_root); ImGui::PopID();}
        ImGui::EndTable();
    }
}
std::filesystem::path catalog_initial_dir(const std::filesystem::path& omsi_root, const bool for_spline) {
    if (!omsi_root.empty()) {
        const auto d = omsi_root / (for_spline ? "Splines" : "Sceneryobjects");
        std::error_code ec;
        if (std::filesystem::is_directory(d, ec)) return d;
        if (std::filesystem::is_directory(omsi_root, ec)) return omsi_root;
    }
    std::error_code ec;
    return std::filesystem::current_path(ec);
}

void draw_catalog_actions(EditorSidePanel& panel, const bool spline_tool, const std::filesystem::path& omsi_root) {
    const auto initial = catalog_initial_dir(omsi_root, spline_tool);
    if (ImGui::BeginTable("##cat_act", 2, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextColumn();
        if (ImGui::Button(i18n::tr_lit("Cargar .sco/.sli"), ImVec2(-FLT_MIN, 0.f))) {
            panel.imgui_state().catalog_browser_to_spray = false;
            catalog_browser_open(panel.imgui_state(), initial, spline_tool, false);
        }
        ImGui::TableNextColumn();
        if (ImGui::Button(i18n::tr_lit("Escanear carpeta"), ImVec2(-FLT_MIN, 0.f))) {
            panel.imgui_state().catalog_browser_to_spray = false;
            catalog_browser_open(panel.imgui_state(), initial, spline_tool, true);
        }
        ImGui::TableNextColumn();
        if (ImGui::Button(i18n::tr_lit("Abrir carpeta"), ImVec2(-FLT_MIN, 0.f))) {
            EditorSidePanelState ev{};
            ev.open_catalog_folder = true;
            fire(panel, ev);
        }
        ImGui::TableNextColumn();
        if (ImGui::Button(i18n::tr_lit("Ver en .map"), ImVec2(-FLT_MIN, 0.f))) {
            EditorSidePanelState ev{};
            ev.open_instance_map = true;
            fire(panel, ev);
        }
        ImGui::TableNextColumn();
        if (ImGui::Button(i18n::tr_lit("Editar archivo"), ImVec2(-FLT_MIN, 0.f))) {
            EditorSidePanelState ev{};
            ev.edit_catalog_file = true;
            fire(panel, ev);
        }
        ImGui::TableNextColumn();
        if (ImGui::Button(i18n::tr_lit("Recargar"), ImVec2(-FLT_MIN, 0.f))) {
            EditorSidePanelState ev{};
            ev.reload_catalog_file = true;
            fire(panel, ev);
        }
        ImGui::EndTable();
    }
    if (spline_tool && ImGui::Button(i18n::tr_lit("Creador splines"), ImVec2(-FLT_MIN, 0.f))) {
        EditorSidePanelState ev{};
        ev.open_spline_creator = true;
        fire(panel, ev);
    }
}

void object_item_card(const EditorObjectProps& obj) {
    const char* full = obj.sco_name.c_str();
    const char* base = full;
    for (const char* p = full; *p; ++p) {
        if (*p == '\\' || *p == '/') base = p + 1;
    }
    ImGui::BeginChild("##item_card", ImVec2(0.f, 52.f), ImGuiChildFlags_Borders);
    ImGui::TextDisabled("item");
    if (obj.valid) {
        ImGui::SameLine();
        ImGui::Text("#%d", obj.id);
    }
    ImGui::TextUnformatted(obj.valid && base[0] ? base : "—");
    ImGui::EndChild();
}

void spline_item_card(const EditorSplineProps& sp) {
    ImGui::BeginChild("##sli_card", ImVec2(0.f, 52.f), ImGuiChildFlags_Borders);
    ImGui::TextDisabled("sli");
    if (sp.valid) {
        ImGui::SameLine();
        ImGui::Text("#%d", sp.id);
    }
    ImGui::TextUnformatted(sp.valid && !sp.sli_name.empty() ? sp.sli_name.c_str() : "—");
    ImGui::EndChild();
}

void draw_object_props_single(EditorSidePanel& panel) {
    auto& obj = panel.imgui_object_edit();
    if (!obj.valid) {
        ImGui::TextDisabled(i18n::tr_lit("Sin objeto seleccionado — clic en el 3D o en la lista"));
        return;
    }
    auto& st = panel.imgui_state();
    bool dirty = false;
    ImGui::TextUnformatted(i18n::tr_lit("Position"));
    if (ImGui::BeginTable("##obj_xyz", 3, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextColumn();
        dirty |= spin_float(st, "X", "object.x", &obj.x, 0.05f, 0.f, 0.f, "%.3f", false, 0.f, 22.f, false);
        ImGui::TableNextColumn();
        dirty |= spin_float(st, "Y", "object.y", &obj.y, 0.05f, 0.f, 0.f, "%.3f", false, 0.f, 22.f, false);
        ImGui::TableNextColumn();
        dirty |= spin_float(st, "Z", "object.z", &obj.z, 0.05f, 0.f, 0.f, "%.3f", false, 0.f, 22.f, false);
        ImGui::EndTable();
    }
    ImGui::TextUnformatted(i18n::tr_lit("Rotation"));
    if (ImGui::BeginTable("##obj_rot", 3, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextColumn();
        dirty |= spin_float(st, "X", "object.pitch", &obj.pitch, 0.2f, 0.f, 0.f, "%.3f", false, 0.f, 22.f);
        ImGui::TableNextColumn();
        dirty |= spin_float(st, "Y", "object.rot", &obj.rot, 0.2f, 0.f, 0.f, "%.3f", false, 0.f, 22.f);
        ImGui::TableNextColumn();
        dirty |= spin_float(st, "Z", "object.bank", &obj.bank, 0.2f, 0.f, 0.f, "%.3f", false, 0.f, 22.f);
        ImGui::EndTable();
    }
    float parent_f = static_cast<float>(st.parent_id_field);
    float attach_f = static_cast<float>(st.attach_point_field);
    if (ImGui::BeginTable("##obj_ids", 4, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("pb", ImGuiTableColumnFlags_WidthFixed, 72.f);
        ImGui::TableSetupColumn("pi", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("ab", ImGuiTableColumnFlags_WidthFixed, 72.f);
        ImGui::TableSetupColumn("ai", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableNextColumn();
        if (ImGui::Button(i18n::tr_lit("Parent"), ImVec2(-FLT_MIN, 0.f))) {
            EditorSidePanelState ev{};
            ev.parent_to_object = true;
            fire(panel, ev);
        }
        ImGui::TableNextColumn();
        if (spin_float(st, i18n::tr_lit("Parent"), "object.parent", &parent_f, 1.f, 0.f, 999999.f, "%.0f", true)) {
            st.parent_id_field = static_cast<std::int32_t>(parent_f);
            obj.varparent_id = st.parent_id_field;
            dirty = true;
        }
        ImGui::TableNextColumn();
        if (ImGui::Button(i18n::tr_lit("Attach"), ImVec2(-FLT_MIN, 0.f))) {
            EditorSidePanelState ev{};
            ev.attach_to_object = true;
            ev.attach_point = st.attach_point_field;
            fire(panel, ev);
        }
        ImGui::TableNextColumn();
        if (spin_float(st, i18n::tr_lit("Attach"), "object.attach", &attach_f, 1.f, 0.f, 64.f, "%.0f", true)) {
            st.attach_point_field = static_cast<int>(attach_f);
            dirty = true;
        }
        ImGui::EndTable();
    }
    if (ImGui::Button(i18n::tr_lit("Attach spl"), ImVec2(-FLT_MIN, 0.f))) {
        EditorSidePanelState ev{};
        ev.attach_to_spline = true;
        fire(panel, ev);
    }
    // [object] + [spline_terrain_align]  OR  SA AlignTerrain (Z on heightfield)
    if (ImGui::Checkbox(i18n::tr_lit("AlignTerrain"), &obj.spline_align_terrain)) dirty = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("[object]/SA: escribe [spline_terrain_align] en el .map\n"
                          "Z sobre heightfield. Hueco en terreno solo si existe .sco.hole\n"
                          "(OMSI TComplObj_RefreshTerrainHoles) — sin .hole no se corta el césped");
    if (obj.attach_kind == EditorObjectProps::AttachKind::SplineAttach) {
        if (ImGui::Checkbox(i18n::tr_lit("Tangential to spline"), &obj.spline_tangential)) dirty = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("[splineAttachement] body: 1=si 0=no (tras repeat_range)");
        dirty |= spin_float(st, i18n::tr_lit("Repeat step"), "object.rep_step", &obj.spline_repeat_step, 0.1f, 0.f, 500.f);
        dirty |= spin_float(st, i18n::tr_lit("Repeat range"), "object.rep_range", &obj.spline_repeat_range, 0.1f, 0.f, 500.f);
        if (obj.parent_spline_id > 0) {
            ImGui::TextDisabled("Spline padre id=%d", obj.parent_spline_id);
            if (ImGui::Button(i18n::tr_lit("Seleccionar spline"))) {
                panel.queue_imgui_tool_tab(static_cast<int>(EditorTool::Spline));
                EditorSidePanelState ev{};
                ev.select_spline_id = obj.parent_spline_id;
                fire(panel, ev);
            }
        }
    }
    if (obj.text_slot_count > 0) {
        ImGui::Separator();
        ImGui::Text("Lineas extra (%d)", obj.text_slot_count);
        while (static_cast<int>(obj.strings.size()) < obj.text_slot_count) obj.strings.emplace_back();
        for (int i = 0; i < obj.text_slot_count; ++i) {
            char buf[256]{};
            const char* label = buf;
            if (i < static_cast<int>(obj.string_labels.size()) && !obj.string_labels[static_cast<std::size_t>(i)].empty())
                label = obj.string_labels[static_cast<std::size_t>(i)].c_str();
            else
                std::snprintf(buf, sizeof(buf), "%d", i + 1);
            std::string& slot = obj.strings[static_cast<std::size_t>(i)];
            if (i == 0 && !obj.traffic_light_names.empty()) {
                int cur = 0;
                try {
                    cur = std::stoi(slot);
                } catch (...) {
                    cur = 0;
                }
                if (cur < 0) cur = 0;
                if (cur >= static_cast<int>(obj.traffic_light_names.size()))
                    cur = static_cast<int>(obj.traffic_light_names.size()) - 1;
                if (ImGui::BeginCombo(label, obj.traffic_light_names[static_cast<std::size_t>(cur)].c_str())) {
                    for (int n = 0; n < static_cast<int>(obj.traffic_light_names.size()); ++n) {
                        const bool sel = (n == cur);
                        const std::string item = std::to_string(n) + ": " + obj.traffic_light_names[static_cast<std::size_t>(n)];
                        if (ImGui::Selectable(item.c_str(), sel)) {
                            slot = std::to_string(n);
                            dirty = true;
                        }
                        if (sel) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
            } else {
                char edit[512]{};
                std::snprintf(edit, sizeof(edit), "%s", slot.c_str());
                if (ImGui::InputText(label, edit, sizeof(edit))) {
                    slot = edit;
                    dirty = true;
                }
            }
        }
    }
    if (dirty) {
        // Always arm undo: spin first frame, checkbox, combo, or discrete step.
        // Continuous scrub after first frame keeps arm=false so one undo entry per gesture.
        const bool arm = g_spin_gesture_began || !ImGui::IsAnyItemActive() || g_spin_gesture_ended;
        fire_apply_object(panel, arm);
        g_spin_gesture_began = false;
        if (g_spin_gesture_ended) g_spin_gesture_ended = false;
    }
}

void draw_spline_props_single(EditorSidePanel& panel, std::size_t spline_group_count) {
    auto& sp = panel.imgui_spline_edit();
    if (!sp.valid) {
        ImGui::TextDisabled(i18n::tr_lit("Sin spline seleccionada — clic en el 3D o en la lista"));
        return;
    }
    auto& st = panel.imgui_state();
    bool dirty = false;
    if (st.spline_pending_place > 0) {
        // Edited splines stay as wireframe (no white untextured strips) until they are placed.
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.55f, 0.25f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.68f, 0.30f, 1.f));
        char lab[64];
        std::snprintf(lab, sizeof(lab), "%s (%d)  [Enter]", i18n::tr_lit("Colocar"), st.spline_pending_place);
        if (ImGui::Button(lab, ImVec2(-FLT_MIN, 0.f))) {
            EditorSidePanelState ev{};
            ev.spline_place_commit = true;
            fire(panel, ev);
        }
        ImGui::PopStyleColor(2);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", i18n::tr_lit("Aplica la malla final con texturas a las splines editadas "
                                                 "(siguen en wireframe hasta colocarlas)."));
    }
    // Misma lógica que objetos: label corto (22px) + StretchSame → anchos iguales sin deformar
    constexpr float kLab = 22.f;
    const ImGuiTableFlags tf = ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.f, 1.f));
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(3.f, 1.f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(3.f, 2.f));

    if (ImGui::BeginTable("##spl_xyz", 3, tf)) {
        ImGui::TableNextColumn();
        dirty |= spin_float(st, "X", "spline.x", &sp.x, 0.05f, 0.f, 0.f, "%.3f", false, 0.f, kLab, false);
        ImGui::TableNextColumn();
        dirty |= spin_float(st, "Y", "spline.y", &sp.y, 0.05f, 0.f, 0.f, "%.3f", false, 0.f, kLab, false);
        ImGui::TableNextColumn();
        dirty |= spin_float(st, "Z", "spline.z", &sp.z, 0.05f, 0.f, 0.f, "%.3f", false, 0.f, kLab, false);
        ImGui::EndTable();
    }
    // Largo / Rotación / Radio apilados a ancho completo (como pide el usuario)
    dirty |= spin_float(st, i18n::tr_lit("Largo"), "spline.length", &sp.length, 0.1f, 0.f, 1.e6f, "%.3f", false, 0.f, 70.f);
    dirty |= spin_float(st, "Rotación", "spline.rot", &sp.rot, 0.2f, -360.f, 360.f, "%.2f", false, 0.f, 70.f);
    {
        const float x_w = ImGui::GetFrameHeight() + 6.f;
        dirty |= spin_float(st, i18n::tr_lit("Radio"), "spline.radius", &sp.radius, 0.1f, 0.f, 0.f, "%.3f", false, x_w, 70.f);
        ImGui::SameLine(0.f, 2.f);
        if (ImGui::Button("x##rad0", ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()))) {
            sp.radius = 0.f;
            dirty = true;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(i18n::tr_lit("Radio = 0 (recto)"));
    }
    if (sp.length < 0.f) {
        sp.length = 0.f;
        dirty = true;
    }
    if (ImGui::BeginTable("##spl_g", 2, tf)) {
        ImGui::TableNextColumn();
        dirty |= spin_float(st, "Grad0", "spline.start_grad", &sp.start_grad, 0.05f, 0.f, 0.f, "%.3f", false, 0.f, 70.f, false);
        ImGui::TableNextColumn();
        dirty |= spin_float(st, "Grad1", "spline.end_grad", &sp.end_grad, 0.05f, 0.f, 0.f, "%.3f", false, 0.f, 70.f, false);
        ImGui::EndTable();
    }
    if (ImGui::BeginTable("##spl_dh", 2, tf)) {
        ImGui::TableNextColumn();
        if (ImGui::Checkbox(i18n::tr_lit("ΔH"), &sp.use_delta_h)) dirty = true;
        ImGui::TableNextColumn();
        ImGui::BeginDisabled(!sp.use_delta_h);
        dirty |= spin_float(st, "Valor dH", "spline.delta_h", &sp.delta_h, 0.05f, 0.f, 0.f, "%.3f", false, 0.f, 70.f, false);
        ImGui::EndDisabled();
        ImGui::EndTable();
    }
    if (ImGui::BeginTable("##spl_sk", 2, tf)) {
        ImGui::TableNextColumn();
        dirty |= spin_float(st, "Skew0", "spline.skew_start", &sp.skew_start, 0.05f, 0.f, 0.f, "%.3f", false, 0.f, 70.f, false);
        ImGui::TableNextColumn();
        dirty |= spin_float(st, "Skew1", "spline.skew_end", &sp.skew_end, 0.05f, 0.f, 0.f, "%.3f", false, 0.f, 70.f, false);
        ImGui::EndTable();
    }
    if (ImGui::BeginTable("##spl_ct", 2, tf)) {
        ImGui::TableNextColumn();
        dirty |= spin_float(st, "Cant0", "spline.cant_start", &sp.cant_start, 0.01f, 0.f, 0.f, "%.3f", false, 0.f, 70.f, false);
        ImGui::TableNextColumn();
        dirty |= spin_float(st, "Cant1", "spline.cant_end", &sp.cant_end, 0.01f, 0.f, 0.f, "%.3f", false, 0.f, 70.f, false);
        ImGui::EndTable();
    }
    if (ImGui::BeginTable("##spl_mir", 2, tf)) {
        ImGui::TableNextColumn();
        if (ImGui::Checkbox(i18n::tr_lit("Mirror"), &sp.mirror)) dirty = true;
        ImGui::TableNextColumn();
        ImGui::BeginDisabled(spline_group_count < 2);
        if (ImGui::Button(i18n::tr_lit("Mirror grupal"), ImVec2(-FLT_MIN, 0.f))) {
            EditorSidePanelState ev{};
            ev.apply_spline_mirror_only = true;
            fire(panel, ev);
        }
        ImGui::EndDisabled();
        ImGui::EndTable();
    }
    if (ImGui::BeginTable("##spl_rmax", 2, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextColumn();
        dirty |= spin_float(st, "R max", "spline.max_radius", &st.spline_max_radius, 1.f, 1.f, 2000.f, "%.3f",
                            false, 0.f, 70.f, false);
        ImGui::TableNextColumn();
        if (ImGui::Button(i18n::tr_lit("Complete"), ImVec2(-FLT_MIN, 0.f))) {
            EditorSidePanelState ev{};
            ev.spline_complete = true;
            fire(panel, ev);
        }
        ImGui::EndTable();
    }
    if (ImGui::BeginTable("##spl_cut", 2, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextColumn();
        if (ImGui::Button(i18n::tr_lit("Split"), ImVec2(-FLT_MIN, 0.f))) {
            EditorSidePanelState ev{};
            ev.spline_split = true;
            ev.spline_split_cuts = st.spline_cut_count;
            fire(panel, ev);
        }
        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputInt(i18n::tr_lit("numbers of cuts"), &st.spline_cut_count);
        if (st.spline_cut_count < 1) st.spline_cut_count = 1;
        if (st.spline_cut_count > 32) st.spline_cut_count = 32;
        ImGui::EndTable();
    }
    {
        // Must sit before the dirty→fire gate so combo changes reach edit_splines / .map patch.
        static const char* kAlignTerrain[] = {
            "No",
            "Yes, both THs",
            "Yes, only start TH",
            "Yes, only end TH",
            "Yes, no THs",
        };
        int al = sp.terrain_align;
        if (al < 0) al = 0;
        if (al > 4) al = 4;
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::Combo(i18n::tr_lit("AlignTerrain"), &al, kAlignTerrain, IM_ARRAYSIZE(kAlignTerrain))) {
            sp.terrain_align = al;
            dirty = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("[spline_terrain_align_2] — estampa heightfield + corte del suelo\n"
                             "Hueco: [terrainholeprofile] o, si falta, trough auto desde [profile]\n"
                             "(OMSI load 0x5adc52 / openOMSI terrain_hole_profiles)");
    }
    if (dirty) {
        const bool began = g_spin_gesture_began;
        const bool ended = g_spin_gesture_ended;
        const bool dragging = ImGui::IsAnyItemActive() && began && !ended;
        // Arm undo on first spin frame, checkbox, combo, or release — never mid-scrub.
        const bool arm = began || !ImGui::IsAnyItemActive() || ended;
        EditorSidePanelState ev{};
        ev.spline_edit = sp;
        ev.spline_edit.valid = true;
        if (arm) panel.arm_spline_grad_undo();
        if (dragging) {
            ev.spline_grad_preview = true;
            ev.undo_before_spline = arm;
        } else {
            ev.spline_grad_preview_end = true;
            ev.undo_before_spline = arm;
        }
        fire(panel, ev);
        g_spin_gesture_began = false;
        g_spin_gesture_ended = false;
    }
    if (ImGui::BeginTable("##spl_act", 2, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextColumn();
        if (ImGui::Button(i18n::tr_lit("Align start"), ImVec2(-FLT_MIN, 0.f))) {
            EditorSidePanelState ev{};
            ev.spline_align_start = true;
            fire(panel, ev);
        }
        ImGui::TableNextColumn();
        if (ImGui::Button(i18n::tr_lit("Align end"), ImVec2(-FLT_MIN, 0.f))) {
            EditorSidePanelState ev{};
            ev.spline_align_end = true;
            fire(panel, ev);
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar(3);
}

void draw_tile_tab(EditorSidePanel& panel, const EditorTilePageInfo& info, const char* map_name,
                   const char* map_friendly, const bool view_only) {
    auto& st = panel.imgui_state();
    auto& geo = st.tile_geo;
    if (!st.map_names_seeded) {
        if (map_name) std::snprintf(st.map_name_buf, sizeof(st.map_name_buf), "%s", map_name);
        if (map_friendly) std::snprintf(st.map_friendly_buf, sizeof(st.map_friendly_buf), "%s", map_friendly);
        else if (map_name) std::snprintf(st.map_friendly_buf, sizeof(st.map_friendly_buf), "%s", map_name);
        st.map_names_seeded = true;
    }
    // Geo keys / aerial flags are per-map prefs: save as soon as a field is committed (they used to be saved
    // only by the download buttons, so ref lat/lon and API keys were lost on restart).
    auto save_geo = [&]() {
        EditorSidePanelState ev{};
        ev.save_geo_settings = true;
        fire(panel, ev);
    };
    const float label_w = 150.f;
    // Label on the left, control on the right, same x for every row of a section.
    auto row = [&](const char* label) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine(label_w);
        ImGui::SetNextItemWidth(-FLT_MIN);
    };
    // Full-width buttons in a two-column grid (no SameLine chains that overflow at narrow widths).
    auto grid_button = [&](const char* label, bool enabled, const char* tip_text, auto&& on_click) {
        ImGui::TableNextColumn();
        ImGui::BeginDisabled(!enabled);
        if (ImGui::Button(label, ImVec2(-FLT_MIN, 0.f)) && enabled) on_click();
        ImGui::EndDisabled();
        if (tip_text && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", tip_text);
    };
    const bool can_edit = !view_only;

    // ── Tile activo ──────────────────────────────────────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader(i18n::tr_lit("Tile activo"), ImGuiTreeNodeFlags_DefaultOpen)) {
        if (info.valid) {
            ImGui::Text("Tile (%d, %d)   %.1f m", info.grid_x, info.grid_y, info.tile_size_m);
            if (!info.hint.empty()) ImGui::TextWrapped("%s", info.hint.c_str());
            if (!info.wgs84_summary.empty()) ImGui::TextDisabled("%s", info.wgs84_summary.c_str());
        }
        ImGui::TextDisabled("%s", i18n::tr_lit("RMB centra la camara. Flecha verde = celda vacia."));
        ImGui::Spacing();
        // Neighbour pad: 3x3 grid, the centre is the active tile.
        auto pad_btn = [&](const char* label, bool can_create, std::int32_t dx, std::int32_t dy) {
            ImGui::TableNextColumn();
            const bool active = info.tile_in_map && can_create && can_edit;
            if (active) {
                ImGui::PushStyleColor(ImGuiCol_Button, theme::rgb(theme::success, 0.85f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::rgb(theme::success));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::rgb(theme::success, 0.70f));
                ImGui::PushStyleColor(ImGuiCol_Text, theme::rgb(theme::crust));
            } else {
                ImGui::PushStyleColor(ImGuiCol_Button, theme::rgb(theme::surface));
                ImGui::PushStyleColor(ImGuiCol_Text, theme::rgb(theme::disabled));
            }
            ImGui::BeginDisabled(!active);
            if (ImGui::Button(label, ImVec2(56.f, 40.f)) && active) {
                EditorSidePanelState ev{};
                ev.create_tile = true;
                ev.create_tile_dx = dx;
                ev.create_tile_dy = dy;
                fire(panel, ev);
            }
            ImGui::EndDisabled();
            ImGui::PopStyleColor(active ? 4 : 2);
        };
        if (ImGui::BeginTable("##tile_pad", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoHostExtendX)) {
            ImGui::TableSetupColumn("a", ImGuiTableColumnFlags_WidthFixed, 56.f);
            ImGui::TableSetupColumn("b", ImGuiTableColumnFlags_WidthFixed, 56.f);
            ImGui::TableSetupColumn("c", ImGuiTableColumnFlags_WidthFixed, 56.f);
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); // empty corner
            pad_btn("N##tileN", info.can_create_n, 0, 1);
            ImGui::TableNextRow();
            pad_btn("O##tileW", info.can_create_w, -1, 0);
            ImGui::TableNextColumn();
            ImGui::TextDisabled("tile"); // the active tile, centred
            pad_btn("E##tileE", info.can_create_e, 1, 0);
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); // empty corner
            pad_btn("S##tileS", info.can_create_s, 0, -1);
            ImGui::EndTable();
        }
        ImGui::TextDisabled("%s", i18n::tr_lit("Crear tile: botones verdes (N/S/E/O) junto al tile activo."));
        ImGui::Spacing();
        if (ImGui::BeginTable("##tile_actions", 2, ImGuiTableFlags_SizingStretchSame)) {
            grid_button(i18n::tr_lit("Abrir .map"), info.tile_in_map, nullptr, [&]() {
                EditorSidePanelState ev{};
                ev.request_open_tile_map = true;
                fire(panel, ev);
            });
            grid_button(i18n::tr_lit("Borrar tile"), can_edit && info.tile_in_map, "Elimina el tile activo del mapa.",
                        [&]() {
                            EditorSidePanelState ev{};
                            ev.delete_tile = true;
                            fire(panel, ev);
                        });
            if (info.can_convert)
                grid_button(i18n::tr_lit("Convertir a world coordinates"), can_edit, nullptr, [&]() {
                    EditorSidePanelState ev{};
                    ev.convert_world_coords = true;
                    fire(panel, ev);
                });
            ImGui::EndTable();
        }
    }

    // ── Nombre del mapa ───────────────────────────────────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader(i18n::tr_lit("Nombre del mapa (global.cfg)"))) {
        ImGui::BeginDisabled(!can_edit);
        row("[name]");
        ImGui::InputText("##map_name", st.map_name_buf, sizeof(st.map_name_buf));
        row("[friendlyname]");
        ImGui::InputText("##map_friendly", st.map_friendly_buf, sizeof(st.map_friendly_buf));
        ImGui::Spacing();
        if (ImGui::Button(i18n::tr_lit("Guardar nombre en global.cfg"), ImVec2(-FLT_MIN, 0.f))) {
            EditorSidePanelState ev{};
            ev.apply_map_names = true;
            ev.map_name_value = st.map_name_buf;
            ev.map_friendly_value = st.map_friendly_buf;
            fire(panel, ev);
        }
        ImGui::EndDisabled();
    }

    // ── Origen geografico y mapas (aerial + DEM) ──────────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader(i18n::tr_lit("Origen geografico y mapas"))) {
        ImGui::TextWrapped("%s", "World: bounds del tile. Mapa classic: pone lat/lon global (obligatorio para "
                                 "descargas).");
        ImGui::Spacing();
        float ref_lat = static_cast<float>(geo.ref_lat);
        float ref_lon = static_cast<float>(geo.ref_lon);
        row(i18n::tr_lit("Ref lat"));
        if (ImGui::DragFloat("##ref_lat", &ref_lat, 0.0001f, -90.f, 90.f, "%.4f")) geo.ref_lat = ref_lat;
        if (ImGui::IsItemDeactivatedAfterEdit()) save_geo();
        row(i18n::tr_lit("Ref lon"));
        if (ImGui::DragFloat("##ref_lon", &ref_lon, 0.0001f, -180.f, 180.f, "%.4f")) geo.ref_lon = ref_lon;
        if (ImGui::IsItemDeactivatedAfterEdit()) save_geo();

        ImGui::Separator();
        row(i18n::tr_lit("Satelite"));
        {
            int sat = static_cast<int>(geo.satellite_provider);
            if (ImGui::Combo("##sat", &sat, kSatProviders, IM_ARRAYSIZE(kSatProviders))) {
                geo.satellite_provider = static_cast<map::SatelliteProvider>(sat);
                save_geo();
            }
        }
        char api[512]{};
        std::snprintf(api, sizeof(api), "%s", geo.google_api_key.c_str());
        row(i18n::tr_lit("Google API key"));
        if (ImGui::InputText("##gkey", api, sizeof(api), ImGuiInputTextFlags_Password)) geo.google_api_key = api;
        if (ImGui::IsItemDeactivatedAfterEdit()) save_geo();
        char bing[256]{};
        std::snprintf(bing, sizeof(bing), "%s", geo.bing_api_key.c_str());
        row(i18n::tr_lit("Bing API key"));
        if (ImGui::InputText("##bkey", bing, sizeof(bing), ImGuiInputTextFlags_Password)) geo.bing_api_key = bing;
        if (ImGui::IsItemDeactivatedAfterEdit()) save_geo();
        char mt[256]{};
        std::snprintf(mt, sizeof(mt), "%s", geo.maptiler_api_key.c_str());
        row(i18n::tr_lit("MapTiler key"));
        if (ImGui::InputText("##mkey", mt, sizeof(mt), ImGuiInputTextFlags_Password)) geo.maptiler_api_key = mt;
        if (ImGui::IsItemDeactivatedAfterEdit()) save_geo();

        ImGui::Separator();
        row(i18n::tr_lit("Aerial en tile"));
        if (ImGui::Checkbox("##aerial_vis", &st.tile_aerial_visible)) save_geo();
        ImGui::SameLine();
        ImGui::TextDisabled("%s", i18n::tr_lit("(mostrar ortofoto en el terreno)"));
        {
            char ar[64]{};
            std::snprintf(ar, sizeof(ar), "%d px", map::aerial_resolution_label_px(st.aerial_resolution_pos));
            row(i18n::tr_lit("Resolucion aerial"));
            if (ImGui::SliderInt("##aerial_res", &st.aerial_resolution_pos, 0, 5, ar)) save_geo();
        }
        ImGui::BeginDisabled(!can_edit);
        if (ImGui::Button(i18n::tr("btn.download_aerial"), ImVec2(-FLT_MIN, 0.f))) {
            EditorSidePanelState ev{};
            ev.download_tile_aerial = true;
            fire(panel, ev);
        }
        ImGui::EndDisabled();

        ImGui::Separator();
        {
            static const map::ElevationProvider kDtmMap[] = {
                map::ElevationProvider::Fabdem, map::ElevationProvider::CopernicusGlo30,
                map::ElevationProvider::OpenMeteo, map::ElevationProvider::OpenTopoData,
                map::ElevationProvider::NasaSrtm3, map::ElevationProvider::Terrarium,
                map::ElevationProvider::OpenElevation, map::ElevationProvider::Mapterhorn};
            int dtm_ui = 0;
            for (int i = 0; i < IM_ARRAYSIZE(kDtmMap); ++i)
                if (geo.elevation_provider == kDtmMap[i]) {
                    dtm_ui = i;
                    break;
                }
            row(i18n::tr_lit("DTM (elevacion)"));
            if (ImGui::Combo("##dtm", &dtm_ui, kDtmProviders, IM_ARRAYSIZE(kDtmProviders)))
                if (dtm_ui >= 0 && dtm_ui < IM_ARRAYSIZE(kDtmMap)) {
                    geo.elevation_provider = kDtmMap[dtm_ui];
                    save_geo();
                }
        }
        ImGui::BeginDisabled(!can_edit);
        if (ImGui::Button(i18n::tr("btn.download_dem"), ImVec2(-FLT_MIN, 0.f))) {
            EditorSidePanelState ev{};
            ev.apply_tile_dtm = true;
            fire(panel, ev);
        }
        if (ImGui::Button(i18n::tr("btn.import_dem"), ImVec2(-FLT_MIN, 0.f))) {
            wchar_t file[MAX_PATH]{};
            OPENFILENAMEW ofn{};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = nullptr;
            ofn.lpstrFilter = L"DEM / heightmap\0*.tif;*.tiff;*.png;*.bmp;*.jpg;*.jpeg\0All\0*.*\0";
            ofn.lpstrFile = file;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
            if (GetOpenFileNameW(&ofn)) {
                EditorSidePanelState ev{};
                ev.import_tile_dem = true;
                ev.import_tile_dem_path = file;
                fire(panel, ev);
            }
        }
        ImGui::EndDisabled();
        ImGui::TextWrapped("%s", i18n::tr("dem.hint"));
    }

    // ── Bake del tile ─────────────────────────────────────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader(i18n::tr_lit("Bake del tile (.o3d / .sco / .fbx)"))) {
        ImGui::TextWrapped("Sceneryobjects\\bake_tiles\\<mapa>\\  (.sco)  model\\ (.o3d)  texture\\  fbx\\");
        ImGui::Spacing();
        static int bake_parts = 1;
        row(i18n::tr_lit("Partes"));
        ImGui::SliderInt("##bake_parts", &bake_parts, 1, 5);
        auto& ist = panel.imgui_state();
        const bool busy = ist.tile_bake_busy;
        const bool has_o3d = ist.tile_bake_has_o3d;
        const bool has_fbx = ist.tile_bake_has_fbx;
        if (busy) {
            ImGui::TextColored(ImVec4(1.f, 0.85f, 0.2f, 1.f), "%s", i18n::tr_lit("Bake en curso..."));
            if (ist.busy_progress >= 0.f) ImGui::ProgressBar(ist.busy_progress, ImVec2(-1, 0));
            if (!ist.busy_detail.empty()) ImGui::TextWrapped("%s", ist.busy_detail.c_str());
        }
        if (ImGui::BeginTable("##bake_grid", 2, ImGuiTableFlags_SizingStretchSame)) {
            grid_button(i18n::tr_lit("Bake in .o3d"), can_edit && !busy && !has_o3d, nullptr, [&]() {
                const int ans = MessageBoxW(nullptr,
                    L"Bakear este tile a .o3d/.sco?\r\nSe escribira en Sceneryobjects\\bake_tiles\\<mapa>\\",
                    L"Tile bake O3D", MB_YESNOCANCEL | MB_ICONQUESTION);
                if (ans == IDYES) {
                    EditorSidePanelState ev{};
                    ev.tile_bake_parts = bake_parts;
                    ev.tile_bake_run = true;
                    fire(panel, ev);
                }
            });
            grid_button(i18n::tr_lit("Replace tile to .sco"), can_edit && !busy && has_o3d, nullptr, [&]() {
                EditorSidePanelState ev{};
                ev.tile_bake_replace = true;
                fire(panel, ev);
            });
            grid_button(i18n::tr_lit("Clean bake .o3d"), can_edit && !busy && has_o3d, nullptr, [&]() {
                const int ans = MessageBoxW(nullptr,
                    L"Borrar el bake .o3d/.sco de este tile?\r\nNo deshace el Replace en el .map.",
                    L"Clean bake O3D", MB_YESNOCANCEL | MB_ICONWARNING);
                if (ans == IDYES) {
                    EditorSidePanelState ev{};
                    ev.tile_bake_clean_o3d = true;
                    fire(panel, ev);
                }
            });
            grid_button(i18n::tr_lit("Bake in .fbx"), can_edit && !busy && !has_fbx, nullptr, [&]() {
                const int ans = MessageBoxW(nullptr,
                    L"Exportar bake FBX de este tile?\r\nCarpeta: Sceneryobjects\\bake_tiles\\<mapa>\\fbx\\",
                    L"Tile bake FBX", MB_YESNOCANCEL | MB_ICONQUESTION);
                if (ans == IDYES) {
                    EditorSidePanelState ev{};
                    ev.tile_bake_parts = bake_parts;
                    ev.tile_bake_fbx = true;
                    fire(panel, ev);
                }
            });
            grid_button(i18n::tr_lit("Clean .fbx"), can_edit && !busy && has_fbx, nullptr, [&]() {
                const int ans = MessageBoxW(nullptr, L"Borrar la carpeta FBX de este tile?", L"Clean FBX",
                                            MB_YESNOCANCEL | MB_ICONWARNING);
                if (ans == IDYES) {
                    EditorSidePanelState ev{};
                    ev.tile_bake_clean_fbx = true;
                    fire(panel, ev);
                }
            });
            grid_button(i18n::tr_lit("Abrir carpeta del bake"), !busy && !ist.tile_bake_folder.empty(), nullptr,
                        [&]() {
                            EditorSidePanelState ev{};
                            ev.tile_bake_open_folder = true;
                            fire(panel, ev);
                        });
            ImGui::EndTable();
        }
        if (has_o3d && !busy)
            ImGui::TextColored(ImVec4(0.4f, 1.f, 0.4f, 1.f), "%s", i18n::tr_lit("O3D bake presente: Replace disponible"));
        if (has_fbx && !busy)
            ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.f, 1.f), "%s", i18n::tr_lit("FBX bake presente"));
    }
}

// Layer thumbnail: texture downsampled once into a small pixel block and cached by path.
void draw_layer_thumb(ImDrawList* dl, const ImVec2& p0, const std::filesystem::path& tex, const std::filesystem::path& omsi_root,
                      int px_size) {
    static std::map<std::string, std::vector<ImU32>> cache;
    dl->AddRect(p0, ImVec2(p0.x + static_cast<float>(px_size), p0.y + static_cast<float>(px_size)), IM_COL32(70, 74, 82, 255), 2.f);
    const std::string key = omsi::path_text(tex) + "|" + std::to_string(px_size);
    auto it = cache.find(key);
    if (it == cache.end()) {
        std::vector<ImU32> px(static_cast<std::size_t>(px_size * px_size), IM_COL32(40, 42, 46, 255));
        std::filesystem::path resolved;
        std::error_code ec;
        for (const auto& cand : {tex, omsi_root / tex, omsi_root / "Texture" / tex.filename(), omsi_root / "texture" / tex.filename()}) {
            if (!cand.empty() && std::filesystem::is_regular_file(cand, ec)) { resolved = cand; break; }
        }
        if (!resolved.empty()) {
            if (auto img = gfx::dx12::load_image_cpu_rgba(resolved)) {
                for (int y = 0; y < px_size; ++y)
                    for (int x = 0; x < px_size; ++x) {
                        const std::uint32_t sx = static_cast<std::uint32_t>(x) * img->width / static_cast<std::uint32_t>(px_size);
                        const std::uint32_t sy = static_cast<std::uint32_t>(y) * img->height / static_cast<std::uint32_t>(px_size);
                        const std::uint8_t* q = &img->rgba[(static_cast<std::size_t>(sy) * img->width + sx) * 4];
                        px[static_cast<std::size_t>(y * px_size + x)] = IM_COL32(q[0], q[1], q[2], 255);
                    }
            }
        }
        it = cache.emplace(key, std::move(px)).first;
    }
    blit_thumb_size(dl, p0, it->second, px_size, px_size);
}

void draw_terrain_tab(EditorSidePanel& panel, const MapEditorImGuiArgs& args_water) {
    auto& st = panel.imgui_state();
    auto& brush = st.terrain_brush;
    const bool ro = args_water.view_only; // Solo visualizar: nothing in the terrain may be edited
    auto tip = [](const char* t) {
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", t);
    };
    auto sec = [](const char* t) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.45f, 1.f), "%s", t);
        ImGui::Separator();
    };
    const auto is_layer_mode = [](TerrainBrushModeUi m) {
        return m == TerrainBrushModeUi::PaintLayer || m == TerrainBrushModeUi::RubberLayer;
    };
    const float label_w = 130.f;
    auto row = [&](const char* label) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine(label_w);
        ImGui::SetNextItemWidth(-FLT_MIN);
    };

    if (ro) {
        ImGui::TextColored(ImVec4(1.f, 0.62f, 0.3f, 1.f), "%s",
                           i18n::tr_lit("Solo visualizacion: el terreno no se puede editar."));
    }
    // Each sub-section sets terrain_active_sub while it is drawn; the 3D brush only acts for its own section.
    st.terrain_active_sub = -1;
    if (!ImGui::BeginTabBar("##terrain_sub")) return;

    // ── Terreno: alturas ────────────────────────────────────────────────────────────────────────────────
    if (ImGui::BeginTabItem(i18n::tr_lit("Terreno"))) {
        st.terrain_active_sub = 0;
        if (is_layer_mode(brush.mode)) brush.mode = TerrainBrushModeUi::Higher; // layer paint lives in Capas
        ImGui::BeginDisabled(ro);
        sec("Herramienta de altura");
        static const TerrainBrushModeUi kHeightModes[4] = {TerrainBrushModeUi::Higher, TerrainBrushModeUi::Lower,
                                                          TerrainBrushModeUi::SetHeight, TerrainBrushModeUi::FitMesh};
        static const char* kHeightLabels[4] = {"Subir", "Bajar", "Altura fija", "Ajustar a SLI/SCO"};
        int mode_idx = 0;
        for (int i = 0; i < 4; ++i)
            if (brush.mode == kHeightModes[i]) mode_idx = i;
        row("Modo");
        if (ImGui::Combo("##brush_mode", &mode_idx, kHeightLabels, IM_ARRAYSIZE(kHeightLabels)))
            brush.mode = kHeightModes[mode_idx];
        tip("Subir / Bajar: modifica la altura con el pincel (Shift invierte).\n"
            "Altura fija: lleva el terreno a la altura indicada.\n"
            "Ajustar a SLI/SCO: nivela el terreno bajo las splines y objetos.");

        sec("Pincel");
        row("Tamano (m)");
        ImGui::DragFloat("##brush_sz", &brush.brush_size_m, 0.5f, 1.f, 500.f, "%.1f m");
        tip("Radio de influencia del pincel en metros.");
        row("Velocidad");
        ImGui::SliderFloat("##brush_sp", &brush.speed, 0.1f, 60.f, "%.2f m/s", ImGuiSliderFlags_Logarithmic);
        tip("Velocidad real de subida/bajada en el centro del pincel, en metros por segundo, con el click "
            "sostenido. Es independiente de los FPS. Escala logaritmica: arriba de 10 m/s se mueve en pasos mas grandes.");
        if (brush.mode == TerrainBrushModeUi::SetHeight) {
            row("Altura fija (m)");
            ImGui::DragFloat("##brush_h", &brush.set_height, 0.05f, -2000.f, 5000.f, "%.2f m");
            tip("Altura usada por el modo 'Altura fija'.");
        }

        sec("Vertices");
        ImGui::Checkbox(i18n::tr_lit("Editar vertices (61x61)"), &st.terrain_vertex_edit);
        tip("Muestra los vertices del tile actual para ajustarlos de a uno con clicks en ▲/▼.");
        ImGui::EndDisabled();
        ImGui::EndTabItem();
    }

    // ── Agua ────────────────────────────────────────────────────────────────────────────────────────────
    if (ImGui::BeginTabItem(i18n::tr_lit("Agua"))) {
        st.terrain_active_sub = 1;
        if (is_layer_mode(brush.mode)) brush.mode = TerrainBrushModeUi::Higher;
        ImGui::BeginDisabled(ro);
        sec("Agua");
        ImGui::Checkbox("Mostrar agua", &st.water_visible);
        tip("Muestra u oculta el agua de todos los tiles en el editor.");
        if (!args_water.water_tile_valid) {
            ImGui::TextDisabled("La camara no esta sobre ningun tile.");
        } else {
            ImGui::Text("Tile (%d, %d)", args_water.water_tile_gx, args_water.water_tile_gy);
            const int tile_id = args_water.water_tile_gx * 10000 + args_water.water_tile_gy;
            if (st.water_ui_tile != tile_id) {
                for (int i = 0; i < 4; ++i) st.water_h_ui[i] = args_water.water_has ? args_water.water_h[i] : 0.f;
                st.water_ui_tile = tile_id;
            }
            if (!args_water.water_has) {
                ImGui::TextWrapped("Este tile no tiene agua. Crear escribe el archivo .water del tile (se guarda con Save).");
                row("Altura inicial (m)");
                ImGui::DragFloat("##wh_new", &st.water_h_ui[0], 0.05f, -500.f, 5000.f, "%.2f m");
                tip("Altura del plano de agua. Se crea un plano (4 esquinas iguales); despues podes inclinarlo.");
                if (ImGui::Button("Crear archivo de agua", ImVec2(-FLT_MIN, 0.f))) {
                    EditorSidePanelState ev{};
                    ev.water_create = true;
                    for (int i = 0; i < 4; ++i) ev.water_h[i] = st.water_h_ui[0];
                    fire(panel, ev);
                    for (int i = 1; i < 4; ++i) st.water_h_ui[i] = st.water_h_ui[0];
                }
                tip("Crea el agua de este tile: archivo <tile>.map.water + seccion [water] en el .map.");
            } else {
                sec("Alturas de las esquinas (m)");
                static const char* names[4] = {"SO", "SE", "NO", "NE"};
                bool chg = false;
                // 2x2 grid of corner heights, aligned in columns (no SameLine overflow).
                if (ImGui::BeginTable("##water_corners", 2, ImGuiTableFlags_SizingStretchSame)) {
                    for (int i = 0; i < 4; ++i) {
                        ImGui::TableNextColumn();
                        char id[24];
                        std::snprintf(id, sizeof(id), "%s##wc%d", names[i], i);
                        ImGui::SetNextItemWidth(-FLT_MIN);
                        chg |= ImGui::DragFloat(id, &st.water_h_ui[i], 0.05f, -500.f, 5000.f, "%.2f");
                    }
                    ImGui::EndTable();
                }
                tip("Altura de cada esquina del plano de agua (SO = suroeste ... NE = noreste). Valores distintos inclinan el agua.");
                if (ImGui::Button("Igualar todas a SO", ImVec2(-FLT_MIN, 0.f))) {
                    for (int i = 1; i < 4; ++i) st.water_h_ui[i] = st.water_h_ui[0];
                    chg = true;
                }
                if (chg) {
                    EditorSidePanelState ev{};
                    ev.water_set = true;
                    for (int i = 0; i < 4; ++i) ev.water_h[i] = st.water_h_ui[i];
                    fire(panel, ev);
                }
                if (ImGui::Button("Eliminar agua de este tile", ImVec2(-FLT_MIN, 0.f))) {
                    EditorSidePanelState ev{};
                    ev.water_delete = true;
                    fire(panel, ev);
                    st.water_ui_tile = -1;
                }
                tip("Quita el agua del tile (se borra el .water y la seccion [water] al guardar).");
            }
        }
        ImGui::EndDisabled();
        ImGui::EndTabItem();
    }

    // ── Capas: pintar / goma / lazo de capa de suelo ───────────────────────────────────────────────────
    if (ImGui::BeginTabItem(i18n::tr_lit("Capas"))) {
        st.terrain_active_sub = 2;
        // This tab only paints: never fall back to the height brush while it is open (that raised the terrain).
        if (!is_layer_mode(brush.mode)) brush.mode = TerrainBrushModeUi::PaintLayer;
        ImGui::BeginDisabled(ro);
        sec("Pincel de capa");
        if (ImGui::RadioButton("Pintar capa", brush.mode == TerrainBrushModeUi::PaintLayer))
            brush.mode = TerrainBrushModeUi::PaintLayer;
        tip("Pinta la capa activa sobre el terreno con el pincel (tamano y velocidad de la pestana Terreno no aplican).");
        ImGui::SameLine();
        if (ImGui::RadioButton("Goma capa", brush.mode == TerrainBrushModeUi::RubberLayer))
            brush.mode = TerrainBrushModeUi::RubberLayer;
        tip("Borra la capa activa con el pincel.");
        ImGui::Checkbox("Lazo##terrain_lasso", &st.terrain_lasso);
        tip("Pinta o borra una capa dibujando un area con el mouse (al soltar se aplica).");
        if (st.terrain_lasso) {
            static const char* actions[2] = {"Pintar area", "Borrar area"};
            ImGui::SameLine();
            ImGui::SetNextItemWidth(160.f);
            ImGui::Combo("##lasso_action", &st.terrain_lasso_action, actions, 2);
        }

        sec("Capas de suelo");
        const auto& layers = st.ground_layer_labels;
        if (layers.empty()) {
            ImGui::TextDisabled("Este mapa no tiene capas de suelo cargadas.");
        } else {
            brush.ground_layer = (std::clamp)(brush.ground_layer, 0, static_cast<int>(layers.size()) - 1);
            ImGui::BeginChild("##layer_list", ImVec2(-FLT_MIN, 240.f), true);
            for (int i = 0; i < static_cast<int>(layers.size()); ++i) {
                ImGui::PushID(i);
                const ImVec2 p0 = ImGui::GetCursorScreenPos();
                const bool sel = brush.ground_layer == i;
                if (ImGui::Selectable("##layer", sel, 0, ImVec2(0.f, 56.f))) brush.ground_layer = i;
                const bool hov = ImGui::IsItemHovered();
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const bool has_tex = i < static_cast<int>(args_water.ground_layer_diffuse.size());
                if (has_tex)
                    draw_layer_thumb(dl, ImVec2(p0.x + 4.f, p0.y + 4.f),
                                     args_water.ground_layer_diffuse[static_cast<std::size_t>(i)], args_water.omsi_root,
                                     48);
                const float tx = p0.x + 60.f;
                dl->AddText(ImVec2(tx, p0.y + 6.f), IM_COL32(240, 240, 240, 255),
                            (std::to_string(i) + "  " + layers[static_cast<std::size_t>(i)]).c_str());
                if (has_tex)
                    dl->AddText(ImVec2(tx, p0.y + 30.f), IM_COL32(150, 155, 165, 255),
                                omsi::path_utf8(args_water.ground_layer_diffuse[static_cast<std::size_t>(i)].filename()).c_str());
                if (hov && has_tex) {
                    ImGui::SetTooltip("%s\nescala %.1f m",
                                      args_water.ground_layer_diffuse[static_cast<std::size_t>(i)].generic_string().c_str(),
                                      i < static_cast<int>(args_water.ground_layer_scale.size())
                                          ? static_cast<double>(args_water.ground_layer_scale[static_cast<std::size_t>(i)])
                                          : 0.0);
                }
                ImGui::PopID();
            }
            ImGui::EndChild();
            tip("Capa activa: es la que usa el pincel y el lazo.");
        }
        // Three equal-width buttons in a row: no overlap at any panel width.
        if (ImGui::BeginTable("##layer_btns", 3, ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableNextColumn();
            if (ImGui::Button(i18n::tr_lit("Nueva capa"), ImVec2(-FLT_MIN, 0.f))) {
                st.layer_dlg_index = -1;
                std::snprintf(st.layer_dlg_main, sizeof(st.layer_dlg_main), "texture\\new.bmp");
                std::snprintf(st.layer_dlg_detail, sizeof(st.layer_dlg_detail), "texture\\noise_low.bmp");
                st.layer_dlg_main_rep = 1;
                st.layer_dlg_det_rep = 40;
                st.layer_dlg_open = true;
            }
            tip("Crea una capa de suelo nueva (textura principal + detalle + repeticiones).");
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(layers.empty());
            if (ImGui::Button(i18n::tr_lit("Editar capa"), ImVec2(-FLT_MIN, 0.f))) {
                const std::size_t li = static_cast<std::size_t>((std::max)(0, brush.ground_layer));
                st.layer_dlg_index = brush.ground_layer;
                std::snprintf(st.layer_dlg_main, sizeof(st.layer_dlg_main), "%s",
                              li < args_water.ground_layer_diffuse.size() ? omsi::path_text(args_water.ground_layer_diffuse[li]).c_str() : "");
                std::snprintf(st.layer_dlg_detail, sizeof(st.layer_dlg_detail), "%s",
                              li < args_water.ground_layer_detail.size() ? omsi::path_text(args_water.ground_layer_detail[li]).c_str() : "");
                st.layer_dlg_main_rep = li < args_water.ground_layer_main_rep.size() ? args_water.ground_layer_main_rep[li] : 1;
                st.layer_dlg_det_rep = li < args_water.ground_layer_scale.size() ? static_cast<int>(args_water.ground_layer_scale[li]) : 60;
                st.layer_dlg_open = true;
            }
            ImGui::EndDisabled();
            tip("Abre el dialogo de la capa activa: texturas principal y detalle, repeticiones y previsualizacion.");
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(layers.empty());
            if (ImGui::Button(i18n::tr_lit("Borrar capa"), ImVec2(-FLT_MIN, 0.f))) {
                EditorSidePanelState ev{};
                ev.ground_delete_layer = true;
                fire(panel, ev);
            }
            ImGui::EndDisabled();
            tip("Elimina la capa activa del mapa.");
            ImGui::EndTable();
        }
        if (st.layer_dlg_open) {
            ImGui::OpenPopup("Editar capa de suelo");
            st.layer_dlg_open = false;
        }
        ImGui::SetNextWindowSize(ImVec2(560.f, 0.f), ImGuiCond_Appearing);
        if (ImGui::BeginPopupModal("Editar capa de suelo", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            if (st.layer_dlg_index >= 0)
                ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.45f, 1.f), "Capa %d", st.layer_dlg_index);
            else
                ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.45f, 1.f), "Capa nueva");
            ImGui::Separator();
            auto browse = [&](const char* id, char* buf, std::size_t cap) {
                if (!ImGui::Button(id, ImVec2(34.f, 0.f))) return;
                OPENFILENAMEW ofn{};
                wchar_t file[MAX_PATH]{};
                ofn.lStructSize = sizeof(ofn);
                ofn.lpstrFilter = L"Texturas (*.bmp;*.dds;*.png;*.tga;*.jpg)\0*.bmp;*.dds;*.png;*.tga;*.jpg\0Todos\0*.*\0";
                ofn.lpstrFile = file;
                ofn.nMaxFile = MAX_PATH;
                ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
                if (!GetOpenFileNameW(&ofn)) return;
                std::filesystem::path abs = file, rel = abs;
                std::error_code ec;
                if (!args_water.omsi_root.empty()) {
                    auto r = std::filesystem::relative(abs.lexically_normal(), args_water.omsi_root.lexically_normal(), ec);
                    if (!ec && !r.empty() && r.native().find(L"..") == std::wstring::npos) rel = r;
                }
                std::snprintf(buf, cap, "%s", omsi::path_text(rel).c_str());
            };
            // Aligned table: label | path (stretch) | browse | repeats. Every row has the same columns.
            if (ImGui::BeginTable("##layer_paths", 4, ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("lbl", ImGuiTableColumnFlags_WidthFixed, 74.f);
                ImGui::TableSetupColumn("path", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("br", ImGuiTableColumnFlags_WidthFixed, 40.f);
                ImGui::TableSetupColumn("rep", ImGuiTableColumnFlags_WidthFixed, 110.f);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted("Principal");
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-FLT_MIN);
                ImGui::InputText("##lmain", st.layer_dlg_main, sizeof(st.layer_dlg_main));
                ImGui::TableNextColumn();
                browse("...##bm", st.layer_dlg_main, sizeof(st.layer_dlg_main));
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-FLT_MIN);
                ImGui::InputInt("##lmr", &st.layer_dlg_main_rep, 1, 5);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted("Detalle");
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-FLT_MIN);
                ImGui::InputText("##ldet", st.layer_dlg_detail, sizeof(st.layer_dlg_detail));
                ImGui::TableNextColumn();
                browse("...##bd", st.layer_dlg_detail, sizeof(st.layer_dlg_detail));
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-FLT_MIN);
                ImGui::InputInt("##ldr", &st.layer_dlg_det_rep, 1, 10);
                ImGui::EndTable();
            }
            ImGui::TextDisabled("Rep. principal = veces que la textura se repite; rep. detalle = escala del detalle por tile.");
            st.layer_dlg_main_rep = (std::clamp)(st.layer_dlg_main_rep, 0, 255);
            st.layer_dlg_det_rep = (std::clamp)(st.layer_dlg_det_rep, 0, 255);
            ImGui::Separator();
            ImGui::TextUnformatted("Previsualizacion");
            const std::filesystem::path pm = st.layer_dlg_main, pd = st.layer_dlg_detail;
            // Thumbnails side by side: each one gets its own fixed-size slot.
            if (ImGui::BeginTable("##layer_prev", 3, ImGuiTableFlags_SizingFixedFit)) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("Principal");
                ImGui::TableNextColumn();
                ImGui::TextDisabled("Detalle");
                ImGui::TableNextColumn();
                ImGui::TextDisabled("Mosaico (repeticion principal)");
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                {
                    const ImVec2 p0 = ImGui::GetCursorScreenPos();
                    ImGui::Dummy(ImVec2(112.f, 112.f));
                    draw_layer_thumb(ImGui::GetWindowDrawList(), p0, pm, args_water.omsi_root, 112);
                }
                ImGui::TableNextColumn();
                {
                    const ImVec2 p0 = ImGui::GetCursorScreenPos();
                    ImGui::Dummy(ImVec2(112.f, 112.f));
                    draw_layer_thumb(ImGui::GetWindowDrawList(), p0, pd, args_water.omsi_root, 112);
                }
                ImGui::TableNextColumn();
                {
                    const ImVec2 p0 = ImGui::GetCursorScreenPos();
                    ImGui::Dummy(ImVec2(112.f, 112.f));
                    const int n = (std::clamp)(st.layer_dlg_main_rep, 1, 6);
                    const float cell = 112.f / static_cast<float>(n);
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    dl->PushClipRect(p0, ImVec2(p0.x + 112.f, p0.y + 112.f), true);
                    for (int ty = 0; ty < n; ++ty)
                        for (int tx = 0; tx < n; ++tx)
                            draw_layer_thumb(dl, ImVec2(p0.x + static_cast<float>(tx) * cell, p0.y + static_cast<float>(ty) * cell),
                                             pm, args_water.omsi_root, (std::max)(4, static_cast<int>(cell)));
                    dl->PopClipRect();
                }
                ImGui::EndTable();
            }
            ImGui::Separator();
            const bool ok_path = st.layer_dlg_main[0] != 0;
            ImGui::BeginDisabled(!ok_path);
            if (ImGui::Button("OK", ImVec2(110.f, 0.f))) {
                EditorSidePanelState ev{};
                ev.ground_apply = true;
                ev.ground_apply_index = st.layer_dlg_index;
                ev.ground_apply_main = st.layer_dlg_main;
                ev.ground_apply_detail = st.layer_dlg_detail;
                ev.ground_apply_main_rep = st.layer_dlg_main_rep;
                ev.ground_apply_det_rep = st.layer_dlg_det_rep;
                fire(panel, ev);
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Cancelar", ImVec2(110.f, 0.f))) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::EndDisabled();
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
}

// Square icon buttons (icon drawn with ImDrawList; label underneath). No external SVG dependency.
static bool sco_icon_button(const char* label, const char* id, int icon, float size, bool enabled = true) {
    ImGui::PushID(id);
    if (!enabled) ImGui::BeginDisabled();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::Button("##icon", ImVec2(size, size + 16.f));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c(p.x + size * 0.5f, p.y + size * 0.52f);
    const ImU32 col = ImGui::GetColorU32(enabled ? ImGuiCol_Text : ImGuiCol_TextDisabled);
    const float s = size * 0.30f;
    switch (icon) {
    case 0: // plus
        dl->AddLine(ImVec2(c.x - s, c.y), ImVec2(c.x + s, c.y), col, 2.f);
        dl->AddLine(ImVec2(c.x, c.y - s), ImVec2(c.x, c.y + s), col, 2.f);
        break;
    case 1: // trash
        dl->AddRect(ImVec2(c.x - s * 0.7f, c.y - s * 0.4f), ImVec2(c.x + s * 0.7f, c.y + s), col, 2.f, 0, 2.f);
        dl->AddLine(ImVec2(c.x - s, c.y - s * 0.6f), ImVec2(c.x + s, c.y - s * 0.6f), col, 2.f);
        break;
    case 2: // save
        dl->AddRect(ImVec2(c.x - s, c.y - s), ImVec2(c.x + s, c.y + s), col, 2.f, 0, 2.f);
        dl->AddRectFilled(ImVec2(c.x - s * 0.5f, c.y - s), ImVec2(c.x + s * 0.5f, c.y - s * 0.2f), col);
        break;
    case 3: // cancel
        dl->AddLine(ImVec2(c.x - s, c.y - s), ImVec2(c.x + s, c.y + s), col, 2.f);
        dl->AddLine(ImVec2(c.x - s, c.y + s), ImVec2(c.x + s, c.y - s), col, 2.f);
        break;
    case 4: // undo
        dl->AddLine(ImVec2(c.x - s, c.y), ImVec2(c.x + s, c.y - s * 0.6f), col, 2.f);
        dl->AddTriangleFilled(ImVec2(c.x - s, c.y), ImVec2(c.x - s * 0.4f, c.y - s * 0.4f),
                              ImVec2(c.x - s * 0.4f, c.y + s * 0.4f), col);
        break;
    case 5: // redo
        dl->AddLine(ImVec2(c.x + s, c.y), ImVec2(c.x - s, c.y - s * 0.6f), col, 2.f);
        dl->AddTriangleFilled(ImVec2(c.x + s, c.y), ImVec2(c.x + s * 0.4f, c.y - s * 0.4f),
                              ImVec2(c.x + s * 0.4f, c.y + s * 0.4f), col);
        break;
    default: break;
    }
    const ImVec2 ts = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(p.x + (size - ts.x) * 0.5f, p.y + size + 1.f), col, label);
    if (!enabled) ImGui::EndDisabled();
    ImGui::PopID();
    return pressed;
}

int traffic_rule_index(const EditorPathRuleProps& p) {
    // OMSI SetRule case 6: priority byte 0..255 (default 0x80). Brush radios are not path_index.
    if (p.crossed_out || p.density <= 1.e-4f) return 2;
    if (p.has_speed) return 0;
    if (p.no_overtake) return 1;
    if (p.has_priority) return (p.priority > 128.f) ? 8 : 9;
    if (p.no_cars) return 10;
    if (p.trucks) return 11;
    if (p.buses) return 12;
    if (p.density < 0.15f) return 3;
    if (p.density < 0.35f) return 4;
    if (p.density < 0.65f) return 5;
    if (p.density >= 1.2f) return 7;
    return 6;
}

void apply_traffic_rule_index(EditorPathRuleProps& p, int idx) {
    p.has_speed = p.has_priority = p.no_cars = p.buses = p.trucks = p.no_overtake = false;
    p.crossed_out = false;
    p.density = 1.f;
    p.traffic_brush = idx;
    switch (idx) {
    case 0:
        p.has_speed = true;
        break;
    case 1:
        p.no_overtake = true;
        break;
    case 2:
        p.crossed_out = true;
        p.density = 0.f;
        break;
    case 3:
        p.density = 0.1f;
        break;
    case 4:
        p.density = 0.25f;
        break;
    case 5:
        p.density = 0.5f;
        break;
    case 6:
        p.density = 1.f;
        break;
    case 7:
        p.density = 1.5f;
        break;
    case 8:
        p.has_priority = true;
        p.priority = 160.f;
        break;
    case 9:
        p.has_priority = true;
        p.priority = 96.f;
        break;
    case 10:
        p.no_cars = true;
        break;
    case 11:
        p.trucks = true;
        break;
    case 12:
        p.buses = true;
        break;
    default:
        break;
    }
}

void draw_traffic_tab(EditorSidePanel& panel) {
    auto& st = panel.imgui_state();
    auto& rule = panel.imgui_path_rule_edit();
    ImGui::TextUnformatted(i18n::tr_lit("Set Rules"));
    if (st.uvg_groups.empty()) {
        ImGui::TextDisabled(i18n::tr_lit("Sin unsched_vehgroups.txt en el mapa"));
    } else {
        const char* preview = (st.uvg_sel >= 0 && st.uvg_sel < static_cast<int>(st.uvg_groups.size()))
                                  ? st.uvg_groups[static_cast<std::size_t>(st.uvg_sel)].c_str()
                                  : "";
        if (ImGui::BeginCombo(i18n::tr_lit("Grupo unsched"), preview)) {
            for (int i = 0; i < static_cast<int>(st.uvg_groups.size()); ++i) {
                const bool sel = (i == st.uvg_sel);
                if (ImGui::Selectable(st.uvg_groups[static_cast<std::size_t>(i)].c_str(), sel)) {
                    st.uvg_sel = i;
                    rule.density_uvg = i;
                }
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    }
    rule.density_uvg = st.uvg_sel;
    int idx = rule.traffic_brush;
    if (idx < 0 || idx >= kTrafficRuleCount) idx = 6;
    ImGui::BeginChild("omsi_set_rules", ImVec2(0.f, 30.f * static_cast<float>(kTrafficRuleCount) + 8.f), true);
    for (int i = 0; i < kTrafficRuleCount; ++i) {
        if (ImGui::RadioButton(kTrafficRules[i], idx == i)) {
            idx = i;
            apply_traffic_rule_index(rule, i);
        }
        if (i == 0 && rule.has_speed) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(56.f);
            ImGui::DragFloat("##spd", &rule.speed_kmh, 0.5f, 5.f, 200.f, "%.0f");
        }
        if ((i == 8 || i == 9) && rule.has_priority && idx == i) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(56.f);
            if (ImGui::DragFloat("##prio", &rule.priority, 1.f, 0.f, 255.f, "%.0f")) {
                if (rule.priority < 0.f) rule.priority = 0.f;
                if (rule.priority > 255.f) rule.priority = 255.f;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(i18n::tr_lit("SetRule case 6: priority 0..255 (128 = neutro). path_index va en la 1a linea del [rule]."));
        }
    }
    ImGui::EndChild();
    if (rule.valid) {
        ImGui::Text("path_index %d", rule.path_index);
        ImGui::TextUnformatted(rule.label.c_str());
    } else {
        ImGui::TextDisabled(i18n::tr_lit("Click a path in the viewport to paint the selected rule (OMSI)."));
    }
    if (ImGui::Button(i18n::tr_lit("Aplicar regla")) && rule.valid) {
        EditorSidePanelState ev{};
        ev.apply_path_rule = true;
        fire(panel, ev);
    }
}

void draw_spawn_options(EditorSidePanel& panel, const bool extras_only) {
    auto& st = panel.imgui_state();
    ImGui::Separator();
    bool markers_changed = false;
    markers_changed |= ImGui::Checkbox(i18n::tr_lit("Flechas spline"), &st.show_spline_ends);
    markers_changed |= ImGui::Checkbox(i18n::tr_lit("Flechas path"), &st.show_path_ends);
    if (!extras_only) markers_changed |= ImGui::Checkbox(i18n::tr_lit("Rails path"), &st.show_path_rails);
    ImGui::Checkbox(i18n::tr_lit("Ocultar splines onlyeditor"), &st.hide_onlyeditor_splines);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(i18n::tr_lit("Oculta la malla visual de splines con [onlyeditor]; los paths siguen activos."));
    if (markers_changed) {
        EditorSidePanelState ev{};
        ev.toggled_markers = true;
        ev.save_ui_settings = true;
        fire(panel, ev);
    }
    bool spawn_changed = false;
    if (!extras_only) {
        spawn_changed |= ImGui::Checkbox(i18n::tr_lit("Stick terreno"), &st.spawn_stick_terrain);
        spawn_changed |= ImGui::Checkbox(i18n::tr_lit("Spawn en meshes"), &st.spawn_on_meshes);
        spawn_changed |= ImGui::Checkbox(i18n::tr_lit("Align pitch/bank"), &st.spawn_align_pitch_bank);
    }
    if (panel.current_tool() == EditorTool::Object) {
        load_editor_docks(st);
        spawn_changed |= ImGui::Checkbox(i18n::tr_lit("Generar al azar"), &st.spawn_random);
        if (st.spawn_random) {
            const char* mode_labels[] = {"Al azar del catálogo", "Lista de favoritos", i18n::tr_lit("Escanear carpeta")};
            st.spawn_random_mode = (std::clamp)(st.spawn_random_mode, 0, 2);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::Combo("##spawn_rnd_mode_side", &st.spawn_random_mode, mode_labels, 3))
                spawn_changed = true;
            if (st.spawn_random_mode == 1) {
                if (st.fav_cats.empty()) st.fav_cats.push_back({"General", {}});
                st.spawn_random_fav_cat =
                    (std::clamp)(st.spawn_random_fav_cat, 0, static_cast<int>(st.fav_cats.size()) - 1);
                std::vector<std::string> names;
                names.reserve(st.fav_cats.size());
                for (const auto& c : st.fav_cats) names.push_back(c.name);
                std::vector<const char*> name_ptrs;
                name_ptrs.reserve(names.size());
                for (const auto& n : names) name_ptrs.push_back(n.c_str());
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::Combo("##spawn_rnd_fav_side", &st.spawn_random_fav_cat, name_ptrs.data(),
                                 static_cast<int>(name_ptrs.size())))
                    spawn_changed = true;
            } else if (st.spawn_random_mode == 2) {
                ImGui::TextDisabled("%s · %zu SCO",
                                    st.spawn_random_folder.empty()
                                        ? "Sin carpeta"
                                        : omsi::path_utf8(st.spawn_random_folder.filename()).c_str(),
                                    st.spawn_random_folder_pool.size());
                if (ImGui::Button(i18n::tr_lit("Escanear carpeta##side"))) {
                    st.catalog_browser_to_spawn_random = true;
                    EditorSidePanelState ev{};
                    ev.spawn_random_scan_folder = true;
                    fire(panel, ev);
                }
            }
        }
    }
    if (spawn_changed) {
        EditorSidePanelState ev{};
        ev.save_ui_settings = true;
        fire(panel, ev);
    }
    if (ImGui::Button(i18n::tr_lit("Guardar prefs UI"))) {
        EditorSidePanelState ev{};
        ev.save_ui_settings = true;
        fire(panel, ev);
    }
}

void draw_herramientas_panel(EditorSidePanel& /*panel*/, const MapEditorImGuiArgs& /*args*/) {
    // Herramientas vive solo en el menú superior (junto a Vista); sin panel plegable.
}

// ── Multi-selection: group mode of the data panels, "edit as list" window, select-same popup ───────────────────

namespace {
void gtip(const char* t) {
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", t);
}
void gsec(const char* t) {
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.45f, 1.f), "%s", t);
    ImGui::Separator();
}
// Drag value showing the group's current centre / mean; a change returns the DELTA to add to every item.
// One undo step per drag gesture: `new_gesture` is true only on the first change after the item was activated.
bool group_drag(EditorSidePanelImGuiState& st, const char* id, float cur, float speed, float& delta, bool& new_gesture) {
    float v = cur;
    ImGui::SetNextItemWidth(-FLT_MIN);
    const bool ch = ImGui::DragFloat(id, &v, speed, 0.f, 0.f, "%.3f");
    if (ImGui::IsItemActivated()) st.group_gesture_open = false;
    if (ch) {
        new_gesture = !st.group_gesture_open;
        st.group_gesture_open = true;
        delta = v - cur;
    }
    if (ImGui::IsItemDeactivated()) st.group_gesture_open = false;
    return ch;
}
void group_fire_delta(EditorSidePanel& panel, EditorSidePanelImGuiState& st, const float d[4], const float pb[2], bool new_gesture) {
    EditorSidePanelState ev{};
    ev.group_apply_delta = true;
    ev.group_new_gesture = new_gesture;
    ev.group_about_center = st.group_about_center_ui;
    for (int i = 0; i < 4; ++i) ev.group_delta[i] = d[i];
    for (int i = 0; i < 2; ++i) ev.group_delta_pb[i] = pb[i];
    fire(panel, ev);
}
void group_common_header(EditorSidePanel& panel, EditorSidePanelImGuiState& st, const MapEditorImGuiArgs& args) {
    ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.35f, 1.f), "Grupo: %zu obj / %zu spl", args.object_group_count, args.spline_group_count);
    ImGui::SameLine();
    ImGui::TextDisabled("(valores = centro del grupo)");
    gtip("Con varios elementos elegidos, los campos muestran el CENTRO del grupo. Al cambiarlos, la diferencia\n"
         "se suma a todos los elementos (conservan sus posiciones relativas). Los campos en gris solo\n"
         "se pueden editar de a uno.");
    ImGui::Checkbox("Rotar y altura alrededor del centro del grupo", &st.group_about_center_ui);
    gtip("Activado: la rotacion gira todo el grupo alrededor de su centro (como una sola pieza).\n"
         "Desactivado: cada elemento rota sobre su propio origen.");
}
void group_xyz_rot(EditorSidePanel& panel, EditorSidePanelImGuiState& st, const MapEditorImGuiArgs& args, bool objects) {
    gsec("Posicion (centro del grupo)");
    float d[4]{}, pb[2]{};
    bool ng = true, any = false;
    if (ImGui::BeginTable("##grp_xyz", 3, ImGuiTableFlags_SizingStretchSame)) {
        static const char* ids[3] = {"##gx", "##gy", "##gz"};
        static const char* labs[3] = {"X", "Y", "Z"};
        for (int i = 0; i < 3; ++i) {
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(labs[i]);
            ImGui::SameLine();
            float dv = 0.f;
            if (group_drag(st, ids[i], args.group_center[i], 0.05f, dv, ng)) { d[i] = dv; any = true; }
        }
        ImGui::EndTable();
    }
    gsec("Rotacion");
    if (objects) {
        if (ImGui::BeginTable("##grp_rot", 3, ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableNextColumn(); ImGui::TextUnformatted("X");
            ImGui::SameLine();
            float dv = 0.f;
            if (group_drag(st, "##gpitch", args.group_mean_rot[0], 0.2f, dv, ng)) { pb[0] = dv; any = true; }
            ImGui::TableNextColumn(); ImGui::TextUnformatted("Y");
            ImGui::SameLine();
            if (group_drag(st, "##grot", args.group_mean_rot[1], 0.2f, dv, ng)) { d[3] = dv; any = true; }
            ImGui::TableNextColumn(); ImGui::TextUnformatted("Z");
            ImGui::SameLine();
            if (group_drag(st, "##gbank", args.group_mean_rot[2], 0.2f, dv, ng)) { pb[1] = dv; any = true; }
            ImGui::EndTable();
        }
    } else {
        float dv = 0.f;
        ImGui::TextUnformatted("Rotacion");
        ImGui::SameLine();
        if (group_drag(st, "##grot", args.group_mean_rot[1], 0.2f, dv, ng)) { d[3] = dv; any = true; }
    }
    gtip("Valor medio de rotacion del grupo. Al cambiarlo se suma la diferencia a cada elemento.");
    if (any) group_fire_delta(panel, st, d, pb, ng);
}
void group_list_button(EditorSidePanelImGuiState& st) {
    ImGui::Spacing();
    if (ImGui::Button(st.group_list_open ? "Cerrar lista" : "Editar en lista...", ImVec2(-FLT_MIN, 0.f))) {
        st.group_list_open = !st.group_list_open;
        st.group_list_edit.clear();
    }
    gtip("Abre una tabla con todos los elegidos y todos sus campos para ajustarlos uno por uno, con presets\n"
         "por columna. Se confirma con Aplicar.");
}
} // namespace

void draw_group_object_block(EditorSidePanel& panel, const MapEditorImGuiArgs& args) {
    auto& st = panel.imgui_state();
    group_common_header(panel, st, args);
    group_xyz_rot(panel, st, args, true);
    if (args.group_has_attach) {
        gsec("Attachments del grupo");
        ImGui::TextUnformatted("AlignTerrain:");
        ImGui::SameLine();
        if (ImGui::SmallButton("Si##gal")) { EditorSidePanelState ev{}; ev.group_sco_flags = true; ev.group_sco_align = 1; ev.group_sco_tangential = -1; fire(panel, ev); }
        ImGui::SameLine();
        if (ImGui::SmallButton("No##gal")) { EditorSidePanelState ev{}; ev.group_sco_flags = true; ev.group_sco_align = 0; ev.group_sco_tangential = -1; fire(panel, ev); }
        gtip("Alinea al terreno (o no) todos los attachments del grupo.");
        ImGui::TextUnformatted("Tangencial a la spline:");
        ImGui::SameLine();
        if (ImGui::SmallButton("Si##gtn")) { EditorSidePanelState ev{}; ev.group_sco_flags = true; ev.group_sco_align = -1; ev.group_sco_tangential = 1; fire(panel, ev); }
        ImGui::SameLine();
        if (ImGui::SmallButton("No##gtn")) { EditorSidePanelState ev{}; ev.group_sco_flags = true; ev.group_sco_align = -1; ev.group_sco_tangential = 0; fire(panel, ev); }
        gtip("Los attachments siguen la direccion de su spline (o mantienen rotacion fija).");
    }
    group_list_button(st);
}

void draw_group_spline_block(EditorSidePanel& panel, const MapEditorImGuiArgs& args) {
    auto& st = panel.imgui_state();
    group_common_header(panel, st, args);
    group_xyz_rot(panel, st, args, false);
    gsec("Acciones grupales");
    if (ImGui::BeginTable("##grp_spl_btn", 2, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextColumn();
        if (ImGui::Button("Mirror grupal", ImVec2(-FLT_MIN, 0.f))) {
            EditorSidePanelState ev{};
            ev.apply_spline_mirror_only = true;
            fire(panel, ev);
        }
        gtip("Invierte (mirror) todas las splines del grupo.");
        ImGui::TableNextColumn();
        if (ImGui::Button("Split grupal", ImVec2(-FLT_MIN, 0.f))) {
            EditorSidePanelState ev{};
            ev.group_split = true;
            ev.spline_split_cuts = st.spline_cut_count;
            fire(panel, ev);
        }
        gtip("Parte cada spline del grupo en 'cortes + 1' tramos iguales.");
        ImGui::EndTable();
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputInt("cortes por spline", &st.spline_cut_count);
    if (st.spline_cut_count < 1) st.spline_cut_count = 1;
    if (st.spline_cut_count > 32) st.spline_cut_count = 32;
    static const char* kAlign[] = {"No", "Yes, both THs", "Yes, only start TH", "Yes, only end TH", "Yes, no THs"};
    static int al = 0;
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::Combo("AlignTerrain grupal", &al, kAlign, IM_ARRAYSIZE(kAlign))) {
        EditorSidePanelState ev{};
        ev.group_spline_align = true;
        ev.group_spline_align_value = al;
        fire(panel, ev);
    }
    gtip("Aplica el mismo AlignTerrain a todas las splines del grupo.");
    group_list_button(st);
}

void draw_object_props(EditorSidePanel& panel, const MapEditorImGuiArgs& args) {
    if (args.object_group_count + args.spline_group_count > 1) {
        draw_group_object_block(panel, args);
        ImGui::Spacing();
        ImGui::TextDisabled("Resto de campos (solo de a uno):");
        ImGui::BeginDisabled();
        draw_object_props_single(panel);
        ImGui::EndDisabled();
        return;
    }
    draw_object_props_single(panel);
}

void draw_spline_props(EditorSidePanel& panel, const MapEditorImGuiArgs& args) {
    if (args.object_group_count + args.spline_group_count > 1) {
        draw_group_spline_block(panel, args);
        ImGui::Spacing();
        ImGui::TextDisabled("Resto de campos (solo de a uno):");
        ImGui::BeginDisabled();
        draw_spline_props_single(panel, args.spline_group_count);
        ImGui::EndDisabled();
        return;
    }
    draw_spline_props_single(panel, args.spline_group_count);
}

// "Edit as list" window + the select-same attachments question.
void draw_group_windows(EditorSidePanel& panel, const ImGuiViewport* vp, const MapEditorImGuiArgs& args) {
    auto& st = panel.imgui_state();
    const auto tool = panel.current_tool();
    if (tool != EditorTool::Object && tool != EditorTool::Spline) {
        st.group_list_open = false;
        return;
    }
    if (st.group_attach_popup) {
        ImGui::OpenPopup("Seleccionar iguales##att");
        st.group_attach_popup = false;
    }
    if (ImGui::BeginPopupModal("Seleccionar iguales##att", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Entre los %zu .sco iguales hay %zu attachments (spline / objeto).", args.group_same_total,
                    args.group_same_attach);
        ImGui::TextUnformatted("Cada uno se mueve y rota distinto. Que elijo?");
        if (ImGui::Button("Solo los simples", ImVec2(160.f, 0.f))) {
            EditorSidePanelState ev{};
            ev.group_select_same = true;
            ev.group_select_attach = 1;
            fire(panel, ev);
            ImGui::CloseCurrentPopup();
        }
        gtip("Selecciona solo los .sco colocados sueltos en el mapa.");
        ImGui::SameLine();
        if (ImGui::Button("Incluir attachments", ImVec2(160.f, 0.f))) {
            EditorSidePanelState ev{};
            ev.group_select_same = true;
            ev.group_select_attach = 2;
            fire(panel, ev);
            ImGui::CloseCurrentPopup();
        }
        gtip("Tambien selecciona los .sco pegados a una spline: su X/Y/Z es lateral / along / altura.");
        ImGui::SameLine();
        if (ImGui::Button("Cancelar", ImVec2(100.f, 0.f))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (!st.group_list_open) return;
    // Keep pending edits while the same elements stay selected; refresh when the set of ids changes.
    {
        std::uint64_t sig = 1469598103934665603ULL;
        for (const auto& r : args.group_rows_live) sig = (sig ^ static_cast<std::uint64_t>(r.id * 2 + (r.spline ? 1 : 0))) * 1099511628211ULL;
        static std::uint64_t s_sig = 0;
        if (st.group_list_edit.empty() || sig != s_sig) {
            st.group_list_edit = args.group_rows_live;
            s_sig = sig;
        }
    }
    ImGui::SetNextWindowSize(ImVec2(900.f, 520.f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    bool open = true;
    if (ImGui::Begin("Editar seleccion en lista", &open, ImGuiWindowFlags_NoSavedSettings)) {
        auto& rows = st.group_list_edit;
        ImGui::TextDisabled("%zu elementos. Los cambios se aplican con Aplicar. Click en la flecha de una columna = valor para toda la columna.", rows.size());
        struct Col { const char* title; int idx; int kind; }; // kind 0 float, 1 checkbox, 2 align combo
        static const Col sco_cols[] = {{"X", 0, 0}, {"Y", 1, 0}, {"Z", 2, 0}, {"Pitch", 3, 0}, {"Rot", 4, 0}, {"Bank", 5, 0}, {"Align", 6, 1}, {"Tang.", 7, 1}};
        static const Col sli_cols[] = {{"X", 0, 0}, {"Y", 1, 0}, {"Z", 2, 0}, {"Rot", 3, 0}, {"Largo", 4, 0}, {"Radio", 5, 0}, {"Grad0", 6, 0}, {"Grad1", 7, 0},
                                       {"dH", 8, 0}, {"Skew0", 9, 0}, {"Skew1", 10, 0}, {"Cant0", 11, 0}, {"Cant1", 12, 0}, {"Mirror", 13, 1}, {"Align", 14, 2}};
        static const char* kAlign[] = {"No", "Both", "Start", "End", "None"};
        auto draw_table = [&](bool spline) {
            const Col* cols = spline ? sli_cols : sco_cols;
            const int ncols = spline ? EditorSidePanelState::kSliCols : EditorSidePanelState::kScoCols;
            if (!ImGui::BeginTable(spline ? "##grp_sli" : "##grp_sco", ncols + 2,
                                   ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX |
                                       ImGuiTableFlags_Resizable, ImVec2(0.f, -34.f)))
                return;
            ImGui::TableSetupColumn("Nombre", ImGuiTableColumnFlags_WidthFixed, 150.f);
            ImGui::TableSetupColumn("Id", ImGuiTableColumnFlags_WidthFixed, 50.f);
            for (int c = 0; c < ncols; ++c) ImGui::TableSetupColumn(cols[c].title, ImGuiTableColumnFlags_WidthFixed, cols[c].kind == 0 ? 78.f : 64.f);
            ImGui::TableSetupScrollFreeze(2, 2);
            // header row 1: titles
            ImGui::TableHeadersRow();
            // header row 2: one dropdown preset per column (quick values + a typed value) applied to every row
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("Preset columna:");
            for (int c = 0; c < ncols; ++c) {
                ImGui::TableSetColumnIndex(c + 2);
                ImGui::PushID(c + (spline ? 100 : 0));
                char btn[32];
                std::snprintf(btn, sizeof(btn), "preset %s##pb", cols[c].title);
                if (ImGui::SmallButton("v")) ImGui::OpenPopup("##colpreset");
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Aplicar un valor a toda la columna %s", cols[c].title);
                if (ImGui::BeginPopup("##colpreset")) {
                    auto set_col = [&](float v) {
                        for (auto& r : rows)
                            if (r.spline == spline) r.v[cols[c].idx] = v;
                    };
                    if (cols[c].kind == 0) {
                        for (float q : {0.f, 0.1f, 0.5f, 1.f, -1.f}) {
                            char t[32];
                            std::snprintf(t, sizeof(t), "Todos = %g", static_cast<double>(q));
                            if (ImGui::Selectable(t)) set_col(q);
                        }
                        static float typed = 0.f;
                        ImGui::Separator();
                        ImGui::SetNextItemWidth(90.f);
                        ImGui::InputFloat("##typed", &typed, 0.f, 0.f, "%.3f");
                        ImGui::SameLine();
                        if (ImGui::Button("Aceptar")) { set_col(typed); ImGui::CloseCurrentPopup(); }
                    } else if (cols[c].kind == 1) {
                        if (ImGui::Selectable("Todos = Si")) set_col(1.f);
                        if (ImGui::Selectable("Todos = No")) set_col(0.f);
                    } else {
                        for (int k = 0; k < 5; ++k)
                            if (ImGui::Selectable((std::string("Todos = ") + kAlign[k]).c_str())) set_col(static_cast<float>(k));
                    }
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
            ImGuiListClipper clip;
            int n = 0;
            for (const auto& r : rows) n += r.spline == spline ? 1 : 0;
            std::vector<int> idx;
            idx.reserve(static_cast<std::size_t>(n));
            for (int i = 0; i < static_cast<int>(rows.size()); ++i)
                if (rows[static_cast<std::size_t>(i)].spline == spline) idx.push_back(i);
            clip.Begin(n);
            while (clip.Step()) {
                for (int k = clip.DisplayStart; k < clip.DisplayEnd; ++k) {
                    auto& r = rows[static_cast<std::size_t>(idx[static_cast<std::size_t>(k)])];
                    ImGui::PushID(idx[static_cast<std::size_t>(k)]);
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted((r.name + (r.attach ? "  [att]" : "")).c_str());
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%d", static_cast<int>(r.id));
                    for (int c = 0; c < ncols; ++c) {
                        ImGui::TableSetColumnIndex(c + 2);
                        ImGui::PushID(c);
                        float& v = r.v[cols[c].idx];
                        if (cols[c].kind == 0) {
                            ImGui::SetNextItemWidth(-FLT_MIN);
                            ImGui::InputFloat("##f", &v, 0.f, 0.f, "%.3f");
                        } else if (cols[c].kind == 1) {
                            bool b = v > 0.5f;
                            const bool off = !spline && !r.attach && cols[c].idx == 7; // tangential only for attachments
                            if (off) ImGui::BeginDisabled();
                            if (ImGui::Checkbox("##b", &b)) v = b ? 1.f : 0.f;
                            if (off) ImGui::EndDisabled();
                        } else {
                            int a = (std::clamp)(static_cast<int>(std::lround(v)), 0, 4);
                            ImGui::SetNextItemWidth(-FLT_MIN);
                            if (ImGui::Combo("##a", &a, kAlign, 5)) v = static_cast<float>(a);
                        }
                        ImGui::PopID();
                    }
                    ImGui::PopID();
                }
            }
            ImGui::EndTable();
        };
        bool has_sco = false, has_sli = false;
        for (const auto& r : rows) (r.spline ? has_sli : has_sco) = true;
        if (ImGui::BeginTabBar("##grp_tabs")) {
            if (has_sco && ImGui::BeginTabItem("SCO")) { draw_table(false); ImGui::EndTabItem(); }
            if (has_sli && ImGui::BeginTabItem("SLI")) { draw_table(true); ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }
        if (rows.empty()) ImGui::TextDisabled("No hay elementos seleccionados.");
        if (ImGui::Button("Aplicar", ImVec2(110.f, 0.f))) {
            EditorSidePanelState ev{};
            ev.group_list_apply = true;
            ev.group_rows = rows;
            fire(panel, ev);
            open = false;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Escribe los valores de la tabla en cada elemento (en memoria; se guarda con Save).");
        ImGui::SameLine();
        if (ImGui::Button("Cancelar", ImVec2(110.f, 0.f))) open = false;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Cierra sin cambiar nada.");
    }
    ImGui::End();
    if (!open) {
        st.group_list_open = false;
        st.group_list_edit.clear();
    }
}

void draw_spawn_float(EditorSidePanel& panel, const ImGuiViewport* vp, const MapEditorImGuiArgs& args) {
    const auto tool = panel.current_tool();
    const std::size_t spline_group_count = args.spline_group_count;
    const std::size_t object_group_count = args.object_group_count;
    const bool spawn_tools = tool == EditorTool::Object || tool == EditorTool::Spline;
    if (!spawn_tools && args.edit_mode_label.empty()) return;
    auto& st = panel.imgui_state();
    const float left_panel = 400.f;
    const float rest_w = (std::max)(160.f, vp->WorkSize.x - left_panel);
    const float spawn_w = (std::clamp)(rest_w * 0.24f, 220.f, 280.f);
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + left_panel + 8.f, vp->WorkPos.y + 8.f), ImGuiCond_Always,
                            ImVec2(0.f, 0.f));
    ImGui::SetNextWindowSize(ImVec2(spawn_w, 0.f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.94f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.f, 8.f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.f, 5.f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings;
    if (!ImGui::Begin("##spawn_float", nullptr, flags)) {
        ImGui::End();
        ImGui::PopStyleVar(3);
        return;
    }

    // Cozy header + minimize
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.85f, 0.35f, 1.f));
        if (!args.edit_mode_label.empty())
            ImGui::TextUnformatted(args.edit_mode_label.c_str());
        else
            ImGui::TextUnformatted(i18n::tr_lit("Vista 3D"));
        ImGui::PopStyleColor();
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 28.f);
        const char* min_lab = st.spawn_float_collapsed ? "+##spawn_min" : "−##spawn_min";
        if (ImGui::SmallButton(min_lab)) st.spawn_float_collapsed = !st.spawn_float_collapsed;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(st.spawn_float_collapsed ? i18n::tr_lit("Expandir panel") : i18n::tr_lit("Minimizar panel"));
    }
    if (st.spawn_float_collapsed) {
        ImGui::End();
        ImGui::PopStyleVar(3);
        return;
    }
    if (!spawn_tools) {
        ImGui::End();
        ImGui::PopStyleVar(3);
        return;
    }

    bool spawn_changed = false;
    bool markers_changed = false;
    auto section = [](const char* t) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.45f, 1.f), "%s", t);
        ImGui::Separator();
    };
    if (tool == EditorTool::Spline) {
        section("Encadenado de splines");
        markers_changed |= ImGui::Checkbox(i18n::tr_lit("Pegar cadena"), &st.spline_stick_chain);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Pegar cadena: al mover o editar una spline, las conectadas (anterior y siguiente)\n"
                              "se ajustan solas para que la union siga continua.\n"
                              "Usalo al editar tramos de carretera/via ya conectados.");
        markers_changed |= ImGui::Checkbox(i18n::tr_lit("Rellenar cad."), &st.spline_fill_chain);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Rellenar cadena: al mover un extremo, crea tramos temporales que rellenan el hueco\n"
                              "hasta el vecino en vez de dejar la union rota.\n"
                              "Usalo al arrastrar un nodo lejos de su posicion original.");
    }
    section("Colocacion");
    spawn_changed |= ImGui::Checkbox(i18n::tr_lit("Spawn mesh"), &st.spawn_on_meshes);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Spawn mesh: al colocar (N) el objeto/spline se apoya sobre otras mallas\n"
                          "(edificios, puentes) y no solo sobre el terreno.\n"
                          "Desactivalo para colocar siempre a la altura del terreno.");
    if (tool == EditorTool::Object) {
        load_editor_docks(st);
        spawn_changed |= ImGui::Checkbox(i18n::tr_lit("Generar al azar"), &st.spawn_random);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(i18n::tr_lit("Con N: colocar un .sco distinto cada vez (solo objetos)."));
        if (st.spawn_random) {
            const char* mode_labels[] = {"Al azar del catálogo", "Lista de favoritos", i18n::tr_lit("Escanear carpeta")};
            st.spawn_random_mode = (std::clamp)(st.spawn_random_mode, 0, 2);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo("##spawn_rnd_mode", mode_labels[st.spawn_random_mode])) {
                for (int m = 0; m < 3; ++m) {
                    if (ImGui::Selectable(mode_labels[m], st.spawn_random_mode == m)) {
                        st.spawn_random_mode = m;
                        spawn_changed = true;
                    }
                }
                ImGui::EndCombo();
            }
            if (st.spawn_random_mode == 1) {
                if (st.fav_cats.empty()) st.fav_cats.push_back({"General", {}});
                st.spawn_random_fav_cat =
                    (std::clamp)(st.spawn_random_fav_cat, 0, static_cast<int>(st.fav_cats.size()) - 1);
                const auto& cur = st.fav_cats[static_cast<std::size_t>(st.spawn_random_fav_cat)];
                char fav_preview[96];
                std::snprintf(fav_preview, sizeof(fav_preview), "%s (%zu)", cur.name.c_str(), cur.items.size());
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::BeginCombo("##spawn_rnd_fav", fav_preview)) {
                    for (int i = 0; i < static_cast<int>(st.fav_cats.size()); ++i) {
                        const auto& c = st.fav_cats[static_cast<std::size_t>(i)];
                        char lab[96];
                        std::snprintf(lab, sizeof(lab), "%s (%zu)", c.name.c_str(), c.items.size());
                        if (ImGui::Selectable(lab, st.spawn_random_fav_cat == i)) {
                            st.spawn_random_fav_cat = i;
                            spawn_changed = true;
                        }
                    }
                    ImGui::EndCombo();
                }
            } else if (st.spawn_random_mode == 2) {
                if (!st.spawn_random_folder.empty()) {
                    ImGui::TextDisabled("%s · %zu SCO", omsi::path_utf8(st.spawn_random_folder.filename()).c_str(),
                                        st.spawn_random_folder_pool.size());
                } else {
                    ImGui::TextDisabled(i18n::tr_lit("Sin carpeta escaneada"));
                }
                if (ImGui::Button(i18n::tr_lit("Escanear carpeta"), ImVec2(-FLT_MIN, 0.f))) {
                    st.catalog_browser_to_spawn_random = true;
                    const auto root = catalog_initial_dir(args.omsi_root, false);
                    catalog_browser_open(st, root, false, true);
                }
            }
        }
    }
    {
        const std::int32_t oid = panel.imgui_object_edit().valid ? panel.imgui_object_edit().id : 0;
        const std::int32_t sid = panel.imgui_spline_edit().valid ? panel.imgui_spline_edit().id : 0;
        const bool have_sel = (tool == EditorTool::Spline && sid > 0) || (tool == EditorTool::Object && oid > 0);
        if (have_sel) {
            bool hid = (tool == EditorTool::Spline && st.hidden_spline_ids.count(sid)) ||
                       (tool == EditorTool::Object && st.hidden_object_ids.count(oid));
            if (ImGui::Checkbox(i18n::tr_lit("Ocultar (H)"), &hid)) {
                EditorSidePanelState ev{};
                ev.toggle_editor_hide = true;
                fire(panel, ev);
            }
            ImGui::SameLine();
            const bool any_hidden = !st.hidden_object_ids.empty() || !st.hidden_spline_ids.empty();
            ImGui::BeginDisabled(!any_hidden);
            if (ImGui::SmallButton(i18n::tr_lit("Desocultar"))) {
                EditorSidePanelState ev{};
                ev.unhide_all_editor = true;
                fire(panel, ev);
            }
            ImGui::EndDisabled();
        }
        section("Visibilidad");
        ImGui::SetNextItemWidth(-70.f);
        ImGui::SliderFloat("##opac", &st.mesh_opacity, 0.05f, 1.f, "opacidad %.2f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Opacidad de todas las mallas del mapa. Bajala para ver paths, terreno\n"
                              "y objetos escondidos detras de edificios; vuelve a 1.00 para trabajar normal.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Default")) st.mesh_opacity = 1.f;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Vuelve la opacidad a 1.00 (todo solido).");
    }

    ImGui::Spacing();
    if (ImGui::CollapsingHeader(i18n::tr_lit("Seleccion"), ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Como elegis objetos/splines con el mouse: de a uno, en recuadro, con circulo,\n"
                              "con lazo o sumando clicks. Con varios elegidos el panel de datos edita el grupo.");
        // Keep combo in sync with live state (marquee / multi).
        if (st.spline_multi_pick || st.object_multi_pick) st.sel_ui_mode = 4;
        else if (st.sel_mode_lasso) st.sel_ui_mode = 3;
        else if (st.sel_mode_circle) st.sel_ui_mode = 2;
        else if (st.sel_ui_mode == 4) st.sel_ui_mode = 0;
        const char* sel_labels[] = {"Pick simple", "Recuadro", "Circulo", i18n::tr_lit("Lazo"),
                                    tool == EditorTool::Spline ? "Multi splines" : "Multi objetos"};
        if (st.sel_ui_mode < 0 || st.sel_ui_mode > 4) st.sel_ui_mode = 0;
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##sel_mode", sel_labels[st.sel_ui_mode])) {
            for (int m = 0; m < 5; ++m) {
                const bool on = st.sel_ui_mode == m;
                if (ImGui::Selectable(sel_labels[m], on)) {
                    st.sel_ui_mode = m;
                    EditorSidePanelState ev{};
                    bool fire_ev = true;
                    if (m == 0) {
                        ev.cancel_group_marquee = true;
                    } else if (m == 4) {
                        if (tool == EditorTool::Spline) {
                            if (st.spline_multi_pick) fire_ev = false;
                            else ev.toggle_spline_multi_pick = true;
                        } else {
                            if (st.object_multi_pick) fire_ev = false;
                            else ev.toggle_object_multi_pick = true;
                        }
                    } else {
                        ev.start_group_marquee = true;
                        ev.sel_use_circle = (m == 2);
                        ev.sel_use_lasso = (m == 3);
                    }
                    if (fire_ev) fire(panel, ev);
                }
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Pick simple = click uno.\nRecuadro = arrastrar y soltar.\nCirculo = hold LMB pinta (Shift quita).\n"
                "Lazo = dibujar y soltar.\nMulti = cada click suma.\nEl modo sigue hasta Pick simple.");
        if (st.sel_ui_mode == 4) {
            if (tool == EditorTool::Spline && spline_group_count > 0)
                ImGui::TextDisabled("Grupo: %zu spl", spline_group_count);
            if (tool == EditorTool::Object && object_group_count > 0)
                ImGui::TextDisabled("Grupo: %zu obj", object_group_count);
        } else if (object_group_count + spline_group_count > 0) {
            ImGui::TextDisabled("Grupo: %zu obj / %zu spl", object_group_count, spline_group_count);
        }
        if (object_group_count + spline_group_count > 0) {
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputTextWithHint("##preset_name", i18n::tr_lit("nombre preset"), st.preset_name_buf,
                                     sizeof(st.preset_name_buf));
            if (ImGui::Button(i18n::tr_lit("Guardar seleccion"), ImVec2(-FLT_MIN, 0.f))) {
                if (st.preset_name_buf[0] == '\0')
                    std::snprintf(st.preset_name_buf, sizeof(st.preset_name_buf), "preset");
                EditorSidePanelState ev{};
                ev.save_selection_preset = true;
                fire(panel, ev);
            }
        }
    }

    {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.45f, 1.f), "Step (paso de los campos)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Cuanto suma o resta cada click en las flechas (- / +) de los campos de\n"
                              "posicion, rotacion, largo, etc. Usa 0.01 para ajuste fino y 5 para mover rapido.");
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 28.f);
        const char* step_min = st.step_section_collapsed ? "+##step_min" : "−##step_min";
        if (ImGui::SmallButton(step_min)) st.step_section_collapsed = !st.step_section_collapsed;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(st.step_section_collapsed ? i18n::tr_lit("Expandir Step") : i18n::tr_lit("Minimizar Step"));
    }
    if (!st.step_section_collapsed) {
        const char* step_key = (tool == EditorTool::Spline) ? "spline.z" : "object.x";
        float& step = field_step(st, step_key, 0.05f);
        char val[32]{};
        std::snprintf(val, sizeof(val), "%.4g", step);
        ImGui::TextDisabled("paso actual: %s", val);
        const float presets[] = {0.01f, 0.1f, 1.f, 5.f};
        auto apply_steps = [&](float v) {
            step = v;
            if (tool == EditorTool::Object) {
                for (const char* k :
                     {"object.x", "object.y", "object.z", "object.pitch", "object.rot", "object.bank"})
                    st.field_steps[k] = v;
            } else {
                for (const char* k : {"spline.x", "spline.y", "spline.z", "spline.rot", "spline.length",
                                      "spline.radius", "spline.skew_start", "spline.skew_end", "spline.start_grad",
                                      "spline.end_grad", "spline.cant_start", "spline.cant_end", "spline.delta_h",
                                      "spline.max_radius"})
                    st.field_steps[k] = v;
            }
            st.field_steps_dirty = true;
        };
        for (int i = 0; i < 4; ++i) {
            if (i) ImGui::SameLine();
            char lab[16]{};
            std::snprintf(lab, sizeof(lab), "%.4g", presets[i]);
            ImGui::PushID(i);
            if (ImGui::SmallButton(lab)) apply_steps(presets[i]);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Pone el paso de TODOS los campos en %s.", lab);
            ImGui::PopID();
        }
    }

    if (spawn_changed) {
        EditorSidePanelState ev{};
        ev.save_ui_settings = true;
        fire(panel, ev);
    }
    if (markers_changed) {
        EditorSidePanelState ev{};
        ev.toggled_markers = true;
        ev.save_ui_settings = true;
        fire(panel, ev);
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
}


omsi::tt::ui::Host make_tt_host(EditorSidePanel& panel) {
    omsi::tt::ui::Host h;
    h.status = [&panel](const std::string& m) { panel.set_status(m); };
    h.camera_jump = [&panel]() {
        EditorSidePanelState ev{};
        ev.tt_request_camera_jump = true;
        fire(panel, ev);
    };
    h.set_mode = [&panel](int m) {
        EditorSidePanelState ev{};
        ev.tt_mode = m;
        fire(panel, ev);
    };
    h.find_errors_on_tiles = [&panel]() {
        EditorSidePanelState ev{};
        ev.tt_find_errors = true;
        fire(panel, ev);
    };
    return h;
}

// Tracks tab: every button/list/sub-window lives in omsi::tt::ui (src/tt/tt_editor_ui.cpp) and works
// directly on TimeTableMan, mirroring Unit134 (TForm_main, Tform_profiles, Tform_tt, ...).
void draw_tracks_trips_tab(EditorSidePanel& panel) {
    auto& st = panel.imgui_state();
    const auto host = make_tt_host(panel);
    omsi::tt::ui::global_ui().draw_tracks_tab(omsi::tt::global_tt_man(), host, st.tt_mode);
}

void draw_station_links_tab(EditorSidePanel& panel) {
    auto& st = panel.imgui_state();
    auto& man = omsi::tt::global_tt_man();

    ImGui::TextUnformatted(i18n::tr_lit("Station Links"));

    if (ImGui::Button(i18n::tr_lit("Filter Start Station"))) {
        EditorSidePanelState ev{};
        ev.sl_filter = true;
        fire(panel, ev);
    }

    ImGui::TextUnformatted(i18n::tr_lit("Links already available:"));
    ImGui::BeginChild("##sl_links", ImVec2(0.f, 120.f), true);
    {
        for (int i = 0; i < (int)man.data.stn_links.size(); ++i) {
            const auto& L = man.data.stn_links[(size_t)i];
            char buf[128];
            std::snprintf(buf, sizeof(buf), "%d: %d → %d  (%zu seg)", i, (int)L.from_id, (int)L.to_id,
                          L.entries.size());
            const bool sel = (man.selected_stnlink == i);
            if (ImGui::Selectable(buf, sel)) {
                man.selected_stnlink = i;
                man.selected_stnlink_entry = -1;
            }
        }
        if (man.data.stn_links.empty())
            ImGui::TextDisabled("%s", i18n::tr_lit("(no links — New)"));
    }
    ImGui::EndChild();

    if (ImGui::Button(i18n::tr_lit("New"))) {
        EditorSidePanelState ev{};
        ev.sl_new = true;
        fire(panel, ev);
    }
    ImGui::SameLine();
    if (ImGui::Button(i18n::tr_lit("Search Next Erroneous"))) {
        EditorSidePanelState ev{};
        ev.sl_search_err = true;
        fire(panel, ev);
    }

    ImGui::Spacing();
    ImGui::TextUnformatted(i18n::tr_lit("Path Segments:"));
    ImGui::BeginChild("##sl_entries", ImVec2(0.f, 90.f), true);
    if (man.selected_stnlink >= 0 && man.selected_stnlink < (int)man.data.stn_links.size()) {
        const auto& L = man.data.stn_links[(size_t)man.selected_stnlink];
        for (int i = 0; i < (int)L.entries.size(); ++i) {
            const auto& e = L.entries[(size_t)i];
            char buf[96];
            std::snprintf(buf, sizeof(buf), "%d: %d / path %d", i, (int)e.id, (int)e.path_index);
            const bool sel = (man.selected_stnlink_entry == i);
            if (ImGui::Selectable(buf, sel))
                man.selected_stnlink_entry = i;
        }
        if (L.entries.empty())
            ImGui::TextDisabled("%s", i18n::tr_lit("(no segments)"));
    }
    ImGui::EndChild();

    st.sl_mode = (st.sl_mode < 0 || st.sl_mode > 2) ? 0 : st.sl_mode;
    const char* modes[] = {
        i18n::tr_lit("Set Start Station"),
        i18n::tr_lit("Set End Station"),
        i18n::tr_lit("Add Path Segments"),
    };
    for (int i = 0; i < 3; ++i) {
        if (ImGui::RadioButton(modes[i], st.sl_mode == i)) {
            st.sl_mode = i;
            EditorSidePanelState ev{};
            ev.sl_mode = i;
            fire(panel, ev);
        }
    }

    if (ImGui::Button(i18n::tr_lit("Delete"))) {
        EditorSidePanelState ev{};
        ev.sl_delete = true;
        fire(panel, ev);
    }
    ImGui::SameLine();
    if (ImGui::Button(i18n::tr_lit("Refresh##sl"))) {
        EditorSidePanelState ev{};
        ev.refresh_tt = true;
        fire(panel, ev);
    }
}

void draw_signal_routes_tab(EditorSidePanel& panel) {
    auto& st = panel.imgui_state();
    ImGui::TextUnformatted(i18n::tr_lit("Signal Routes (signalroutes.cfg)"));
    if (ImGui::Button(i18n::tr_lit("Refresh##sig"))) {
        EditorSidePanelState ev{};
        ev.refresh_tt = true;
        fire(panel, ev);
    }
    ImGui::SameLine();
    if (ImGui::Button(i18n::tr_lit("Open selected##sig"))) {
        EditorSidePanelState ev{};
        ev.open_tt_file = true;
        ev.tt_file_to_open = panel.tt_selected_path();
        fire(panel, ev);
    }
    ImGui::BeginChild("##sig_list", ImVec2(0.f, 200.f), true);
    if (st.tt_signals.empty()) {
        ImGui::TextDisabled(i18n::tr_lit("Sin signal routes cargadas."));
    } else {
        for (int i = 0; i < static_cast<int>(st.tt_signals.size()); ++i) {
            const auto& p = st.tt_signals[static_cast<std::size_t>(i)];
            if (ImGui::Selectable(omsi::path_utf8(p.filename()).c_str(), st.tt_signal_sel == i))
                st.tt_signal_sel = i;
        }
    }
    ImGui::EndChild();
}

void draw_tool_content(EditorSidePanel& panel, const EditorTool tool, const EditorTilePageInfo& tile_info,
                       const int selected_object_index, const int selected_spline_index, int& last_cat_sel,
                       int& last_obj_inst_sel, int& last_spl_inst_sel, const std::filesystem::path& omsi_root,
                       const MapEditorImGuiArgs& args) {
    switch (tool) {
    case EditorTool::Tile:
        draw_tile_tab(panel, tile_info, args.map_name, args.map_friendly_name, args.view_only);
        break;
    case EditorTool::Object:
        draw_catalog_actions(panel, false, omsi_root);
        object_item_card(panel.imgui_object_edit());
        draw_catalog_list(panel, panel.catalog_objects(), false, last_cat_sel, omsi_root);
        ImGui::SetNextItemOpen(false, ImGuiCond_Appearing);
        if (ImGui::CollapsingHeader(i18n::tr_lit("Instancias##obj"))) {
            draw_instance_list(panel, panel.object_instances(), false, selected_object_index, last_obj_inst_sel,
                               omsi_root);
        }
        draw_object_props(panel, args);
        break;
    case EditorTool::Spline:
        draw_catalog_actions(panel, true, omsi_root);
        spline_item_card(panel.imgui_spline_edit());
        draw_catalog_list(panel, panel.catalog_splines(), true, last_cat_sel, omsi_root);
        ImGui::SetNextItemOpen(false, ImGuiCond_Appearing);
        if (ImGui::CollapsingHeader(i18n::tr_lit("Instancias##spl"))) {
            draw_instance_list(panel, panel.spline_instances(), true, selected_spline_index, last_spl_inst_sel,
                               omsi_root);
        }
        draw_spline_props(panel, args);
        break;
    case EditorTool::Terrain:
        draw_terrain_tab(panel, args);
        break;
    case EditorTool::TrafficRules:
        draw_traffic_tab(panel);
        break;
    case EditorTool::TracksTrips:
        draw_tracks_trips_tab(panel);
        break;
    case EditorTool::StationLinks:
        draw_station_links_tab(panel);
        break;
    case EditorTool::SignalRts:
        draw_signal_routes_tab(panel);
        break;
    case EditorTool::SplineExp:
        ImGui::TextUnformatted(i18n::tr_lit("Spline-Exp. — perfil + instancia"));
        ImGui::TextDisabled(i18n::tr_lit("Usa el tab Splines para crear/editar .sli; Ctrl+R = trim length."));
        break;
    default:
        ImGui::TextWrapped(i18n::tr_lit("Pestaña en desarrollo — usa las acciones del menu Archivo/Herramientas."));
        break;
    }
}

// Collect the whole panel state into the global prefs and write omsi_f4_editor.cfg next to the exe.
void save_ui_prefs_now(const MapEditorImGuiArgs& args) {
    if (!args.editor_ui_settings) return;
    if (args.panel) args.panel->collect_ui_settings(*args.editor_ui_settings);
    (void)map::save_editor_ui_settings({}, *args.editor_ui_settings);
}

// "Opciones del editor".
//  * Live settings (min screen %, union ball %, grad circle %, path style, VSync) apply while dragging and are
//    saved when the control is released.
//  * Tile radius / visual radius need a re-stream: they are drafts, applied + saved on Aceptar.
//  * Cancelar / X / Esc revert the live settings to the values the dialog opened with (VSync included).
void draw_options_editor(const MapEditorImGuiArgs& args) {
    if (!args.panel) return;
    auto& st = args.panel->imgui_state();
    auto* ui = args.editor_ui_settings;
    if (!st.options_editor_open) {
        st.options_editor_seeded = false;
        return;
    }
    if (!st.options_editor_seeded) {
        st.options_draft_tile_radius =
            args.options_tile_radius < 1 ? 1 : (args.options_tile_radius > 20 ? 20 : args.options_tile_radius);
        int vis = static_cast<int>(std::lround(args.options_visual_radius_m));
        if (vis < 0) vis = 0;
        if (vis > 2000) vis = 2000;
        st.options_draft_visual_m = vis;
        st.options_draft_union_pct = ui ? ui->union_ball_screen_pct : args.options_union_pct;
        st.options_draft_grad_pct = ui ? ui->grad_circle_screen_pct : args.options_grad_pct;
        if (ui) {
            st.options_draft_min_screen_pct = ui->min_screen_size_pct;
            st.options_draft_vsync = ui->vsync;
        }
        st.options_snap_min_screen_pct = st.options_draft_min_screen_pct;
        st.options_snap_tile_radius = st.options_draft_tile_radius;
        st.options_snap_visual_m = st.options_draft_visual_m;
        st.options_snap_union_pct = st.options_draft_union_pct;
        st.options_snap_grad_pct = st.options_draft_grad_pct;
        st.options_snap_vsync = st.options_draft_vsync;
        st.options_snap_path_style = st.path_style;
        st.options_editor_seeded = true;
    }
    // Push the live drafts into the runtime prefs (renderer / handles read them every frame).
    auto apply_live = [&]() {
        if (!ui) return;
        ui->min_screen_size_pct = st.options_draft_min_screen_pct;
        ui->union_ball_screen_pct = st.options_draft_union_pct;
        ui->grad_circle_screen_pct = st.options_draft_grad_pct;
    };
    auto fire_vsync = [&](bool on) {
        if (ui) ui->vsync = on;
        EditorSidePanelState ev{};
        ev.options_vsync = on;
        ev.options_vsync_set = true;
        fire(*args.panel, ev);
    };
    auto revert = [&]() {
        st.options_draft_min_screen_pct = st.options_snap_min_screen_pct;
        st.options_draft_union_pct = st.options_snap_union_pct;
        st.options_draft_grad_pct = st.options_snap_grad_pct;
        st.path_style = st.options_snap_path_style;
        if (ui) {
            ui->editor_tile_radius = st.options_snap_tile_radius;
            ui->editor_visual_radius_m = static_cast<float>(st.options_snap_visual_m);
        }
        apply_live();
        if (st.options_draft_vsync != st.options_snap_vsync) {
            st.options_draft_vsync = st.options_snap_vsync;
            fire_vsync(st.options_snap_vsync);
        }
        save_ui_prefs_now(args);
        st.options_editor_open = false;
        st.options_editor_seeded = false;
    };

    const char* title = i18n::tr_lit("Opciones del editor");
    ImGui::OpenPopup(title);
    ImGui::SetNextWindowSize(ImVec2(480.f, 0.f), ImGuiCond_Appearing);
    bool keep_open = true;
    if (!ImGui::BeginPopupModal(title, &keep_open, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (!keep_open) revert(); // closed with the X: same as Cancelar
        return;
    }
    if (!keep_open) {
        revert();
        ImGui::EndPopup();
        return;
    }
    const float lbl_w = 200.f;
    const float ctl_w = 240.f;
    // Label column + control column: every row uses the same x so nothing overlaps.
    auto row_label = [&](const char* text) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(text);
        ImGui::SameLine(lbl_w);
        ImGui::SetNextItemWidth(ctl_w);
    };

    ImGui::SeparatorText(i18n::tr_lit("Carga de tiles (se aplica con Aceptar)"));
    row_label(i18n::tr_lit("Radio de tiles"));
    ImGui::SliderInt("##opt_tile_r", &st.options_draft_tile_radius, 1, 20);
    {
        const int side = 2 * st.options_draft_tile_radius + 1;
        ImGui::TextDisabled("R=%d -> %dx%d = %d tiles (Chebyshev, como OMSI Neighbour)", st.options_draft_tile_radius,
                            side, side, side * side);
    }
    row_label(i18n::tr_lit("Radio visual / CPU"));
    if (ImGui::SliderInt("##opt_vis_r", &st.options_draft_visual_m, 0, 2000,
                         st.options_draft_visual_m <= 0 ? "sin limite" : "%d m")) {
        if (st.options_draft_visual_m > 0 && st.options_draft_visual_m < 50) st.options_draft_visual_m = 50;
    }
    if (ImGui::IsItemDeactivatedAfterEdit() && st.options_draft_visual_m > 0)
        st.options_draft_visual_m = (std::max)(50, (st.options_draft_visual_m / 50) * 50);

    ImGui::SeparatorText(i18n::tr_lit("Dibujo (se aplica al instante)"));
    row_label(i18n::tr_lit("Tamano min. en pantalla"));
    if (ImGui::SliderFloat("##opt_min_scr", &st.options_draft_min_screen_pct, 0.f, 5.f,
                           st.options_draft_min_screen_pct <= 0.f ? "desactivado" : "%.2f %%"))
        apply_live();
    if (ImGui::IsItemDeactivatedAfterEdit()) save_ui_prefs_now(args);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", i18n::tr_lit("SCO/SLI cuyo tamano proyectado sea menor a este % del alto de pantalla "
                                             "no se dibujan. 0 = desactivado."));
    float union_pct100 = st.options_draft_union_pct * 100.f;
    row_label(i18n::tr_lit("Bola de union (spline)"));
    if (ImGui::SliderFloat("##opt_union", &union_pct100, 0.5f, 10.f, "%.1f %%")) {
        st.options_draft_union_pct = union_pct100 * 0.01f;
        apply_live();
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) save_ui_prefs_now(args);
    float grad_pct100 = st.options_draft_grad_pct * 100.f;
    row_label(i18n::tr_lit("Circulo de gradiente"));
    if (ImGui::SliderFloat("##opt_grad", &grad_pct100, 0.5f, 10.f, "%.1f %%")) {
        st.options_draft_grad_pct = grad_pct100 * 0.01f;
        apply_live();
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) save_ui_prefs_now(args);
    row_label(i18n::tr("paths.style"));
    {
        const char* styles[2] = {i18n::tr("paths.style.original"), i18n::tr("paths.style.modern")};
        if (ImGui::Combo("##path_style_opt", &st.path_style, styles, 2)) save_ui_prefs_now(args);
    }

    ImGui::SeparatorText(i18n::tr_lit("Pantalla"));
    if (ImGui::Checkbox(i18n::tr_lit("Sincronizacion vertical (VSync)"), &st.options_draft_vsync)) {
        fire_vsync(st.options_draft_vsync); // the run loop applies it to the swapchain and saves
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", i18n::tr_lit("(limita FPS al Hz del monitor)"));

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextDisabled("%s", i18n::tr_lit("Se guarda en omsi_f4_editor.cfg (junto al exe, igual para todos los mapas)."));
    const float bw = 120.f;
    ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - (bw * 2.f + ImGui::GetStyle().ItemSpacing.x));
    if (ImGui::Button(i18n::tr_lit("Aceptar"), ImVec2(bw, 0.f))) {
        apply_live();
        if (ui) {
            ui->editor_tile_radius = st.options_draft_tile_radius;
            ui->editor_visual_radius_m = static_cast<float>(st.options_draft_visual_m);
            ui->vsync = st.options_draft_vsync;
        }
        EditorSidePanelState ev{};
        ev.apply_editor_options = true; // run loop: applies radii (re-stream), VSync, and saves
        ev.options_tile_radius = st.options_draft_tile_radius;
        ev.options_visual_radius_m = st.options_draft_visual_m;
        ev.options_union_pct = st.options_draft_union_pct;
        ev.options_grad_pct = st.options_draft_grad_pct;
        ev.options_vsync = st.options_draft_vsync;
        ev.options_vsync_set = true;
        fire(*args.panel, ev);
        st.options_editor_open = false;
        st.options_editor_seeded = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(i18n::tr_lit("Cancelar"), ImVec2(bw, 0.f))) {
        revert();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void draw_shortcuts_editor(const MapEditorImGuiArgs& args) {
    if (!args.panel || !args.shortcuts) return;
    auto& st = args.panel->imgui_state();
    if (!st.shortcuts_editor_open) return;
    EditorShortcutTable& tab = *args.shortcuts;
    tab.capture_tick();
    ImGui::SetNextWindowSize(ImVec2(520.f, 460.f), ImGuiCond_FirstUseEver);
    float_slot_begin("atajos_editor", 80);
    const bool sc_drawn = ImGui::Begin(i18n::tr_lit("Atajos del editor"), &st.shortcuts_editor_open);
    float_slot_end();
    if (!sc_drawn) {
        ImGui::End();
        return;
    }
    ImGui::TextWrapped(
        "Clic en «Cambiar» y pulsá la combinación (Ctrl/Shift/Alt + tecla). Esc cancela la captura. "
        "Se guarda en omsi_f4_shortcuts.cfg junto al exe.");
    ImGui::Separator();
    ImGui::TextWrapped(
        "Clics (no se reasignan): Shift+click o Ctrl+click en 3D/lista = sumar/quitar del grupo. "
        "Ctrl+click en un valor = escribir. Ctrl+Z = deshacer. Ctrl+Y / Ctrl+Shift+Y / Ctrl+Shift+Z = rehacer. "
        "Ctrl+Alt+Z = undo camara, Ctrl+Shift+Alt+Z = redo camara. Shift+D clonar recuadro, Shift+S seleccionar recuadro.");
    if (ImGui::Button(i18n::tr_lit("Restaurar predeterminados"))) {
        tab.reset_defaults();
        tab.save();
        tab.listen = -1;
    }
    ImGui::Separator();
    if (ImGui::BeginTable("binds", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY,
                          ImVec2(0.f, -28.f))) {
        ImGui::TableSetupColumn(i18n::tr_lit("Función"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(i18n::tr_lit("Atajo"), ImGuiTableColumnFlags_WidthFixed, 160.f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 90.f);
        ImGui::TableHeadersRow();
        for (int i = 0; i < static_cast<int>(EditorAction::Count); ++i) {
            const auto act = static_cast<EditorAction>(i);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(EditorShortcutTable::name(act));
            ImGui::TableSetColumnIndex(1);
            if (tab.listen == i)
                ImGui::TextUnformatted(i18n::tr_lit("pulsá una tecla…"));
            else
                ImGui::TextUnformatted(EditorShortcutTable::format(tab.chords[static_cast<std::size_t>(i)]).c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::PushID(i);
            if (ImGui::Button(i18n::tr_lit("Cambiar"))) tab.listen = i;
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

void draw_adjust_hud_imgui(const MapEditorImGuiArgs& args, const ImGuiViewport* vp) {
    if (!args.adjust_hud || !args.adjust_hud->active || !args.panel || !vp) return;
    EditorAdjustHudImGui& hud = *args.adjust_hud;
    auto& st = args.panel->imgui_state();
    const bool height_mode = hud.mode == 1;
    const char* title = height_mode ? "Ajuste altura (Z)" : "Ajuste rotacion";
    const char* step_key = height_mode ? "adjust.z" : "adjust.yaw";
    const float def_step = height_mode ? 0.05f : 0.2f;

    ImGui::SetNextWindowPos(
        ImVec2(vp->WorkPos.x + 410.f, vp->WorkPos.y + vp->WorkSize.y - 88.f - 10.f), ImGuiCond_Always,
        ImVec2(0.f, 1.f));
    ImGui::SetNextWindowBgAlpha(0.94f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings;
    if (!ImGui::Begin("##adjust_hud", nullptr, flags)) {
        ImGui::End();
        return;
    }
    ImGui::TextUnformatted(title);
    ImGui::Text("Anterior: %.4g", hud.previous);
    float cur = hud.current;
    if (spin_float(st, "Actual", step_key, &cur, def_step, -1.e6f, 1.e6f, height_mode ? "%.4g" : "%.2f")) {
        hud.user_editing = true;
        hud.commit_pending = true;
        hud.commit_value = cur;
        hud.current = cur;
    }
    if (ImGui::IsItemActive())
        hud.user_editing = true;
    else if (ImGui::IsItemDeactivated())
        hud.user_editing = false;
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        hud.commit_pending = true;
        hud.commit_value = cur;
        hud.current = cur;
    }
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        ImGui::IsKeyPressed(ImGuiKey_Enter)) {
        hud.commit_pending = true;
        hud.commit_value = cur;
        hud.current = cur;
    }
    ImGui::Separator();
    if (ImGui::Button(height_mode ? i18n::tr_lit("Restaurar altura") : i18n::tr_lit("Restaurar rotacion"), ImVec2(-FLT_MIN, 0.f)))
        hud.restore_requested = true;
    ImGui::End();
}

void draw_preset_group_preview(const EditorSidePanelImGuiState::EditorPreset& pr,
                               const std::filesystem::path& omsi_root) {
    constexpr int kW = 120;
    constexpr int kH = 64;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(static_cast<float>(kW), static_cast<float>(kH)));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + static_cast<float>(kW), p0.y + static_cast<float>(kH)),
                      IM_COL32(18, 20, 22, 255), 3.f);
    if (pr.items.empty()) {
        dl->AddRect(p0, ImVec2(p0.x + static_cast<float>(kW), p0.y + static_cast<float>(kH)), IM_COL32(70, 74, 82, 255),
                    3.f);
        return;
    }
    const std::string key = "pg|" + pr.name + "|" + std::to_string(pr.items.size());
    if (g_thumb_pix.find(key) == g_thumb_pix.end()) {
        std::vector<ImU32> canvas(static_cast<std::size_t>(kW * kH), IM_COL32(22, 24, 28, 255));
        float minx = 1.e9f, miny = 1.e9f, maxx = -1.e9f, maxy = -1.e9f;
        for (const auto& it : pr.items) {
            minx = (std::min)(minx, it.dx);
            maxx = (std::max)(maxx, it.dx);
            miny = (std::min)(miny, it.dy);
            maxy = (std::max)(maxy, it.dy);
        }
        const float span = (std::max)((std::max)(maxx - minx, maxy - miny), 4.f);
        const float pad = span * 0.2f;
        minx -= pad;
        miny -= pad;
        const float den = span + pad * 2.f;
        for (const auto& it : pr.items) {
            if (it.spline)
                draw_sli_topdown_preview(it.path, it.length, it.radius, false, omsi_root, false);
            else
                fill_sco_thumb_view(it.path, omsi_root, 0);
            const std::string sk = it.spline
                                       ? (omsi::path_text(it.path) + "|" + std::to_string(it.length) + "|" +
                                          std::to_string(it.radius))
                                       : ("sco4|" + omsi::path_text(it.path) + "|0");
            auto sit = g_thumb_pix.find(sk);
            if (sit == g_thumb_pix.end()) continue;
            const int sks = it.spline ? 56 : 56;
            const float ang = it.spline ? it.heading * 0.017453292f : it.rot_x * 0.017453292f;
            const float c = std::cos(ang), s = std::sin(ang);
            const float px = ((it.dx - minx) / den) * static_cast<float>(kW - 1);
            const float py = (1.f - (it.dy - miny) / den) * static_cast<float>(kH - 1);
            constexpr int kStamp = 22;
            for (int y = 0; y < kStamp; ++y) {
                for (int x = 0; x < kStamp; ++x) {
                    const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(kStamp) - 0.5f;
                    const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(kStamp) - 0.5f;
                    const float ru = u * c - v * s;
                    const float rv = u * s + v * c;
                    const int sx = static_cast<int>((ru + 0.5f) * static_cast<float>(sks - 1));
                    const int sy = static_cast<int>((rv + 0.5f) * static_cast<float>(sks - 1));
                    if (sx < 0 || sy < 0 || sx >= sks || sy >= sks) continue;
                    const ImU32 col = sit->second[static_cast<std::size_t>(sy * sks + sx)];
                    const int r = (col >> IM_COL32_R_SHIFT) & 255;
                    const int g = (col >> IM_COL32_G_SHIFT) & 255;
                    const int b = (col >> IM_COL32_B_SHIFT) & 255;
                    if (r + g + b < 40) continue;
                    const int dxp = static_cast<int>(px) + x - kStamp / 2;
                    const int dyp = static_cast<int>(py) + y - kStamp / 2;
                    if (dxp < 0 || dyp < 0 || dxp >= kW || dyp >= kH) continue;
                    canvas[static_cast<std::size_t>(dyp * kW + dxp)] = col;
                }
            }
        }
        g_thumb_pix.emplace(key, std::move(canvas));
    }
    blit_thumb_size(dl, p0, g_thumb_pix[key], kW, kH);
    dl->AddRect(p0, ImVec2(p0.x + static_cast<float>(kW), p0.y + static_cast<float>(kH)), IM_COL32(70, 74, 82, 255),
                3.f);
}


bool dock_icon_button(const char* id, int icon, const ImVec2& size = ImVec2(28.f, 26.f)) {
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(id, size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const bool hovered = ImGui::IsItemHovered();
    const ImU32 bg = ImGui::GetColorU32(hovered ? ImGuiCol_ButtonHovered : ImGuiCol_Button);
    dl->AddRectFilled(p0, ImVec2(p0.x + size.x, p0.y + size.y), bg, 5.f);
    const ImU32 fg = ImGui::GetColorU32(ImGuiCol_Text);
    const float cx = p0.x + size.x * .5f, cy = p0.y + size.y * .5f;
    if (icon == 0) { // minimize
        dl->AddLine(ImVec2(cx - 6, cy), ImVec2(cx + 6, cy), fg, 2.f);
    } else if (icon == 1) { // restore
        dl->AddRect(ImVec2(cx - 6, cy - 5), ImVec2(cx + 5, cy + 6), fg, 1.5f);
    } else if (icon == 2) { // maximize
        dl->AddRect(ImVec2(cx - 7, cy - 6), ImVec2(cx + 7, cy + 7), fg, 1.5f);
    } else if (icon == 3) { // pencil
        dl->AddLine(ImVec2(cx - 6, cy + 5), ImVec2(cx + 5, cy - 6), fg, 2.f);
        dl->AddLine(ImVec2(cx + 4, cy - 7), ImVec2(cx + 6, cy - 5), fg, 2.f);
    } else if (icon == 4) { // trash
        dl->AddRect(ImVec2(cx - 5, cy - 4), ImVec2(cx + 5, cy + 7), fg, 1.f);
        dl->AddLine(ImVec2(cx - 7, cy - 7), ImVec2(cx + 7, cy - 7), fg, 2.f);
        dl->AddLine(ImVec2(cx - 2, cy - 9), ImVec2(cx + 2, cy - 9), fg, 2.f);
    } else { // clone
        dl->AddRect(ImVec2(cx - 6, cy - 6), ImVec2(cx + 5, cy + 5), fg, 1.f);
        dl->AddRect(ImVec2(cx - 3, cy - 3), ImVec2(cx + 8, cy + 8), fg, 1.f);
    }
    return clicked;
}

static std::string lower_ascii(const std::string& in) {
    std::string s = in;
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool docks_matches_search(const std::string& hay, const char* filter) {
    if (!filter || !*filter) return true;
    return lower_ascii(hay).find(lower_ascii(filter)) != std::string::npos;
}

bool docks_matches_search_path(const std::string& label, const std::filesystem::path& path, const char* filter) {
    return docks_matches_search(label, filter) || docks_matches_search(omsi::path_text(path.filename()), filter) ||
           docks_matches_search(omsi::path_text(path), filter);
}

void draw_hover_catalog_preview(EditorSidePanelImGuiState& st, const std::filesystem::path& p,
                                bool spline, const std::filesystem::path& omsi_root) {
    if (p.empty()) return;
    const double now = ImGui::GetTime();
    if (st.hover_preview_path != p || st.hover_preview_spline != spline) {
        st.hover_preview_path = p;
        st.hover_preview_spline = spline;
        st.hover_preview_since = now;
        return;
    }
    if (now - st.hover_preview_since < 0.42) return;
    ImGui::BeginTooltip();
    ImGui::TextUnformatted(omsi::path_utf8(p.filename()).c_str());
    if (spline) draw_sli_topdown_preview(p, 12.f, 0.f, false, omsi_root);
    else draw_sco_iso_preview(p, false, omsi_root);
    ImGui::EndTooltip();
}

void draw_fav_preset_docks(EditorSidePanel& panel, const ImGuiViewport* vp, const std::filesystem::path& omsi_root) {
    auto& st=panel.imgui_state(); load_editor_docks(st);
    const float left=400.f, rest=(std::max)(320.f,vp->WorkSize.x-left);
    const float w=st.docks_size_mode==2?rest*.80f:(st.docks_size_mode==1?360.f:(std::clamp)(rest*.60f,500.f,1000.f));
    const float h=st.docks_size_mode==2?vp->WorkSize.y*.80f:(st.docks_size_mode==1?44.f:(std::clamp)(vp->WorkSize.y*.36f,240.f,480.f));
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x+left+(rest-w)*.5f,vp->WorkPos.y+vp->WorkSize.y-96.f),ImGuiCond_Always,ImVec2(0,1));
    ImGui::SetNextWindowSize(ImVec2(w,h),ImGuiCond_Always);
    if(!ImGui::Begin(i18n::tr_lit("Contenido del editor"),nullptr,ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoCollapse)){ImGui::End();return;}
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(i18n::tr_lit("Biblioteca"));
    const float controls_w = 3.f*30.f + 2.f*3.f;
    ImGui::SameLine((std::max)(8.f,ImGui::GetWindowContentRegionMax().x-controls_w));
    if(dock_icon_button("dock_min",0)) st.docks_size_mode=1; if(ImGui::IsItemHovered())ImGui::SetTooltip("Minimizar la biblioteca"); ImGui::SameLine(0,3);
    if(dock_icon_button("dock_restore",1)) st.docks_size_mode=0; if(ImGui::IsItemHovered())ImGui::SetTooltip("Tamano normal"); ImGui::SameLine(0,3);
    if(dock_icon_button("dock_max",2)) st.docks_size_mode=2; if(ImGui::IsItemHovered())ImGui::SetTooltip("Maximizar la biblioteca");
    if(st.docks_size_mode!=1){
        ImGui::Separator();
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##dock_search",i18n::tr_lit("Buscar SCO/SLI…"),st.docks_search,sizeof(st.docks_search));
    }
    if(st.docks_pending_fav_dialog){ImGui::OpenPopup("elegir_lista_favorito");st.docks_pending_fav_dialog=false;}
    if(st.docks_pending_rename){ImGui::OpenPopup("dock_rename");st.docks_pending_rename=false;}
    ImGui::SetNextWindowSize(ImVec2(320.f,0.f),ImGuiCond_Appearing);
    if(ImGui::BeginPopupModal("dock_rename",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputText(i18n::tr_lit("Nombre"),st.docks_edit_list,sizeof(st.docks_edit_list));
        if(ImGui::Button(i18n::tr_lit("Guardar")) && st.docks_edit_list[0]) {
            if(st.docks_edit_list_kind==0 && st.docks_edit_list_index>=0 && st.docks_edit_list_index<(int)st.fav_cats.size()) st.fav_cats[(size_t)st.docks_edit_list_index].name=st.docks_edit_list;
            else if(st.docks_edit_list_kind==1 && st.docks_edit_list_index>=0 && st.docks_edit_list_index<(int)st.preset_lists.size()) {
                const std::string old=st.preset_lists[(size_t)st.docks_edit_list_index], neu=st.docks_edit_list;
                st.preset_lists[(size_t)st.docks_edit_list_index]=neu; for(auto& pr:st.presets) if(pr.list==old) pr.list=neu;
            } else if(st.docks_edit_list_kind==2 && st.docks_edit_list_index>=0 && st.docks_edit_list_index<(int)st.spray_categories.size()) st.spray_categories[(size_t)st.docks_edit_list_index].name=st.docks_edit_list;
            save_editor_docks(st); ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine(); if(ImGui::Button(i18n::tr_lit("Cancelar"))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if(ImGui::BeginPopupModal("elegir_lista_favorito",nullptr,ImGuiWindowFlags_AlwaysAutoResize)){
        ImGui::TextUnformatted(i18n::tr_lit("Guardar favorito en lista"));
        if(st.fav_cats.empty()) st.fav_cats.push_back({"General",{}});
        for(int i=0;i<(int)st.fav_cats.size();++i){if(ImGui::Selectable(st.fav_cats[(size_t)i].name.c_str(),st.fav_active_cat==i)){auto& c=st.fav_cats[(size_t)i];bool dup=false;for(auto&q:c.items)if(q.path==st.docks_pending_item&&q.spline==st.docks_pending_item_spline)dup=true;if(!dup&&!st.docks_pending_item.empty())c.items.push_back({st.docks_pending_item_spline,st.docks_pending_item,omsi::path_utf8(st.docks_pending_item.filename())});st.fav_active_cat=i;st.docks_pending_item.clear();save_editor_docks(st);ImGui::CloseCurrentPopup();}}
        if(ImGui::Button(i18n::tr_lit("Cancelar"))){st.docks_pending_item.clear();ImGui::CloseCurrentPopup();} ImGui::EndPopup();
    }
    if (st.docks_size_mode == 1) { ImGui::End(); return; }

    // Same layout in every tab: [lists column: toolbar on top, then the lists] | [content column: action bar on top, then items].
    auto tt = [](const char* t) { if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", t); };
    // "+ lista" fills the whole width of the lists column; rename/clone/delete sit on the active list's own row.
    std::function<void()> cur_clone, cur_del;
    int cur_kind = 0, cur_active = 0;
    bool cur_can_del = false;
    const float row_btn_w = 3.f * 30.f + 2.f * 3.f + 4.f;
    auto list_toolbar = [&](const char* id, const char* new_popup, int kind, int active, bool can_delete, auto&& on_clone, auto&& on_delete) {
        ImGui::PushID(id);
        if (ImGui::Button("+ lista", ImVec2(-FLT_MIN, 0))) ImGui::OpenPopup(new_popup);
        tt("Crea una lista nueva.");
        ImGui::PopID();
        ImGui::Separator();
        cur_kind = kind; cur_active = active; cur_can_del = can_delete;
        cur_clone = on_clone; cur_del = on_delete;
    };
    // ImGui 1.92 Selectable takes a negative width literally (inverted box, label clipped away): the list
    // names did not show. Always pass the positive width left for the label.
    auto row_w = [&](int idx) {
        return (std::max)(40.f, ImGui::GetContentRegionAvail().x - (idx == cur_active ? row_btn_w : 0.f));
    };
    auto row_actions = [&](int idx) {
        if (idx != cur_active) return;
        ImGui::SameLine(0, 4);
        if (dock_icon_button("rename", 3)) {
            st.docks_edit_list_kind = cur_kind;
            st.docks_edit_list_index = cur_active;
            st.docks_pending_rename = true;
        }
        tt("Renombrar la lista seleccionada.");
        ImGui::SameLine(0, 3);
        if (dock_icon_button("clone", 5) && cur_clone) cur_clone();
        tt("Duplicar la lista seleccionada con todo su contenido.");
        ImGui::SameLine(0, 3);
        if (dock_icon_button("del", 4) && cur_can_del && cur_del) cur_del();
        tt("Borrar la lista seleccionada (siempre queda al menos una).");
    };
    auto name_popup = [&](const char* popup, char* buf, std::size_t cap, auto&& on_create) {
        if (ImGui::BeginPopup(popup)) {
            ImGui::InputText(i18n::tr_lit("Nombre"), buf, cap);
            if (ImGui::Button("Crear") && buf[0]) { on_create(); buf[0] = 0; save_editor_docks(st); ImGui::CloseCurrentPopup(); }
            ImGui::EndPopup();
        }
    };
    struct FavDrag { int cat; int item; bool spline; char path[1024]; };
    if (ImGui::BeginTabBar("content_tabs")) {
        // ───────────────────────────── Favoritos ─────────────────────────────
        if (ImGui::BeginTabItem(i18n::tr_lit("Favoritos"))) {
            st.docks_tab = 0;
            if (st.fav_cats.empty()) st.fav_cats.push_back({"General", {}});
            st.fav_active_cat = (std::clamp)(st.fav_active_cat, 0, static_cast<int>(st.fav_cats.size()) - 1);
            if (ImGui::BeginTable("dock_layout", 2, ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) {
                ImGui::TableSetupColumn(i18n::tr_lit("Listas"), ImGuiTableColumnFlags_WidthFixed, (std::clamp)(w * 0.40f, 220.f, 320.f));
                ImGui::TableSetupColumn(i18n::tr_lit("Contenido"), ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                list_toolbar("fav", "fav_new_list", 0, st.fav_active_cat, st.fav_cats.size() > 1,
                             [&] { auto c = st.fav_cats[(size_t)st.fav_active_cat]; c.name += " copy"; st.fav_cats.push_back(std::move(c)); st.fav_active_cat = (int)st.fav_cats.size() - 1; save_editor_docks(st); },
                             [&] { st.fav_cats.erase(st.fav_cats.begin() + st.fav_active_cat); st.fav_active_cat = (std::max)(0, st.fav_active_cat - 1); save_editor_docks(st); });
                name_popup("fav_new_list", st.fav_new_cat, sizeof(st.fav_new_cat), [&] { st.fav_cats.push_back({st.fav_new_cat, {}}); st.fav_active_cat = (int)st.fav_cats.size() - 1; });
                for (int i = 0; i < (int)st.fav_cats.size(); ++i) {
                    ImGui::PushID(1000 + i);
                    const std::string& nm = st.fav_cats[(size_t)i].name;
                    char trunc[56];
                    std::snprintf(trunc, sizeof(trunc), "%.34s%s  (%zu)", nm.c_str(), nm.size() > 34 ? "…" : "", st.fav_cats[(size_t)i].items.size());
                    if (ImGui::Selectable(trunc, st.fav_active_cat == i, ImGuiSelectableFlags_None, ImVec2(row_w(i), 0))) st.fav_active_cat = i;
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\nArrastra aqui un favorito para moverlo a esta lista.", nm.c_str());
                    if (ImGui::BeginDragDropTarget()) {
                        if (auto* pl = ImGui::AcceptDragDropPayload("FAV_MOVE"); pl && pl->DataSize == (int)sizeof(FavDrag)) {
                            const auto q = *static_cast<const FavDrag*>(pl->Data);
                            if (q.cat >= 0 && q.cat < (int)st.fav_cats.size() && q.item >= 0 && q.item < (int)st.fav_cats[(size_t)q.cat].items.size() && q.cat != i) {
                                auto moved = st.fav_cats[(size_t)q.cat].items[(size_t)q.item];
                                st.fav_cats[(size_t)q.cat].items.erase(st.fav_cats[(size_t)q.cat].items.begin() + q.item);
                                st.fav_cats[(size_t)i].items.push_back(std::move(moved));
                                save_editor_docks(st);
                            }
                        }
                        ImGui::EndDragDropTarget();
                    }
                    row_actions(i);
                    ImGui::PopID();
                }
                ImGui::TableNextColumn();
                ImGui::TextDisabled("Click = elegir para colocar. Arrastra a otra lista para mover. x = quitar. Se guarda al instante.");
                ImGui::Separator();
                auto& cat = st.fav_cats[(size_t)st.fav_active_cat];
                for (int ii = 0; ii < (int)cat.items.size(); ++ii) {
                    auto& it = cat.items[(size_t)ii];
                    if (!docks_matches_search_path(it.label, it.path, st.docks_search)) continue;
                    ImGui::PushID(20000 + ii);
                    if (it.spline) draw_sli_topdown_preview(it.path, 12, 0, false, omsi_root);
                    else draw_sco_iso_preview(it.path, false, omsi_root);
                    ImGui::SameLine();
                    char trunc[56];
                    std::snprintf(trunc, sizeof(trunc), "%.44s%s", it.label.c_str(), it.label.size() > 44 ? "…" : "");
                    const float lab_w = (std::max)(80.f, ImGui::GetContentRegionAvail().x - 44.f);
                    if (ImGui::Selectable(trunc, false, ImGuiSelectableFlags_None, ImVec2(lab_w, 56.f))) {
                        EditorSidePanelState ev{};
                        bool found = false;
                        if (it.spline) {
                            const auto& cl = panel.catalog_splines();
                            for (int ci = 0; ci < (int)cl.size(); ++ci) if (cl[(size_t)ci].path == it.path) { ev.select_catalog_spline = ci; found = true; break; }
                        } else {
                            const auto& cl = panel.catalog_objects();
                            for (int ci = 0; ci < (int)cl.size(); ++ci) if (cl[(size_t)ci].path == it.path) { ev.select_catalog_object = ci; found = true; break; }
                        }
                        if (!found) panel.set_status("Este favorito no esta en el catalogo cargado: abri su catalogo/carpeta primero");
                        fire(panel, ev);
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", it.label.c_str());
                    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                        FavDrag q{st.fav_active_cat, ii, it.spline, {}};
                        std::snprintf(q.path, sizeof(q.path), "%s", omsi::path_text(it.path).c_str());
                        ImGui::SetDragDropPayload("FAV_MOVE", &q, sizeof(q));
                        ImGui::TextUnformatted(it.label.c_str());
                        ImGui::EndDragDropSource();
                    }
                    if (ImGui::IsItemHovered()) draw_hover_catalog_preview(st, it.path, it.spline, omsi_root);
                    ImGui::SameLine();
                    if (ImGui::SmallButton("×")) { cat.items.erase(cat.items.begin() + ii); save_editor_docks(st); ImGui::PopID(); break; }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        // ───────────────────────────── Presets ─────────────────────────────
        if (ImGui::BeginTabItem(i18n::tr_lit("Presets"))) {
            st.docks_tab = 1;
            if (st.preset_lists.empty()) st.preset_lists.push_back("General");
            st.preset_active_list = (std::clamp)(st.preset_active_list, 0, (int)st.preset_lists.size() - 1);
            if (ImGui::BeginTable("dock_layout", 2, ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) {
                ImGui::TableSetupColumn(i18n::tr_lit("Listas"), ImGuiTableColumnFlags_WidthFixed, (std::clamp)(w * 0.40f, 220.f, 320.f));
                ImGui::TableSetupColumn(i18n::tr_lit("Contenido"), ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                list_toolbar("preset", "preset_new_list", 1, st.preset_active_list, st.preset_lists.size() > 1,
                             [&] {
                                 const std::string old_list = st.preset_lists[(size_t)st.preset_active_list], cp = old_list + " copy";
                                 std::vector<EditorSidePanelImGuiState::EditorPreset> copies;
                                 for (const auto& pr : st.presets) if (pr.list == old_list) { auto c = pr; c.list = cp; copies.push_back(std::move(c)); }
                                 st.preset_lists.push_back(cp);
                                 for (auto& c : copies) st.presets.push_back(std::move(c));
                                 st.preset_active_list = (int)st.preset_lists.size() - 1;
                                 save_editor_docks(st);
                             },
                             [&] {
                                 const std::string doomed = st.preset_lists[(size_t)st.preset_active_list];
                                 st.presets.erase(std::remove_if(st.presets.begin(), st.presets.end(), [&](const auto& pr) { return pr.list == doomed; }), st.presets.end());
                                 st.preset_lists.erase(st.preset_lists.begin() + st.preset_active_list);
                                 st.preset_active_list = (std::max)(0, st.preset_active_list - 1);
                                 save_editor_docks(st);
                             });
                name_popup("preset_new_list", st.preset_new_list, sizeof(st.preset_new_list), [&] { st.preset_lists.push_back(st.preset_new_list); st.preset_active_list = (int)st.preset_lists.size() - 1; });
                for (int i = 0; i < (int)st.preset_lists.size(); ++i) {
                    int n = 0;
                    for (const auto& pr : st.presets) n += pr.list == st.preset_lists[(size_t)i] ? 1 : 0;
                    char lab[96];
                    std::snprintf(lab, sizeof(lab), "%.34s  (%d)##pl%d", st.preset_lists[(size_t)i].c_str(), n, i);
                    ImGui::PushID(2000 + i);
                    if (ImGui::Selectable(lab, st.preset_active_list == i, ImGuiSelectableFlags_None, ImVec2(row_w(i), 0))) st.preset_active_list = i;
                    row_actions(i);
                    ImGui::PopID();
                }
                ImGui::TableNextColumn();
                ImGui::TextDisabled("Presets = selecciones guardadas (varios objetos/splines con sus posiciones relativas). Click = pegar.");
                ImGui::Separator();
                const std::string cur = st.preset_lists[(size_t)st.preset_active_list];
                for (int pi = 0; pi < (int)st.presets.size(); ++pi) {
                    auto& pr = st.presets[(size_t)pi];
                    if (pr.list != cur || !docks_matches_search(pr.name, st.docks_search)) continue;
                    ImGui::PushID(30000 + pi);
                    const float pr_w = (std::max)(60.f, ImGui::GetContentRegionAvail().x -
                                                            ImGui::CalcTextSize("0000 elementos").x - 8.f);
                    if (ImGui::Selectable(pr.name.c_str(), false, 0, ImVec2(pr_w, 32))) { EditorSidePanelState ev{}; ev.spawn_preset_index = pi; fire(panel, ev); }
                    ImGui::SameLine();
                    ImGui::TextDisabled("%d elementos", (int)pr.items.size());
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        // ───────────────────────────── Aerosol ─────────────────────────────
        if (ImGui::BeginTabItem(i18n::tr_lit("Aerosol"))) {
            st.docks_tab = 2;
            if (st.spray_categories.empty()) st.spray_categories.push_back({"Pasto", {}, true, false});
            st.spray_active_category = (std::clamp)(st.spray_active_category, 0, (int)st.spray_categories.size() - 1);
            if (ImGui::BeginTable("dock_layout", 2, ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) {
                ImGui::TableSetupColumn(i18n::tr_lit("Listas"), ImGuiTableColumnFlags_WidthFixed, (std::clamp)(w * 0.40f, 220.f, 320.f));
                ImGui::TableSetupColumn(i18n::tr_lit("Contenido"), ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                list_toolbar("spray", "new_spray_cat", 2, st.spray_active_category, st.spray_categories.size() > 1,
                             [&] { auto cp = st.spray_categories[(size_t)st.spray_active_category]; cp.name += " copy"; st.spray_categories.push_back(std::move(cp)); st.spray_active_category = (int)st.spray_categories.size() - 1; save_editor_docks(st); },
                             [&] { st.spray_categories.erase(st.spray_categories.begin() + st.spray_active_category); st.spray_active_category = (std::max)(0, st.spray_active_category - 1); save_editor_docks(st); });
                name_popup("new_spray_cat", st.spray_new_category, sizeof(st.spray_new_category), [&] { st.spray_categories.push_back({st.spray_new_category, {}, true, false}); st.spray_active_category = (int)st.spray_categories.size() - 1; });
                for (int ci = 0; ci < (int)st.spray_categories.size(); ++ci) {
                    ImGui::PushID(40000 + ci);
                    ImGui::Checkbox("##cat_on", &st.spray_categories[(size_t)ci].enabled);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("La lista participa del aerosol");
                    ImGui::SameLine();
                    const std::string& nm = st.spray_categories[(size_t)ci].name;
                    char trunc[64];
                    std::snprintf(trunc, sizeof(trunc), "%.32s%s  (%zu)", nm.c_str(), nm.size() > 32 ? "…" : "", st.spray_categories[(size_t)ci].items.size());
                    if (ImGui::Selectable(trunc, st.spray_active_category == ci, ImGuiSelectableFlags_None, ImVec2((std::max)(60.f, ImGui::GetContentRegionAvail().x - (ci == cur_active ? row_btn_w : 0.f) - 4.f), 0))) st.spray_active_category = ci;

                    if (ImGui::BeginDragDropTarget()) {
                        if (auto* pl = ImGui::AcceptDragDropPayload("SCO_PATH")) {
                            const char* path = (const char*)pl->Data;
                            if (path && *path && std::filesystem::path(path).extension() == ".sco")
                                st.spray_categories[(size_t)ci].items.push_back({std::filesystem::path(path), omsi::path_utf8(std::filesystem::path(path).filename()), true, 1});
                            save_editor_docks(st);
                        }
                        ImGui::EndDragDropTarget();
                    }
                    row_actions(ci);
                    ImGui::PopID();
                }
                ImGui::TableNextColumn();
                // action bar (same place as the other tabs: top of the content column)
                ImGui::Checkbox(i18n::tr_lit("Activar aerosol (LMB sostenido)"), &st.spray_enabled);
                tt("Con el aerosol activo, mantene el click izquierdo sobre el terreno para pintar objetos al azar de las listas marcadas.");
                ImGui::SameLine();
                if (ImGui::SmallButton(st.spray_pick_mode ? i18n::tr_lit("Cancelar pick") : i18n::tr_lit("Pick SCO en 3D"))) st.spray_pick_mode = !st.spray_pick_mode;
                tt("Click en objetos del mapa para agregarlos a la lista (no pinta hasta cancelar).");
                ImGui::SameLine();
                const auto spray_root = catalog_initial_dir(omsi_root, false);
                if (ImGui::SmallButton(i18n::tr_lit("Catálogo…"))) { st.catalog_browser_to_spray = true; catalog_browser_open(st, spray_root, false, false); }
                tt("Agrega .sco a la lista elegida desde el catalogo con miniaturas.");
                ImGui::SameLine();
                if (ImGui::SmallButton(i18n::tr_lit("Carpeta…"))) { st.catalog_browser_to_spray = true; catalog_browser_open(st, spray_root, false, true); }
                tt("Agrega todos los .sco de una carpeta.");
                if (st.spray_pick_mode) ImGui::TextDisabled("%s", i18n::tr_lit("Pick activo: click SCOs (no pinta hasta cancelar)"));
                if (ImGui::CollapsingHeader("Ajustes de pintado")) {
                    ImGui::SliderFloat("Velocidad (obj/s)", &st.spray_speed, 1, 180);
                    ImGui::SliderFloat(i18n::tr_lit("Zona (m)"), &st.spray_radius, .25f, 100);
                    ImGui::SliderFloat(i18n::tr_lit("Separación (m)"), &st.spray_spacing, 0, 30);
                    ImGui::SliderInt(i18n::tr_lit("Definición"), &st.spray_definition, 4, 128);
                    ImGui::SliderInt(i18n::tr_lit("Cantidad 0=sin límite"), &st.spray_max_count, 0, 5000);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Pincel: maximo de objetos por trazo.\nLazo: objetos dentro del area (si es 0 se usa Definicion x 8).\nCambiar cualquier ajuste regenera el ultimo lote (queda seleccionado).");
                    const char* sh[] = {i18n::tr_lit("Círculo"), i18n::tr_lit("Cuadrado"), i18n::tr_lit("Lazo")};
                    int si = (int)st.spray_shape;
                    if (ImGui::Combo(i18n::tr_lit("Forma"), &si, sh, 3)) st.spray_shape = (EditorSidePanelImGuiState::SprayShapeUi)si;
                    ImGui::SliderFloat(i18n::tr_lit("Rotación yaw (°)"), &st.spray_yaw_deg, 0.f, 360.f, "%.1f");
                    if (ImGui::Button(st.spray_random_rotation ? "Rotación aleatoria: ON" : "Rotación aleatoria: OFF")) st.spray_random_rotation = !st.spray_random_rotation;
                    ImGui::TextDisabled("Cada spawn elige un SCO al azar (segun el peso).");
                }
                ImGui::Separator();
                auto& cat = st.spray_categories[(size_t)st.spray_active_category];
                for (int ii = 0; ii < (int)cat.items.size(); ++ii) {
                    auto& it = cat.items[(size_t)ii];
                    if (!docks_matches_search_path(it.label, it.path, st.docks_search)) continue;
                    ImGui::PushID(50000 + ii);
                    ImGui::Checkbox("##on", &it.enabled);
                    ImGui::SameLine();
                    ImGui::TextUnformatted(it.label.c_str());
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(72);
                    ImGui::DragFloat("##w", &it.weight, .05f, .05f, 20);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Peso: probabilidad relativa de salir");
                    ImGui::SameLine();
                    if (ImGui::SmallButton("×")) { cat.items.erase(cat.items.begin() + ii); save_editor_docks(st); ImGui::PopID(); break; }
                    if (ImGui::BeginDragDropSource()) {
                        struct P { int cat; int item; } q{st.spray_active_category, ii};
                        ImGui::SetDragDropPayload("SPRAY_MOVE", &q, sizeof(q));
                        ImGui::TextUnformatted(it.label.c_str());
                        ImGui::EndDragDropSource();
                    }
                    ImGui::PopID();
                }
                if (ImGui::BeginDragDropTarget()) {
                    if (auto* pl = ImGui::AcceptDragDropPayload("SCO_PATH")) {
                        const char* path = (const char*)pl->Data;
                        if (path && *path) { auto pp = std::filesystem::path(path); if (pp.extension() == ".sco") cat.items.push_back({pp, omsi::path_utf8(pp.filename()), true, 1}); save_editor_docks(st); }
                    }
                    if (auto* pl = ImGui::AcceptDragDropPayload("SPRAY_MOVE")) {
                        struct P { int cat; int item; };
                        if (pl->DataSize == (int)sizeof(P)) {
                            auto q = *static_cast<const P*>(pl->Data);
                            if (q.cat >= 0 && q.cat < (int)st.spray_categories.size() && q.item >= 0 && q.item < (int)st.spray_categories[(size_t)q.cat].items.size() && q.cat != st.spray_active_category) {
                                auto item = st.spray_categories[(size_t)q.cat].items[(size_t)q.item];
                                st.spray_categories[(size_t)q.cat].items.erase(st.spray_categories[(size_t)q.cat].items.begin() + q.item);
                                cat.items.push_back(std::move(item));
                                save_editor_docks(st);
                            }
                        }
                    }
                    ImGui::EndDragDropTarget();
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

} // namespace

std::filesystem::path editor_docks_path() {
    return options::editor_ui_prefs_path().parent_path() / "omsi_f4_docks.cfg";
}

void load_editor_docks(EditorSidePanelImGuiState& st) {
    if (st.docks_loaded) return;
    st.docks_loaded = true;
    std::ifstream in(editor_docks_path());
    if (!in) return;
    st.fav_cats.clear(); st.presets.clear(); st.preset_lists.clear(); st.spray_categories.clear();
    std::string line;
    EditorSidePanelImGuiState::CatalogFavCat* fav = nullptr;
    EditorSidePanelImGuiState::EditorPreset* pr = nullptr;
    EditorSidePanelImGuiState::SprayCategory* spray = nullptr;
    EditorSidePanelImGuiState::EditorPresetItem cur{};
    bool have_item=false, in_preset=false;
    auto flush=[&](){ if(have_item && pr){ if(!cur.path.empty()) pr->items.push_back(cur); } have_item=false; cur={}; };
    auto make_fav=[&](const std::string& n){ st.fav_cats.push_back({n,{}}); fav=&st.fav_cats.back(); pr=nullptr; spray=nullptr; };
    while(std::getline(in,line)){ if(!line.empty()&&line.back()=='\r') line.pop_back();
        if(line.rfind("collapsed=",0)==0) st.docks_collapsed=std::atoi(line.c_str()+10)!=0;
        else if(line.rfind("size=",0)==0) st.docks_size_mode=std::clamp(std::atoi(line.c_str()+5),0,2);
        else if(line.rfind("tab=",0)==0) st.docks_tab=std::clamp(std::atoi(line.c_str()+4),0,2);
        else if(line.rfind("search=",0)==0) std::snprintf(st.docks_search,sizeof(st.docks_search),"%s",line.c_str()+7);
        else if(line.rfind("fav_active=",0)==0) st.fav_active_cat=std::atoi(line.c_str()+11);
        else if(line.rfind("preset_active=",0)==0) st.preset_active_list=std::atoi(line.c_str()+14);
        else if(line.rfind("spray_active=",0)==0) st.spray_active_category=std::atoi(line.c_str()+13);
        else if(line.rfind("spray_stop_no_fit=",0)==0) st.spray_stop_no_fit=std::atoi(line.c_str()+19)!=0;
        else if(line.rfind("[fav]",0)==0){ flush(); make_fav(line.substr(5)); in_preset=false; }
        else if(line.rfind("item=",0)==0 && fav){ const auto bar=line.find('|',5); if(bar!=std::string::npos){ EditorSidePanelImGuiState::CatalogFavItem it{}; it.spline=std::atoi(line.substr(5,bar-5).c_str())!=0; it.path=line.substr(bar+1); it.label=omsi::path_utf8(it.path.filename()); if(!it.path.empty()) fav->items.push_back(std::move(it)); }}
        else if(line.rfind("[presetlist]",0)==0){ flush(); st.preset_lists.push_back(line.substr(12)); fav=nullptr; spray=nullptr; pr=nullptr; in_preset=false; }
        else if(line.rfind("[preset]",0)==0){ flush(); const auto bar=line.find('|',8); st.presets.emplace_back(); pr=&st.presets.back(); if(bar!=std::string::npos){pr->list=line.substr(8,bar-8);pr->name=line.substr(bar+1);}else{pr->list="General";pr->name=line.substr(8);} if(pr->list.empty())pr->list="General"; fav=nullptr; spray=nullptr; in_preset=true; }
        else if(line.rfind("pitem=",0)==0 && pr){ flush(); cur={}; cur.spline=std::atoi(line.c_str()+6)!=0; have_item=true; }
        else if(have_item && line.rfind("path=",0)==0) cur.path=line.substr(5);
        else if(have_item && line.rfind("dx=",0)==0) cur.dx=std::strtof(line.c_str()+3,nullptr);
        else if(have_item && line.rfind("dy=",0)==0) cur.dy=std::strtof(line.c_str()+3,nullptr);
        else if(have_item && line.rfind("dz=",0)==0) cur.dz=std::strtof(line.c_str()+3,nullptr);
        else if(have_item && line.rfind("hdg=",0)==0) cur.heading=std::strtof(line.c_str()+4,nullptr);
        else if(have_item && line.rfind("rx=",0)==0) cur.rot_x=std::strtof(line.c_str()+3,nullptr);
        else if(have_item && line.rfind("ry=",0)==0) cur.rot_y=std::strtof(line.c_str()+3,nullptr);
        else if(have_item && line.rfind("rz=",0)==0) cur.rot_z=std::strtof(line.c_str()+3,nullptr);
        else if(have_item && line.rfind("len=",0)==0) cur.length=std::strtof(line.c_str()+4,nullptr);
        else if(have_item && line.rfind("rad=",0)==0) cur.radius=std::strtof(line.c_str()+4,nullptr);
        else if(line.rfind("[spray]",0)==0){ flush(); const auto bar=line.find('|',7); st.spray_categories.emplace(st.spray_categories.end()); spray=&st.spray_categories.back(); spray->name=(bar==std::string::npos?line.substr(7):line.substr(7,bar-7)); spray->enabled=(bar==std::string::npos||std::atoi(line.c_str()+bar+1)!=0); fav=nullptr; pr=nullptr; in_preset=false; }
        else if(line.rfind("sitem=",0)==0 && spray){ const auto b=line.find('|',6), c=line.find('|',b==std::string::npos?b:b+1); if(b!=std::string::npos&&c!=std::string::npos){ EditorSidePanelImGuiState::SprayItem it{}; it.enabled=std::atoi(line.substr(6,b-6).c_str())!=0; it.weight=std::strtof(line.substr(b+1,c-b-1).c_str(),nullptr); it.path=line.substr(c+1); it.label=omsi::path_utf8(it.path.filename()); if(!it.path.empty()&&it.path.extension()==".sco") spray->items.push_back(std::move(it)); }}
        else if(line.rfind("spray_speed=",0)==0) st.spray_speed=std::strtof(line.c_str()+12,nullptr);
        else if(line.rfind("spray_radius=",0)==0) st.spray_radius=std::strtof(line.c_str()+13,nullptr);
        else if(line.rfind("spray_spacing=",0)==0) st.spray_spacing=std::strtof(line.c_str()+14,nullptr);
        else if(line.rfind("spray_definition=",0)==0) st.spray_definition=std::clamp(std::atoi(line.c_str()+17),4,128);
        else if(line.rfind("spray_max_count=",0)==0) st.spray_max_count=(std::max)(0,std::atoi(line.c_str()+16));
        else if(line.rfind("spray_shape=",0)==0) st.spray_shape=(EditorSidePanelImGuiState::SprayShapeUi)std::clamp(std::atoi(line.c_str()+12),0,2);
        else if(line.rfind("spray_random_rotation=",0)==0) st.spray_random_rotation=std::atoi(line.c_str()+22)!=0;
        else if(line.rfind("spray_yaw_deg=",0)==0) st.spray_yaw_deg=std::strtof(line.c_str()+14,nullptr);
    }
    flush();
    if(st.fav_cats.empty()) st.fav_cats.push_back({"General",{}});
    if(st.preset_lists.empty()) st.preset_lists.push_back("General");
    if(st.spray_categories.empty()) st.spray_categories.push_back({"Pasto",{},true,false});
    st.fav_active_cat=std::clamp(st.fav_active_cat,0,(int)st.fav_cats.size()-1);
    st.preset_active_list=std::clamp(st.preset_active_list,0,(int)st.preset_lists.size()-1);
    st.spray_active_category=std::clamp(st.spray_active_category,0,(int)st.spray_categories.size()-1);
}

void save_editor_docks(const EditorSidePanelImGuiState& st) {
    std::ostringstream ss;
    ss << "version=2\n";
    ss << "collapsed=" << (st.docks_collapsed ? 1 : 0) << "\n";
    ss << "size=" << st.docks_size_mode << "\n";
    ss << "tab=" << st.docks_tab << "\n";
    ss << "search=" << st.docks_search << "\n";
    ss << "fav_active=" << st.fav_active_cat << "\n";
    ss << "preset_active=" << st.preset_active_list << "\n";
    ss << "spray_active=" << st.spray_active_category << "\n";
    ss << "spray_stop_no_fit=" << (st.spray_stop_no_fit ? 1 : 0) << "\n";
    for (const auto& c : st.fav_cats) {
        ss << "[fav]" << c.name << "\n";
        for (const auto& it : c.items) ss << "item=" << (it.spline ? 1 : 0) << "|" << omsi::path_text(it.path) << "\n";
    }
    for (const auto& name : st.preset_lists) ss << "[presetlist]" << name << "\n";
    for (const auto& pr : st.presets) {
        ss << "[preset]" << pr.list << "|" << pr.name << "\n";
        for (const auto& it : pr.items) {
            ss << "pitem=" << (it.spline ? 1 : 0) << "\n";
            ss << "path=" << omsi::path_text(it.path) << "\n";
            ss << "dx=" << it.dx << "\n"; ss << "dy=" << it.dy << "\n"; ss << "dz=" << it.dz << "\n";
            ss << "hdg=" << it.heading << "\n"; ss << "rx=" << it.rot_x << "\n"; ss << "ry=" << it.rot_y << "\n"; ss << "rz=" << it.rot_z << "\n";
            ss << "len=" << it.length << "\n"; ss << "rad=" << it.radius << "\n";
        }
    }
    for (const auto& c : st.spray_categories) {
        ss << "[spray]" << c.name << "|" << (c.enabled ? 1 : 0) << "\n";
        for (const auto& it : c.items) ss << "sitem=" << (it.enabled ? 1 : 0) << "|" << it.weight << "|" << omsi::path_text(it.path) << "\n";
    }
    ss << "spray_speed=" << st.spray_speed << "\n";
    ss << "spray_radius=" << st.spray_radius << "\n";
    ss << "spray_spacing=" << st.spray_spacing << "\n";
    ss << "spray_definition=" << st.spray_definition << "\n";
    ss << "spray_max_count=" << st.spray_max_count << "\n";
    ss << "spray_shape=" << static_cast<int>(st.spray_shape) << "\n";
    ss << "spray_random_rotation=" << (st.spray_random_rotation ? 1 : 0) << "\n";
    ss << "spray_yaw_deg=" << st.spray_yaw_deg << "\n";
    const std::string payload = ss.str();
    static std::string last_payload;
    if (payload == last_payload) return;
    last_payload = payload;
    std::error_code ec;
    const auto path = editor_docks_path();
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::trunc);
    if (!out) return;
    out << payload;
}
void invalidate_catalog_thumb_cache(const std::filesystem::path& src) {
    std::error_code ec;
    if (src.empty()) {
        catalog_thumbs_clear_mem();
        std::filesystem::remove_all(catalog_thumbs_dir(), ec);
        return;
    }
    const auto norm = omsi::path_text(src.lexically_normal());
    for (auto it = g_thumb_pix.begin(); it != g_thumb_pix.end();) {
        if (it->first.find(norm) != std::string::npos)
            it = g_thumb_pix.erase(it);
        else
            ++it;
    }
    const auto evict_disk = [&](const std::filesystem::path& tga) {
        std::filesystem::remove(tga, ec);
        if (g_thumb_srv_evict) g_thumb_srv_evict(tga);
    };
    evict_disk(catalog_thumb_tga(src, 0));
    evict_disk(catalog_thumb_tga(src, 1));
    const auto h = std::hash<std::string>{}(norm);
    char buf[40]{};
    std::snprintf(buf, sizeof(buf), "%016llx.tga", static_cast<unsigned long long>(h));
    evict_disk(catalog_thumbs_dir() / buf);
}


static bool catalog_path_ext_ci(const std::filesystem::path& p, const char* ext) {
    const std::string e = omsi::path_text(p.extension());
    const std::string want(ext);
    if (e.size() != want.size()) return false;
    for (std::size_t i = 0; i < e.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(e[i])) != std::tolower(static_cast<unsigned char>(want[i])))
            return false;
    }
    return true;
}

void browser_index_ensure(const std::filesystem::path& root) {
    std::error_code ec;
    if (root.empty() || !std::filesystem::is_directory(root, ec)) return;
    const auto n = root.lexically_normal();
    const auto now = std::chrono::steady_clock::now();
    std::shared_ptr<BrowserIndex> cur;
    {
        std::lock_guard<std::mutex> lock(g_browser_idx.mu);
        cur = g_browser_idx.current;
        if (cur && cur->ready.load() && cur->root_native == n.native() &&
            now - cur->built_at < std::chrono::seconds(120))
            return;
        if (g_browser_idx.building.load()) {
            g_browser_idx.pending = n;
            return;
        }
        g_browser_idx.building.store(true);
    }
    std::thread([n]() {
        auto idx = std::make_shared<BrowserIndex>();
        idx->root_native = n.native();
        idx->built_at = std::chrono::steady_clock::now();
        try {
            browser_index_build(n, idx);
        } catch (const std::filesystem::filesystem_error&) {
        }
        idx->ready.store(true);
        std::filesystem::path next;
        {
            std::lock_guard<std::mutex> lock(g_browser_idx.mu);
            g_browser_idx.current = idx;
            g_browser_idx.building.store(false);
            next = std::move(g_browser_idx.pending);
            g_browser_idx.pending.clear();
        }
        if (!next.empty()) browser_index_ensure(next);
    }).detach();
}

bool browser_index_ready(const std::filesystem::path& root) {
    const auto n = root.lexically_normal();
    std::lock_guard<std::mutex> lock(g_browser_idx.mu);
    return g_browser_idx.current && g_browser_idx.current->ready.load() &&
           g_browser_idx.current->root_native == n.native();
}

bool browser_index_busy() { return g_browser_idx.building.load(); }

bool browser_name_matches(std::string_view name, std::string_view filt_lower) {
    if (filt_lower.empty()) return true;
    std::string n(name);
    for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return n.find(filt_lower) != std::string::npos;
}

bool browser_dir_has_match(const std::filesystem::path& dir, std::string_view filt_lower,
                           const std::function<bool(const std::filesystem::path&)>& file_ok, int,
                           int&) {
    if (filt_lower.empty()) return true;
    std::shared_ptr<BrowserIndex> idx;
    {
        std::lock_guard<std::mutex> lock(g_browser_idx.mu);
        idx = g_browser_idx.current;
    }
    if (!idx || !idx->ready.load()) return false;
    const std::wstring dir_n = dir.lexically_normal().native();
    if (!native_is_under(dir_n, idx->root_native) && dir_n != idx->root_native) return false;
    for (const auto& e : idx->ents) {
        if (!native_is_under(e.full, dir_n)) continue;
        if (e.full == dir_n) continue;
        if (!browser_name_matches(e.name_lower, filt_lower)) continue;
        if (e.is_dir) return true;
        if (!file_ok || file_ok(std::filesystem::path(e.full))) return true;
    }
    return false;
}

std::filesystem::path catalog_browser_start_dir(const std::filesystem::path& omsi_root, const bool for_spline) {
    if (!omsi_root.empty()) {
        const auto d = omsi_root / (for_spline ? "Splines" : "Sceneryobjects");
        std::error_code ec;
        if (std::filesystem::is_directory(d, ec)) return d;
        if (std::filesystem::is_directory(omsi_root, ec)) return omsi_root;
    }
    std::error_code ec;
    return std::filesystem::current_path(ec);
}

void catalog_browser_draw_modal(EditorSidePanelImGuiState& st, const MapEditorImGuiArgs& args) {
    if (!st.catalog_browser_open) return;
    const char* ext = st.catalog_browser_spline ? ".sli" : ".sco";
    const char* title = st.catalog_browser_scan_dir ? "Escanear carpeta de tipos" : "Abrir .sco / .sli";

    ImGui::OpenPopup("CatalogBrowser");
    ImGui::SetNextWindowSize(ImVec2(620.f, 520.f), ImGuiCond_FirstUseEver);
    if (!ImGui::BeginPopupModal("CatalogBrowser", &st.catalog_browser_open, ImGuiWindowFlags_NoCollapse)) return;

    ImGui::TextUnformatted(title);
    ImGui::Separator();

    std::error_code ec;
    if (st.catalog_browser_cwd.empty() || !std::filesystem::is_directory(st.catalog_browser_cwd, ec))
        st.catalog_browser_cwd = catalog_browser_start_dir(args.omsi_root, st.catalog_browser_spline);

    const auto index_root = catalog_browser_start_dir(args.omsi_root, st.catalog_browser_spline);
    std::filesystem::path want_root = index_root;
    {
        const auto cwd_n = st.catalog_browser_cwd.lexically_normal();
        const auto root_n = index_root.lexically_normal();
        if (!native_is_under(cwd_n.native(), root_n.native()) && cwd_n.native() != root_n.native())
            want_root = cwd_n;
    }
    browser_index_ensure(want_root);

    char cwd_buf[512]{};
    const std::string cwd_utf8 = path_utf8(st.catalog_browser_cwd);
    std::snprintf(cwd_buf, sizeof(cwd_buf), "%s", cwd_utf8.c_str());
    ImGui::SetNextItemWidth(-1.f);
    if (ImGui::InputText(i18n::tr_lit("Ruta"), cwd_buf, sizeof(cwd_buf), ImGuiInputTextFlags_EnterReturnsTrue)) {
        const std::filesystem::path p = cwd_buf;
        if (std::filesystem::is_directory(p, ec)) st.catalog_browser_cwd = p.lexically_normal();
    }
    if (ImGui::Button(i18n::tr_lit("Ir"))) {
        const std::filesystem::path p = cwd_buf;
        if (std::filesystem::is_directory(p, ec)) st.catalog_browser_cwd = p.lexically_normal();
    }
    ImGui::SameLine();
    if (ImGui::Button("..") && st.catalog_browser_cwd.has_parent_path())
        st.catalog_browser_cwd = st.catalog_browser_cwd.parent_path();

    struct Row {
        bool dir = false;
        std::filesystem::path path;
        std::string label;
        std::uintmax_t size = 0;
        std::filesystem::file_time_type mtime{};
    };
    std::vector<Row> rows;
    rows.reserve(128);
    {
        std::error_code lec;
        std::filesystem::directory_iterator it(
            st.catalog_browser_cwd, std::filesystem::directory_options::skip_permission_denied, lec);
        const std::filesystem::directory_iterator end;
        for (; it != end; it.increment(lec)) {
            if (lec) {
                lec.clear();
                continue;
            }
            try {
                Row r;
                r.path = it->path();
                r.label = path_utf8(r.path.filename());
                std::error_code iec;
                if (it->is_directory(iec) && !iec) {
                    r.dir = true;
                    r.mtime = std::filesystem::last_write_time(r.path, iec);
                    rows.push_back(std::move(r));
                } else if (it->is_regular_file(iec) && !iec && catalog_path_ext_ci(r.path, ext)) {
                    r.size = std::filesystem::file_size(r.path, iec);
                    r.mtime = std::filesystem::last_write_time(r.path, iec);
                    rows.push_back(std::move(r));
                }
            } catch (const std::filesystem::filesystem_error&) {
                continue;
            }
        }
    }
    std::sort(rows.begin(), rows.end(), [&](const Row& a, const Row& b) {
        if (a.dir != b.dir) return a.dir > b.dir;
        int cmp = 0;
        if (st.catalog_browser_sort == 1) {
            cmp = a.mtime < b.mtime ? -1 : (a.mtime > b.mtime ? 1 : 0);
        } else if (st.catalog_browser_sort == 2) {
            cmp = a.size < b.size ? -1 : (a.size > b.size ? 1 : 0);
        } else {
            cmp = a.label < b.label ? -1 : (a.label > b.label ? 1 : 0);
        }
        if (cmp == 0) cmp = a.label < b.label ? -1 : (a.label > b.label ? 1 : 0);
        return st.catalog_browser_sort_desc ? cmp > 0 : cmp < 0;
    });

    ImGui::SetNextItemWidth(-1.f);
    ImGui::InputTextWithHint("##cfil", i18n::tr_lit("Buscar por nombre..."), st.catalog_browser_filter,
                             IM_ARRAYSIZE(st.catalog_browser_filter));
    if (browser_index_busy() || !browser_index_ready(want_root))
        ImGui::TextUnformatted(i18n::tr_lit("Indexando…"));
    const char* sort_labs[] = {i18n::tr_lit("Nombre"), "Fecha", "Tamano"};
    ImGui::SetNextItemWidth(140.f);
    ImGui::Combo(i18n::tr_lit("Orden"), &st.catalog_browser_sort, sort_labs, 3);
    ImGui::SameLine();
    ImGui::Checkbox(i18n::tr_lit("Invertir"), &st.catalog_browser_sort_desc);

    std::vector<int> shown;
    shown.reserve(rows.size());
    const std::string filt = st.catalog_browser_filter;
    auto filt_l = filt;
    for (char& c : filt_l) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    int match_budget = 0;
    auto file_ok = [&](const std::filesystem::path& p) { return catalog_path_ext_ci(p, ext); };
    const bool idx_ready = browser_index_ready(want_root);
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        const auto& row = rows[static_cast<std::size_t>(i)];
        if (filt_l.empty() || browser_name_matches(row.label, filt_l)) {
            shown.push_back(i);
            continue;
        }
        if (row.dir && idx_ready && browser_dir_has_match(row.path, filt_l, file_ok, 0, match_budget))
            shown.push_back(i);
    }

    if (ImGui::BeginChild("cat_rows", ImVec2(-1.f, 200.f), true)) {
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(shown.size()));
        while (clipper.Step()) {
            for (int si = clipper.DisplayStart; si < clipper.DisplayEnd; ++si) {
                const auto& row = rows[static_cast<std::size_t>(shown[static_cast<std::size_t>(si)])];
                const bool sel = row.path == st.catalog_browser_selected;
                const std::string line = (row.dir ? "[DIR] " : "      ") + row.label;
                if (ImGui::Selectable(line.c_str(), sel)) {
                    st.catalog_browser_selected = row.path;
                    if (row.dir) {
                        st.catalog_browser_cwd = row.path;
                        st.catalog_browser_selected.clear();
                    }
                }
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    if (row.dir) {
                        st.catalog_browser_cwd = row.path;
                        st.catalog_browser_selected.clear();
                    } else if (!st.catalog_browser_scan_dir) {
                        if (st.catalog_browser_to_spray) {
                            if (st.spray_categories.empty()) st.spray_categories.push_back({"Pasto", {}, true, false});
                            const int ci = (std::clamp)(st.spray_active_category, 0,
                                                        static_cast<int>(st.spray_categories.size()) - 1);
                            auto& cat = st.spray_categories[static_cast<std::size_t>(ci)];
                            if (row.path.extension() == ".sco") {
                                bool dup = false;
                                for (const auto& it : cat.items)
                                    if (it.path == row.path) dup = true;
                                if (!dup) cat.items.push_back({row.path, omsi::path_text(row.path.filename()), true, 1.f});
                                save_editor_docks(st);
                            }
                            st.catalog_browser_to_spray = false;
                            st.catalog_browser_open = false;
                            ImGui::CloseCurrentPopup();
                        } else if (st.catalog_browser_to_replace) {
                            if (row.path.extension() == ".sco" || row.path.extension() == ".sli") {
                                EditorSidePanelState rev{};
                                rev.group_replace_apply = true;
                                rev.group_replace_path = row.path;
                                rev.group_replace_spline = row.path.extension() == ".sli";
                                fire(*args.panel, rev);
                                st.catalog_browser_to_replace = false;
                                st.catalog_browser_open = false;
                                ImGui::CloseCurrentPopup();
                            }
                        } else if (args.catalog_on_file_picked) {
                            args.catalog_on_file_picked(row.path, st.catalog_browser_spline);
                            st.catalog_browser_open = false;
                            ImGui::CloseCurrentPopup();
                        }
                    }
                }
            }
        }
    }
    ImGui::EndChild();

    static bool show_browser_preview = false;
    const bool file_sel =
        !st.catalog_browser_selected.empty() && std::filesystem::is_regular_file(st.catalog_browser_selected, ec);
    if (!st.catalog_browser_scan_dir && file_sel) {
        if (ImGui::Button(i18n::tr_lit("Previsualizar"))) show_browser_preview = true;
        if (show_browser_preview) {
            ImGui::SameLine();
            ImGui::TextUnformatted(omsi::path_text(st.catalog_browser_selected.filename()).c_str());
            if (st.catalog_browser_spline)
                draw_sli_topdown_preview(st.catalog_browser_selected, 12.f, 0.f, false, args.omsi_root);
            else
                draw_sco_iso_preview(st.catalog_browser_selected, false, args.omsi_root);
        }
    } else {
        show_browser_preview = false;
    }

    bool close = false;
    if (st.catalog_browser_scan_dir) {
        if (ImGui::Button(i18n::tr_lit("Escanear esta carpeta"))) {
            if (st.catalog_browser_to_spawn_random) {
                st.spawn_random_folder = st.catalog_browser_cwd;
                rebuild_spawn_random_folder_pool(st);
                st.spawn_random = true;
                st.spawn_random_mode = 2;
                if (args.panel) {
                    args.panel->set_status("Generar al azar: " +
                                           std::to_string(st.spawn_random_folder_pool.size()) +
                                           " SCO en carpeta");
                    EditorSidePanelState ev{};
                    ev.save_ui_settings = true;
                    fire(*args.panel, ev);
                }
                st.catalog_browser_to_spawn_random = false;
                close = true;
            } else if (st.catalog_browser_to_spray) {
                if (st.spray_categories.empty()) st.spray_categories.push_back({"Pasto", {}, true, false});
                const int ci = (std::clamp)(st.spray_active_category, 0,
                                            static_cast<int>(st.spray_categories.size()) - 1);
                auto& cat = st.spray_categories[static_cast<std::size_t>(ci)];
                int added = 0;
                std::error_code rec;
                for (std::filesystem::recursive_directory_iterator it(
                         st.catalog_browser_cwd,
                         std::filesystem::directory_options::skip_permission_denied, rec),
                     end;
                     it != end; it.increment(rec)) {
                    if (rec) {
                        rec.clear();
                        continue;
                    }
                    if (!it->is_regular_file(rec)) continue;
                    if (it->path().extension() != ".sco") continue;
                    bool dup = false;
                    for (const auto& e : cat.items)
                        if (e.path == it->path()) {
                            dup = true;
                            break;
                        }
                    if (dup) continue;
                    cat.items.push_back({it->path(), omsi::path_text(it->path().filename()), true, 1.f});
                    ++added;
                    if (added >= 5000) break;
                }
                save_editor_docks(st);
                if (args.panel)
                    args.panel->set_status("Aerosol: +" + std::to_string(added) + " SCO desde carpeta");
                st.catalog_browser_to_spray = false;
                close = true;
            } else if (args.catalog_on_dir_picked) {
                args.catalog_on_dir_picked(st.catalog_browser_cwd, st.catalog_browser_spline);
                close = true;
            }
        }
    } else if (ImGui::Button(i18n::tr_lit("Abrir"))) {
        std::filesystem::path pick = st.catalog_browser_selected;
        if (!pick.empty() && std::filesystem::is_regular_file(pick, ec)) {
            if (st.catalog_browser_to_spray) {
                if (st.spray_categories.empty()) st.spray_categories.push_back({"Pasto", {}, true, false});
                const int ci = (std::clamp)(st.spray_active_category, 0,
                                            static_cast<int>(st.spray_categories.size()) - 1);
                auto& cat = st.spray_categories[static_cast<std::size_t>(ci)];
                if (pick.extension() == ".sco") {
                    bool dup = false;
                    for (const auto& it : cat.items)
                        if (it.path == pick) dup = true;
                    if (!dup) cat.items.push_back({pick, omsi::path_text(pick.filename()), true, 1.f});
                    save_editor_docks(st);
                }
                st.catalog_browser_to_spray = false;
                close = true;
            } else if (st.catalog_browser_to_replace) {
                EditorSidePanelState rev{};
                rev.group_replace_apply = true;
                rev.group_replace_path = pick;
                rev.group_replace_spline = pick.extension() == ".sli";
                fire(*args.panel, rev);
                st.catalog_browser_to_replace = false;
                close = true;
            } else if (args.catalog_on_file_picked) {
                args.catalog_on_file_picked(pick, st.catalog_browser_spline);
                close = true;
            }
        } else if (args.panel) {
            args.panel->set_status(std::string("Selecciona un") + ext + " en la lista");
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(i18n::tr_lit("Cancelar"))) {
        st.catalog_browser_to_spray = false;
        st.catalog_browser_to_spawn_random = false;
        st.catalog_browser_to_replace = false;
        close = true;
    }
    if (close) {
        st.catalog_browser_open = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void catalog_browser_open(EditorSidePanelImGuiState& st, const std::filesystem::path& start_dir, const bool for_spline,
                          const bool pick_directory) {
    st.catalog_browser_open = true;
    st.catalog_browser_scan_dir = pick_directory;
    st.catalog_browser_spline = for_spline;
    st.catalog_browser_cwd = start_dir;
    st.catalog_browser_selected.clear();
    std::error_code ec;
    if (st.catalog_browser_cwd.empty() || !std::filesystem::is_directory(st.catalog_browser_cwd, ec))
        st.catalog_browser_cwd = std::filesystem::current_path(ec);
    browser_index_ensure(st.catalog_browser_cwd);
}

void rebuild_spawn_random_folder_pool(EditorSidePanelImGuiState& st) {
    st.spawn_random_folder_pool.clear();
    if (st.spawn_random_folder.empty()) return;
    std::error_code ec;
    if (!std::filesystem::is_directory(st.spawn_random_folder, ec)) return;
    std::error_code rec;
    for (std::filesystem::recursive_directory_iterator it(
             st.spawn_random_folder, std::filesystem::directory_options::skip_permission_denied, rec),
         end;
         it != end; it.increment(rec)) {
        if (rec) {
            rec.clear();
            continue;
        }
        if (!it->is_regular_file(rec)) continue;
        if (it->path().extension() != ".sco") continue;
        st.spawn_random_folder_pool.push_back(it->path());
        if (st.spawn_random_folder_pool.size() >= 5000) break;
    }
}

void set_thumb_texture_provider(std::function<std::uint64_t(const std::filesystem::path&)> fn) {
    g_thumb_tex_provider = std::move(fn);
}

namespace {
struct FloatSlot {
    std::string key;
    int order = 0;
    ImVec2 size{0.f, 0.f}; // measured last frame (0: not yet)
    ImVec2 pos{0.f, 0.f};  // animated, what the window is drawn at
    bool has_pos = false;
    int last_frame = -1000; // last frame the window was drawn
    bool ghosted = true;    // its close animation was started (or it never closed)
};
std::vector<FloatSlot> g_float_slots;
// A closed window slides out to the right and fades (drawn as its frame on the foreground list).
struct FloatGhost {
    ImVec2 pos, size;
    float t = 0.f;
};
std::vector<FloatGhost> g_float_ghosts;
int g_float_cur = -1;

// Slot targets for the windows open now (drawn this or the last frame), in order: a column against the right
// edge from the top down; one that does not fit below starts a new column to the left. A slot that would land
// on the .map viewer goes below it, or to its left when there is no room below.
ImVec2 float_slot_target(std::size_t want) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const int fr = ImGui::GetFrameCount();
    constexpr float kGap = 8.f, kMargin = 12.f, kStatusH = 96.f;
    const float top = vp->WorkPos.y + kMargin;
    const float bottom = vp->WorkPos.y + vp->WorkSize.y - kStatusH;
    float vl = 0.f, vt = 0.f, vr = 0.f, vb = 0.f;
    const bool viewer = map_text_editor_client_rect(vl, vt, vr, vb);
    std::vector<std::size_t> order;
    for (std::size_t i = 0; i < g_float_slots.size(); ++i)
        if (g_float_slots[i].last_frame >= fr - 1 || i == want) order.push_back(i);
    std::stable_sort(order.begin(), order.end(),
                     [](std::size_t a, std::size_t b) { return g_float_slots[a].order < g_float_slots[b].order; });
    float col_right = vp->WorkPos.x + vp->WorkSize.x - kMargin;
    float col_left = col_right;
    float y = top;
    ImVec2 out{col_right - 320.f, top};
    for (const std::size_t i : order) {
        const auto& sl = g_float_slots[i];
        const float w = sl.size.x > 1.f ? sl.size.x : 320.f;
        const float h = sl.size.y > 1.f ? sl.size.y : 160.f;
        auto hits_viewer = [&](float x0, float y0) {
            return viewer && x0 < vr + kGap && x0 + w > vl - kGap && y0 < vb + kGap && y0 + h > vt - kGap;
        };
        if (y > top && y + h > bottom) { // next column
            col_right = col_left - kGap;
            col_left = col_right;
            y = top;
        }
        float x = col_right - w;
        if (hits_viewer(x, y)) {
            if (vb + kGap + h <= bottom) {
                y = vb + kGap;
            } else {
                col_right = vl - kGap;
                col_left = col_right;
                x = col_right - w;
                y = top;
            }
        }
        if (i == want) out = ImVec2(x, y);
        col_left = (std::min)(col_left, x);
        y += h + kGap;
    }
    return out;
}
} // namespace

void float_layout_frame() {
    const int fr = ImGui::GetFrameCount();
    for (auto& sl : g_float_slots) {
        if (sl.last_frame == fr - 2 && !sl.ghosted && sl.size.x > 1.f) {
            g_float_ghosts.push_back({sl.pos, sl.size, 0.f});
            sl.ghosted = true;
        }
    }
    if (g_float_ghosts.empty()) return;
    const float dt = (std::min)(0.1f, (std::max)(0.f, ImGui::GetIO().DeltaTime));
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    const ImGuiStyle& sty = ImGui::GetStyle();
    const float title_h = ImGui::GetFrameHeight();
    for (auto it = g_float_ghosts.begin(); it != g_float_ghosts.end();) {
        it->t += dt / 0.22f;
        if (it->t >= 1.f) {
            it = g_float_ghosts.erase(it);
            continue;
        }
        const float e = it->t * it->t * (3.f - 2.f * it->t); // smoothstep
        const float a = 1.f - e;
        const ImVec2 p0(it->pos.x + 70.f * e, it->pos.y);
        const ImVec2 p1(p0.x + it->size.x, p0.y + it->size.y * (1.f - 0.25f * e));
        ImVec4 bg = sty.Colors[ImGuiCol_WindowBg];
        ImVec4 tb = sty.Colors[ImGuiCol_TitleBg];
        bg.w = 0.92f * a;
        tb.w *= a;
        fg->AddRectFilled(p0, p1, ImGui::GetColorU32(bg), sty.WindowRounding);
        fg->AddRectFilled(p0, ImVec2(p1.x, (std::min)(p1.y, p0.y + title_h)), ImGui::GetColorU32(tb), sty.WindowRounding,
                          ImDrawFlags_RoundCornersTop);
        ++it;
    }
}

void float_slot_begin(const char* key, int order) {
    const int fr = ImGui::GetFrameCount();
    std::size_t idx = g_float_slots.size();
    for (std::size_t i = 0; i < g_float_slots.size(); ++i)
        if (g_float_slots[i].key == key) idx = i;
    if (idx == g_float_slots.size()) {
        g_float_slots.push_back({});
        g_float_slots.back().key = key;
    }
    auto& sl = g_float_slots[idx];
    sl.order = order;
    const bool reopened = sl.last_frame < fr - 1;
    const ImVec2 target = float_slot_target(idx);
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    if (!sl.has_pos || reopened) {
        // (re)opened: slides in from the right edge at its slot's height
        sl.pos = ImVec2(vp->WorkPos.x + vp->WorkSize.x, target.y);
        sl.has_pos = true;
    } else {
        const float dt = (std::min)(0.1f, (std::max)(0.f, ImGui::GetIO().DeltaTime));
        const float k = 1.f - std::exp(-dt * 14.f);
        sl.pos.x += (target.x - sl.pos.x) * k;
        sl.pos.y += (target.y - sl.pos.y) * k;
        if (std::fabs(target.x - sl.pos.x) < 0.5f) sl.pos.x = target.x;
        if (std::fabs(target.y - sl.pos.y) < 0.5f) sl.pos.y = target.y;
    }
    ImGui::SetNextWindowPos(ImVec2(std::floor(sl.pos.x), std::floor(sl.pos.y)), ImGuiCond_Always);
    g_float_cur = static_cast<int>(idx);
}

void float_slot_end() {
    if (g_float_cur < 0 || g_float_cur >= static_cast<int>(g_float_slots.size())) return;
    auto& sl = g_float_slots[static_cast<std::size_t>(g_float_cur)];
    sl.size = ImGui::GetWindowSize();
    sl.last_frame = ImGui::GetFrameCount();
    sl.ghosted = false;
    g_float_cur = -1;
}

TexturePreview texture_preview(const std::filesystem::path& tex) {
    // A texture's preview: decoded and shrunk to 128 px on a worker thread (a 2048 px .dds took tens of ms on
    // the UI thread), written as a small TGA in the thumb folder, then shown through the thumb SRV provider.
    struct Entry {
        std::atomic<int> state{0}; // 0 decoding, 1 ready, 2 failed
        std::filesystem::path tga;
        int w = 0, h = 0;
    };
    static std::mutex s_mu;
    static std::unordered_map<std::wstring, std::shared_ptr<Entry>> s_prev;
    TexturePreview out;
    if (tex.empty()) {
        out.failed = true;
        return out;
    }
    std::shared_ptr<Entry> e;
    {
        std::lock_guard<std::mutex> lk(s_mu);
        auto& slot = s_prev[tex.native()];
        if (!slot) {
            slot = std::make_shared<Entry>();
            std::uint64_t hs = 1469598103934665603ull;
            for (const wchar_t c : tex.native()) {
                hs ^= static_cast<std::uint64_t>(c);
                hs *= 1099511628211ull;
            }
            char buf[48]{};
            std::snprintf(buf, sizeof(buf), "texprev_%016llx.tga", static_cast<unsigned long long>(hs));
            slot->tga = catalog_thumbs_dir() / buf;
            std::thread([e2 = slot, src = tex]() {
                const auto img = omsi::gfx::dx12::load_image_cpu_rgba(src);
                if (!img || img->width == 0 || img->height == 0 ||
                    img->rgba.size() < static_cast<std::size_t>(img->width) * img->height * 4u) {
                    e2->state.store(2);
                    return;
                }
                const float k = 128.f / static_cast<float>((std::max)(img->width, img->height));
                const int w = (std::max)(1, static_cast<int>(static_cast<float>(img->width) * (std::min)(1.f, k)));
                const int h = (std::max)(1, static_cast<int>(static_cast<float>(img->height) * (std::min)(1.f, k)));
                std::vector<ImU32> px(static_cast<std::size_t>(w * h));
                for (int y = 0; y < h; ++y)
                    for (int x = 0; x < w; ++x) {
                        // box filter over the source texels of this preview pixel
                        const std::uint32_t x0 = static_cast<std::uint32_t>(x) * img->width / static_cast<std::uint32_t>(w);
                        const std::uint32_t x1 = (std::max)(x0 + 1, static_cast<std::uint32_t>(x + 1) * img->width / static_cast<std::uint32_t>(w));
                        const std::uint32_t y0 = static_cast<std::uint32_t>(y) * img->height / static_cast<std::uint32_t>(h);
                        const std::uint32_t y1 = (std::max)(y0 + 1, static_cast<std::uint32_t>(y + 1) * img->height / static_cast<std::uint32_t>(h));
                        std::uint32_t acc[4]{}, n = 0;
                        for (std::uint32_t sy = y0; sy < y1 && sy < img->height; sy += 1 + (y1 - y0) / 4)
                            for (std::uint32_t sx = x0; sx < x1 && sx < img->width; sx += 1 + (x1 - x0) / 4) {
                                const std::uint8_t* t = img->rgba.data() + (static_cast<std::size_t>(sy) * img->width + sx) * 4u;
                                for (int c = 0; c < 4; ++c) acc[c] += t[c];
                                ++n;
                            }
                        if (n == 0) n = 1;
                        // alpha shown over a checker-free dark grey (cutout leaves read as leaves)
                        const std::uint32_t a = acc[3] / n;
                        auto mix = [&](std::uint32_t c) { return static_cast<int>((c / n * a + 40u * (255u - a)) / 255u); };
                        px[static_cast<std::size_t>(y * w + x)] = IM_COL32(mix(acc[0]), mix(acc[1]), mix(acc[2]), 255);
                    }
                if (!write_tga_wh(e2->tga, px, w, h)) {
                    e2->state.store(2);
                    return;
                }
                e2->w = w;
                e2->h = h;
                e2->state.store(1);
            }).detach();
        }
        e = slot;
    }
    const int st = e->state.load();
    if (st == 2) {
        out.failed = true;
        return out;
    }
    if (st != 1 || !g_thumb_tex_provider) return out;
    out.id = g_thumb_tex_provider(e->tga);
    out.w = e->w;
    out.h = e->h;
    return out;
}

void set_catalog_thumb_srv_evictor(std::function<void(const std::filesystem::path&)> fn) {
    g_thumb_srv_evict = std::move(fn);
}

void set_coop_session_tab(std::function<void()> draw) { g_coop_tab_draw = std::move(draw); }

void set_mesh_edit_tab(std::function<bool()> active, std::function<void()> draw) {
    g_mesh_tab_active = std::move(active);
    g_mesh_tab_draw = std::move(draw);
}

void catalog_browser_draw(const MapEditorImGuiArgs& args) {
    if (!args.panel) return;
    catalog_browser_draw_modal(args.panel->imgui_state(), args);
}

static bool g_hide_all_ui = false;
void set_hide_all_ui(bool hide) { g_hide_all_ui = hide; }

void build_map_editor_imgui_ui(const MapEditorImGuiArgs& args) {
    // (HUD_PART: a block of the panel over 20 ms - hud_build spikes of 0.2-0.9 s while driving)
    auto hud_part_t = std::chrono::steady_clock::now();
    auto hud_mark = [&](const char* part) {
        const auto now = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(now - hud_part_t).count();
        if (ms > 20.0) omsi::log::warn(std::string("HUD_PART before ") + part + " " + std::to_string(ms) + " ms");
        hud_part_t = now;
    };
    if (!args.panel || g_hide_all_ui) return;
    auto _w_ui = omsi::MainThreadWatchdog::active_scope("imgui.build_editor_panel_ui");
    EditorSidePanel& panel = *args.panel;
    float_layout_frame(); // floating window slots: close animations
    load_editor_docks(panel.imgui_state());
    static int last_cat_sel = -1;
    static int last_obj_inst_sel = -1;
    static int last_spl_inst_sel = -1;
    static int last_tool_tab = -1;

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y));
    ImGui::SetNextWindowSize(ImVec2(400.f, vp->WorkSize.y));
    ImGui::SetNextWindowBgAlpha(0.94f);
    ImFont* _title_font = ui_font_title();
    if (_title_font) ImGui::PushFont(_title_font);
    const bool _editor_open = ImGui::Begin("Feditor###Editor OMSI", nullptr,
                                           ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove |
                                               ImGuiWindowFlags_NoResize | ImGuiWindowFlags_MenuBar);
    if (_title_font) ImGui::PopFont();
    if (_editor_open) {
        if (ImGui::BeginMenuBar()) {
            if (ImGui::BeginMenu(i18n::tr_lit("Archivo"))) {
                if (ImGui::MenuItem(i18n::tr_lit("Cargar mapa (global.cfg)..."))) {
                    EditorSidePanelState ev{};
                    ev.request_load_map = true;
                    fire(panel, ev);
                }
                if (ImGui::MenuItem(i18n::tr_lit("Abrir tile .map..."))) {
                    EditorSidePanelState ev{};
                    ev.request_open_tile_map = true;
                    fire(panel, ev);
                }
                ImGui::Separator();
                if (ImGui::MenuItem(i18n::tr_lit("Empaquetar mapa (.zip)..."))) panel.imgui_state().pack_dialog_open = true;
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", i18n::tr_lit("Junta el mapa y lo que usa (objetos, splines, vehiculos) en un "
                                                         ".zip, sin el contenido original ni DLC de Steam."));
                if (ImGui::MenuItem(i18n::tr_lit("Sesion cooperativa..."))) panel.imgui_state().coop_window_open = true;
                ImGui::Separator();
                if (ImGui::MenuItem(i18n::tr_lit("Guardar prefs UI"))) {
                    EditorSidePanelState ev{};
                    ev.save_ui_settings = true;
                    fire(panel, ev);
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu(i18n::tr_lit("Herramientas"))) {
                auto& hst = panel.imgui_state();
                const auto htool = panel.current_tool();
                if (ImGui::MenuItem(i18n::tr_lit("Atajos…"))) {
                    EditorSidePanelState ev{};
                    ev.show_shortcuts_dialog = true;
                    fire(panel, ev);
                }
                if (ImGui::MenuItem(i18n::tr_lit("Entrypoints…"))) {
                    EditorSidePanelState ev{};
                    ev.open_entrypoint_picker = true;
                    fire(panel, ev);
                }
                if (ImGui::MenuItem(i18n::tr_lit("Opciones editor…"))) {
                    panel.imgui_state().options_editor_open = true;
                }
                // the timetable buses under way (AI traffic): Seguir / Conducir per bus
                if (ImGui::MenuItem(i18n::tr_lit("Buses IA (horario)…"), nullptr, panel.imgui_state().show_ai_bus_list))
                    panel.imgui_state().show_ai_bus_list = !panel.imgui_state().show_ai_bus_list;
                ImGui::Separator();
                {
                    const bool same_ok = !args.group_same_name.empty() &&
                                         (htool == EditorTool::Object || htool == EditorTool::Spline);
                    const std::string sel_lab = std::string("Seleccionar todos los iguales") +
                                                (same_ok ? " (" + args.group_same_name + ")" : "");
                    if (ImGui::MenuItem(sel_lab.c_str(), nullptr, false, same_ok)) {
                        if (args.group_same_attach > 0 && htool == EditorTool::Object) {
                            hst.group_attach_popup = true;
                        } else {
                            EditorSidePanelState ev{};
                            ev.group_select_same = true;
                            ev.group_select_attach = 2;
                            fire(panel, ev);
                        }
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Elige TODAS las apariciones del mismo .sco / .sli que el elemento seleccionado.");
                    if (ImGui::MenuItem("Reemplazar todos por...", nullptr, false, same_ok)) {
                        hst.catalog_browser_to_replace = true;
                        const bool spl = htool == EditorTool::Spline;
                        catalog_browser_open(hst, catalog_initial_dir(args.omsi_root, spl), spl, false);
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Cambia todas las apariciones de la pieza actual por otra del catalogo (con miniaturas).\nQueda en memoria hasta Save y se puede deshacer.");
                }
                ImGui::Separator();
                if (ImGui::MenuItem(i18n::tr_lit("Bake mesh .o3d"))) {
                    EditorSidePanelState ev{};
                    ev.enter_mesh_edit = true;
                    fire(panel, ev);
                }
                if (ImGui::MenuItem(i18n::tr_lit("Editar mesh .o3d…"))) {
                    EditorSidePanelState ev{};
                    ev.edit_o3d_file = true;
                    fire(panel, ev);
                }
                if (ImGui::MenuItem(i18n::tr_lit("Editar paths .sco"))) {
                    EditorSidePanelState ev{};
                    ev.enter_sco_path_edit = true;
                    fire(panel, ev);
                    // Debug: confirmar que el evento se dispara
                    panel.set_status("Editando paths .sco — selecciona un .sco primero si no ves la ventana");
                }
                if (htool == EditorTool::Spline) {
                    if (ImGui::MenuItem(i18n::tr_lit("Seleccionar SLI (bake)"))) {
                        EditorSidePanelState ev{};
                        ev.sli_bake_pick = true;
                        fire(panel, ev);
                    }
                    if (args.sli_bake_pick_active || args.sli_bake_ready) {
                        ImGui::TextDisabled("SLI sel: %zu  (click 3D / Esc cancela)", args.spline_group_count);
                        if (ImGui::MenuItem(i18n::tr_lit("Bake SLI → .sco"))) {
                            EditorSidePanelState ev{};
                            ev.sli_bake_run = true;
                            fire(panel, ev);
                        }
                        if (args.sli_bake_ready) {
                            if (ImGui::MenuItem(i18n::tr_lit("Reemplazar por .sco"))) {
                                EditorSidePanelState ev{};
                                ev.sli_bake_replace = true;
                                fire(panel, ev);
                            }
                            if (!args.sli_bake_label.empty()) ImGui::TextUnformatted(args.sli_bake_label.c_str());
                        }
                    }
                    ImGui::SetNextItemWidth(72.f);
                    ImGui::InputInt(i18n::tr_lit("Calidad adaptar"), &hst.spline_clearance_quality, 1, 1);
                    hst.spline_clearance_quality = (std::clamp)(hst.spline_clearance_quality, 1, 10);
                    if (ImGui::MenuItem(i18n::tr_lit("Adaptar spline ahora"))) {
                        hst.spline_clearance_enabled = true;
                        EditorSidePanelState ev{};
                        ev.adapt_spline_clearance = true;
                        ev.spline_clearance_quality = hst.spline_clearance_quality;
                        fire(panel, ev);
                    }
                }
                ImGui::MenuItem(i18n::tr_lit("Semaforos x5"), nullptr, &hst.traffic_light_x5);
                if (ImGui::MenuItem(i18n::tr_lit("Creador de splines…"))) {
                    EditorSidePanelState ev{};
                    ev.open_spline_creator = true;
                    fire(panel, ev);
                }
                ImGui::Separator();
                ImGui::TextUnformatted(i18n::tr_lit("Spawn OMSI bus"));
                {
                    static char bus_filter[96]{};
                    static int bus_sel = -1;
                    const auto* cat = args.bus_catalog;
                    if (cat && bus_sel < 0 && !args.last_omsi_bus.empty()) {
                        for (int i = 0; i < static_cast<int>(cat->size()); ++i) {
                            if (omsi::path_text((*cat)[static_cast<std::size_t>(i)].bus_path) == args.last_omsi_bus) {
                                bus_sel = i;
                                break;
                            }
                        }
                    }
                    ImGui::SetNextItemWidth(220.f);
                    ImGui::InputTextWithHint("##busfilt", i18n::tr_lit("filtro (carpeta o modelo)"), bus_filter,
                                             sizeof(bus_filter));
                    std::vector<int> shown;
                    if (cat) {
                        auto needle = std::string(bus_filter);
                        for (char& ch : needle) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                        for (int i = 0; i < static_cast<int>(cat->size()); ++i) {
                            const auto& e = (*cat)[static_cast<std::size_t>(i)];
                            if (!needle.empty()) {
                                auto hay = e.list_label + " " + e.folder_name + " " + e.file_name;
                                for (char& ch : hay) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                                if (hay.find(needle) == std::string::npos) continue;
                            }
                            shown.push_back(i);
                        }
                    }
                    const bool sel_ok = cat && bus_sel >= 0 && bus_sel < static_cast<int>(cat->size());
                    ImGui::SetNextItemWidth(220.f);
                    const char* preview = sel_ok ? (*cat)[static_cast<std::size_t>(bus_sel)].list_label.c_str() : "(elige)";
                    if (ImGui::BeginCombo("##buspick", preview)) {
                        for (int gi : shown) {
                            const auto& e = (*cat)[static_cast<std::size_t>(gi)];
                            if (ImGui::Selectable(e.list_label.c_str(), gi == bus_sel)) bus_sel = gi;
                        }
                        ImGui::EndCombo();
                    }
                    if (ImGui::MenuItem(i18n::tr_lit("Spawn OMSI bus"), nullptr, false, sel_ok)) {
                        EditorSidePanelState ev{};
                        ev.spawn_omsi_bus = true;
                        ev.omsi_bus_path = omsi::path_text((*cat)[static_cast<std::size_t>(bus_sel)].bus_path);
                        fire(panel, ev);
                    }
                    if (ImGui::MenuItem(i18n::tr_lit("Reset OMSI bus"))) {
                        EditorSidePanelState ev{};
                        ev.omsi_bus_reset = true;
                        fire(panel, ev);
                    }
                    if (ImGui::MenuItem(i18n::tr_lit("Despawn OMSI bus"))) {
                        EditorSidePanelState ev{};
                        ev.omsi_bus_despawn = true;
                        fire(panel, ev);
                    }
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu(i18n::tr_lit("Vista"))) {
                auto& st = panel.imgui_state();
                bool vista_changed = false;
                if (ImGui::MenuItem(i18n::tr_lit("Modo peaton (primera persona)"), "F7", st.walk_active_ui))
                    st.walk_toggle_request = true;
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", i18n::tr_lit("F7: peaton editor (la mira selecciona y coloca), otra vez F7: "
                                                         "solo peaton, otra vez: salir. E: cursor para la interfaz."));
                ImGui::Separator();
                if (ImGui::BeginMenu(i18n::tr_lit("Ayudas del editor"))) {
                    vista_changed |= ImGui::MenuItem(i18n::tr_lit("Flechas spline"), nullptr, &st.show_spline_ends);
                    vista_changed |= ImGui::MenuItem(i18n::tr_lit("Flechas path"), nullptr, &st.show_path_ends);
                    vista_changed |= ImGui::MenuItem(i18n::tr_lit("Flechas 3D (path + spline)"), nullptr, &st.arrows_3d);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", i18n::tr_lit("Una sola flecha 3D translucida en cada extremo, en vez de "
                                                             "la flecha de spline y las esferas de path por separado."));
                    vista_changed |= ImGui::MenuItem(i18n::tr_lit("Ocultar nodos"), nullptr, &st.hide_nodes);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", i18n::tr_lit("Sin nodos de spline ni de path: no se dibujan ni se pueden "
                                                             "seleccionar o arrastrar."));
                    if (ImGui::MenuItem(i18n::tr_lit("Paths modernos"), nullptr, st.path_style == 1)) {
                        st.path_style = st.path_style == 1 ? 0 : 1;
                        vista_changed = true;
                    }
                    vista_changed |= ImGui::MenuItem(i18n::tr_lit("Rails path"), nullptr, &st.show_path_rails);
                    vista_changed |= ImGui::MenuItem(i18n::tr_lit("Ocultar splines [onlyeditor] (malla)"), nullptr,
                                                     &st.hide_onlyeditor_splines);
                    vista_changed |= ImGui::MenuItem(i18n::tr_lit("Punto en el origen del .sco seleccionado"), nullptr,
                                                     &st.show_sco_origin);
                    vista_changed |= ImGui::MenuItem(i18n::tr_lit("Car park real"), nullptr, &st.parked_cars_on);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", i18n::tr_lit("Activado: un auto real de la parklist en cada [carpark_p], "
                                                             "como OMSI. Desactivado: las cajas comunes del editor."));
                    ImGui::EndMenu();
                }
                if (ImGui::BeginMenu(i18n::tr_lit("Cielo y tiempo"))) {
                ImGui::SliderFloat(i18n::tr_lit("Hora"), &st.sim_hour, 0.f, 23.99f, "%.2f");
                vista_changed |= ImGui::IsItemDeactivatedAfterEdit();
                ImGui::SliderInt(i18n::tr_lit("Mes"), &st.sim_month, 1, 12);
                vista_changed |= ImGui::IsItemDeactivatedAfterEdit();
                ImGui::SliderInt(i18n::tr_lit("Día"), &st.sim_day, 1, 31);
                vista_changed |= ImGui::IsItemDeactivatedAfterEdit();
                ImGui::SetNextItemWidth(120.f);
                ImGui::InputInt(i18n::tr_lit("Año"), &st.sim_year, 1, 10);
                if (st.sim_year < 1900) st.sim_year = 1900;
                if (st.sim_year > 2200) st.sim_year = 2200;
                vista_changed |= ImGui::IsItemDeactivatedAfterEdit();
                // the clock runs on its own (day, night, the date moves on at midnight), at x speed
                vista_changed |= ImGui::Checkbox(i18n::tr_lit("El tiempo corre"), &st.time_running);
                ImGui::SameLine();
                {
                    static const float kSpeeds[] = {1.f, 2.f, 5.f, 10.f, 30.f, 60.f, 120.f, 300.f, 600.f, 1800.f, 3600.f};
                    char cur[32];
                    std::snprintf(cur, sizeof(cur), "x%g", static_cast<double>(st.time_speed));
                    ImGui::SetNextItemWidth(90.f);
                    if (ImGui::BeginCombo("##time_speed", cur)) {
                        for (float sp : kSpeeds) {
                            char lab[32];
                            std::snprintf(lab, sizeof(lab), "x%g", static_cast<double>(sp));
                            if (ImGui::Selectable(lab, st.time_speed == sp)) {
                                st.time_speed = sp;
                                vista_changed = true;
                            }
                        }
                        ImGui::EndCombo();
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", i18n::tr_lit("Velocidad del tiempo (x1 = tiempo real, x60 = un minuto por segundo)"));
                }
                // Chrono scenarios: < [date  name] > ; the middle opens the whole list
                if (!st.chrono_event_dates.empty()) {
                    const int n_ev = static_cast<int>(st.chrono_event_dates.size());
                    int pos = 0; // 0 = plain map
                    if (st.chrono_date != 0)
                        for (int i = 0; i < n_ev; ++i)
                            if (st.chrono_event_dates[static_cast<std::size_t>(i)] == st.chrono_date) pos = i + 1;
                    auto label_of = [&](int p) {
                        if (p <= 0) return std::string(i18n::tr_lit("Mapa base (sin cronos)"));
                        const int d = st.chrono_event_dates[static_cast<std::size_t>(p - 1)];
                        char buf[48];
                        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d  ", d / 10000, (d / 100) % 100, d % 100);
                        return std::string(buf) + st.chrono_event_names[static_cast<std::size_t>(p - 1)];
                    };
                    ImGui::TextUnformatted(i18n::tr_lit("Cronos"));
                    ImGui::BeginDisabled(pos <= 0);
                    if (ImGui::ArrowButton("##chrono_prev", ImGuiDir_Left)) st.chrono_pick = pos - 1;
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    char mid[160];
                    std::snprintf(mid, sizeof(mid), "%s  (%d/%d)##chrono_mid", label_of(pos).c_str(), pos, n_ev);
                    if (ImGui::Button(mid, ImVec2(300.f, 0.f))) ImGui::OpenPopup("##chrono_list");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", i18n::tr_lit("Escenarios Chrono del mapa por fecha de inicio. Elegir uno "
                                                             "recarga el mapa con los cronos vigentes en esa fecha "
                                                             "(vista previa: no se puede guardar)."));
                    ImGui::SameLine();
                    ImGui::BeginDisabled(pos >= n_ev);
                    if (ImGui::ArrowButton("##chrono_next", ImGuiDir_Right)) st.chrono_pick = pos + 1;
                    ImGui::EndDisabled();
                    if (ImGui::BeginPopup("##chrono_list")) {
                        for (int p = 0; p <= n_ev; ++p)
                            if (ImGui::Selectable(label_of(p).c_str(), p == pos)) st.chrono_pick = p;
                        ImGui::EndPopup();
                    }
                }
                ImGui::SliderInt(i18n::tr_lit("Clima id"), &st.weather_id, 0, 5);
                    vista_changed |= ImGui::IsItemDeactivatedAfterEdit();
                    ImGui::SameLine();
                    ImGui::TextUnformatted(omsi::sky::weather_preset(st.weather_id).name);
                    ImGui::EndMenu();
                }
                if (ImGui::BeginMenu(i18n::tr_lit("Graficos"))) {
                    vista_changed |= ImGui::MenuItem(i18n::tr_lit("Sombras solares"), nullptr, &st.show_sun_shadows);
                    vista_changed |= ImGui::MenuItem(i18n::tr_lit("Rayos de luz Plus (mas costoso)"), nullptr, &st.shaft_plus);
                    {
                        // 0 = as far as the visual radius
                        float dist = st.shadow_distance_m > 0.f ? st.shadow_distance_m : 0.f;
                        ImGui::SetNextItemWidth(200.f);
                        ImGui::SliderFloat(i18n::tr_lit("Distancia de sombras"), &dist, 0.f, 3000.f,
                                           dist > 0.f ? "%.0f m" : "radio visual");
                        st.shadow_distance_m = dist;
                        vista_changed |= ImGui::IsItemDeactivatedAfterEdit();
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("%s", i18n::tr_lit("Hasta donde llegan las sombras (0 = el radio visual). "
                                                                 "Mas lejos = sombras mas gruesas cerca."));
                    }
                    vista_changed |= ImGui::MenuItem(i18n::tr_lit("Niebla"), nullptr, &st.fog_enabled);
                    if (ImGui::MenuItem(i18n::tr_lit("Límite texturas 256px"), nullptr, &st.tex_max_256)) {
                        EditorSidePanelState ev{};
                        ev.reload_file_textures = true;
                        ev.save_ui_settings = true;
                        fire(panel, ev);
                    }
                    ImGui::EndMenu();
                }
                if (ImGui::BeginMenu(i18n::tr_lit("Trafico"))) {
                    ImGui::Checkbox(i18n::tr_lit("Trafico IA"), &st.ai_traffic_on);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", i18n::tr_lit("Trafico aleatorio del mapa (ailists.cfg) con la IA de openOMSI: "
                                                             "autos reales con sus scripts y animaciones, semaforos y prioridades."));
                    ImGui::SetNextItemWidth(180.f);
                    ImGui::SliderInt("##ai_cars", &st.ai_traffic_cars, 5, 400, "%d autos");
                    vista_changed |= ImGui::Checkbox(i18n::tr_lit("Car park real"), &st.parked_cars_on);
                    vista_changed |= ImGui::Checkbox(i18n::tr_lit("Show carpark"), &st.show_carpark_helpers);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", i18n::tr_lit("Las cajas de ayuda de los estacionamientos [carpark_p] y entrypoints."));
                    ImGui::Checkbox(i18n::tr_lit("Seguir bus IA"), &st.follow_ai_bus);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", i18n::tr_lit("La camara sigue a un bus de servicio al azar (horario del mapa)."));
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", i18n::tr_lit("Activado: un auto real de la parklist en cada [carpark_p], "
                                                             "como OMSI. Desactivado: las cajas comunes del editor."));
                    ImGui::EndMenu();
                }
                if (vista_changed) {
                    EditorSidePanelState ev{};
                    ev.toggled_markers = true;
                    ev.save_ui_settings = true;
                    fire(panel, ev);
                }
                ImGui::Separator();
                if (ImGui::BeginMenu(i18n::tr("menu.language"))) {
                    const bool es = i18n::language() == i18n::Lang::Es;
                    bool lang_changed = false;
                    if (ImGui::MenuItem(i18n::tr("menu.spanish"), nullptr, es)) {
                        i18n::set_language(i18n::Lang::Es);
                        lang_changed = true;
                    }
                    if (ImGui::MenuItem(i18n::tr("menu.english"), nullptr, !es)) {
                        i18n::set_language(i18n::Lang::En);
                        lang_changed = true;
                    }
                    if (lang_changed) { // persisted with the other UI prefs (was lost on restart)
                        EditorSidePanelState ev{};
                        ev.save_ui_settings = true;
                        fire(panel, ev);
                    }
                    ImGui::EndMenu();
                }
                ImGui::EndMenu();
            }
            ImGui::Text("  %s", args.map_name ? args.map_name : i18n::tr_lit("(sin nombre)"));
            ImGui::EndMenuBar();
        }

        // Keyboard Tab only: one-shot SetSelected (imgui#7835 — never every frame vs click).
        static int imgui_tab_sel = -1;
        const int cur_tool_tab = static_cast<int>(panel.current_tool());
        if (cur_tool_tab != last_tool_tab) {
            last_obj_inst_sel = -1;
            last_spl_inst_sel = -1;
            last_tool_tab = cur_tool_tab;
        }
        const int queued_tab = panel.imgui_state().queued_tool_tab;
        const bool sco_path_edit_mode = panel.is_in_sco_path_edit_mode();

        if (panel.sco_path_pick_pending()) {
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 210, 60, 255));
            ImGui::TextWrapped(i18n::tr_lit("Pick .o3d: hacé click en un objeto con [path] en el viewport y luego 'Aceptar'."));
            ImGui::PopStyleColor();
            if (ImGui::Button(i18n::tr_lit("Aceptar##sco_pick"), ImVec2(110.f, 0.f))) {
                EditorSidePanelState ev{};
                ev.sco_path_pick_accept = true;
                fire(panel, ev);
            }
            ImGui::SameLine();
            if (ImGui::Button(i18n::tr_lit("Cancelar##sco_pick"), ImVec2(110.f, 0.f))) panel.clear_sco_path_pick();
            ImGui::Separator();
        }

        if (ImGui::BeginTabBar("Tools", ImGuiTabBarFlags_None)) {
            if (args.beamng_mode) {
                if (args.bng_scene_tab && ImGui::BeginTabItem("Escena")) {
                    if (ImGui::BeginChild("##bng_scene_scroll", ImVec2(0.f, 0.f), ImGuiChildFlags_None)) args.bng_scene_tab();
                    ImGui::EndChild();
                    ImGui::EndTabItem();
                }
                if (args.bng_roads_tab) {
                    // One-shot SetSelected when a road was picked with the mouse in the 3D view
                    // (never every frame — imgui#7835).
                    const bool bng_roads_sel = panel.imgui_state().bng_roads_tab_selected;
                    const ImGuiTabItemFlags bng_roads_flags =
                        bng_roads_sel ? ImGuiTabItemFlags_SetSelected : static_cast<ImGuiTabItemFlags>(0);
                    if (ImGui::BeginTabItem("Carreteras", nullptr, bng_roads_flags)) {
                        if (ImGui::BeginChild("##bng_roads_scroll", ImVec2(0.f, 0.f), ImGuiChildFlags_None)) args.bng_roads_tab();
                        ImGui::EndChild();
                        ImGui::EndTabItem();
                    }
                    if (bng_roads_sel) panel.imgui_state().bng_roads_tab_selected = false;
                }
                if (args.bng_vegetation_tab) {
                    if (ImGui::BeginTabItem("Vegetacion")) {
                        if (ImGui::BeginChild("##bng_vegetation_scroll", ImVec2(0.f, 0.f), ImGuiChildFlags_None))
                            args.bng_vegetation_tab();
                        ImGui::EndChild();
                        ImGui::EndTabItem();
                    }
                }
                if (args.bng_terrain_tab) {
                    const bool bng_ter_sel = panel.imgui_state().bng_terrain_tab_selected;
                    const ImGuiTabItemFlags bng_ter_flags =
                        bng_ter_sel ? ImGuiTabItemFlags_SetSelected : static_cast<ImGuiTabItemFlags>(0);
                    if (ImGui::BeginTabItem("Terreno", nullptr, bng_ter_flags)) {
                        if (ImGui::BeginChild("##bng_terrain_scroll", ImVec2(0.f, 0.f), ImGuiChildFlags_None))
                            args.bng_terrain_tab();
                        ImGui::EndChild();
                        ImGui::EndTabItem();
                    }
                    if (bng_ter_sel) panel.imgui_state().bng_terrain_tab_selected = false;
                }
            }
            for (int i = 0; i < kToolCount; ++i) {
                // BeamNG: only the generic object tools apply (tiles / OMSI splines / traffic / TT do not exist)
                if (args.beamng_mode && i != static_cast<int>(EditorTool::Object)) continue;
                // Bloquear cambio de tab si estamos editando paths .sco
                const bool tab_blocked = sco_path_edit_mode && static_cast<int>(panel.current_tool()) != i;
                if (!tab_blocked) {
                    ImGuiTabItemFlags flags = 0;
                    if (queued_tab >= 0 && i == queued_tab) flags |= ImGuiTabItemFlags_SetSelected;
                    if (ImGui::BeginTabItem(i18n::tr(kToolKeys[i]), nullptr, flags)) {
                        imgui_tab_sel = i;
                        const bool apply_tab_tool = queued_tab < 0 || i == queued_tab;
                        if (apply_tab_tool && static_cast<int>(panel.current_tool()) != i)
                            panel.imgui_set_tool(static_cast<EditorTool>(i));
                        if (static_cast<int>(panel.current_tool()) == i) {
                            if (ImGui::BeginChild("##tool_scroll", ImVec2(0.f, 0.f), ImGuiChildFlags_None)) {
                                draw_tool_content(panel, static_cast<EditorTool>(i), args.tile_info,
                                                  args.selected_object_index, args.selected_spline_index, last_cat_sel,
                                                  last_obj_inst_sel, last_spl_inst_sel, args.omsi_root, args);
                            }
                            ImGui::EndChild();
                        }
                        ImGui::EndTabItem();
                    }
                }
                // Co-op session tab: right after "Tile" (index 0), before "Objeto".
                if (i == 0 && g_coop_tab_draw) {
                    static bool s_coop_tab_was = false;
                    bool coop_on = panel.imgui_state().coop_window_open;
                    if (coop_on) {
                        const ImGuiTabItemFlags cflags = s_coop_tab_was ? 0 : ImGuiTabItemFlags_SetSelected;
                        if (ImGui::BeginTabItem(i18n::tr_lit("Sesion"), &coop_on, cflags)) {
                            if (ImGui::BeginChild("##coop_tab_scroll", ImVec2(0.f, 0.f), ImGuiChildFlags_None))
                                g_coop_tab_draw();
                            ImGui::EndChild();
                            ImGui::EndTabItem();
                        }
                        if (!coop_on) panel.imgui_state().coop_window_open = false;
                    }
                    s_coop_tab_was = panel.imgui_state().coop_window_open;
                }
                // Path editor tab: right after "Objeto" (index 1), before "Spline".
                if (i == 1 && panel.path_editor()) {
                    static bool s_path_tab_was = false;
                    const bool path_on = panel.path_editor()->active;
                    if (path_on) {
                        const ImGuiTabItemFlags sflags = s_path_tab_was ? 0 : ImGuiTabItemFlags_SetSelected;
                        if (ImGui::BeginTabItem("Paths", nullptr, sflags)) {
                            if (ImGui::BeginChild("##path_editor_scroll", ImVec2(0.f, 0.f), ImGuiChildFlags_None)) {
                                draw_path_editor_tab(panel);
                            }
                            ImGui::EndChild();
                            ImGui::EndTabItem();
                        }
                    }
                    s_path_tab_was = path_on;
                }
            }
            {
                static bool s_mesh_tab_was = false;
                const bool mesh_on = g_mesh_tab_active && g_mesh_tab_active();
                if (mesh_on) {
                    const ImGuiTabItemFlags mflags = s_mesh_tab_was ? 0 : ImGuiTabItemFlags_SetSelected;
                    if (ImGui::BeginTabItem(i18n::tr_lit("Mesh .o3d"), nullptr, mflags)) {
                        if (ImGui::BeginChild("##mesh_tab_scroll", ImVec2(0.f, 0.f), ImGuiChildFlags_None) &&
                            g_mesh_tab_draw)
                            g_mesh_tab_draw();
                        ImGui::EndChild();
                        ImGui::EndTabItem();
                    }
                }
                s_mesh_tab_was = mesh_on;
            }
            if (args.beamng_mode && args.bng_quality_tab && ImGui::BeginTabItem("Calidad")) {
                if (ImGui::BeginChild("##bng_quality_scroll", ImVec2(0.f, 0.f), ImGuiChildFlags_None)) args.bng_quality_tab();
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
            if (queued_tab >= 0) panel.queue_imgui_tool_tab(-1);
        }
    }
    ImGui::End();

    hud_mark("draw_spawn_float");
    draw_spawn_float(panel, vp, args);
    hud_mark("draw_group_windows");
    draw_group_windows(panel, vp, args);
    hud_mark("draw_fav_preset_docks");
    draw_fav_preset_docks(panel, vp, args.omsi_root);

    hud_mark("draw_adjust_hud_imgui");
    draw_adjust_hud_imgui(args, vp);
    hud_mark("draw_shortcuts_editor");
    draw_shortcuts_editor(args);
    hud_mark("draw_options_editor");
    draw_options_editor(args);
    {
        // Tracks & Trips sub-windows (Edit Profiles / Edit Timetable / Add from List / ...)
        const auto tt_host = make_tt_host(panel);
        omsi::tt::ui::global_ui().draw_windows(omsi::tt::global_tt_man(), tt_host);
    }

    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y + vp->WorkSize.y - 88.f));
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, 88.f));
    ImGui::SetNextWindowBgAlpha(0.90f);
    hud_mark("Estado");
    if (ImGui::Begin(i18n::tr_lit("Estado"), nullptr,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoTitleBar)) {
        if (ImGui::BeginTable("stats", 4, ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            if (args.beamng_mode) ImGui::Text("BeamNG: %s", args.map_name ? args.map_name : "");
            else ImGui::Text("Tiles: %zu", args.tile_count);
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("Obj: %zu", args.object_count);
            ImGui::TableSetColumnIndex(2);
            if (args.beamng_mode) ImGui::TextUnformatted(args.bng_status.c_str());
            else ImGui::Text("Spl: %zu", args.spline_count);
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("Tool: %s", i18n::tr(kToolKeys[static_cast<int>(panel.current_tool())]));
            ImGui::EndTable();
        }
        if (!panel.selection_message().empty())
            ImGui::TextWrapped("Sel: %s", panel.selection_message().c_str());
        ImGui::TextWrapped("%s", panel.status_message().c_str());
    }
    ImGui::End();

    if (!args.spline_attach_hud.empty()) {
        float_slot_begin("attachados", 10);
        ImGui::SetNextWindowSizeConstraints(ImVec2(220.f, 80.f), ImVec2(360.f, 420.f));
        ImGui::SetNextWindowBgAlpha(0.92f);
        const bool sa_hud_open = ImGui::Begin(i18n::tr_lit("Attachados"), nullptr,
                                              ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse |
                                                  ImGuiWindowFlags_NoSavedSettings);
        float_slot_end();
        if (sa_hud_open) {
            if (args.spline_attach_parent_id > 0)
                ImGui::Text("Spline id=%d", args.spline_attach_parent_id);
            ImGui::TextDisabled("%zu bloque(s)", args.spline_attach_hud.size());
            ImGui::Separator();
            if (ImGui::BeginChild("##sa_hud_list", ImVec2(280.f, 220.f), ImGuiChildFlags_None,
                                  ImGuiWindowFlags_HorizontalScrollbar)) {
                for (const auto& it : args.spline_attach_hud) {
                    if (ImGui::Selectable(it.label.c_str())) {
                        panel.queue_imgui_tool_tab(static_cast<int>(EditorTool::Object));
                        EditorSidePanelState ev{};
                        ev.select_object_id = it.object_id;
                        fire(panel, ev);
                    }
                }
            }
            ImGui::EndChild();
        }
        ImGui::End();
    }

    // Busy toast (bottom-right): Ctrl+S spinner + stream/boot % — pattern from imgui#1901 + toast overlays.
    const auto& busy = panel.imgui_state();
    if (busy.boot_screen) {
        // Boot loading screen: opaque black over the whole viewport, centered status; tiles load async behind it.
        ImGui::SetNextWindowPos(vp->Pos);
        ImGui::SetNextWindowSize(vp->Size);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.f, 0.f, 0.f, 1.f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
        if (ImGui::Begin("##BootScreen", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus)) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 c(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f);
            const float t = static_cast<float>(ImGui::GetTime());
            const float r = 22.f;
            dl->PathArcTo(ImVec2(c.x, c.y - 40.f), r, t * 6.f, t * 6.f + 4.4f, 32);
            dl->PathStroke(ImGui::GetColorU32(ImVec4(0.45f, 0.75f, 1.f, 1.f)), 3.f);
            const char* title = busy.busy_title.empty() ? "Cargando" : busy.busy_title.c_str();
            const ImVec2 ts = ImGui::CalcTextSize(title);
            ImGui::SetCursorScreenPos(ImVec2(c.x - ts.x * 0.5f, c.y));
            ImGui::TextUnformatted(title);
            if (!busy.busy_detail.empty()) {
                const ImVec2 ds = ImGui::CalcTextSize(busy.busy_detail.c_str());
                ImGui::SetCursorScreenPos(ImVec2(c.x - ds.x * 0.5f, c.y + 22.f));
                ImGui::TextDisabled("%s", busy.busy_detail.c_str());
            }
            if (busy.busy_progress >= 0.f) {
                const float p = (std::min)(1.f, (std::max)(0.f, busy.busy_progress));
                char pct[32]{};
                std::snprintf(pct, sizeof(pct), "%.0f%%", p * 100.f);
                ImGui::SetCursorScreenPos(ImVec2(c.x - 160.f, c.y + 48.f));
                ImGui::ProgressBar(p, ImVec2(320.f, 0.f), pct);
            }
        }
        ImGui::End();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    } else if (busy.busy_overlay) {
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - 16.f, vp->WorkPos.y + vp->WorkSize.y - 100.f),
                                ImGuiCond_Always, ImVec2(1.f, 1.f));
        ImGui::SetNextWindowBgAlpha(0.92f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.f, 12.f));
        if (ImGui::Begin("##BusyToast", nullptr,
                         ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDecoration |
                             ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                             ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings)) {
            if (busy.busy_spinner) {
                const ImVec2 pos = ImGui::GetCursorScreenPos();
                const float radius = 10.f;
                ImGui::Dummy(ImVec2(radius * 2.f + 4.f, radius * 2.f + 4.f));
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const ImVec2 c(pos.x + radius + 2.f, pos.y + radius + 2.f);
                const float t = static_cast<float>(ImGui::GetTime());
                dl->PathArcTo(c, radius, t * 8.f, t * 8.f + 4.2f, 18);
                // ImGui 1.92+: PathStroke(col, thickness, flags) — not (col, closed, thickness).
                dl->PathStroke(ImGui::GetColorU32(ImVec4(0.45f, 0.75f, 1.f, 1.f)), 2.5f);
                ImGui::SameLine();
            }
            ImGui::BeginGroup();
            if (!busy.busy_title.empty()) ImGui::TextUnformatted(busy.busy_title.c_str());
            if (!busy.busy_detail.empty()) {
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 240.f);
                ImGui::TextDisabled("%s", busy.busy_detail.c_str());
                ImGui::PopTextWrapPos();
            }
            if (busy.busy_progress >= 0.f) {
                const float p = (std::min)(1.f, (std::max)(0.f, busy.busy_progress));
                char pct[32]{};
                std::snprintf(pct, sizeof(pct), "%.0f%%", p * 100.f);
                ImGui::ProgressBar(p, ImVec2(240.f, 0.f), pct);
            }
            ImGui::EndGroup();
        }
        ImGui::End();
        ImGui::PopStyleVar(2);
    }

    if (panel.imgui_state().field_steps_dirty) {
        panel.imgui_state().field_steps_dirty = false;
        EditorSidePanelState ev{};
        ev.save_ui_settings = true;
        fire(panel, ev);
    }

    hud_mark("catalog_browser_draw");
    catalog_browser_draw(args);
    hud_mark("catalog_browser_draw (itself)");
}

} // namespace omsi::ui
