#include "ui/ultralight_overlay.hpp"

#include "gfx/dx12/hello_o3d_renderer.hpp"
#include "gfx/dx12/dx12_helpers.hpp"
#include "omsi/logger.hpp"
#include "ui/imgui_overlay.hpp"

#ifdef OMSI_ULTRALIGHT_UI

#include <Ultralight/Ultralight.h>
#include <Ultralight/KeyCodes.h>
#include <Ultralight/platform/Surface.h>
#include <AppCore/AppCore.h>
#include <AppCore/JSHelpers.h>

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_dx12.h>

#include <d3d12.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace omsi::ui {
namespace {

using Microsoft::WRL::ComPtr;
using ultralight::BitmapSurface;
using ultralight::JSContext;
using ultralight::JSObject;
using ultralight::LoadListener;
using ultralight::MessageLevel;
using ultralight::MessageSource;
using ultralight::MouseEvent;
using ultralight::KeyEvent;
using ultralight::RefPtr;
using ultralight::String;
using ultralight::View;
using ultralight::ViewListener;

std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>{});
}

std::string js_escape_single_quoted(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '\'': out += "\\'"; break;
        case '\n': out += "\\n"; break;
        case '\r': break;
        default: out += c;
        }
    }
    return out;
}

void inject_panel_css(View* caller, const std::filesystem::path& assets_dir) {
    const std::string css = read_text_file(assets_dir / "panel.css");
    if (css.empty()) {
        log::warn("Ultralight: panel.css not found: " + (assets_dir / "panel.css").string());
        return;
    }
    const std::string js = "(function(){var o=document.getElementById('omsi-panel-css');if(o)o.remove();"
                           "var s=document.createElement('style');s.id='omsi-panel-css';s.textContent='" +
                           js_escape_single_quoted(css) + "';document.head.appendChild(s);})();";
    caller->EvaluateScript(String(js.c_str()));
    log::info("Ultralight: injected panel.css (" + std::to_string(css.size()) + " bytes)");
}

class PanelViewListener : public ViewListener {
public:
    void OnAddConsoleMessage(View*, MessageSource, MessageLevel, const String& message, uint32_t, uint32_t,
                             const String&) override {
        log::warn("Ultralight editor console: " + std::string(message.utf8().data()));
    }
};

class PanelLoadListener : public LoadListener {
public:
    std::function<void(const std::string&)> event_cb_;
    std::function<void()> on_ready_;
    std::filesystem::path assets_dir_;

    void bind_native(View* caller) {
        if (!caller) return;
        RefPtr<JSContext> ctx = caller->LockJSContext();
        ultralight::SetJSContext(ctx->ctx());
        ultralight::JSObject global = ultralight::JSGlobalObject();
        global["omsiPost"] = ultralight::JSCallback(
            [this](const ultralight::JSObject&, const ultralight::JSArgs& args) {
                if (args.size() < 1 || !event_cb_) return;
                event_cb_(static_cast<ultralight::String>(args[0].ToString()).utf8().data());
            });
    }

    void OnWindowObjectReady(View* caller, uint64_t, bool is_main_frame, const String&) override {
        if (!is_main_frame) return;
        bind_native(caller);
    }

