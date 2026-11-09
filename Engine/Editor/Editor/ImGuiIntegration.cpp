// ============================================================
// ImGuiIntegration.cpp — ImGui v1.91 + GLFW + Vulkan / D3D12 / D3D11
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
#include "Platform/IVfs.h"   // 鸿蒙：字体从沙箱解析
#include "Platform/IWindow.h"  // GetContentScale（UI 密度缩放）
#include "imgui.h"
#if HE_PLATFORM_WINDOWS
#include "Platform/WindowWin32.h"   // GetGlfwWindow(IWindow*)
#include "imgui_impl_glfw.h"
#endif
#if HE_PLATFORM_OHOS
#include "imgui_impl_ohos.h"   // 触摸输入后端
#endif
#include "imgui_impl_vulkan.h"
#if defined(HE_RHI_D3D12)
#include "D3D12/D3D12Device.h"   // D3D12 原生句柄（imgui_impl_dx12 需要）
#include "D3D12/D3D12Converters.h"   // ToDxgiFormat
#include "imgui_impl_dx12.h"
#endif
#if defined(HE_RHI_D3D11)
#include "D3D11/D3D11Device.h"   // D3D11 原生设备 / 上下文（imgui_impl_dx11 需要）
#include "imgui_impl_dx11.h"
#endif

