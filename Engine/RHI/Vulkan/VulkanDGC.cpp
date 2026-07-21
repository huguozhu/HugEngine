// RHI/Vulkan/VulkanDGC.cpp — VK_EXT_device_generated_commands 实现
#include "VulkanDGC.h"
#include "Core/Log.h"
#include "Core/Assert.h"

#include <vector>
#include <cstring>

namespace he::rhi {

// ============================================================
// 静态：查询物理设备是否支持 DGC 扩展
// ============================================================
bool VulkanDGC::IsSupported(VkPhysicalDevice pd) {
    u32 extCount = 0;
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> exts(extCount);
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &extCount, exts.data());

    for (auto& ext : exts) {
        if (strcmp(ext.extensionName, VK_EXT_DEVICE_GENERATED_COMMANDS_EXTENSION_NAME) == 0) {
            return true;
        }
    }
    return false;
}

// ============================================================
// Initialize — 创建 DGC Layout + Execution Set + Preprocess Buffer
// ============================================================
bool VulkanDGC::Initialize(VkDevice device, VkPhysicalDevice physical,
                           VkPipeline pipeline,
                           u32 maxSequences, u32 maxDraws,
                           const VulkanDGCFuncs& funcs) {
    if (m_Initialized) {
        HE_CORE_WARN("VulkanDGC: 重复初始化，跳过");
        return true;
    }

    m_Funcs        = &funcs;
    m_MaxSequences = maxSequences;
    m_MaxDraws     = maxDraws;

    // 1. 创建 IndirectCommandsLayout（单 DRAW_INDEXED 令牌）
    VkIndirectCommandsLayoutTokenEXT token{};
    token.sType = VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_TOKEN_EXT;
    token.type  = VK_INDIRECT_COMMANDS_TOKEN_TYPE_DRAW_INDEXED_EXT;
    token.offset = 0;

    VkIndirectCommandsLayoutCreateInfoEXT layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_CREATE_INFO_EXT;
    layoutInfo.flags = 0;
    // 声明此 DGC 布局使用的着色器阶段
    layoutInfo.shaderStages   = VK_SHADER_STAGE_VERTEX_BIT
                                | VK_SHADER_STAGE_FRAGMENT_BIT;
    // 每条序列步长 = VkDrawIndexedIndirectCommand（5×u32 = 20 字节）
    layoutInfo.indirectStride = kDGCDrawIndexedIndirectStride;  // VkDrawIndexedIndirectCommand
    layoutInfo.pipelineLayout = VK_NULL_HANDLE;
    layoutInfo.tokenCount     = 1;
    layoutInfo.pTokens        = &token;

    VkResult result = funcs.vkCreateIndirectCommandsLayoutEXT(
        device, &layoutInfo, nullptr, &m_Layout);
    if (result != VK_SUCCESS) {
        HE_CORE_ERROR("VulkanDGC: vkCreateIndirectCommandsLayoutEXT 失败 ({})", (int)result);
        return false;
    }
    HE_CORE_INFO("VulkanDGC: IndirectCommandsLayout 创建成功 (maxSeq={}, maxDraws={})",
                 maxSequences, maxDraws);

    // 2. 创建 IndirectExecutionSet（关联 GBuffer 管线）
    VkIndirectExecutionSetPipelineInfoEXT pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_INDIRECT_EXECUTION_SET_PIPELINE_INFO_EXT;
    pipelineInfo.initialPipeline  = pipeline;
    pipelineInfo.maxPipelineCount = 1;

    VkIndirectExecutionSetInfoEXT execSetInfo{};
    execSetInfo.pPipelineInfo = &pipelineInfo;

    VkIndirectExecutionSetCreateInfoEXT execSetCI{};
    execSetCI.sType = VK_STRUCTURE_TYPE_INDIRECT_EXECUTION_SET_CREATE_INFO_EXT;
    execSetCI.type  = VK_INDIRECT_EXECUTION_SET_INFO_TYPE_PIPELINES_EXT;
    execSetCI.info  = execSetInfo;

    result = funcs.vkCreateIndirectExecutionSetEXT(
        device, &execSetCI, nullptr, &m_ExecutionSet);
    if (result != VK_SUCCESS) {
        HE_CORE_ERROR("VulkanDGC: vkCreateIndirectExecutionSetEXT 失败 ({})", (int)result);
        Shutdown(device);
        return false;
    }
    HE_CORE_INFO("VulkanDGC: IndirectExecutionSet 创建成功");

    // 3. 查询预处理缓冲大小
    VkGeneratedCommandsMemoryRequirementsInfoEXT memReq{};
    memReq.sType = VK_STRUCTURE_TYPE_GENERATED_COMMANDS_MEMORY_REQUIREMENTS_INFO_EXT;
    memReq.indirectExecutionSet   = m_ExecutionSet;
    memReq.indirectCommandsLayout = m_Layout;
    memReq.maxSequenceCount       = maxSequences;
    memReq.maxDrawCount           = maxDraws;

    VkMemoryRequirements2 memReq2 = {};
    memReq2.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;

    funcs.vkGetGeneratedCommandsMemoryRequirementsEXT(device, &memReq, &memReq2);

    m_PreprocessSize = memReq2.memoryRequirements.size;

    if (m_PreprocessSize == 0) {
        HE_CORE_WARN("VulkanDGC: 预处理缓冲大小为 0，驱动可能不需要预处理");
        m_Initialized = true;
        return true;
    }

    // 4. 查找设备本地内存类型
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(physical, &memProps);

    u32 memoryTypeIndex = ~0u;
    VkMemoryPropertyFlags desiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    VkMemoryRequirements reqs = memReq2.memoryRequirements;

    for (u32 i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((reqs.memoryTypeBits & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & desiredFlags) == desiredFlags) {
            memoryTypeIndex = i;
            break;
        }
    }
    if (memoryTypeIndex == ~0u) {
        HE_CORE_ERROR("VulkanDGC: 找不到合适的设备本地内存类型");
        Shutdown(device);
        return false;
    }

    // 5. 创建预处理缓冲
    VkBufferCreateInfo bufInfo{};
    bufInfo.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufInfo.size        = m_PreprocessSize;
    bufInfo.usage       = VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT
                          | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    result = vkCreateBuffer(device, &bufInfo, nullptr, &m_PreprocessBuffer);
    if (result != VK_SUCCESS) {
        HE_CORE_ERROR("VulkanDGC: 创建预处理缓冲失败 ({})", (int)result);
        Shutdown(device);
        return false;
    }

    // 6. 分配内存（设备本地 + 设备地址）
    VkMemoryAllocateFlagsInfo flagsInfo{};
    flagsInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
    flagsInfo.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.pNext           = &flagsInfo;
    allocInfo.allocationSize  = m_PreprocessSize;
    allocInfo.memoryTypeIndex = memoryTypeIndex;

    result = vkAllocateMemory(device, &allocInfo, nullptr, &m_PreprocessMemory);
    if (result != VK_SUCCESS) {
        HE_CORE_ERROR("VulkanDGC: 分配预处理内存失败 ({})", (int)result);
        Shutdown(device);
        return false;
    }

    vkBindBufferMemory(device, m_PreprocessBuffer, m_PreprocessMemory, 0);

    // 7. 查询设备地址
    VkBufferDeviceAddressInfo addrInfo{};
    addrInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    addrInfo.buffer = m_PreprocessBuffer;
    m_PreprocessAddress = vkGetBufferDeviceAddress(device, &addrInfo);

    m_Initialized = true;
    HE_CORE_INFO("VulkanDGC: 初始化完成 (preprocess size={}, addr={:#x})",
                 m_PreprocessSize, (u64)m_PreprocessAddress);
    return true;
}

