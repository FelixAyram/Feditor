#include "ui/imgui_overlay.hpp"

#include "gfx/dx12/hello_o3d_renderer.hpp"
#include "omsi/logger.hpp"
#include "omsi/main_thread_watchdog.hpp"
#include "omsi/options.hpp"

#include <GLFW/glfw3.h>

#include <imgui.h>
#include <fstream>
#include <memory>
#include <vector>
#include <imgui_impl_dx12.h>
#include <imgui_impl_glfw.h>
#include "ui/detail/imgui_impl_win32.h"
#include "ui/theme.hpp"

#include <d3d12.h>
#include <wrl/client.h>

#include <filesystem>

namespace omsi::ui {
namespace {

// imgui.ini used to be relative to the CWD, so window layout (Opciones size, docks) was lost or differed
// whenever the exe was started from another folder. Keep it next to the exe like omsi_f4_editor.cfg.
void pin_imgui_ini_next_to_exe() {
    static std::string s_ini_path;
    s_ini_path = (omsi::options::editor_ui_prefs_path().parent_path() / "imgui.ini").string();
    ImGui::GetIO().IniFilename = s_ini_path.c_str();
}

using Microsoft::WRL::ComPtr;

// Editor fonts: bundled cozy font (assets/ui/fonts/UI-*.ttf or Nunito-*.ttf) with a system
// Segoe UI fallback. Three weights so headers/titles read clearly.
ImFont* g_font_body = nullptr;
ImFont* g_font_strong = nullptr;
ImFont* g_font_title = nullptr;

// Glyphs the editor fonts lack (ñ is in them; Cyrillic, Greek, Hebrew, Arabic, symbols, Chinese, Japanese, Korean
// are not): Windows fonts merged behind each editor font. ImGui 1.92 loads glyphs on demand, so only the file bytes
// cost memory, and each file is read once and shared by the three fonts.
std::vector<std::unique_ptr<std::vector<char>>> g_fallback_font_blobs;
void load_fallback_font_blobs() {
    if (!g_fallback_font_blobs.empty()) return;
    wchar_t win[MAX_PATH]{};
    const UINT n = GetWindowsDirectoryW(win, MAX_PATH);
    const std::filesystem::path fonts = (n > 0 && n < MAX_PATH) ? std::filesystem::path(win) / L"Fonts"
                                                                : std::filesystem::path(L"C:\\Windows\\Fonts");
    const wchar_t* files[] = {L"segoeui.ttf", L"seguisym.ttf", L"msyh.ttc", L"malgun.ttf", L"Nirmala.ttc", L"Nirmala.ttf"};
    for (const wchar_t* f : files) {
        std::ifstream in(fonts / f, std::ios::binary);
        if (!in) continue;
        auto blob = std::make_unique<std::vector<char>>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (blob->size() < 1024) continue;
        g_fallback_font_blobs.push_back(std::move(blob));
    }
    log::info("ImGui fallback fonts: " + std::to_string(g_fallback_font_blobs.size()));
}
void merge_fallback_fonts(float size) {
    ImGuiIO& io = ImGui::GetIO();
    for (auto& b : g_fallback_font_blobs) {
        ImFontConfig cfg;
        cfg.MergeMode = true;
        cfg.FontDataOwnedByAtlas = false; // shared, kept alive in g_fallback_font_blobs
        io.Fonts->AddFontFromMemoryTTF(b->data(), static_cast<int>(b->size()), size, &cfg);
    }
}

void try_load_editor_font() {
    ImGuiIO& io = ImGui::GetIO();
    load_fallback_font_blobs();
    auto add = [&](const char* path, float size) -> ImFont* {
        if (!path || !path[0]) return nullptr;
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) return nullptr;
        ImFont* f = io.Fonts->AddFontFromFileTTF(path, size, nullptr, nullptr);
        if (f) log::info(std::string("ImGui font: ") + path);
        return f;
    };
    const char* roots[] = {"", "../", "../../",
#ifdef OMSI_F4_SOURCE_DIR
                           OMSI_F4_SOURCE_DIR "/",
#endif
    };
    auto add_rel = [&](const char* rel, float size) -> ImFont* {
        for (const char* r : roots) {
            if (ImFont* f = add((std::string(r) + rel).c_str(), size)) return f;
        }
        return nullptr;
    };
    g_font_body = add_rel("assets/ui/fonts/UI-Regular.ttf", 16.f);
    if (!g_font_body) g_font_body = add_rel("assets/ui/fonts/Nunito-Regular.ttf", 16.f);
    if (!g_font_body) g_font_body = add("C:/Windows/Fonts/segoeui.ttf", 16.f);
    if (!g_font_body) g_font_body = io.Fonts->AddFontDefault();
    merge_fallback_fonts(16.f); // merges into the font just added
    g_font_strong = add_rel("assets/ui/fonts/UI-SemiBold.ttf", 16.f);
    if (!g_font_strong) g_font_strong = add_rel("assets/ui/fonts/Nunito-SemiBold.ttf", 16.f);
    if (!g_font_strong) g_font_strong = add("C:/Windows/Fonts/seguisb.ttf", 16.f);
    if (g_font_strong) merge_fallback_fonts(16.f);
    g_font_title = add_rel("assets/ui/fonts/UI-Bold.ttf", 18.f);
    if (!g_font_title) g_font_title = add_rel("assets/ui/fonts/Nunito-Bold.ttf", 18.f);
    if (!g_font_title) g_font_title = add("C:/Windows/Fonts/segoeuib.ttf", 18.f);
    if (g_font_title) merge_fallback_fonts(18.f);
    io.FontDefault = g_font_body;
    if (!g_font_strong) g_font_strong = g_font_body;
    if (!g_font_title) g_font_title = g_font_strong;
}

struct Dx12UiHeap {
    ComPtr<ID3D12DescriptorHeap> srv_heap;
    D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu{};
    std::uint32_t srv_stride = 0;
    int srv_next = 0;
};

Dx12UiHeap* g_srv_heap = nullptr;
constexpr int kImguiSrvCapacity = 256;

void imgui_srv_alloc(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* out_cpu,
                     D3D12_GPU_DESCRIPTOR_HANDLE* out_gpu) {
    if (!g_srv_heap || g_srv_heap->srv_next >= kImguiSrvCapacity) return;
    const int slot = g_srv_heap->srv_next++;
    out_cpu->ptr = g_srv_heap->srv_cpu.ptr + static_cast<SIZE_T>(slot) * g_srv_heap->srv_stride;
    out_gpu->ptr = g_srv_heap->srv_gpu.ptr + static_cast<SIZE_T>(slot) * g_srv_heap->srv_stride;
}

void imgui_srv_free(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_GPU_DESCRIPTOR_HANDLE) {}

// Cozy "warm coffee" theme: warm neutrals, ONE amber accent, sage for selection/success.
// Colours/spacing come from ui/theme.hpp so the whole editor shares one palette.
void apply_editor_theme() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = theme::rounding;
    style.ChildRounding = theme::rounding - 1.f;
    style.FrameRounding = 8.f;
    style.PopupRounding = theme::rounding;
    style.ScrollbarRounding = 8.f;
    style.GrabRounding = 8.f;
    style.TabRounding = 8.f;
    style.WindowPadding = ImVec2(12.f, 10.f);
    style.FramePadding = ImVec2(theme::pad_x, theme::pad_y);
    style.ItemSpacing = ImVec2(theme::gap_x, theme::gap_y);
    style.ItemInnerSpacing = ImVec2(theme::inner_x, theme::inner_y);
    style.CellPadding = ImVec2(theme::cell_x, theme::cell_y);
    style.IndentSpacing = theme::indent;
    style.ScrollbarSize = theme::scrollbar;
    style.GrabMinSize = theme::grab;
    style.WindowBorderSize = 1.f;
    style.ChildBorderSize = 1.f;
    style.PopupBorderSize = 1.f;
    style.FrameBorderSize = 0.f;
    style.DisabledAlpha = 0.55f;
    style.CircleTessellationMaxError = 0.30f;

