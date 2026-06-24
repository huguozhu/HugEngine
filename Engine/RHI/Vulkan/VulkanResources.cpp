#include "RHI/RHI.h"
#include "Core/Log.h"
#include "Core/Assert.h"

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <cstring>

namespace he::rhi {

// ============================================================
// Vulkan Buffer (plain allocation, no VMA)
// ============================================================
class VulkanBuffer final : public IRHIBuffer {
public:
    VulkanBuffer(VkDevice device, VkPhysicalDevice physical, const BufferDesc& desc);
    ~VulkanBuffer() override;

    usize GetSize()  const override { return m_Size; }
    void* Map()            override;
    void  Unmap()          override;
    u64   GetDeviceAddress() const override { return m_DeviceAddress; }

    VkBuffer GetHandle() const { return m_Buffer; }

private:
    VkDevice          m_Device        = VK_NULL_HANDLE;
    VkBuffer          m_Buffer        = VK_NULL_HANDLE;
    VkDeviceMemory    m_Memory        = VK_NULL_HANDLE;
    usize             m_Size          = 0;
    u64               m_DeviceAddress = 0;
    bool              m_IsMapped      = false;
    void*             m_MappedPtr     = nullptr;
};

static u32 FindMemoryType(VkPhysicalDevice physical, u32 typeFilter, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(physical, &memProps);
    for (u32 i = 0; i < memProps.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && (memProps.memoryTypes[i].propertyFlags & props) == props)
            return i;
    }
    HE_ASSERT(false, "No suitable memory type found");
    return 0;
}