    void OnDOMReady(View* caller, uint64_t, bool is_main_frame, const String&) override {
        if (!is_main_frame || !caller) return;
        bind_native(caller);
        // Guaranteed scroll helper even if app.js is stale/cached.
        caller->EvaluateScript(R"JS(
window.omsiScroll = function(dyPx,x,y){
  var px = Number(dyPx)||0; if(!px) return false;
  var cx = Math.max(0, Number(x)||0), cy = Math.max(0, Number(y)||0);
  var el = document.elementFromPoint(cx, cy);
  while (el && el !== document.documentElement) {
    if (el.scrollHeight > el.clientHeight + 1) {
      var st = window.getComputedStyle(el), oy = st.overflowY;
      if (oy==='auto'||oy==='scroll'||oy==='overlay'||st.overflow==='auto'||st.overflow==='scroll') {
        el.scrollTop += px; return true;
      }
    }
    el = el.parentElement;
  }
  var nodes = document.querySelectorAll('.scroll-y, #tool-content');
  for (var i=0;i<nodes.length;i++) {
    if (nodes[i].scrollHeight > nodes[i].clientHeight + 1) { nodes[i].scrollTop += px; return true; }
  }
  var main = document.getElementById('tool-content');
  if (main) { main.scrollTop += px; return true; }
  return false;
};
window.omsiReplaceInstances = function(html, range, selId){
  var box = document.querySelector('.listbox[data-list="instanceSel"] .listbox-b');
  var host = document.querySelector('.listbox[data-list="instanceSel"]');
  if (!box) return false;
  box.innerHTML = html || '<div class="empty">(vacio)</div>';
  box.scrollTop = 0;
  var badge = host && host.querySelector('.listbox-h .muted');
  if (badge && range) badge.textContent = range;
  return true;
};
window.omsiFocusInstanceSearch = function(filterText, html, range, selId){
  var host = document.querySelector('.listbox[data-list="instanceSel"]');
  if (!host) return false;
  var inp = host.querySelector('input[data-filter="instanceFilter"]');
  if (inp) inp.value = filterText || '';
  var badge = host.querySelector('.listbox-h .muted');
  if (badge && range) badge.textContent = range;
  var box = host.querySelector('.listbox-b');
  if (box) { box.innerHTML = html || '<div class="empty">(vacio)</div>'; box.scrollTop = 0; }
  return true;
};
)JS");
        inject_panel_css(caller, assets_dir_);
        // file:// + <script src> is unreliable in Ultralight; inject app.js from disk (docs.ultralig.ht OnDOMReady).
        const auto js_path = assets_dir_ / "app.js";
        const std::string app_js = read_text_file(js_path);
        if (app_js.empty()) {
            log::error("Ultralight: app.js not found or empty: " + js_path.string());
            return;
        }
        caller->EvaluateScript(ultralight::String(app_js.c_str()));
        log::info("Ultralight: injected app.js (" + std::to_string(app_js.size()) + " bytes)");
        if (on_ready_) on_ready_();
    }
};

std::filesystem::path exe_map_editor_ui_dir() {
    wchar_t buf[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, buf, MAX_PATH) == 0) return {};
    return std::filesystem::path(buf).parent_path() / "assets" / "ui" / "map_editor";
}

std::vector<std::filesystem::path> ui_dir_candidates() {
    namespace fs = std::filesystem;
    std::vector<fs::path> candidates;
    // Prefer compile-time / env source tree so F5 + edits hit assets/ui (exe copy is often stale).
#ifdef OMSI_UI_ASSETS_DIR
    candidates.emplace_back(OMSI_UI_ASSETS_DIR);
#endif
    if (const char* env = std::getenv("OMSI_UI_ASSETS_DIR"); env && env[0])
        candidates.emplace_back(env);
    if (const auto exe_me = exe_map_editor_ui_dir(); !exe_me.empty()) candidates.emplace_back(exe_me);
    candidates.emplace_back(fs::current_path() / "assets" / "ui" / "map_editor");
    candidates.emplace_back(fs::path("assets") / "ui" / "map_editor");
    candidates.emplace_back(fs::current_path() / ".." / ".." / "assets" / "ui" / "map_editor");
    return candidates;
}

std::filesystem::path canonical_existing(const std::filesystem::path& p) {
    std::error_code ec;
    const auto abs = std::filesystem::weakly_canonical(p, ec);
    return ec ? p : abs;
}

// HTML/JS for hot-reload (source tree OK even without Ultralight ICU data).
std::filesystem::path resolve_ui_html_dir() {
    namespace fs = std::filesystem;
    for (const auto& p : ui_dir_candidates()) {
        const auto check = canonical_existing(p);
        if (fs::exists(check / "index.html")) return fs::absolute(check);
    }
    return fs::path("assets/ui/map_editor");
}

