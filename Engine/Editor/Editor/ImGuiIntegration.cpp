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
#if defined(_WIN32)
#include "D3D12/D3D12Device.h"   // D3D12 原生句柄（imgui_impl_dx12 需要）
#include "D3D12/D3D12Converters.h"   // ToDxgiFormat
#include "imgui_impl_dx12.h"
#endif

namespace he::editor {

ImGuiIntegration::ImGuiIntegration() = default;
ImGuiIntegration::~ImGuiIntegration() { Shutdown(); }

void ImGuiIntegration::Initialize(GLFWwindow* window, rhi::IRHIDevice* device,
                                   rhi::IRHISwapChain* swapchain) {
    m_Window = window;
    m_Device = device;

    // 【多后端】Vulkan 用 imgui_impl_vulkan，D3D12 用 imgui_impl_dx12。
    // 其它后端直接跳过：调用方（样例）已经用 `if (imgui.IsInitialized())` 守卫所有
    // UI 代码，跳过只是没有 UI，渲染照常。注意不能对非 Vulkan 设备走 Vulkan 路径——
    // static_cast<VulkanDevice*> 会拿到垃圾指针，实测表现为进程死等、不出帧。
    m_IsD3D12 = device && device->GetBackend() == rhi::Backend::D3D12;
#if !defined(_WIN32)
    m_IsD3D12 = false;
#endif
    if (device && device->GetBackend() != rhi::Backend::Vulkan && !m_IsD3D12) {
        HE_CORE_INFO("ImGui: 当前后端不是 Vulkan（{}），跳过 UI 初始化",
                     static_cast<int>(device->GetBackend()));
        m_Initialized = false;
        return;
    }
    IMGUI_CHECKVERSION();
    m_Context = ImGui::CreateContext();
    ImGui::StyleColorsDark();

    // --- 加载中文字体支持 ---
    {
        ImGuiIO& io = ImGui::GetIO();

        static const ImWchar chineseRanges[] = {
            0x0020, 0x00FF,  // Basic Latin + Latin Supplement
            0x2000, 0x206F,  // General Punctuation（—— 破折号等）
            0x3000, 0x303F,  // CJK Symbols and Punctuation（。、？；）
            0x4E00, 0x9FFF,  // CJK Unified Ideographs
            0xFF00, 0xFFEF,  // Fullwidth Forms（全角括号（）等）
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

    if (m_IsD3D12) ImGui_ImplGlfw_InitForOther(window, true);
    else           ImGui_ImplGlfw_InitForVulkan(window, true);
    CreateImGuiResources(device, swapchain);

    m_Initialized = true;
    HE_CORE_INFO("ImGuiIntegration initialized");
}

void ImGuiIntegration::CreateImGuiResources(rhi::IRHIDevice* device,
                                             rhi::IRHISwapChain* swapchain) {
#if defined(_WIN32)
    if (m_IsD3D12) {
        // 字体 SRV 放在主 CBV_SRV_UAV 堆的保留槽位，渲染时与场景共用同一张堆
        auto* d3d = static_cast<rhi::d3d12::D3D12Device*>(device);
        m_DescPool = device->CreateImGuiDescriptorPool();
        const DXGI_FORMAT rtvFormat = rhi::d3d12::ToDxgiFormat(swapchain->GetColorFormat());
        ImGui_ImplDX12_Init(d3d->GetDevice(), static_cast<int>(rhi::kMaxFramesInFlight), rtvFormat,
                            static_cast<ID3D12DescriptorHeap*>(m_DescPool),
                            d3d->GetImGuiFontSrvCpu(), d3d->GetImGuiFontSrvGpu());
        HE_CORE_INFO("ImGui D3D12 backend initialized");
        return;
    }
#endif
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
    if (device && !m_IsD3D12) {
        device->DestroyImGuiRenderPass(m_RenderPass);
        device->DestroyImGuiDescriptorPool(m_DescPool);
    }
    m_RenderPass = nullptr;
    m_DescPool   = nullptr;
}

void ImGuiIntegration::Shutdown() {
    if (!m_Initialized) return;
    m_Initialized = false;
#if defined(_WIN32)
    if (m_IsD3D12) {
        if (m_Device) m_Device->WaitIdle();   // ImGui 的顶点/索引缓冲可能仍被在飞帧使用
        ImGui_ImplDX12_Shutdown();
    } else
#endif
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
#if defined(_WIN32)
    if (m_IsD3D12) ImGui_ImplDX12_NewFrame();
    else
#endif
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

void ImGuiIntegration::EndFrame(rhi::IRHICommandList* cmd) {
    ImGui::Render();
#if defined(_WIN32)
    if (m_IsD3D12) {
        // 返回原生列表前已绑定引擎的描述符堆（含 ImGui 字体 SRV 槽位）
        auto* list = static_cast<ID3D12GraphicsCommandList*>(m_Device->GetImGuiCommandBuffer(cmd));
        if (list) ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), list);
        return;
    }
#endif
    // 通过 RHI 接口获取 VkCommandBuffer，无需 static_cast<VulkanCommandList*>
    auto* vkCmdBuf = static_cast<VkCommandBuffer>(m_Device->GetImGuiCommandBuffer(cmd));
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), vkCmdBuf);
}

} // namespace he::editor