namespace he::editor {

ImGuiIntegration::ImGuiIntegration() = default;
ImGuiIntegration::~ImGuiIntegration() { Shutdown(); }

void ImGuiIntegration::Initialize(IWindow* window, rhi::IRHIDevice* device,
                                   rhi::IRHISwapChain* swapchain) {
    m_Window = window;
#if HE_PLATFORM_WINDOWS
    // Windows：ImGui 的 GLFW 后端需要 GLFWwindow*（鸿蒙走 imgui_impl_ohos，见 P3）
    GLFWwindow* glfwWindow = GetGlfwWindow(window);
#else
    void* glfwWindow = nullptr;
#endif
    m_Device = device;

    // 【多后端】Vulkan 用 imgui_impl_vulkan，D3D12 用 imgui_impl_dx12，D3D11 用 imgui_impl_dx11。
    // 其它后端直接跳过：调用方（样例）已经用 `if (imgui.IsInitialized())` 守卫所有
    // UI 代码，跳过只是没有 UI，渲染照常。注意不能对非 Vulkan 设备走 Vulkan 路径——
    // static_cast<VulkanDevice*> 会拿到垃圾指针，实测表现为进程死等、不出帧。
    m_IsD3D12 = device && device->GetBackend() == rhi::Backend::D3D12;
#if !defined(HE_RHI_D3D12)
    m_IsD3D12 = false;
#endif
    m_IsD3D11 = device && device->GetBackend() == rhi::Backend::D3D11;
#if !defined(HE_RHI_D3D11)
    m_IsD3D11 = false;
#endif
    if (device && device->GetBackend() != rhi::Backend::Vulkan && !m_IsD3D12 && !m_IsD3D11) {
        HE_CORE_INFO("ImGui: 当前后端不是 Vulkan（{}），跳过 UI 初始化",
                     static_cast<int>(device->GetBackend()));
        m_Initialized = false;
        return;
    }
    IMGUI_CHECKVERSION();
    m_Context = ImGui::CreateContext();
    ImGui::StyleColorsDark();

    // --- 内容缩放（手机高 DPI：1260px 宽的屏上 16px 字会小到看不清）---
    // Windows 恒为 1；鸿蒙由 ArkTS 把 display.densityPixels 传进 WindowOHOS。
    const f32 uiScale = (window ? window->GetContentScale() : 1.0f);
    if (uiScale > 0.0f && uiScale != 1.0f) {
        ImGui::GetStyle().ScaleAllSizes(uiScale);   // 行高、内边距、滚动条一起放大
        HE_CORE_INFO("ImGui: UI 缩放 = {}", uiScale);
    }

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
        // 字号同样乘以 UI 缩放：直接按物理像素栅格化，比放大位图清晰

#if HE_PLATFORM_WINDOWS
        const char* fontCandidates[] = { "C:\\Windows\\Fonts\\simhei.ttf" };
#else
        // 鸿蒙：字体随包放在 rawfile 的 content/font/ 下，解压到沙箱后由 vfs 解析。
        // 优先思源黑体（NotoSansSC，需自行放入 rawfile），退回随包 Roboto（无中文）。
        // 鸿蒙：优先用**设备自带**的中文字体（免随包分发，也没有授权问题）；
        // 其次找随包的思源黑体；最后退回 Roboto（无中文，中文会显示成问号）。
        static const String s_fontCjk   = vfs::Resolve("font/NotoSansSC-Regular.otf");
        static const String s_fontLatin = vfs::Resolve("font/Roboto-Regular.ttf");
        const char* fontCandidates[] = {
            "/system/fonts/HarmonyOS_Sans_SC.ttf",   // 设备系统字体（HarmonyOS Sans 简体中文）
            s_fontCjk.c_str(),
            s_fontLatin.c_str(),
        };
#endif
        ImFont* cjkFont = nullptr;
        // 【图集大小】高 DPI 下字号被放大（手机上 ~52px），若仍按"全 CJK 区段"（2 万字形）
        // 烘焙，图集会超过纹理上限 ⇒ 字体纹理创建失败、ImGui 整块不绘制（真机实测）。
        // 放大时改用 ImGui 内置的**常用汉字表**（约 2500 字 + 标点），图集装得下。
        const ImWchar* glyphRanges = chineseRanges;
#if HE_PLATFORM_OHOS
        // 常用字表（约 2500 字）不含"渲/栅"等界面用字，单独把它们补进去：
        // 用 GlyphRangesBuilder 拼一份"常用字 + 本工程面板文案用字"的区间表。
        static ImVector<ImWchar> s_ohosRanges;
        if (uiScale > 1.5f) {
            if (s_ohosRanges.empty()) {
                ImFontGlyphRangesBuilder builder;
                builder.AddRanges(io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
                builder.AddText("渲染后端光栅化当前模式转速停转立方体球体相机阴影天空盒"
                                "可调垂直同步分辨率帧率三角形支持不可用关闭开启");
                builder.BuildRanges(&s_ohosRanges);
            }
            glyphRanges = s_ohosRanges.Data;
        }
#endif
        for (const char* fontPath : fontCandidates) {
            cjkFont = io.Fonts->AddFontFromFileTTF(fontPath, 16.0f * uiScale, &fontConfig, glyphRanges);
            if (cjkFont) {
                io.FontDefault = cjkFont;
                HE_CORE_INFO("ImGui 中文字体已加载: {}（字号 {}，图集 {}x{}）", fontPath, 16.0f * uiScale,
                             io.Fonts->TexWidth, io.Fonts->TexHeight);
                break;
            }
        }
        if (!cjkFont) {
            HE_CORE_WARN("ImGui 中文字体加载失败，中文将无法正常显示（鸿蒙请把字体放到 rawfile/content/font/）");
        }
    }

#if HE_PLATFORM_OHOS
    ImGui_ImplOhos_Init(window);          // 鸿蒙：触摸后端（Vulkan 渲染后端两端共用）
#else
    if (m_IsD3D12 || m_IsD3D11) ImGui_ImplGlfw_InitForOther(glfwWindow, true);
    else           ImGui_ImplGlfw_InitForVulkan(glfwWindow, true);
#endif
    CreateImGuiResources(device, swapchain);

    m_Initialized = true;
    HE_CORE_INFO("ImGuiIntegration initialized");
}

void ImGuiIntegration::CreateImGuiResources(rhi::IRHIDevice* device,
                                             rhi::IRHISwapChain* swapchain) {
#if defined(HE_RHI_D3D12)
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
#if defined(HE_RHI_D3D11)
    if (m_IsD3D11) {
        // D3D11 没有描述符堆 / render pass：ImGui 直接用设备与 immediate context，字体纹理由后端自建
        auto* d3d = static_cast<rhi::d3d11::D3D11Device*>(device);
        ImGui_ImplDX11_Init(d3d->GetDevice(), d3d->GetContext());
        HE_CORE_INFO("ImGui D3D11 backend initialized");
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

    // 【A2】把"画 UI"这件事注册成后端的叠加记录器：直接录制时当场执行（与改造前等价），
    // 命令流模式下则由影子列表回放时调用 —— 那时命令缓冲才真正处在 render pass 内部。
    device->SetOverlayRecorder(&ImGuiIntegration::RecordVulkanOverlay);

    HE_CORE_INFO("ImGui Vulkan backend initialized");
}

void ImGuiIntegration::RecordVulkanOverlay(void* nativeCmdBuffer) {
    // 静态回调：ImGui 的 draw data 是全局的，Vulkan 后端把它编码进给定命令缓冲。
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(),
                                    static_cast<VkCommandBuffer>(nativeCmdBuffer));
}

void ImGuiIntegration::DestroyImGuiResources(rhi::IRHIDevice* device) {
    if (device && !m_IsD3D12 && !m_IsD3D11) {
        device->DestroyImGuiRenderPass(m_RenderPass);
        device->DestroyImGuiDescriptorPool(m_DescPool);
    }
    m_RenderPass = nullptr;
    m_DescPool   = nullptr;
}

void ImGuiIntegration::Shutdown() {
    if (!m_Initialized) return;
    m_Initialized = false;
#if defined(HE_RHI_D3D12)
    if (m_IsD3D12) {
        if (m_Device) m_Device->WaitIdle();   // ImGui 的顶点/索引缓冲可能仍被在飞帧使用
        ImGui_ImplDX12_Shutdown();
    } else
#endif
#if defined(HE_RHI_D3D11)
    if (m_IsD3D11) {
        ImGui_ImplDX11_Shutdown();
    } else
#endif
    ImGui_ImplVulkan_Shutdown();
#if HE_PLATFORM_OHOS
    ImGui_ImplOhos_Shutdown();
#else
    ImGui_ImplGlfw_Shutdown();
#endif
    ImGui::DestroyContext(m_Context);
    m_Context = nullptr;
    m_Window  = nullptr;
    DestroyImGuiResources(m_Device);
    m_Device  = nullptr;
    HE_CORE_INFO("ImGuiIntegration shut down");
}

void ImGuiIntegration::BeginFrame() {
#if defined(HE_RHI_D3D12)
    if (m_IsD3D12) ImGui_ImplDX12_NewFrame();
    else
#endif
#if defined(HE_RHI_D3D11)
    if (m_IsD3D11) ImGui_ImplDX11_NewFrame();
    else
#endif
    ImGui_ImplVulkan_NewFrame();
#if HE_PLATFORM_OHOS
    ImGui_ImplOhos_NewFrame(m_Window);
#else
    ImGui_ImplGlfw_NewFrame();
#endif
    ImGui::NewFrame();
}

void ImGuiIntegration::EndFrame(rhi::IRHICommandList* cmd) {
    ImGui::Render();
#if defined(HE_RHI_D3D12)
    if (m_IsD3D12) {
        // 返回原生列表前已绑定引擎的描述符堆（含 ImGui 字体 SRV 槽位）
        auto* list = static_cast<ID3D12GraphicsCommandList*>(m_Device->GetImGuiCommandBuffer(cmd));
        if (list) ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), list);
        return;
    }
#endif
#if defined(HE_RHI_D3D11)
    if (m_IsD3D11) {
        // 先让上下文装上该命令列表的渲染目标与视口，ImGui 画在当前绑定的目标上（结束后它会恢复上下文状态）。
        // 返回的是命令列表录制所用的上下文：模式 2 下是 deferred context，ImGui 要画进同一个上下文
        if (auto* ctx = static_cast<ID3D11DeviceContext*>(m_Device->GetImGuiCommandBuffer(cmd))) {
            ImGui_ImplDX11_SetDeviceContext(ctx);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        }
        return;
    }
#endif
    // 通过 RHI 接口获取 VkCommandBuffer，无需 static_cast<VulkanCommandList*>
    // 【A2】不能直接拿原生句柄画：命令流模式下源列表没有已开始录制的命令缓冲，
    // 必须走 RHI 的叠加入口 —— 直接录制时同步绘制，流模式下转成一条回放命令。
    cmd->DrawOverlay();
}

} // namespace he::editor
