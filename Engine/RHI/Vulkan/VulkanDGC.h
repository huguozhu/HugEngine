#pragma once

// ============================================================
// VulkanDGC.h — VK_EXT_device_generated_commands 封装
//
// 管理 DGC 管线对象：
//   - VkIndirectCommandsLayoutEXT：定义命令格式（DRAW_INDEXED 令牌）
//   - VkIndirectExecutionSetEXT：关联管线与执行序列
//   - Preprocess Buffer：驱动预处理所需的内存
//
// 用法：
//   1. 查询设备支持：VulkanDGC::IsSupported()
//   2. 创建 DGC 对象：Initialize(device, physicalDevice, pipeline, ...)
//   3. 执行：由 VulkanCommandList 调用 vkCmdExecuteGeneratedCommandsEXT
//   4. 销毁：Shutdown()
// ============================================================

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include "Core/Types.h"

namespace he::rhi {

// DGC 间接绘制命令步长：VkDrawIndexedIndirectCommand 结构体大小
// indexCount + instanceCount + firstIndex + vertexOffset + firstInstance = 5 × u32
constexpr u32 kDGCDrawIndexedIndirectStride = sizeof(u32) * 5;

// ============================================================
// VulkanDGCFuncs — DGC 扩展函数指针表
// VulkanDevice 在初始化时填充此表，供 VulkanDGC / VulkanCommandList 使用
// ============================================================
struct VulkanDGCFuncs {
    PFN_vkCreateIndirectCommandsLayoutEXT           vkCreateIndirectCommandsLayoutEXT           = nullptr;
    PFN_vkDestroyIndirectCommandsLayoutEXT          vkDestroyIndirectCommandsLayoutEXT          = nullptr;
    PFN_vkCreateIndirectExecutionSetEXT             vkCreateIndirectExecutionSetEXT             = nullptr;
    PFN_vkDestroyIndirectExecutionSetEXT            vkDestroyIndirectExecutionSetEXT            = nullptr;
    PFN_vkGetGeneratedCommandsMemoryRequirementsEXT vkGetGeneratedCommandsMemoryRequirementsEXT = nullptr;
    PFN_vkCmdExecuteGeneratedCommandsEXT            vkCmdExecuteGeneratedCommandsEXT            = nullptr;
    PFN_vkCmdPreprocessGeneratedCommandsEXT         vkCmdPreprocessGeneratedCommandsEXT         = nullptr;
};

// ============================================================
// VulkanDGC — Device Generated Commands 封装
// ============================================================
class VulkanDGC {
public:
    VulkanDGC() = default;
    ~VulkanDGC() { /* Shutdown() 应由调用方显式调用 */ }

    /// 查询物理设备是否支持 DGC 扩展
    static bool IsSupported(VkPhysicalDevice pd);

    /// 初始化 DGC 管线对象
    /// 必须在 GBuffer PSO 创建之后调用
    bool Initialize(VkDevice device, VkPhysicalDevice physical,
                    VkPipeline pipeline,
                    u32 maxSequences, u32 maxDraws,
                    const VulkanDGCFuncs& funcs);
    void Shutdown(VkDevice device);

    // 访问器
    VkIndirectCommandsLayoutEXT GetLayout()        const { return m_Layout; }
    VkIndirectExecutionSetEXT   GetExecutionSet()  const { return m_ExecutionSet; }
    VkDeviceAddress             GetPreprocessAddress() const { return m_PreprocessAddress; }
    VkDeviceSize                GetPreprocessSize() const { return m_PreprocessSize; }
    u32                         GetMaxSequences()  const { return m_MaxSequences; }
    const VulkanDGCFuncs*       GetFuncs()         const { return m_Funcs; }
    bool                        IsInitialized()    const { return m_Initialized; }

private:
    VkIndirectCommandsLayoutEXT m_Layout          = VK_NULL_HANDLE;
    VkIndirectExecutionSetEXT   m_ExecutionSet    = VK_NULL_HANDLE;

    VkBuffer         m_PreprocessBuffer  = VK_NULL_HANDLE;
    VkDeviceMemory   m_PreprocessMemory  = VK_NULL_HANDLE;
    VkDeviceAddress  m_PreprocessAddress = 0;
    VkDeviceSize     m_PreprocessSize    = 0;

    u32  m_MaxSequences = 0;
    u32  m_MaxDraws     = 0;
    bool m_Initialized  = false;

    // 持有 DGC 函数指针表的指针（VulkanDevice 保证生命周期覆盖）
    const VulkanDGCFuncs* m_Funcs = nullptr;
};

} // namespace he::rhi
