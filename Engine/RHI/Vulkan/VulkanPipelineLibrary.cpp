#include "VulkanPipelineLibrary.h"
#include "DeferredDestructionQueue.h"
#include "Core/Log.h"

namespace he::rhi {

// ============================================================
// FNV-1a 哈希辅助（与 VulkanPipeline.cpp 的 HashPipelineStateDesc 一致）
// ============================================================
namespace {
uint64_t FnvHashBytes(uint64_t h, const void* data, size_t size) {
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) { h ^= bytes[i]; h *= 0x100000001b3ULL; }
    return h;
}
uint64_t FnvHashU32(uint64_t h, u32 v) { return FnvHashBytes(h, &v, sizeof(v)); }
uint64_t FnvHashShader(uint64_t h, const ShaderBytecode* bc) {
    if (!bc) return h;
    return FnvHashBytes(h, bc->spirv.data(), bc->spirv.size() * sizeof(u32));
}
} // namespace

// 单段库哈希：仅纳入该段相关的 PipelineStateDesc 字段
uint64_t HashPipelinePart(const PipelineStateDesc& desc, PipelinePartKind kind) {
    uint64_t h = 0xcbf29ce484222325ULL;
    switch (kind) {
    case PipelinePartKind::VertexInput:
        h = FnvHashU32(h, desc.vertexLayout.stride);
        for (auto& a : desc.vertexLayout.attributes) {
            h = FnvHashU32(h, a.location);
            h = FnvHashU32(h, a.binding);
            h = FnvHashU32(h, static_cast<u32>(a.format));
            h = FnvHashU32(h, a.offset);
        }
        break;
    case PipelinePartKind::PreRaster:
        h = FnvHashShader(h, desc.vertexShader);
        h = FnvHashU32(h, static_cast<u32>(desc.topology));
        h = FnvHashU32(h, static_cast<u32>(desc.cullMode));
        h = FnvHashU32(h, static_cast<u32>(desc.frontFace));
        h = FnvHashU32(h, static_cast<u32>(desc.fillMode));
        h = FnvHashU32(h, desc.depthTest ? 1u : 0u);
        h = FnvHashU32(h, desc.depthWrite ? 1u : 0u);
        h = FnvHashU32(h, static_cast<u32>(desc.depthCompare));
        for (auto& pc : desc.pushConstantRanges) {
            h = FnvHashU32(h, pc.stageMask);
            h = FnvHashU32(h, pc.offset);
            h = FnvHashU32(h, pc.size);
        }
        break;
    case PipelinePartKind::FragmentShader:
        h = FnvHashShader(h, desc.pixelShader);
        for (auto& pc : desc.pushConstantRanges) {
            h = FnvHashU32(h, pc.stageMask);
            h = FnvHashU32(h, pc.offset);
            h = FnvHashU32(h, pc.size);
        }
        break;
    case PipelinePartKind::FragmentOutput:
        h = FnvHashU32(h, desc.colorAttachmentCount);
        for (u32 i = 0; i < desc.colorAttachmentCount; ++i)
            h = FnvHashU32(h, static_cast<u32>(desc.colorFormats[i]));
        h = FnvHashU32(h, static_cast<u32>(desc.depthFormat));
        for (u32 i = 0; i < desc.colorAttachmentCount; ++i) {
            const auto& cb = desc.colorBlend[i];
            h = FnvHashU32(h, cb.blendEnable ? 1u : 0u);
            h = FnvHashU32(h, static_cast<u32>(cb.srcColorBlendFactor));
            h = FnvHashU32(h, static_cast<u32>(cb.dstColorBlendFactor));
            h = FnvHashU32(h, static_cast<u32>(cb.colorBlendOp));
            h = FnvHashU32(h, static_cast<u32>(cb.writeMask));
        }
        h = FnvHashU32(h, desc.sampleCount);
        h = FnvHashU32(h, desc.subpassIndex);
        break;
    }
    return h;
}

// ============================================================
// PipelineLibraryCache 实现
// ============================================================
void PipelineLibraryCache::Initialize(VkDevice device, VkPipelineCache cache,
                                      DeferredDestructionQueue* ddq) {
    m_Device = device;
    m_Cache  = cache;
    m_DDQ    = ddq;
}

void PipelineLibraryCache::Shutdown() {
    auto destroyAll = [&](std::unordered_map<u64, VkPipeline>& map) {
        for (auto& [hash, lib] : map) {
            (void)hash;
            if (lib != VK_NULL_HANDLE && m_DDQ) {
                VkDevice dev = m_Device;
                m_DDQ->Enqueue([dev, lib]() { vkDestroyPipeline(dev, lib, nullptr); });
            }
        }
        map.clear();
    };
    destroyAll(m_VertexInputLibs);
    destroyAll(m_PreRasterLibs);
    destroyAll(m_FragmentShaderLibs);
    destroyAll(m_FragmentOutputLibs);
}

