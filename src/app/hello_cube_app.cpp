#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

#include <DirectXMath.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>

#include "app/hello_cube_app.hpp"
#include "gfx/dx12/hello_cube_renderer.hpp"
#include "omsi/logger.hpp"

namespace {

// Frank Luna d3d12book BoxApp camera (Chapter 6).
struct LunaCamera {
    float theta = 1.5f * DirectX::XM_PI;
    float phi = DirectX::XM_PIDIV4;
    float radius = 5.f;
    double last_x = 0.0;
    double last_y = 0.0;
    bool has_last = false;

    void update(GLFWwindow* window) {
        double x = 0.0;
        double y = 0.0;
        glfwGetCursorPos(window, &x, &y);

        const bool lmb = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
        const bool rmb = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;

        if (!has_last) {
            last_x = x;
            last_y = y;
            has_last = true;
            return;
        }

        if (lmb) {
            const float dx = DirectX::XMConvertToRadians(0.25f * static_cast<float>(x - last_x));
            const float dy = DirectX::XMConvertToRadians(0.25f * static_cast<float>(y - last_y));
            theta += dx;
            phi += dy;
            phi = std::clamp(phi, 0.1f, DirectX::XM_PI - 0.1f);
        } else if (rmb) {
            const float dx = 0.005f * static_cast<float>(x - last_x);
            const float dy = 0.005f * static_cast<float>(y - last_y);
            radius += dx - dy;
            radius = std::clamp(radius, 3.f, 15.f);
        }

        last_x = x;
        last_y = y;
    }

    void build_world_view_proj(float aspect, float out_transposed[16]) const {
        const float x = radius * std::sinf(phi) * std::cosf(theta);
        const float z = radius * std::sinf(phi) * std::sinf(theta);
        const float y = radius * std::cosf(phi);

        const DirectX::XMVECTOR pos = DirectX::XMVectorSet(x, y, z, 1.f);
        const DirectX::XMVECTOR target = DirectX::XMVectorZero();
        const DirectX::XMVECTOR up = DirectX::XMVectorSet(0.f, 1.f, 0.f, 0.f);

        const DirectX::XMMATRIX view = DirectX::XMMatrixLookAtLH(pos, target, up);
        const DirectX::XMMATRIX proj = DirectX::XMMatrixPerspectiveFovLH(0.25f * DirectX::XM_PI, aspect, 1.f, 1000.f);
        const DirectX::XMMATRIX world = DirectX::XMMatrixIdentity();
        const DirectX::XMMATRIX world_view_proj = world * view * proj;

        DirectX::XMFLOAT4X4 m{};
        DirectX::XMStoreFloat4x4(&m, DirectX::XMMatrixTranspose(world_view_proj));
        std::memcpy(out_transposed, &m, sizeof(m));
    }
};

} // namespace

namespace omsi::app {

int run_hello_cube_app(int max_frames) {
    omsi::log::info("hello cube app (Frank Luna d3d12book BoxApp)");

    if (!glfwInit()) {
        omsi::log::error("glfwInit failed");
        return 5;
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* window = glfwCreateWindow(1280, 720, "OMSI Hello Cube", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        omsi::log::error("glfwCreateWindow failed");
        return 5;
    }

    int fbw = 0;
    int fbh = 0;
    glfwGetFramebufferSize(window, &fbw, &fbh);

    omsi::gfx::dx12::HelloCubeRenderer renderer{};
    if (!renderer.initialize(glfwGetWin32Window(window), static_cast<std::uint32_t>(fbw),
                             static_cast<std::uint32_t>(fbh))) {
        glfwDestroyWindow(window);
        glfwTerminate();
        return 7;
    }

    LunaCamera camera{};

    auto last = std::chrono::steady_clock::now();
    int rendered_frames = 0;
    float fps_accum = 0.f;
    int frame_count = 0;
    int last_fbw = fbw;
    int last_fbh = fbh;

    while (!glfwWindowShouldClose(window)) {
        const auto now = std::chrono::steady_clock::now();
        const float dt = std::chrono::duration<float>(now - last).count();
        last = now;
        fps_accum += dt;
        ++frame_count;

        glfwPollEvents();
        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) glfwSetWindowShouldClose(window, GLFW_TRUE);

        camera.update(window);

        glfwGetFramebufferSize(window, &fbw, &fbh);
        if (fbw > 0 && fbh > 0 && (fbw != last_fbw || fbh != last_fbh)) {
            renderer.resize(static_cast<std::uint32_t>(fbw), static_cast<std::uint32_t>(fbh));
            last_fbw = fbw;
            last_fbh = fbh;
        }

        const float aspect = fbw > 0 && fbh > 0 ? static_cast<float>(fbw) / static_cast<float>(fbh) : 16.f / 9.f;
        float wvp[16]{};
        camera.build_world_view_proj(aspect, wvp);
        renderer.set_mvp(wvp);
        renderer.render();

        if (fps_accum >= 1.f) {
            const float fps_value = static_cast<float>(frame_count) / fps_accum;
            frame_count = 0;
            fps_accum = 0.f;
            std::ostringstream title;
            title << "OMSI Hello Cube | " << static_cast<int>(fps_value)
                  << " FPS | LMB orbitar, RMB zoom, ESC salir";
            glfwSetWindowTitle(window, title.str().c_str());
            omsi::log::info("hello cube fps=" + std::to_string(static_cast<int>(fps_value)));
        }

        if (max_frames > 0 && ++rendered_frames >= max_frames) glfwSetWindowShouldClose(window, GLFW_TRUE);
    }

    renderer.shutdown();
    glfwDestroyWindow(window);
    glfwTerminate();
    omsi::log::info("hello cube exit ok");
    return 0;
}

} // namespace omsi::app
