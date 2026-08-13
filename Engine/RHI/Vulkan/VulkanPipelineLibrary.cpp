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
    // 描述符集布局哈希：预光栅化/片元着色器/片元输出库都持有 pipeline layout，
    // 同一着色器/状态但不同布局的 PSO 必须分配不同段库，否则 link 时 layout 不兼容
    auto hashDescLayouts = [&](uint64_t hh) -> uint64_t {
        for (auto& dsl : desc.descriptorSetLayouts)
            hh = FnvHashU32(hh, static_cast<u32>(dsl));
        return hh;
    };
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
        h = hashDescLayouts(h);
        break;
    case PipelinePartKind::FragmentShader:
        h = FnvHashShader(h, desc.pixelShader);
        // FS 库内嵌 pDepthStencil（非动态深度下 VUID-09035 要求），
        // 深度状态差异也须纳入哈希，否则同 FS 但不同深度的 PSO 会碰撞复用错误的 FS 库
        h = FnvHashU32(h, desc.depthTest ? 1u : 0u);
        h = FnvHashU32(h, desc.depthWrite ? 1u : 0u);
        h = FnvHashU32(h, static_cast<u32>(desc.depthCompare));
        for (auto& pc : desc.pushConstantRanges) {
            h = FnvHashU32(h, pc.stageMask);
            h = FnvHashU32(h, pc.offset);
            h = FnvHashU32(h, pc.size);
        }
        h = hashDescLayouts(h);
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
        h = hashDescLayouts(h);
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
    // 顶点输入接口库 = 顶点输入状态 + 输入装配状态（GPL 规范两者都属于顶点输入接口）
    VkPipeline lib = CreateLibrarySegment(
        m_Device, m_Cache, VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT, p,
        nullptr, 0, &p.vertexInput, &p.inputAssembly, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
        VK_NULL_HANDLE, VK_NULL_HANDLE, 0);
    if (lib != VK_NULL_HANDLE) m_VertexInputLibs[hash] = lib;
    return lib;
}

VkPipeline PipelineLibraryCache::GetOrCreatePreRasterLibrary(u64 hash, const GraphicsPipelineParts& p) {
    auto it = m_PreRasterLibs.find(hash);
    if (it != m_PreRasterLibs.end()) return it->second;
    VkPipelineShaderStageCreateInfo stage = p.vsStage;
    // 预光栅化库 = VS 阶段 + 视口/光栅化/深度模板/动态状态 + layout（输入装配属于顶点输入接口库）
    VkPipeline lib = CreateLibrarySegment(
        m_Device, m_Cache, VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT, p,
        &stage, 1, nullptr, nullptr, &p.viewportState, &p.rasterizer,
        nullptr, &p.depthStencil, nullptr, &p.dynState,
        p.layout, VK_NULL_HANDLE, 0);
    if (lib != VK_NULL_HANDLE) m_PreRasterLibs[hash] = lib;
    return lib;
}

VkPipeline PipelineLibraryCache::GetOrCreateFragmentShaderLibrary(u64 hash, const GraphicsPipelineParts& p) {
    auto it = m_FragmentShaderLibs.find(hash);
    if (it != m_FragmentShaderLibs.end()) return it->second;
    VkPipelineShaderStageCreateInfo stage = p.fsStage;
    // 片元着色器库 = FS 阶段 + 深度模板 + layout（深度/模板影响片元阶段的 early-z/深度写入，
    // 非动态深度状态下需在此提供，否则触发 renderPass-09035 验证错误）
    VkPipeline lib = CreateLibrarySegment(
        m_Device, m_Cache, VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT, p,
        &stage, 1, nullptr, nullptr, nullptr, nullptr, nullptr, &p.depthStencil, nullptr, nullptr,
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
    // 链接阶段只需 VkPipelineLibraryCreateInfoKHR 列出库段；VkGraphicsPipelineLibraryCreateInfoEXT
    // 仅用于库段创建（指定 flags），链接时不得提供（否则触发 flags-requiredbitmask 验证错误）
    VkPipelineLibraryCreateInfoKHR linkInfo{};
    linkInfo.sType        = VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR;
    linkInfo.pNext        = nullptr;
    linkInfo.libraryCount = 4;
    linkInfo.pLibraries   = libs;

    VkGraphicsPipelineCreateInfo ci{};
    ci.sType     = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    ci.pNext     = &linkInfo;
    ci.flags     = linkTimeOptimize ? VK_PIPELINE_CREATE_LINK_TIME_OPTIMIZATION_BIT_EXT : 0;
    ci.layout    = p.layout;
    ci.renderPass = p.renderPass;
    ci.subpass   = p.subpass;
    ci.pDepthStencilState = &p.depthStencil;  // 链接阶段需提供深度模板（非动态深度 + depth attachment）

    VkPipeline pipeline = VK_NULL_HANDLE;
    VkResult vr = vkCreateGraphicsPipelines(m_Device, m_Cache, 1, &ci, nullptr, &pipeline);
    if (vr != VK_SUCCESS) {
        HE_CORE_WARN("PipelineLibraryCache: fast-link 失败 (result={})", static_cast<i32>(vr));
    }
    return pipeline;
}

} // namespace he::rhi
