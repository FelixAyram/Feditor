#include "omsi_bus_camera.hpp"

#include "omsi/o3d_format.hpp"

#include <GLFW/glfw3.h>

#include <DirectXMath.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace omsi::input {
namespace {

void store_transposed(const DirectX::XMMATRIX& m, float out_transposed[16]) {
    DirectX::XMFLOAT4X4 stored{};
    DirectX::XMStoreFloat4x4(&stored, DirectX::XMMatrixTranspose(m));
    std::memcpy(out_transposed, &stored, sizeof(stored));
}

DirectX::XMVECTOR omsi_forward_from_angles(float yaw_deg, float pitch_deg) {
    const float yaw = DirectX::XMConvertToRadians(yaw_deg);
    const float pitch = DirectX::XMConvertToRadians(pitch_deg);
    const float cp = std::cosf(pitch);
    // OMSI bus: X right, Y forward, Z up. yaw=0,pitch=0 -> +Y in file space.
    const f4f23ma::f4 fwd = omsi::o3d::bus_omsi_dir_to_display(std::sinf(yaw) * cp, std::cosf(yaw) * cp, std::sinf(pitch));
    return DirectX::XMVector3Normalize(DirectX::XMVectorSet(fwd.x, fwd.y, fwd.z, 0.f));
}

DirectX::XMVECTOR bus_point_to_display(float x, float y, float z) {
    const f4f23ma::f4 p = omsi::o3d::bus_omsi_to_display(x, y, z);
    return DirectX::XMVectorSet(p.x, p.y, p.z, 1.f);
}

} // namespace

void OmsiBusCamera::set_definition(const bus::BusCamera& cam, int cam_index) {
    if (index != cam_index) reset_look();
    def = cam;
    index = cam_index;
}

void OmsiBusCamera::reset_look() {
    yaw_offset_deg_ = 0.f;
    pitch_offset_deg_ = 0.f;
    zoom_factor_ = 1.f;
    has_last_ = false;
}

void OmsiBusCamera::update_mouse(GLFWwindow* window) {
    const bool mmb = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS;
    const bool rmb = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;

    double x = 0.0;
    double y = 0.0;
    glfwGetCursorPos(window, &x, &y);

    if (!has_last_) {
        last_x_ = x;
        last_y_ = y;
        has_last_ = true;
        return;
    }

    if (mmb) {
        const float dx = static_cast<float>(x - last_x_);
        const float dy = static_cast<float>(y - last_y_);
        yaw_offset_deg_ -= dx * 0.2f;
        pitch_offset_deg_ += dy * 0.2f;
        pitch_offset_deg_ = std::clamp(pitch_offset_deg_, -85.f, 85.f);
    } else if (rmb) {
        const float dx = static_cast<float>(x - last_x_);
        const float dy = static_cast<float>(y - last_y_);
        zoom_factor_ += (-dy + dx) * 0.004f;
        zoom_factor_ = std::clamp(zoom_factor_, 0.35f, 3.f);
    }

    last_x_ = x;
    last_y_ = y;
}

void OmsiBusCamera::set_body_pose(float heave_m, float pitch_rad, float roll_rad, float ride_z_m) {
    body_heave_m_ = heave_m;
    body_pitch_rad_ = pitch_rad;
    body_roll_rad_ = roll_rad;
    ride_z_m_ = ride_z_m;
}

void OmsiBusCamera::build_world_view_proj(float aspect, float out_transposed_wvp[16]) const {
    const float yaw = def.yaw_deg + yaw_offset_deg_;
    const float pitch = def.pitch_deg + pitch_offset_deg_;
    DirectX::XMVECTOR pivot = bus_point_to_display(def.pos_x, def.pos_y, def.pos_z);
    // Ride + Federung around Schwerpunkt: match vehicle body matrix in hello_o3d_app.
    const float cog_z = 1.2f;
    const DirectX::XMMATRIX body =
        DirectX::XMMatrixTranslation(0.f, 0.f, -cog_z) *
        DirectX::XMMatrixRotationX(body_pitch_rad_) *
        DirectX::XMMatrixRotationY(body_roll_rad_) *
        DirectX::XMMatrixTranslation(0.f, 0.f, cog_z + body_heave_m_ + ride_z_m_);
    pivot = DirectX::XMVector3TransformCoord(pivot, body);
    DirectX::XMVECTOR forward = omsi_forward_from_angles(yaw, pitch);
    forward = DirectX::XMVector3TransformNormal(forward, body);
    forward = DirectX::XMVector3Normalize(forward);
    DirectX::XMVECTOR up = DirectX::XMVectorSet(0.f, 0.f, 1.f, 0.f);
    up = DirectX::XMVector3Normalize(DirectX::XMVector3TransformNormal(up, body));
    const DirectX::XMVECTOR eye =
        DirectX::XMVectorAdd(pivot, DirectX::XMVectorScale(forward, -def.rotation_centre_offset));
    const DirectX::XMVECTOR target = DirectX::XMVectorAdd(eye, forward);
    const DirectX::XMMATRIX view = DirectX::XMMatrixLookAtLH(eye, target, up);
    const float base_fov = (std::max)(def.fov_deg, 1.f);
    const float fov_deg = base_fov / zoom_factor_;
    const float fov = DirectX::XMConvertToRadians(fov_deg);
    const DirectX::XMMATRIX proj = DirectX::XMMatrixPerspectiveFovLH(fov, aspect, 0.05f, 500.f);
    store_transposed(view * proj, out_transposed_wvp);
}

void OmsiBusCamera::cycle(int delta, int count) {
    if (count <= 0) return;
    index = (index + delta + count * 1024) % count;
}

void BusCameraController::apply_bus_defaults(int std_driver_index) {
    driver_index = std_driver_index;
    saved_driver_index = std_driver_index;
    passenger_index = 0;
    mode = BusViewMode::driver;
}

void BusCameraController::on_key_f1() {
    mode = BusViewMode::driver;
    driver_index = saved_driver_index;
}

void BusCameraController::on_key_f2() { mode = BusViewMode::passenger; }

void BusCameraController::on_key_f3() { mode = BusViewMode::exterior; }

void BusCameraController::cycle_driver(int delta, int count) {
    if (count <= 0) return;
    driver_index = (driver_index + delta + count * 1024) % count;
}

void BusCameraController::cycle_passenger(int delta, int count) {
    if (count <= 0) return;
    passenger_index = (passenger_index + delta + count * 1024) % count;
}

} // namespace omsi::input