// FileSystem root must contain resources/icudt*.dat or Renderer::Create crashes.
std::filesystem::path resolve_ui_runtime_dir(const std::filesystem::path& html_dir) {
    namespace fs = std::filesystem;
    auto has_icu = [](const fs::path& dir) {
        return fs::exists(dir / "resources" / "icudt67l.dat") || fs::exists(dir / "resources" / "icudt.dat");
    };
    if (has_icu(html_dir)) return html_dir;
    for (const auto& p : ui_dir_candidates()) {
        const auto check = fs::absolute(canonical_existing(p));
        if (has_icu(check)) return check;
    }
    return html_dir;
}

std::string file_url_from_path(const std::filesystem::path& file) {
    auto s = std::filesystem::absolute(file).generic_string();
    return "file:///" + s;
}

int glfw_to_ul_key(int key) {
    if (key >= GLFW_KEY_A && key <= GLFW_KEY_Z) return key;
    if (key >= GLFW_KEY_0 && key <= GLFW_KEY_9) return key;
    switch (key) {
    case GLFW_KEY_BACKSPACE: return ultralight::KeyCodes::GK_BACK;
    case GLFW_KEY_TAB: return ultralight::KeyCodes::GK_TAB;
    case GLFW_KEY_ENTER: return ultralight::KeyCodes::GK_RETURN;
    case GLFW_KEY_ESCAPE: return ultralight::KeyCodes::GK_ESCAPE;
    case GLFW_KEY_DELETE: return ultralight::KeyCodes::GK_DELETE;
    case GLFW_KEY_LEFT: return ultralight::KeyCodes::GK_LEFT;
    case GLFW_KEY_RIGHT: return ultralight::KeyCodes::GK_RIGHT;
    case GLFW_KEY_UP: return ultralight::KeyCodes::GK_UP;
    case GLFW_KEY_DOWN: return ultralight::KeyCodes::GK_DOWN;
    default: return 0;
    }
}

} // namespace

struct UltralightOverlay::State {
    RefPtr<ultralight::Renderer> renderer;
    RefPtr<View> view;
    PanelLoadListener load_listener;
    PanelViewListener view_listener;
    std::filesystem::path assets_dir;   // HTML/JS (hot-reload)
    std::filesystem::path runtime_dir;  // Ultralight FileSystem (ICU resources)
    ComPtr<ID3D12Resource> texture;
    ComPtr<ID3D12Resource> upload;
    D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu{};
    D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu{};
    bool srv_allocated = false;
    ImGuiOverlay* imgui = nullptr;
    std::uint32_t tex_w = 0;
    std::uint32_t tex_h = 0;
    bool texture_valid = false;
    std::vector<std::uint8_t> rgba_scratch;
};

UltralightOverlay::~UltralightOverlay() { shutdown(); }