VulkanBuffer::VulkanBuffer(VkDevice device, VkPhysicalDevice physical, const BufferDesc& desc)
    : m_Device(device), m_Size(desc.size)
{
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size  = desc.size;
    bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                       VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                       VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

    VkResult result = vkCreateBuffer(device, &bufferInfo, nullptr, &m_Buffer);
    HE_ASSERT(result == VK_SUCCESS, "Failed to create buffer");

    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(device, m_Buffer, &memReqs);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize  = memReqs.size;
    allocInfo.memoryTypeIndex = FindMemoryType(physical, memReqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    result = vkAllocateMemory(device, &allocInfo, nullptr, &m_Memory);
    HE_ASSERT(result == VK_SUCCESS, "Failed to allocate buffer memory");

    vkBindBufferMemory(device, m_Buffer, m_Memory, 0);

    // Device address
    VkBufferDeviceAddressInfo addrInfo{};
    addrInfo.sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    addrInfo.buffer = m_Buffer;
    m_DeviceAddress = vkGetBufferDeviceAddress(device, &addrInfo);

    // Upload initial data
    if (desc.initialData) {
        void* dst = Map();
        std::memcpy(dst, desc.initialData, desc.size);
        Unmap();
    }
}

VulkanBuffer::~VulkanBuffer() {
    vkDestroyBuffer(m_Device, m_Buffer, nullptr);
    vkFreeMemory(m_Device, m_Memory, nullptr);
}

void* VulkanBuffer::Map() {
    if (!m_IsMapped) {
        vkMapMemory(m_Device, m_Memory, 0, m_Size, 0, &m_MappedPtr);
        m_IsMapped = true;
    }
    return m_MappedPtr;
}

void VulkanBuffer::Unmap() {
    if (m_IsMapped) {
        vkUnmapMemory(m_Device, m_Memory);
        m_IsMapped = false;
        m_MappedPtr = nullptr;
    }
}

// ============================================================
// Vulkan Pipeline State + Render Pass
// ============================================================
class VulkanPipelineState final : public IRHIPipelineState {
public:
    VulkanPipelineState(VkDevice device, VkPipeline pipeline,
                        VkPipelineLayout layout, VkRenderPass renderPass)
        : m_Device(device), m_Pipeline(pipeline)
        , m_PipelineLayout(layout), m_RenderPass(renderPass) {}

    ~VulkanPipelineState() override;

    VkPipeline       GetPipeline()       const { return m_Pipeline; }
    VkPipelineLayout GetPipelineLayout() const { return m_PipelineLayout; }
    VkRenderPass     GetRenderPass()     const { return m_RenderPass; }

private:
    VkDevice         m_Device;
    VkPipeline       m_Pipeline;
    VkPipelineLayout m_PipelineLayout;
    VkRenderPass     m_RenderPass;
};

VulkanPipelineState::~VulkanPipelineState() {
    vkDestroyPipeline(m_Device, m_Pipeline, nullptr);
    vkDestroyPipelineLayout(m_Device, m_PipelineLayout, nullptr);
    vkDestroyRenderPass(m_Device, m_RenderPass, nullptr);
}

// ============================================================
// Factory functions called from VulkanDevice
// ============================================================

std::unique_ptr<IRHIBuffer> CreateVulkanBuffer(
    VkDevice device, VkPhysicalDevice physical, const BufferDesc& desc)
{
    return std::make_unique<VulkanBuffer>(device, physical, desc);
}

std::unique_ptr<IRHIPipelineState> CreateVulkanPipeline(
    VkDevice device, const PipelineStateDesc& desc)
{
    // 1. Create shader modules
    auto createShader = [&](const ShaderBytecode* bc, VkShaderStageFlagBits stage) -> VkShaderModule {
        if (!bc || bc->spirv.empty()) return VK_NULL_HANDLE;

        VkShaderModuleCreateInfo info{};
        info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        info.codeSize = bc->spirv.size() * sizeof(u32);
        info.pCode    = bc->spirv.data();

        VkShaderModule mod;
        vkCreateShaderModule(device, &info, nullptr, &mod);
        return mod;
    };

    VkShaderModule vert = createShader(desc.vertexShader,   VK_SHADER_STAGE_VERTEX_BIT);
    VkShaderModule frag = createShader(desc.pixelShader,    VK_SHADER_STAGE_FRAGMENT_BIT);

    // 2. Render pass (single color attachment)
    VkAttachmentDescription colorAttach{};
    colorAttach.format        = VK_FORMAT_B8G8R8A8_UNORM;
    colorAttach.samples       = VK_SAMPLE_COUNT_1_BIT;
    colorAttach.loadOp        = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttach.storeOp       = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttach.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttach.finalLayout   = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments    = &colorRef;

    VkSubpassDependency dep{};
    dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass    = 0;
    dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rpInfo{};
    rpInfo.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpInfo.attachmentCount = 1;
    rpInfo.pAttachments    = &colorAttach;
    rpInfo.subpassCount    = 1;
    rpInfo.pSubpasses      = &subpass;
    rpInfo.dependencyCount = 1;
    rpInfo.pDependencies   = &dep;

    VkRenderPass renderPass;
    vkCreateRenderPass(device, &rpInfo, nullptr, &renderPass);

    // 3. Vertex input
    VkVertexInputBindingDescription binding{};
    binding.binding   = 0;
    binding.stride    = desc.vertexLayout.stride > 0 ? desc.vertexLayout.stride : 8;
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attr{};
    attr.location = 0;
    attr.binding  = 0;
    attr.format   = VK_FORMAT_R32G32_SFLOAT;
    attr.offset   = 0;

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount   = 1;
    vertexInput.pVertexBindingDescriptions      = &binding;
    vertexInput.vertexAttributeDescriptionCount = 1;
    vertexInput.pVertexAttributeDescriptions    = &attr;

    // 4. Input assembly
    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    // 5. Dynamic viewport + scissor
    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode  = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blendAttach{};
    blendAttach.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo colorBlend{};
    colorBlend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments    = &blendAttach;

    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynState{};
    dynState.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynState.dynamicStateCount = 2;
    dynState.pDynamicStates    = dyn;

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    VkPipelineLayout pipelineLayout;
    vkCreatePipelineLayout(device, &layoutInfo, nullptr, &pipelineLayout);

    // 6. Shader stages
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName  = "main";

    // 7. Create pipeline
    VkGraphicsPipelineCreateInfo pipeInfo{};
    pipeInfo.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeInfo.stageCount          = 2;
    pipeInfo.pStages             = stages;
    pipeInfo.pVertexInputState   = &vertexInput;
    pipeInfo.pInputAssemblyState = &inputAssembly;
    pipeInfo.pViewportState      = &viewportState;
    pipeInfo.pRasterizationState = &rasterizer;
    pipeInfo.pMultisampleState   = &ms;
    pipeInfo.pColorBlendState    = &colorBlend;
    pipeInfo.pDynamicState       = &dynState;
    pipeInfo.layout              = pipelineLayout;
    pipeInfo.renderPass          = renderPass;
    pipeInfo.subpass             = 0;

    VkPipeline pipeline;
    vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeInfo, nullptr, &pipeline);

    vkDestroyShaderModule(device, vert, nullptr);
    vkDestroyShaderModule(device, frag, nullptr);

    HE_CORE_INFO("Vulkan pipeline created");
    return std::make_unique<VulkanPipelineState>(device, pipeline, pipelineLayout, renderPass);
}

} // namespace he::rhi
