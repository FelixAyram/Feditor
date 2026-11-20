#include "vk_renderer.hpp"

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <stdexcept>

namespace omsi::gfx::vulkan {

VkRenderer::VkRenderer(const GfxConfig& config) : config_(config) {}

VkRenderer::~VkRenderer() { shutdown(); }

bool VkRenderer::initialize(void* native_window) {
    auto* window = static_cast<GLFWwindow*>(native_window);

    VkApplicationInfo app_info{};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = config_.title.c_str();
    app_info.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app_info.pEngineName = "omsi-f4-engine";
    app_info.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    app_info.apiVersion = VK_API_VERSION_1_3;

    uint32_t ext_count = 0;
    const char** glfw_ext = glfwGetRequiredInstanceExtensions(&ext_count);

    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app_info;
    ici.enabledExtensionCount = ext_count;
    ici.ppEnabledExtensionNames = glfw_ext;

    if (vkCreateInstance(&ici, nullptr, &instance_) != VK_SUCCESS) return false;

    uint32_t pd_count = 0;
    vkEnumeratePhysicalDevices(instance_, &pd_count, nullptr);
    if (pd_count == 0) return false;
    std::vector<VkPhysicalDevice> pds(pd_count);
    vkEnumeratePhysicalDevices(instance_, &pd_count, pds.data());

    uint32_t qf_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pds[0], &qf_count, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qf_count);
    vkGetPhysicalDeviceQueueFamilyProperties(pds[0], &qf_count, qf.data());

    uint32_t graphics_family = 0;
    for (uint32_t i = 0; i < qf_count; ++i) {
        if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            graphics_family = i;
            break;
        }
    }

    float prio = 1.f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = graphics_family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;

    const char* dev_ext[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = dev_ext;
    if (vkCreateDevice(pds[0], &dci, nullptr, &device_) != VK_SUCCESS) return false;
    vkGetDeviceQueue(device_, graphics_family, 0, &graphics_queue_);

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (glfwCreateWindowSurface(instance_, window, nullptr, &surface) != VK_SUCCESS) return false;

    VkSwapchainCreateInfoKHR sci{};
    sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    sci.surface = surface;
    sci.minImageCount = 2;
    sci.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
    sci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    sci.imageExtent = {config_.width, config_.height};
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = config_.vsync ? VK_PRESENT_MODE_FIFO_KHR : VK_PRESENT_MODE_MAILBOX_KHR;
    sci.clipped = VK_TRUE;
    if (vkCreateSwapchainKHR(device_, &sci, nullptr, &swapchain_) != VK_SUCCESS) return false;

    uint32_t img_count = 0;
    vkGetSwapchainImagesKHR(device_, swapchain_, &img_count, nullptr);
    swap_images_.resize(img_count);
    vkGetSwapchainImagesKHR(device_, swapchain_, &img_count, swap_images_.data());

    swap_views_.resize(img_count);
    for (uint32_t i = 0; i < img_count; ++i) {
        VkImageViewCreateInfo ivci{};
        ivci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        ivci.image = swap_images_[i];
        ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivci.format = VK_FORMAT_B8G8R8A8_UNORM;
        ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ivci.subresourceRange.levelCount = 1;
        ivci.subresourceRange.layerCount = 1;
        vkCreateImageView(device_, &ivci, nullptr, &swap_views_[i]);
    }

    VkAttachmentDescription color{};
    color.format = VK_FORMAT_B8G8R8A8_UNORM;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference color_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;

    VkRenderPassCreateInfo rpci{};
    rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpci.attachmentCount = 1;
    rpci.pAttachments = &color;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &subpass;
    vkCreateRenderPass(device_, &rpci, nullptr, &render_pass_);

    framebuffers_.resize(img_count);
    for (uint32_t i = 0; i < img_count; ++i) {
        VkFramebufferCreateInfo fbci{};
        fbci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbci.renderPass = render_pass_;
        fbci.attachmentCount = 1;
        fbci.pAttachments = &swap_views_[i];
        fbci.width = config_.width;
        fbci.height = config_.height;
        fbci.layers = 1;
        vkCreateFramebuffer(device_, &fbci, nullptr, &framebuffers_[i]);
    }

    VkCommandPoolCreateInfo cpci{};
    cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci.queueFamilyIndex = graphics_family;
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    vkCreateCommandPool(device_, &cpci, nullptr, &cmd_pool_);

    cmd_bufs_.resize(img_count);
    VkCommandBufferAllocateInfo cbai{};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = cmd_pool_;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = img_count;
    vkAllocateCommandBuffers(device_, &cbai, cmd_bufs_.data());

    image_available_.resize(img_count);
    render_finished_.resize(img_count);
    in_flight_.resize(img_count);
    for (uint32_t i = 0; i < img_count; ++i) {
        VkSemaphoreCreateInfo sci2{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCreateSemaphore(device_, &sci2, nullptr, &image_available_[i]);
        vkCreateSemaphore(device_, &sci2, nullptr, &render_finished_[i]);
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, VK_FENCE_CREATE_SIGNALED_BIT};
        vkCreateFence(device_, &fci, nullptr, &in_flight_[i]);
    }

    (void)native_window;
    return true;
}