bool UltralightOverlay::initialize(GLFWwindow* window, gfx::dx12::HelloO3dRenderer& renderer,
                                   ImGuiOverlay* imgui, int panel_width_px) {
    (void)renderer;
    if (!imgui || !imgui->active()) {
        log::error("Ultralight UI requires active ImGui DX12 overlay for compositing");
        return false;
    }
    shutdown();
    panel_w_ = (std::max)(280, panel_width_px);
    int fh = 900;
    if (window) glfwGetFramebufferSize(window, nullptr, &fh);
    panel_h_ = (std::max)(400, fh);

    auto* st = new State{};
    st->assets_dir = resolve_ui_html_dir();
    st->runtime_dir = resolve_ui_runtime_dir(st->assets_dir);
    if (!std::filesystem::exists(st->assets_dir / "index.html")) {
        log::error("Ultralight UI assets not found: " + st->assets_dir.string());
        delete st;
        return false;
    }
    if (!std::filesystem::exists(st->runtime_dir / "resources" / "icudt67l.dat") &&
        !std::filesystem::exists(st->runtime_dir / "resources" / "icudt.dat")) {
        log::error("Ultralight ICU resources missing under " + st->runtime_dir.string() +
                   "\\resources (deploy Ultralight resources next to the exe)");
        delete st;
        return false;
    }

    ultralight::Config config;
    config.resource_path_prefix = ultralight::String("map_editor/resources/");
    ultralight::Platform::instance().set_config(config);
    ultralight::Platform::instance().set_font_loader(ultralight::GetPlatformFontLoader());
    const auto fs_root = st->assets_dir.parent_path();
    ultralight::Platform::instance().set_file_system(
        ultralight::GetPlatformFileSystem(ultralight::String(fs_root.string().c_str())));

    st->load_listener.assets_dir_ = st->assets_dir;
    st->load_listener.event_cb_ = [this](const std::string& json) {
        keyboard_focus_ = true;
        if (event_cb_) event_cb_(json);
    };
    st->load_listener.on_ready_ = [this]() { js_ready_ = true; };

    st->renderer = ultralight::Renderer::Create();
    if (!st->renderer) {
        log::error("Ultralight Renderer::Create failed");
        delete st;
        return false;
    }
    ultralight::ViewConfig vc;
    vc.is_accelerated = false;
    vc.initial_device_scale = 1.0;
    vc.font_family_standard = ultralight::String("Segoe UI");
    vc.font_family_sans_serif = ultralight::String("Segoe UI");
    st->view = st->renderer->CreateView(static_cast<uint32_t>(panel_w_), static_cast<uint32_t>(panel_h_), vc,
                                        nullptr);
    if (!st->view) {
        log::error("Ultralight CreateView failed");
        delete st;
        return false;
    }
    st->view->set_load_listener(&st->load_listener);
    st->view->set_view_listener(&st->view_listener);
    // Absolute URL so HTML can live in the source tree while ICU stays in the build deploy folder.
    st->view->LoadURL(ultralight::String(file_url_from_path(st->assets_dir / "index.html").c_str()));

    state_ = st;
    state_->imgui = imgui;
    active_ = true;
    js_ready_ = false;
    log::info("Ultralight map editor UI ready (html=" + st->assets_dir.string() + " fs=" + fs_root.string() +
              " runtime=" + st->runtime_dir.string() + ")");
    return true;
}

void UltralightOverlay::shutdown() {
    if (!active_) return;
    if (state_) {
        if (state_->view) {
            state_->view->set_load_listener(nullptr);
            state_->view->set_view_listener(nullptr);
        }
        state_->view = nullptr;
        state_->renderer = nullptr;
    }
    delete state_;
    state_ = nullptr;
    active_ = false;
    js_ready_ = false;
    mouse_over_ = false;
    keyboard_focus_ = false;
}

void UltralightOverlay::resize(int panel_width_px, int height_px) {
    panel_w_ = (std::max)(280, panel_width_px);
    panel_h_ = (std::max)(400, height_px);
    if (!state_ || !state_->view) return;
    state_->view->Resize(static_cast<uint32_t>(panel_w_), static_cast<uint32_t>(panel_h_));
    state_->texture_valid = false;
}

void UltralightOverlay::set_event_callback(EventCallback cb) { event_cb_ = std::move(cb); }

void UltralightOverlay::reload() {
    if (!active_ || !state_ || !state_->view) return;
    state_->texture_valid = false;
    js_ready_ = false;
    // Cache-bust so Ultralight reloads app.js / CSS from disk.
    const auto stamp = std::to_string(
        static_cast<long long>(std::chrono::steady_clock::now().time_since_epoch().count()));
    const std::string url = file_url_from_path(state_->assets_dir / "index.html") + "?r=" + stamp;
    state_->view->LoadURL(ultralight::String(url.c_str()));
    log::info("Ultralight UI reloaded from " + state_->assets_dir.string());
}

