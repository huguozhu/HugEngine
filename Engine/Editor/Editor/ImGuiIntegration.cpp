// ============================================================
// ImGuiIntegration.cpp — ImGui v1.91 + GLFW + Vulkan
//
// Vulkan 特定资源（RenderPass / DescriptorPool）通过 RHI 接口创建，
// 不直接调用 vkCreate* / vkDestroy*。
// ============================================================

#include "Editor/ImGuiIntegration.h"
#include "RHI/RHI.h"
#include "RHI/CommandList.h"
#include "RHI/SwapChain.h"
#include "Core/Log.h"

#include "Vulkan/VulkanDevice.h"  // VulkanDeviceAccess（获取 Vk 句柄）
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_vulkan.h"

namespace he::editor {

ImGuiIntegration::ImGuiIntegration() = default;
ImGuiIntegration::~ImGuiIntegration() { Shutdown(); }

void ImGuiIntegration::Initialize(GLFWwindow* window, rhi::IRHIDevice* device,
                                   rhi::IRHISwapChain* swapchain) {
    m_Window = window;
    m_Device = device;
    IMGUI_CHECKVERSION();
    m_Context = ImGui::CreateContext();
    ImGui::StyleColorsDark();

    // --- 加载中文字体支持 ---
    {
        ImGuiIO& io = ImGui::GetIO();

        static const ImWchar chineseRanges[] = {
            0x0020, 0x00FF,  // Basic Latin + Latin Supplement
            0x4E00, 0x9FFF,  // CJK Unified Ideographs
            0,
        };

        ImFontConfig fontConfig{};
        fontConfig.SizePixels = 16.0f;

        ImFont* cjkFont = io.Fonts->AddFontFromFileTTF(
            "C:\\Windows\\Fonts\\simhei.ttf", 16.0f, &fontConfig, chineseRanges);
        if (cjkFont) {
            io.FontDefault = cjkFont;
            HE_CORE_INFO("ImGui 中文字体已加载: simhei.ttf");
        } else {
            HE_CORE_WARN("ImGui 中文字体加载失败，中文将无法正常显示");
        }
    }

    ImGui_ImplGlfw_InitForVulkan(window, true);
    CreateImGuiResources(device, swapchain);

    m_Initialized = true;
    HE_CORE_INFO("ImGuiIntegration initialized");
}

void ImGuiIntegration::CreateImGuiResources(rhi::IRHIDevice* device,
                                             rhi::IRHISwapChain* swapchain) {
    // 通过 VulkanDeviceAccess 获取后端原生句柄（imgui_impl_vulkan 需要）
    VkInstance       instance       = rhi::VulkanDeviceAccess::GetInstance(device);
    VkPhysicalDevice physicalDevice = rhi::VulkanDeviceAccess::GetPhysical(device);
    VkDevice         vkDevice       = rhi::VulkanDeviceAccess::GetDevice(device);
    u32              graphicsFamily = rhi::VulkanDeviceAccess::GetGraphicsFamily(device);
    VkQueue          graphicsQueue  = rhi::VulkanDeviceAccess::GetGraphicsQueue(device);

    // Descriptor Pool — 通过 RHI 接口创建，封装 vkCreateDescriptorPool
    m_DescPool = device->CreateImGuiDescriptorPool();

    // RenderPass — 通过 RHI 接口创建，封装 vkCreateRenderPass
    u32 swapchainFormat = swapchain->GetBackendFormat();
    m_RenderPass = device->CreateImGuiRenderPass(swapchainFormat);

    // 初始化 ImGui Vulkan 后端
    ImGui_ImplVulkan_InitInfo initInfo{};
    initInfo.Instance        = instance;
    initInfo.PhysicalDevice  = physicalDevice;
    initInfo.Device          = vkDevice;
    initInfo.QueueFamily     = graphicsFamily;
    initInfo.Queue           = graphicsQueue;
    initInfo.DescriptorPool  = reinterpret_cast<VkDescriptorPool>(m_DescPool);
    initInfo.MinImageCount   = rhi::kSwapchainImageCount;
    initInfo.ImageCount      = rhi::kSwapchainImageCount;
    initInfo.RenderPass      = static_cast<VkRenderPass>(m_RenderPass);
    initInfo.Subpass         = 0;
    initInfo.MSAASamples     = VK_SAMPLE_COUNT_1_BIT;

    ImGui_ImplVulkan_Init(&initInfo);

    // Upload fonts
    ImGui_ImplVulkan_CreateFontsTexture();

    HE_CORE_INFO("ImGui Vulkan backend initialized");
}

void ImGuiIntegration::DestroyImGuiResources(rhi::IRHIDevice* device) {
    if (device) {
        device->DestroyImGuiRenderPass(m_RenderPass);
        device->DestroyImGuiDescriptorPool(m_DescPool);
    }
    m_RenderPass = nullptr;
    m_DescPool   = nullptr;
}

void ImGuiIntegration::Shutdown() {
    if (!m_Initialized) return;
    m_Initialized = false;
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext(m_Context);
    m_Context = nullptr;
    m_Window  = nullptr;
    DestroyImGuiResources(m_Device);
    m_Device  = nullptr;
    HE_CORE_INFO("ImGuiIntegration shut down");
}

void ImGuiIntegration::BeginFrame() {
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

void ImGuiIntegration::EndFrame(rhi::IRHICommandList* cmd) {
    ImGui::Render();
    // 通过 RHI 接口获取 VkCommandBuffer，无需 static_cast<VulkanCommandList*>
    auto* vkCmdBuf = static_cast<VkCommandBuffer>(m_Device->GetImGuiCommandBuffer(cmd));
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), vkCmdBuf);
}

} // namespace he::editor
