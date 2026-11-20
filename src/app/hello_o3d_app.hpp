#pragma once

#include <filesystem>

namespace omsi::app {

// If rot_deg_fixed >= 0, applies Unit87 Origin*Rx*OriginI isolation rotation (for gauge needles).
// axis_mode: 0=Unit87 Old*Rot (cfgY→RotZ), 1=Unit87 Left*Rot, 2=wiki names Old*Rot, 3=RX only about GPU X.
int run_hello_o3d_app(const std::filesystem::path& o3d_path, const std::filesystem::path& omsi_root,
                      int max_frames = -1, const std::filesystem::path& screenshot_path = {},
                      float rot_deg_fixed = -1.f, int axis_mode = 0,
                      float origin_rx = -43.9091f, float origin_ry = 90.f, float origin_rz = 0.f,
                      float origin_tx = 0.f, float origin_ty = 0.f, float origin_tz = 0.f,
                      bool has_origin_trans = false);

int run_hello_model_cfg_app(const std::filesystem::path& model_cfg_path, const std::filesystem::path& omsi_root,
                            int max_frames, const std::filesystem::path& screenshot_path);

int run_hello_bus_app(const std::filesystem::path& bus_path, const std::filesystem::path& omsi_root, int max_frames,
                      const std::filesystem::path& screenshot_path = {});

} // namespace omsi::app
