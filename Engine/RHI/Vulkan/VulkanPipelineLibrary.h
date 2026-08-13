#pragma once

// ============================================================
// VulkanPipelineLibrary.h — Graphics Pipeline Library (GPL)
// 将 Graphics PSO 拆分为 4 段独立库并分别缓存：变体场景下
// 只重编变化段，其余段复用，fast-link 组合完整 PSO
// （~0.5ms vs 单片 50ms）。依赖 VK_EXT_graphics_pipeline_library。
// ============================================================

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include "RHI/Shader.h"

#include <array>
#include <unordered_map>
#include <vector>

namespace he::rhi {

class DeferredDestructionQueue;

// 四段库类型
enum class PipelinePartKind {
    VertexInput,
    PreRaster,
    FragmentShader,
    FragmentOutput,
};

// 分解后的图形管线状态（传统 VS+FS 路径），单片与 GPL 共享
struct GraphicsPipelineParts {
    // 由 create info 指针指向的持久数组（保持指针有效到 vkCreate 完成）
    std::array<VkAttachmentDescription, kMaxColorAttachments + 1> attachments{};
    std::array<VkAttachmentReference,   kMaxColorAttachments>     colorRefs{};
    std::array<VkPipelineColorBlendAttachmentState, kMaxColorAttachments> blendStates{};
    std::vector<VkVertexInputAttributeDescription> attrs;
    VkVertexInputBindingDescription binding{};
    std::vector<VkPushConstantRange> pushRanges;

    VkPipelineVertexInputStateCreateInfo   vertexInput{};
    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    VkPipelineViewportStateCreateInfo      viewportState{};
    VkPipelineRasterizationStateCreateInfo rasterizer{};
    VkPipelineMultisampleStateCreateInfo   multisample{};
    VkPipelineDepthStencilStateCreateInfo  depthStencil{};
    VkPipelineColorBlendStateCreateInfo    colorBlend{};
    VkPipelineDynamicStateCreateInfo       dynState{};
    VkPipelineShaderStageCreateInfo        vsStage{};
    VkPipelineShaderStageCreateInfo        fsStage{};

    VkPipelineLayout layout     = VK_NULL_HANDLE;
    VkRenderPass     renderPass = VK_NULL_HANDLE;
    u32              subpass    = 0;

    // 临时着色器模块（路径末端由调用方销毁）
    VkShaderModule vs = VK_NULL_HANDLE;
    VkShaderModule fs = VK_NULL_HANDLE;
};

// 计算单段库的哈希（仅该段相关字段）
uint64_t HashPipelinePart(const PipelineStateDesc& desc, PipelinePartKind kind);

// 四段库缓存 + fast-link
class PipelineLibraryCache {
public:
    void Initialize(VkDevice device, VkPipelineCache cache, DeferredDestructionQueue* ddq);
    void Shutdown();

    // 四段：查/建（缺失时用 VK_PIPELINE_CREATE_LIBRARY_BIT_KHR 创建并缓存）
    VkPipeline GetOrCreateVertexInputLibrary(u64 hash, const GraphicsPipelineParts& p);
    VkPipeline GetOrCreatePreRasterLibrary(u64 hash, const GraphicsPipelineParts& p);
    VkPipeline GetOrCreateFragmentShaderLibrary(u64 hash, const GraphicsPipelineParts& p);
    VkPipeline GetOrCreateFragmentOutputLibrary(u64 hash, const GraphicsPipelineParts& p);

    // fast-link：组合 4 段产出完整 VkPipeline
    // @param linkTimeOptimize  fast-linking 不可用时置 true（带 LTO bit）
    VkPipeline LinkPipeline(const VkPipeline libs[4], const GraphicsPipelineParts& p,
                            bool linkTimeOptimize);

private:
    VkDevice                m_Device = VK_NULL_HANDLE;
    VkPipelineCache         m_Cache  = VK_NULL_HANDLE;
    DeferredDestructionQueue* m_DDQ  = nullptr;

    std::unordered_map<u64, VkPipeline> m_VertexInputLibs;
    std::unordered_map<u64, VkPipeline> m_PreRasterLibs;
    std::unordered_map<u64, VkPipeline> m_FragmentShaderLibs;
    std::unordered_map<u64, VkPipeline> m_FragmentOutputLibs;
};

} // namespace he::rhi