    ImVec4* c = style.Colors;
    c[ImGuiCol_Text] = theme::rgb(theme::text);
    c[ImGuiCol_TextDisabled] = theme::rgb(theme::disabled);
    c[ImGuiCol_WindowBg] = theme::rgb(theme::base, 0.96f);
    c[ImGuiCol_ChildBg] = theme::rgb(theme::mantle, 0.80f);
    c[ImGuiCol_PopupBg] = theme::rgb(theme::mantle, 0.98f);
    c[ImGuiCol_Border] = theme::rgb(theme::line, 0.90f);
    c[ImGuiCol_BorderShadow] = theme::rgb(0x000000, 0.f);
    c[ImGuiCol_FrameBg] = theme::rgb(theme::surface, 0.95f);
    c[ImGuiCol_FrameBgHovered] = theme::rgb(theme::surface_hi, 1.f);
    c[ImGuiCol_FrameBgActive] = theme::rgb(theme::accent, 0.30f);
    c[ImGuiCol_TitleBg] = theme::rgb(theme::crust);
    c[ImGuiCol_TitleBgActive] = theme::rgb(theme::mantle);
    c[ImGuiCol_TitleBgCollapsed] = theme::rgb(theme::crust, 0.80f);
    c[ImGuiCol_MenuBarBg] = theme::rgb(theme::mantle);
    c[ImGuiCol_ScrollbarBg] = theme::rgb(theme::crust, 0.65f);
    c[ImGuiCol_ScrollbarGrab] = theme::rgb(theme::surface_hi);
    c[ImGuiCol_ScrollbarGrabHovered] = theme::rgb(theme::accent, 0.75f);
    c[ImGuiCol_ScrollbarGrabActive] = theme::rgb(theme::accent);
    c[ImGuiCol_CheckMark] = theme::rgb(theme::accent);
    c[ImGuiCol_SliderGrab] = theme::rgb(theme::accent);
    c[ImGuiCol_SliderGrabActive] = theme::rgb(theme::accent_hi);
    c[ImGuiCol_Button] = theme::rgb(theme::surface, 0.95f);
    c[ImGuiCol_ButtonHovered] = theme::rgb(theme::accent, 0.30f);
    c[ImGuiCol_ButtonActive] = theme::rgb(theme::accent, 0.55f);
    c[ImGuiCol_Header] = theme::rgb(theme::selection, 0.28f);
    c[ImGuiCol_HeaderHovered] = theme::rgb(theme::selection, 0.42f);
    c[ImGuiCol_HeaderActive] = theme::rgb(theme::selection, 0.60f);
    c[ImGuiCol_Separator] = theme::rgb(theme::line);
    c[ImGuiCol_SeparatorHovered] = theme::rgb(theme::accent, 0.70f);
    c[ImGuiCol_SeparatorActive] = theme::rgb(theme::accent);
    c[ImGuiCol_ResizeGrip] = theme::rgb(theme::surface_hi, 0.70f);
    c[ImGuiCol_ResizeGripHovered] = theme::rgb(theme::accent, 0.70f);
    c[ImGuiCol_ResizeGripActive] = theme::rgb(theme::accent);
    c[ImGuiCol_Tab] = theme::rgb(theme::mantle);
    c[ImGuiCol_TabHovered] = theme::rgb(theme::accent, 0.45f);
    c[ImGuiCol_TabActive] = theme::rgb(theme::surface);
    c[ImGuiCol_TabUnfocused] = theme::rgb(theme::crust);
    c[ImGuiCol_TabUnfocusedActive] = theme::rgb(theme::mantle);
    c[ImGuiCol_PlotLines] = theme::rgb(theme::info);
    c[ImGuiCol_PlotLinesHovered] = theme::rgb(theme::accent_hi);
    c[ImGuiCol_PlotHistogram] = theme::rgb(theme::accent);
    c[ImGuiCol_PlotHistogramHovered] = theme::rgb(theme::accent_hi);
    c[ImGuiCol_TableHeaderBg] = theme::rgb(theme::mantle);
    c[ImGuiCol_TableBorderStrong] = theme::rgb(theme::line);
    c[ImGuiCol_TableBorderLight] = theme::rgb(theme::line, 0.55f);
    c[ImGuiCol_TableRowBg] = theme::rgb(0x000000, 0.f);
    c[ImGuiCol_TableRowBgAlt] = theme::rgb(theme::surface, 0.30f);
    c[ImGuiCol_TextSelectedBg] = theme::rgb(theme::accent, 0.35f);
    c[ImGuiCol_DragDropTarget] = theme::rgb(theme::accent_hi);
    c[ImGuiCol_NavHighlight] = theme::rgb(theme::accent);
    c[ImGuiCol_NavWindowingHighlight] = theme::rgb(theme::accent_hi);
    c[ImGuiCol_NavWindowingDimBg] = theme::rgb(theme::crust, 0.55f);
    c[ImGuiCol_ModalWindowDimBg] = theme::rgb(theme::crust, 0.60f);
}

} // namespace