void VkRenderer::resize(std::uint32_t width, std::uint32_t height) {
    config_.width = width;
    config_.height = height;
}

void VkRenderer::begin_frame(const f4f23ma::ma4& view, const f4f23ma::ma4& proj) {
    view_ = view;
    proj_ = proj;
    vkWaitForFences(device_, 1, &in_flight_[frame_i_], VK_TRUE, UINT64_MAX);

    uint32_t image_index = 0;
    vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, image_available_[frame_i_], VK_NULL_HANDLE,
                          &image_index);

    vkResetFences(device_, 1, &in_flight_[frame_i_]);
    vkResetCommandBuffer(cmd_bufs_[frame_i_], 0);

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(cmd_bufs_[frame_i_], &bi);

    VkClearValue clear{};
    clear.color = {{0.02f, 0.04f, 0.08f, 1.f}};
    VkRenderPassBeginInfo rpbi{};
    rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpbi.renderPass = render_pass_;
    rpbi.framebuffer = framebuffers_[image_index];
    rpbi.renderArea.extent = {config_.width, config_.height};
    rpbi.clearValueCount = 1;
    rpbi.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd_bufs_[frame_i_], &rpbi, VK_SUBPASS_CONTENTS_INLINE);
}

void VkRenderer::prepare_mesh(const GpuMesh& mesh, const o3d::MappedO3D& source) {
    (void)mesh;
    (void)source;
}

void VkRenderer::draw_mesh(const GpuMesh& mesh, const o3d::MappedO3D& source) {
    (void)mesh;
    (void)source;
    // TODO: upload native O3D vertex/index blobs to VkBuffer and draw with omsi vertex layout.
}

void VkRenderer::prepare_buffer_mesh(std::string_view key, const o3d::Vertex* vertices,
                                     std::uint32_t vertex_count, const std::uint32_t* indices,
                                     std::uint32_t index_count, const std::filesystem::path& texture_path) {
    (void)key;
    (void)vertices;
    (void)vertex_count;
    (void)indices;
    (void)index_count;
    (void)texture_path;
}

void VkRenderer::draw_buffer_mesh(std::string_view key, const f4f23ma::ma4& object) {
    (void)key;
    (void)object;
}

void VkRenderer::end_frame() {
    vkCmdEndRenderPass(cmd_bufs_[frame_i_]);
    vkEndCommandBuffer(cmd_bufs_[frame_i_]);

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &image_available_[frame_i_];
    si.pWaitDstStageMask = &wait_stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd_bufs_[frame_i_];
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &render_finished_[frame_i_];
    vkQueueSubmit(graphics_queue_, 1, &si, in_flight_[frame_i_]);

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &render_finished_[frame_i_];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    uint32_t idx = frame_i_;
    pi.pImageIndices = &idx;
    vkQueuePresentKHR(graphics_queue_, &pi);

    frame_i_ = (frame_i_ + 1) % static_cast<uint32_t>(cmd_bufs_.size());
}

void VkRenderer::wait_for_gpu() {
    if (device_) vkDeviceWaitIdle(device_);
}

void VkRenderer::shutdown() {
    if (!device_) return;
    vkDeviceWaitIdle(device_);
    for (auto s : image_available_) vkDestroySemaphore(device_, s, nullptr);
    for (auto s : render_finished_) vkDestroySemaphore(device_, s, nullptr);
    for (auto f : in_flight_) vkDestroyFence(device_, f, nullptr);
    vkDestroyCommandPool(device_, cmd_pool_, nullptr);
    for (auto fb : framebuffers_) vkDestroyFramebuffer(device_, fb, nullptr);
    vkDestroyRenderPass(device_, render_pass_, nullptr);
    for (auto v : swap_views_) vkDestroyImageView(device_, v, nullptr);
    vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    vkDestroyDevice(device_, nullptr);
    vkDestroyInstance(instance_, nullptr);
    device_ = VK_NULL_HANDLE;
    instance_ = VK_NULL_HANDLE;
}

} // namespace omsi::gfx::vulkan