void UltralightOverlay::begin_frame() {
    if (!active_ || !state_ || !state_->renderer) return;
    flush_pending_scroll();
    state_->renderer->Update();
    state_->renderer->Render();
}

void UltralightOverlay::flush_pending_scroll() {
    if (!active_ || !state_ || !state_->view) return;
    if (pending_scroll_y_ == 0.0) return;
    const double dy = pending_scroll_y_;
    pending_scroll_y_ = 0.0;
    // Docs (ultralig.ht): must forward ScrollEvent from the host. GLFW y+ = scroll up.
    // Samples typically use ScrollByPixel with positive delta_y scrolling content up.
    const int px = static_cast<int>(dy * 80.0);
    if (px == 0) return;

    state_->view->Focus();
    {
        MouseEvent move{};
        move.type = MouseEvent::kType_MouseMoved;
        move.x = static_cast<int>(last_x_);
        move.y = static_cast<int>(last_y_);
        move.button = MouseEvent::kButton_None;
        state_->view->FireMouseEvent(move);
    }
    {
        ultralight::ScrollEvent se{};
        se.type = ultralight::ScrollEvent::kType_ScrollByPixel;
        se.delta_x = 0;
        se.delta_y = px;
        state_->view->FireScrollEvent(se);
    }
    // JS fallback for nested overflow panels (Ultralight issues #303 / #522 — wheel quirks).
    const int js_px = -px;
    char js[256];
    std::snprintf(js, sizeof(js),
                  "if(window.omsiScroll)omsiScroll(%d,%d,%d);", js_px, static_cast<int>(last_x_),
                  static_cast<int>(last_y_));
    state_->view->EvaluateScript(ultralight::String(js));
}

void UltralightOverlay::upload_texture(ID3D12GraphicsCommandList* command_list) {
    if (!active_ || !state_ || !state_->view || !command_list) return;
    auto* surface = static_cast<BitmapSurface*>(state_->view->surface());
    if (!surface) return;
    if (surface->dirty_bounds().IsEmpty() && state_->texture_valid) return;

    ultralight::RefPtr<ultralight::Bitmap> bmp = surface->bitmap();
    if (!bmp || bmp->width() == 0 || bmp->height() == 0) return;

    const std::uint32_t w = bmp->width();
    const std::uint32_t h = bmp->height();
    const void* src_ptr = bmp->LockPixels();
    if (!src_ptr) return;
    const auto* src = static_cast<const std::uint8_t*>(src_ptr);
    const std::uint32_t src_stride = bmp->row_bytes();
    state_->rgba_scratch.resize(static_cast<std::size_t>(w) * h * 4u);
    for (std::uint32_t y = 0; y < h; ++y) {
        const std::uint8_t* row = src + y * src_stride;
        std::uint8_t* dst = state_->rgba_scratch.data() + y * w * 4u;
        for (std::uint32_t x = 0; x < w; ++x) {
            dst[x * 4u + 0] = row[x * 4u + 2];
            dst[x * 4u + 1] = row[x * 4u + 1];
            dst[x * 4u + 2] = row[x * 4u + 0];
            dst[x * 4u + 3] = row[x * 4u + 3];
        }
    }
    bmp->UnlockPixels();

    ID3D12Device* device = nullptr;
    if (FAILED(command_list->GetDevice(IID_PPV_ARGS(&device))) || !device) return;

    if (!state_->texture_valid || state_->tex_w != w || state_->tex_h != h) {
        state_->texture.Reset();
        state_->upload.Reset();
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = w;
        desc.Height = h;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        CD3DX12_HEAP_PROPERTIES default_heap(D3D12_HEAP_TYPE_DEFAULT);
        if (FAILED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                   D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                   IID_PPV_ARGS(state_->texture.GetAddressOf()))))
            return;

        const std::uint64_t upload_size = static_cast<std::uint64_t>(w) * h * 4u;
        CD3DX12_HEAP_PROPERTIES upload_heap(D3D12_HEAP_TYPE_UPLOAD);
        CD3DX12_RESOURCE_DESC upload_desc = CD3DX12_RESOURCE_DESC::Buffer(upload_size);
        if (FAILED(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &upload_desc,
                                                   D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                   IID_PPV_ARGS(state_->upload.GetAddressOf()))))
            return;

        if (!state_->srv_allocated && state_->imgui)
            state_->srv_allocated = state_->imgui->alloc_srv(&state_->srv_cpu, &state_->srv_gpu);
        if (state_->srv_allocated) {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = 1;
            device->CreateShaderResourceView(state_->texture.Get(), &srv, state_->srv_cpu);
        }
        state_->tex_w = w;
        state_->tex_h = h;
    }

    void* mapped = nullptr;
    if (FAILED(state_->upload->Map(0, nullptr, &mapped)) || !mapped) return;
    for (std::uint32_t y = 0; y < h; ++y)
        std::memcpy(static_cast<std::uint8_t*>(mapped) + y * w * 4u, state_->rgba_scratch.data() + y * w * 4u,
                    w * 4u);
    state_->upload->Unmap(0, nullptr);

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = state_->texture.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION src_loc{};
    src_loc.pResource = state_->upload.Get();
    src_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src_loc.PlacedFootprint.Offset = 0;
    src_loc.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    src_loc.PlacedFootprint.Footprint.Width = w;
    src_loc.PlacedFootprint.Footprint.Height = h;
    src_loc.PlacedFootprint.Footprint.Depth = 1;
    src_loc.PlacedFootprint.Footprint.RowPitch = w * 4u;
    command_list->CopyTextureRegion(&dst, 0, 0, 0, &src_loc, nullptr);
    D3D12_RESOURCE_BARRIER barrier = dx12_transition_barrier(
        state_->texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    command_list->ResourceBarrier(1, &barrier);
    state_->texture_valid = true;
    surface->ClearDirtyBounds();
}