// Editor font accessors (defined in the anonymous namespace above).
ImFont* ui_font_body() { return g_font_body; }
ImFont* ui_font_strong() { return g_font_strong; }
ImFont* ui_font_title() { return g_font_title; }

struct ImGuiOverlay::State {
    Dx12UiHeap heap{};
    bool win32_backend = false;
    bool glfw_backend = false;
};

ImGuiOverlay::~ImGuiOverlay() { shutdown(); }

bool ImGuiOverlay::initialize(GLFWwindow* window, gfx::dx12::HelloO3dRenderer& renderer) {
    shutdown();
    if (!window) return false;

    ID3D12Device* device = renderer.dx12_device();
    ID3D12CommandQueue* queue = renderer.dx12_command_queue();
    if (!device || !queue) {
        log::error("imgui init: DX12 device/queue unavailable");
        return false;
    }

    auto* state = new State{};
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = kImguiSrvCapacity;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(state->heap.srv_heap.GetAddressOf())))) {
        log::error("imgui init: SRV heap failed");
        delete state;
        return false;
    }
    state->heap.srv_cpu = state->heap.srv_heap->GetCPUDescriptorHandleForHeapStart();
    state->heap.srv_gpu = state->heap.srv_heap->GetGPUDescriptorHandleForHeapStart();
    state->heap.srv_stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_srv_heap = &state->heap;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    pin_imgui_ini_next_to_exe();
    apply_editor_theme();
    try_load_editor_font();

    if (!ImGui_ImplGlfw_InitForOther(window, true)) {
        log::error("imgui init: GLFW backend failed");
        g_srv_heap = nullptr;
        ImGui::DestroyContext();
        delete state;
        return false;
    }
    state->glfw_backend = true;

    ImGui_ImplDX12_InitInfo dx12_info{};
    dx12_info.Device = device;
    dx12_info.CommandQueue = queue;
    dx12_info.NumFramesInFlight = static_cast<int>(renderer.dx12_num_frames_in_flight());
    dx12_info.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    dx12_info.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    dx12_info.SrvDescriptorHeap = state->heap.srv_heap.Get();
    dx12_info.SrvDescriptorAllocFn = imgui_srv_alloc;
    dx12_info.SrvDescriptorFreeFn = imgui_srv_free;

    if (!ImGui_ImplDX12_Init(&dx12_info)) {
        log::error("imgui init: DX12 backend failed");
        ImGui_ImplGlfw_Shutdown();
        g_srv_heap = nullptr;
        ImGui::DestroyContext();
        delete state;
        return false;
    }

    state_ = state;
    active_ = true;
    renderer.set_imgui_overlay(this);
    log::info("Dear ImGui overlay ready (GLFW + DX12)");
    return true;
}