// 通用：构造带 LIBRARY_BIT 的部分管线 create info 并创建单个库段
static VkPipeline CreateLibrarySegment(
    VkDevice device, VkPipelineCache cache, VkGraphicsPipelineLibraryFlagsEXT flags,
    const GraphicsPipelineParts& p,
    VkPipelineShaderStageCreateInfo* stages, u32 stageCount,
    const VkPipelineVertexInputStateCreateInfo* vi,
    const VkPipelineInputAssemblyStateCreateInfo* ia,
    const VkPipelineViewportStateCreateInfo* vp,
    const VkPipelineRasterizationStateCreateInfo* rs,
    const VkPipelineMultisampleStateCreateInfo* ms,
    const VkPipelineDepthStencilStateCreateInfo* ds,
    const VkPipelineColorBlendStateCreateInfo* cb,
    const VkPipelineDynamicStateCreateInfo* dyn,
    VkPipelineLayout layout, VkRenderPass renderPass, u32 subpass) {

    VkGraphicsPipelineLibraryCreateInfoEXT libInfo{};
    libInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT;
    libInfo.flags = flags;

    VkGraphicsPipelineCreateInfo ci{};
    ci.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    ci.pNext               = &libInfo;
    ci.flags               = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR;
    ci.stageCount          = stageCount;
    ci.pStages             = stages;
    ci.pVertexInputState   = vi;
    ci.pInputAssemblyState = ia;
    ci.pViewportState      = vp;
    ci.pRasterizationState = rs;
    ci.pMultisampleState   = ms;
    ci.pDepthStencilState  = ds;
    ci.pColorBlendState    = cb;
    ci.pDynamicState       = dyn;
    ci.layout              = layout;
    ci.renderPass          = renderPass;
    ci.subpass             = subpass;

    VkPipeline lib = VK_NULL_HANDLE;
    VkResult vr = vkCreateGraphicsPipelines(device, cache, 1, &ci, nullptr, &lib);
    if (vr != VK_SUCCESS) {
        HE_CORE_WARN("PipelineLibraryCache: 库段创建失败 (flags=0x{:x}, result={})",
                     static_cast<u32>(flags), static_cast<i32>(vr));
    }
    return lib;
}

VkPipeline PipelineLibraryCache::GetOrCreateVertexInputLibrary(u64 hash, const GraphicsPipelineParts& p) {
    auto it = m_VertexInputLibs.find(hash);
    if (it != m_VertexInputLibs.end()) return it->second;
    VkPipeline lib = CreateLibrarySegment(
        m_Device, m_Cache, VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT, p,
        nullptr, 0, &p.vertexInput, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
        VK_NULL_HANDLE, VK_NULL_HANDLE, 0);
    if (lib != VK_NULL_HANDLE) m_VertexInputLibs[hash] = lib;
    return lib;
}

VkPipeline PipelineLibraryCache::GetOrCreatePreRasterLibrary(u64 hash, const GraphicsPipelineParts& p) {
    auto it = m_PreRasterLibs.find(hash);
    if (it != m_PreRasterLibs.end()) return it->second;
    VkPipelineShaderStageCreateInfo stage = p.vsStage;
    VkPipeline lib = CreateLibrarySegment(
        m_Device, m_Cache, VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT, p,
        &stage, 1, nullptr, &p.inputAssembly, &p.viewportState, &p.rasterizer,
        nullptr, &p.depthStencil, nullptr, &p.dynState,
        p.layout, VK_NULL_HANDLE, 0);
    if (lib != VK_NULL_HANDLE) m_PreRasterLibs[hash] = lib;
    return lib;
}

VkPipeline PipelineLibraryCache::GetOrCreateFragmentShaderLibrary(u64 hash, const GraphicsPipelineParts& p) {
    auto it = m_FragmentShaderLibs.find(hash);
    if (it != m_FragmentShaderLibs.end()) return it->second;
    VkPipelineShaderStageCreateInfo stage = p.fsStage;
    VkPipeline lib = CreateLibrarySegment(
        m_Device, m_Cache, VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT, p,
        &stage, 1, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
        p.layout, VK_NULL_HANDLE, 0);
    if (lib != VK_NULL_HANDLE) m_FragmentShaderLibs[hash] = lib;
    return lib;
}

VkPipeline PipelineLibraryCache::GetOrCreateFragmentOutputLibrary(u64 hash, const GraphicsPipelineParts& p) {
    auto it = m_FragmentOutputLibs.find(hash);
    if (it != m_FragmentOutputLibs.end()) return it->second;
    VkPipeline lib = CreateLibrarySegment(
        m_Device, m_Cache, VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT, p,
        nullptr, 0, nullptr, nullptr, nullptr, nullptr, &p.multisample, nullptr, &p.colorBlend,
        nullptr, p.layout, p.renderPass, p.subpass);
    if (lib != VK_NULL_HANDLE) m_FragmentOutputLibs[hash] = lib;
    return lib;
}

VkPipeline PipelineLibraryCache::LinkPipeline(const VkPipeline libs[4], const GraphicsPipelineParts& p,
                                              bool linkTimeOptimize) {
    VkGraphicsPipelineLibraryCreateInfoEXT libInfo{};
    libInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT;
    libInfo.flags = 0;  // 链接而非创建库段

    VkPipelineLibraryCreateInfoKHR linkInfo{};
    linkInfo.sType        = VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR;
    linkInfo.pNext        = &libInfo;
    linkInfo.libraryCount = 4;
    linkInfo.pLibraries   = libs;

    VkGraphicsPipelineCreateInfo ci{};
    ci.sType     = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    ci.pNext     = &linkInfo;
    ci.flags     = linkTimeOptimize ? VK_PIPELINE_CREATE_LINK_TIME_OPTIMIZATION_BIT_EXT : 0;
    ci.layout    = p.layout;
    ci.renderPass = p.renderPass;
    ci.subpass   = p.subpass;

    VkPipeline pipeline = VK_NULL_HANDLE;
    VkResult vr = vkCreateGraphicsPipelines(m_Device, m_Cache, 1, &ci, nullptr, &pipeline);
    if (vr != VK_SUCCESS) {
        HE_CORE_WARN("PipelineLibraryCache: fast-link 失败 (result={})", static_cast<i32>(vr));
    }
    return pipeline;
}

} // namespace he::rhi