// ============================================================
// Shutdown — 销毁所有 DGC 对象
// ============================================================
void VulkanDGC::Shutdown(VkDevice device) {
    if (m_PreprocessBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, m_PreprocessBuffer, nullptr);
        m_PreprocessBuffer = VK_NULL_HANDLE;
    }
    if (m_PreprocessMemory != VK_NULL_HANDLE) {
        vkFreeMemory(device, m_PreprocessMemory, nullptr);
        m_PreprocessMemory = VK_NULL_HANDLE;
    }
    if (m_ExecutionSet != VK_NULL_HANDLE && m_Funcs) {
        m_Funcs->vkDestroyIndirectExecutionSetEXT(device, m_ExecutionSet, nullptr);
        m_ExecutionSet = VK_NULL_HANDLE;
    }
    if (m_Layout != VK_NULL_HANDLE && m_Funcs) {
        m_Funcs->vkDestroyIndirectCommandsLayoutEXT(device, m_Layout, nullptr);
        m_Layout = VK_NULL_HANDLE;
    }

    m_PreprocessAddress  = 0;
    m_PreprocessSize     = 0;
    m_MaxSequences = 0;
    m_MaxDraws     = 0;
    m_Initialized  = false;
    m_Funcs        = nullptr;

    HE_CORE_INFO("VulkanDGC: 已关闭");
}

} // namespace he::rhi