bool ImGuiOverlay::initialize_win32(HWND__* hwnd, gfx::dx12::HelloO3dRenderer& renderer) {
    shutdown();
    if (!hwnd) return false;

    ID3D12Device* device = renderer.dx12_device();
    ID3D12CommandQueue* queue = renderer.dx12_command_queue();
    if (!device || !queue) {
        log::error("imgui win32 init: DX12 device/queue unavailable");
        return false;
    }

    auto* state = new State{};
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = kImguiSrvCapacity;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(state->heap.srv_heap.GetAddressOf())))) {
        log::error("imgui win32 init: SRV heap failed");
        delete state;
        return false;
    }
    state->heap.srv_cpu = state->heap.srv_heap->GetCPUDescriptorHandleForHeapStart();
    state->heap.srv_gpu = state->heap.srv_heap->GetGPUDescriptorHandleForHeapStart();
    state->heap.srv_stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_srv_heap = &state->heap;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    pin_imgui_ini_next_to_exe();
    apply_editor_theme();
    try_load_editor_font();

    if (!ImGui_ImplWin32_Init(hwnd)) {
        log::error("imgui win32 init: Win32 backend failed");
        g_srv_heap = nullptr;
        ImGui::DestroyContext();
        delete state;
        return false;
    }
    state->win32_backend = true;

    ImGui_ImplDX12_InitInfo dx12_info{};
    dx12_info.Device = device;
    dx12_info.CommandQueue = queue;
    dx12_info.NumFramesInFlight = static_cast<int>(renderer.dx12_num_frames_in_flight());
    dx12_info.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    dx12_info.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    dx12_info.SrvDescriptorHeap = state->heap.srv_heap.Get();
    dx12_info.SrvDescriptorAllocFn = imgui_srv_alloc;
    dx12_info.SrvDescriptorFreeFn = imgui_srv_free;
    if (!ImGui_ImplDX12_Init(&dx12_info)) {
        log::error("imgui win32 init: DX12 backend failed");
        ImGui_ImplWin32_Shutdown();
        g_srv_heap = nullptr;
        ImGui::DestroyContext();
        delete state;
        return false;
    }

    state_ = state;
    active_ = true;
    renderer.set_imgui_overlay(this);
    log::info("Dear ImGui overlay ready (Win32 + DX12)");
    return true;
}