void UltralightOverlay::end_frame_and_draw() {
    if (!active_ || !state_ || !state_->srv_allocated) return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const int want_h = (std::max)(400, static_cast<int>(vp->WorkSize.y));
    if (want_h != panel_h_) resize(panel_w_, want_h);
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y));
    ImGui::SetNextWindowSize(ImVec2(static_cast<float>(panel_w_), vp->WorkSize.y));
    ImGui::SetNextWindowBgAlpha(0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    if (ImGui::Begin("##ul_panel", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs)) {
        ImGui::Image(static_cast<ImTextureID>(state_->srv_gpu.ptr),
                     ImVec2(static_cast<float>(panel_w_), vp->WorkSize.y));
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

void UltralightOverlay::evaluate_script(const std::string& script) {
    if (!active_ || !state_ || !state_->view || !js_ready_) return;
    state_->view->EvaluateScript(ultralight::String(script.c_str()));
}

bool UltralightOverlay::pointer_over_panel(double view_x, double view_y) const {
    // view_x/y = GLFW window (client) coords — same space as ImGui panel size (not framebuffer).
    return view_x >= 0.0 && view_x < static_cast<double>(panel_w_) && view_y >= 0.0 &&
           view_y < static_cast<double>(panel_h_);
}

void UltralightOverlay::on_mouse_move(double view_x, double view_y) {
    last_x_ = view_x;
    last_y_ = view_y;
    mouse_over_ = pointer_over_panel(view_x, view_y);
    if (!active_ || !state_ || !state_->view || !mouse_over_) return;
    MouseEvent evt{};
    evt.type = MouseEvent::kType_MouseMoved;
    evt.x = static_cast<int>(view_x);
    evt.y = static_cast<int>(view_y);
    evt.button = MouseEvent::kButton_None;
    state_->view->FireMouseEvent(evt);
}

void UltralightOverlay::on_mouse_button(int button, int action, double view_x, double view_y) {
    mouse_over_ = pointer_over_panel(view_x, view_y);
    if (!active_ || !state_ || !state_->view || !mouse_over_) return;
    if (action == GLFW_PRESS) {
        keyboard_focus_ = true;
        state_->view->Focus();
    }
    MouseEvent evt{};
    evt.type = (action == GLFW_PRESS) ? MouseEvent::kType_MouseDown : MouseEvent::kType_MouseUp;
    evt.x = static_cast<int>(view_x);
    evt.y = static_cast<int>(view_y);
    evt.button = (button == GLFW_MOUSE_BUTTON_RIGHT)   ? MouseEvent::kButton_Right
                 : (button == GLFW_MOUSE_BUTTON_MIDDLE) ? MouseEvent::kButton_Middle
                                                        : MouseEvent::kButton_Left;
    state_->view->FireMouseEvent(evt);
}

void UltralightOverlay::on_scroll(double, double dy) {
    if (!active_ || !state_ || !state_->view) return;
    // Queue — EvaluateScript during glfw scroll callback is unreliable.
    pending_scroll_y_ += dy;
}

void UltralightOverlay::on_char(unsigned int codepoint) {
    if (!active_ || !state_ || !state_->view || !keyboard_focus_) return;
    KeyEvent evt{};
    evt.type = KeyEvent::kType_Char;
    evt.text = ultralight::String(std::string(1, static_cast<char>(codepoint & 0xFF)).c_str());
    evt.unmodified_text = evt.text;
    state_->view->FireKeyEvent(evt);
}

void UltralightOverlay::on_key(int key, int, int action, int mods) {
    if (!active_ || !state_ || !state_->view || !keyboard_focus_ || action == GLFW_REPEAT) return;
    const int ulk = glfw_to_ul_key(key);
    if (!ulk) return;
    KeyEvent evt{};
    evt.type = (action == GLFW_RELEASE) ? KeyEvent::kType_KeyUp : KeyEvent::kType_RawKeyDown;
    evt.virtual_key_code = ulk;
    evt.modifiers = 0;
    if (mods & GLFW_MOD_SHIFT) evt.modifiers |= KeyEvent::kMod_ShiftKey;
    if (mods & GLFW_MOD_CONTROL) evt.modifiers |= KeyEvent::kMod_CtrlKey;
    if (mods & GLFW_MOD_ALT) evt.modifiers |= KeyEvent::kMod_AltKey;
    state_->view->FireKeyEvent(evt);
}

} // namespace omsi::ui

#else

namespace omsi::ui {

struct UltralightOverlay::State {};

UltralightOverlay::~UltralightOverlay() { shutdown(); }

bool UltralightOverlay::initialize(GLFWwindow*, gfx::dx12::HelloO3dRenderer&, ImGuiOverlay*, int) { return false; }

void UltralightOverlay::shutdown() {
    active_ = false;
    js_ready_ = false;
    delete state_;
    state_ = nullptr;
}

void UltralightOverlay::resize(int, int) {}
void UltralightOverlay::set_event_callback(EventCallback) {}
void UltralightOverlay::reload() {}
void UltralightOverlay::begin_frame() {}
void UltralightOverlay::flush_pending_scroll() {}
void UltralightOverlay::end_frame_and_draw() {}
void UltralightOverlay::upload_texture(ID3D12GraphicsCommandList*) {}
void UltralightOverlay::evaluate_script(const std::string&) {}
void UltralightOverlay::on_mouse_move(double, double) {}
void UltralightOverlay::on_mouse_button(int, int, double, double) {}
void UltralightOverlay::on_scroll(double, double) {}
void UltralightOverlay::on_char(unsigned int) {}
void UltralightOverlay::on_key(int, int, int, int) {}
bool UltralightOverlay::pointer_over_panel(double, double) const { return false; }

} // namespace omsi::ui

#endif
