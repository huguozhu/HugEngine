// ============================================================
// ImGuiIntegration.cpp — ImGui v1.91 + GLFW + Vulkan
// ============================================================

#include "Editor/ImGuiIntegration.h"
#include "RHI/RHI.h"
#include "RHI/CommandList.h"
#include "Core/Log.h"

#include "VulkanInternal.h"
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_vulkan.h"

namespace he::editor {

ImGuiIntegration::ImGuiIntegration() = default;
ImGuiIntegration::~ImGuiIntegration() { Shutdown(); }

void ImGuiIntegration::Initialize(GLFWwindow* window, rhi::IRHIDevice* device,
                                   rhi::IRHISwapChain* swapchain) {
    m_Window = window;
    IMGUI_CHECKVERSION();
    m_Context = ImGui::CreateContext();
    ImGui::StyleColorsDark();

    ImGui_ImplGlfw_InitForVulkan(window, true);
    CreateVulkanResources(device, swapchain);

    m_Initialized = true;
    HE_CORE_INFO("ImGuiIntegration initialized");
}

void ImGuiIntegration::CreateVulkanResources(rhi::IRHIDevice* device,
                                              rhi::IRHISwapChain* swapchain) {
    auto* vkSC = static_cast<rhi::VulkanSwapChain*>(swapchain);

    VkInstance       instance       = rhi::VulkanDeviceAccess::GetInstance(device);
    VkPhysicalDevice physicalDevice = rhi::VulkanDeviceAccess::GetPhysical(device);
    VkDevice         vkDevice       = rhi::VulkanDeviceAccess::GetDevice(device);
    u32              graphicsFamily = rhi::VulkanDeviceAccess::GetGraphicsFamily(device);
    VkQueue          graphicsQueue  = rhi::VulkanDeviceAccess::GetGraphicsQueue(device);

    // Descriptor Pool
    VkDescriptorPoolSize poolSizes[] = {
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 },
    };
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets       = 2;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes    = poolSizes;
    vkCreateDescriptorPool(vkDevice, &poolInfo, nullptr,
                           reinterpret_cast<VkDescriptorPool*>(&m_DescPool));

    // RenderPass for ImGui
    VkAttachmentDescription colorAttach{};
    colorAttach.format         = vkSC->GetFormat();
    colorAttach.samples        = VK_SAMPLE_COUNT_1_BIT;
    colorAttach.loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;
    colorAttach.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttach.initialLayout  = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttach.finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments    = &colorRef;

    VkRenderPassCreateInfo rpInfo{};
    rpInfo.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpInfo.attachmentCount = 1;
    rpInfo.pAttachments    = &colorAttach;
    rpInfo.subpassCount    = 1;
    rpInfo.pSubpasses      = &subpass;

    VkRenderPass imguiRP;
    vkCreateRenderPass(vkDevice, &rpInfo, nullptr, &imguiRP);

    ImGui_ImplVulkan_InitInfo initInfo{};
    initInfo.Instance        = instance;
    initInfo.PhysicalDevice  = physicalDevice;
    initInfo.Device          = vkDevice;
    initInfo.QueueFamily     = graphicsFamily;
    initInfo.Queue           = graphicsQueue;
    initInfo.DescriptorPool  = reinterpret_cast<VkDescriptorPool>(m_DescPool);
    initInfo.MinImageCount   = 3;
    initInfo.ImageCount      = 3;
    initInfo.RenderPass     = imguiRP;
    initInfo.Subpass        = 0;
    initInfo.MSAASamples    = VK_SAMPLE_COUNT_1_BIT;

    ImGui_ImplVulkan_Init(&initInfo);
    vkDestroyRenderPass(vkDevice, imguiRP, nullptr);

    // Upload fonts
    ImGui_ImplVulkan_CreateFontsTexture();

    HE_CORE_INFO("ImGui Vulkan backend initialized");
}

void ImGuiIntegration::Shutdown() {
    if (!m_Initialized) return;
    m_Initialized = false;
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext(m_Context);
    m_Context = nullptr;
    m_Window  = nullptr;
    HE_CORE_INFO("ImGuiIntegration shut down");
}

void ImGuiIntegration::BeginFrame() {
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

void ImGuiIntegration::EndFrame(rhi::IRHICommandList* cmd) {
    ImGui::Render();
    auto* vkCmd = static_cast<rhi::VulkanCommandList*>(cmd);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), vkCmd->GetHandle());
}

} // namespace he::editor