void ImGuiOverlay::shutdown() {
    if (!active_) return;
    ImGui_ImplDX12_Shutdown();
    if (state_->glfw_backend) ImGui_ImplGlfw_Shutdown();
    if (state_->win32_backend) ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    g_srv_heap = nullptr;
    delete state_;
    state_ = nullptr;
    active_ = false;
}

void ImGuiOverlay::begin_frame() {
    if (!active_) return;
    ImGui_ImplDX12_NewFrame();
    if (state_->glfw_backend) ImGui_ImplGlfw_NewFrame();
    if (state_->win32_backend) ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
}

void ImGuiOverlay::end_frame_and_build() {
    if (!active_) return;
    if (build_fn_) {
        auto _w_fn = omsi::MainThreadWatchdog::active_scope("imgui.build_fn");
        build_fn_();
    }
    {
        auto _w_render = omsi::MainThreadWatchdog::active_scope("imgui.render_core");
        ImGui::Render();
    }
}

void ImGuiOverlay::set_build_fn(BuildFn fn) { build_fn_ = std::move(fn); }

bool ImGuiOverlay::wants_capture_mouse() const {
    return active_ && ImGui::GetIO().WantCaptureMouse;
}

bool ImGuiOverlay::wants_capture_keyboard() const {
    if (!active_) return false;
    const ImGuiIO& io = ImGui::GetIO();
    return io.WantCaptureKeyboard || io.WantTextInput;
}
bool ImGuiOverlay::wants_text_input() const {
    if (!active_) return false;
    return ImGui::GetIO().WantTextInput;
}

bool ImGuiOverlay::alloc_srv(D3D12_CPU_DESCRIPTOR_HANDLE* out_cpu, D3D12_GPU_DESCRIPTOR_HANDLE* out_gpu) {
    if (!active_ || !state_ || !out_cpu || !out_gpu || !g_srv_heap || g_srv_heap->srv_next >= kImguiSrvCapacity)
        return false;
    imgui_srv_alloc(nullptr, out_cpu, out_gpu);
    return true;
}

void ImGuiOverlay::render_dx12(ID3D12GraphicsCommandList* command_list) {
    if (!active_ || !command_list || !state_) return;
    auto _w = omsi::MainThreadWatchdog::active_scope("imgui.dx12_render_draw_data");
    ImDrawData* draw_data = ImGui::GetDrawData();
    if (!draw_data || draw_data->TotalVtxCount <= 0) return;

    ID3D12DescriptorHeap* heaps[] = {state_->heap.srv_heap.Get()};
    command_list->SetDescriptorHeaps(1, heaps);
    ImGui_ImplDX12_RenderDrawData(draw_data, command_list);
}

} // namespace omsi::ui
