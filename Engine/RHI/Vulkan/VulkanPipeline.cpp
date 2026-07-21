// ============================================================
// VulkanPipeline.cpp — Vulkan 管线状态对象（PSO）创建
// 负责 Graphics/Compute Pipeline 构建、ShaderModule、PipelineLayout
// 集成 PSO 缓存：相同配置的管线仅创建一次，后续请求共享 Vulkan 对象
// ============================================================
#include "VulkanPipelineState.h"
#include "VulkanRT.h"  // ToVkFormat / ToVkCompareOp
#include "VulkanDevice.h"  // VulkanDevice::PSOCacheEntry / DeferredDestructionQueue
#include "DeferredDestructionQueue.h"
#include "Core/Log.h"
#include "Core/Assert.h"

#include <cstring>

namespace he::rhi {

// ============================================================
// VulkanPipelineState 析构
//   缓存模式：仅释放 shared_ptr 引用，Vulkan 对象由缓存管理
//   自拥有模式：直接销毁 Vulkan 对象
// ============================================================
VulkanPipelineState::~VulkanPipelineState() {
    if (m_CacheRef) {
        // 缓存模式：当最后一个引用释放时，将 Vulkan 对象入队延迟销毁
        if (m_CacheRef.use_count() == 2) {  // m_CacheRef + 缓存中的副本
            VkDevice dev = m_Device;
            VkPipeline pipeline = m_Pipeline;
            VkPipelineLayout layout = m_PipelineLayout;
            VkRenderPass rp = m_RenderPass;
            auto* queue = m_DeferredDestroy;
            // 捕获到 lambda 中，3 帧后安全销毁
            if (queue) {
                queue->Enqueue([dev, pipeline, layout, rp]() {
                    if (rp)     vkDestroyRenderPass(dev, rp, nullptr);
                    if (layout) vkDestroyPipelineLayout(dev, layout, nullptr);
                    if (pipeline) vkDestroyPipeline(dev, pipeline, nullptr);
                });
            }
        }
        // shared_ptr 自动释放
    } else {
        // 自拥有模式：立即销毁（调用方保证 GPU 已完成）
        if (m_RenderPass)     vkDestroyRenderPass(m_Device, m_RenderPass, nullptr);
        if (m_PipelineLayout) vkDestroyPipelineLayout(m_Device, m_PipelineLayout, nullptr);
        if (m_Pipeline)       vkDestroyPipeline(m_Device, m_Pipeline, nullptr);
    }
}

// ============================================================
// PSO 描述哈希计算 — 用于 PSO 缓存去重
// 对 PipelineStateDesc 的所有关键字段计算哈希，
// 包括 SPIR-V 二进制内容（非指针），确保相同配置返回相同哈希
// ============================================================
static uint64_t HashPipelineStateDesc(const PipelineStateDesc& desc) {
    // FNV-1a 64-bit 哈希辅助函数
    auto hashBytes = [](uint64_t hash, const void* data, size_t size) -> uint64_t {
        const uint8_t* bytes = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < size; ++i) {
            hash ^= bytes[i];
            hash *= 0x100000001b3ULL;  // FNV-1a prime
        }
        return hash;
    };
    auto hashU32 = [&hashBytes](uint64_t hash, u32 val) -> uint64_t {
        return hashBytes(hash, &val, sizeof(val));
    };

    uint64_t h = 0xcbf29ce484222325ULL;  // FNV-1a offset basis

    // 绑定点（Graphics / Compute）
    h = hashU32(h, static_cast<u32>(desc.bindPoint));

    // Shader SPIR-V 内容（字节级哈希，确保内容变更触发缓存失效）
    auto hashShader = [&](const ShaderBytecode* bc, uint64_t hh) -> uint64_t {
        if (bc) {
            hh = hashBytes(hh, bc->spirv.data(), bc->spirv.size() * sizeof(u32));
            hh = hashU32(hh, static_cast<u32>(bc->stage));
        }
        return hh;
    };
    h = hashShader(desc.vertexShader, h);
    h = hashShader(desc.pixelShader, h);
    h = hashShader(desc.computeShader, h);
    h = hashShader(desc.meshShader, h);
    h = hashShader(desc.amplificationShader, h);

    // 顶点输入布局
    h = hashU32(h, desc.vertexLayout.stride);
    for (auto& attr : desc.vertexLayout.attributes) {
        h = hashU32(h, attr.location);
        h = hashU32(h, attr.binding);
        h = hashU32(h, static_cast<u32>(attr.format));
        h = hashU32(h, attr.offset);
    }

    // 渲染状态
    h = hashU32(h, static_cast<u32>(desc.topology));
    h = hashU32(h, desc.depthTest ? 1u : 0u);
    h = hashU32(h, desc.depthWrite ? 1u : 0u);
    h = hashU32(h, static_cast<u32>(desc.depthCompare));

    // 渲染目标配置
    h = hashU32(h, desc.colorAttachmentCount);
    for (u32 i = 0; i < desc.colorAttachmentCount; ++i)
        h = hashU32(h, static_cast<u32>(desc.colorFormats[i]));
    h = hashU32(h, static_cast<u32>(desc.depthFormat));
    h = hashU32(h, static_cast<u32>(desc.colorLoadOp));
    h = hashU32(h, static_cast<u32>(desc.depthLoadOp));
    h = hashU32(h, desc.sampleCount);
    h = hashU32(h, desc.subpassIndex);

    // Push Constant 范围
    for (auto& pc : desc.pushConstantRanges) {
        h = hashU32(h, pc.stageMask);
        h = hashU32(h, pc.offset);
        h = hashU32(h, pc.size);
    }

    // Descriptor Set Layout 句柄（用于区分不同的布局绑定）
    for (auto& dsl : desc.descriptorSetLayouts) {
        h = hashU32(h, static_cast<u32>(dsl));
    }

    return h;
}

// ============================================================
// CreateVulkanPipeline — 工厂函数，由 VulkanDevice 调用
// 集成 PSO 缓存：相同 PipelineStateDesc → 复用底层 Vulkan 对象
// ============================================================
std::unique_ptr<IRHIPipelineState> CreateVulkanPipeline(
    VkDevice device, const PipelineStateDesc& desc,
    const std::vector<VkDescriptorSetLayout>& descLayouts,
    VulkanDevice* vulkanDevice)
{
    // ── PSO 缓存查找 ──
    if (vulkanDevice) {
        uint64_t descHash = HashPipelineStateDesc(desc);
        VkPipeline cachedPipeline = VK_NULL_HANDLE;
        VkPipelineLayout cachedLayout = VK_NULL_HANDLE;
        VkRenderPass cachedRP = VK_NULL_HANDLE;
        auto cachedRef = vulkanDevice->GetCachedPSORef(descHash,
            cachedPipeline, cachedLayout, cachedRP);
        if (cachedRef) {
            // 缓存命中：创建共享引用的 VulkanPipelineState（Vulkan 对象由缓存管理生命周期）
            HE_CORE_INFO("PSO cache hit: hash=0x{:016x}, pipeline={}", descHash,
                         reinterpret_cast<void*>(cachedPipeline));
            VkPipelineBindPoint bp = (desc.bindPoint == PipelineBindPoint::Compute)
                ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS;
            return std::make_unique<VulkanPipelineState>(
                device, cachedPipeline, cachedLayout, cachedRP, bp,
                cachedRef, &vulkanDevice->GetDeferredDestroy());
        }
    }

    // 0. 创建 ShaderModule 的辅助 lambda
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

    // Compute pipeline: 跳过 render pass 和图形管线创建
    if (desc.bindPoint == PipelineBindPoint::Compute) {
        VkShaderModule comp = createShader(desc.computeShader, VK_SHADER_STAGE_COMPUTE_BIT);
        HE_ASSERT(comp, "Compute pipeline requires a valid compute shader");

        // Pipeline layout
        std::vector<VkPushConstantRange> vkPushRanges;
        for (auto& pcRange : desc.pushConstantRanges) {
            VkPushConstantRange vkRange{};
            vkRange.stageFlags = pcRange.stageMask;
            vkRange.offset     = pcRange.offset;
            vkRange.size       = pcRange.size;
            vkPushRanges.push_back(vkRange);
        }
        if (vkPushRanges.empty()) {  // 自动补全避免验证层警告
            VkPushConstantRange r{}; r.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; r.size = kDefaultPushConstantSize;
            vkPushRanges.push_back(r);
        }
        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount         = static_cast<u32>(descLayouts.size());
        layoutInfo.pSetLayouts            = descLayouts.empty() ? nullptr : descLayouts.data();
        layoutInfo.pushConstantRangeCount = static_cast<u32>(vkPushRanges.size());
        layoutInfo.pPushConstantRanges    = vkPushRanges.empty() ? nullptr : vkPushRanges.data();
        VkPipelineLayout pipelineLayout;
        vkCreatePipelineLayout(device, &layoutInfo, nullptr, &pipelineLayout);

        VkComputePipelineCreateInfo compInfo{};
        compInfo.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        compInfo.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        compInfo.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
        compInfo.stage.module = comp;
        compInfo.stage.pName  = "main";
        compInfo.layout       = pipelineLayout;

        VkPipeline pipeline;
        vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &compInfo, nullptr, &pipeline);
        vkDestroyShaderModule(device, comp, nullptr);

        HE_CORE_INFO("Vulkan compute pipeline created");
        // ── 插入 PSO 缓存（首个引用也使用缓存模式，统一生命周期管理）──
        if (vulkanDevice) {
            uint64_t hash = HashPipelineStateDesc(desc);
            vulkanDevice->InsertPSOToCache(hash, pipeline, pipelineLayout, VK_NULL_HANDLE);
            VkPipeline cachedP = VK_NULL_HANDLE; VkPipelineLayout cachedL = VK_NULL_HANDLE;
            VkRenderPass cachedR = VK_NULL_HANDLE;
            auto ref = vulkanDevice->GetCachedPSORef(hash, cachedP, cachedL, cachedR);
            if (ref) {
                return std::make_unique<VulkanPipelineState>(device, pipeline, pipelineLayout,
                    VK_NULL_HANDLE, VK_PIPELINE_BIND_POINT_COMPUTE,
                    ref, &vulkanDevice->GetDeferredDestroy());
            }
        }
        return std::make_unique<VulkanPipelineState>(device, pipeline, pipelineLayout,
                                                      VK_NULL_HANDLE, VK_PIPELINE_BIND_POINT_COMPUTE);
    }

    // ── Mesh Shader Pipeline（无顶点着色器、无 IA、无顶点输入）──
    if (desc.meshShader && !desc.meshShader->spirv.empty()) {
        VkShaderModule mesh = createShader(desc.meshShader, VK_SHADER_STAGE_MESH_BIT_EXT);
        VkShaderModule task = VK_NULL_HANDLE;
        VkShaderModule frag = createShader(desc.pixelShader, VK_SHADER_STAGE_FRAGMENT_BIT);

        if (desc.amplificationShader && !desc.amplificationShader->spirv.empty())
            task = createShader(desc.amplificationShader, VK_SHADER_STAGE_TASK_BIT_EXT);

        // Render pass（与普通管线相同）
        bool hasColor = (desc.colorAttachmentCount > 0);
        bool hasDepth = (desc.depthFormat != Format::Unknown);

        VkAttachmentDescription colorAttachments[kMaxColorAttachments]{};
        VkAttachmentReference   colorRefs[kMaxColorAttachments]{};
        for (u32 c = 0; c < desc.colorAttachmentCount; ++c) {
            colorAttachments[c].format        = ToVkFormat(desc.colorFormats[c]);
            colorAttachments[c].samples       = VK_SAMPLE_COUNT_1_BIT;
            colorAttachments[c].loadOp        = ToVkLoadOp(desc.colorLoadOp);
            colorAttachments[c].storeOp       = VK_ATTACHMENT_STORE_OP_STORE;
            // LOAD 模式下 initialLayout 不能是 UNDEFINED（Vulkan 规范要求）
            colorAttachments[c].initialLayout = (desc.colorLoadOp == LoadOp::Load)
                ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                : VK_IMAGE_LAYOUT_UNDEFINED;
            colorAttachments[c].finalLayout   = (desc.colorFormats[c] == Format::BGRA8_UNORM ||
                                                 desc.colorFormats[c] == Format::BGRA8_SRGB)
                                                ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
                                                : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            colorRefs[c].attachment = c;
            colorRefs[c].layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }

        VkAttachmentDescription depthAttach{};
        depthAttach.format         = hasDepth ? ToVkFormat(desc.depthFormat) : VK_FORMAT_UNDEFINED;
        depthAttach.samples        = VK_SAMPLE_COUNT_1_BIT;
        depthAttach.loadOp         = ToVkLoadOp(desc.depthLoadOp);
        depthAttach.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        depthAttach.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depthAttach.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttach.initialLayout  = (desc.depthLoadOp == LoadOp::Load)
            ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
            : VK_IMAGE_LAYOUT_UNDEFINED;
        depthAttach.finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

        u32 depthAttachIdx = desc.colorAttachmentCount;
        VkAttachmentReference depthRef{depthAttachIdx, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount    = desc.colorAttachmentCount;
        subpass.pColorAttachments       = hasColor ? colorRefs : nullptr;
        subpass.pDepthStencilAttachment = hasDepth ? &depthRef : nullptr;

        VkAttachmentDescription attachments[kMaxColorAttachments + 1];
        u32 attachmentCount = 0;
        for (u32 c = 0; c < desc.colorAttachmentCount; ++c)
            attachments[attachmentCount++] = colorAttachments[c];
        if (hasDepth) attachments[attachmentCount++] = depthAttach;

        VkSubpassDependency dep{};
        dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
        dep.dstSubpass    = 0;
        dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dep.dstStageMask  = (hasColor ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT : 0u) |
                            (hasDepth ? VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT : 0u);
        dep.dstAccessMask = (hasColor ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT : 0u) |
                            (hasDepth ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : 0u);

        VkRenderPassCreateInfo rpInfo{};
        rpInfo.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpInfo.attachmentCount = attachmentCount;
        rpInfo.pAttachments    = attachments;
        rpInfo.subpassCount    = 1;
        rpInfo.pSubpasses      = &subpass;
        rpInfo.dependencyCount = (hasColor || hasDepth) ? 1u : 0u;
        rpInfo.pDependencies   = (hasColor || hasDepth) ? &dep : nullptr;

        VkRenderPass renderPass;
        vkCreateRenderPass(device, &rpInfo, nullptr, &renderPass);

        // 无 IA 阶段，无 Vertex Input（Mesh Shader 自行 fetch 顶点）
        VkPipelineVertexInputStateCreateInfo vertexInput{};
        vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertexInput.vertexBindingDescriptionCount   = 0;
        vertexInput.pVertexBindingDescriptions      = nullptr;
        vertexInput.vertexAttributeDescriptionCount = 0;
        vertexInput.pVertexAttributeDescriptions    = nullptr;

        // 无 Input Assembly（拓扑由 Mesh Shader 的 outputtopology 决定）
        VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
        inputAssembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo viewportState{};
        viewportState.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportState.viewportCount = 1;
        viewportState.scissorCount  = 1;

        VkPipelineRasterizationStateCreateInfo rasterizer{};
        rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterizer.lineWidth   = 1.0f;
        rasterizer.cullMode    = ToVkCullMode(desc.cullMode);
        rasterizer.frontFace   = ToVkFrontFace(desc.frontFace);
        rasterizer.polygonMode = ToVkFillMode(desc.fillMode);

        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo depthStencil{};
        depthStencil.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depthStencil.depthTestEnable  = desc.depthTest ? VK_TRUE : VK_FALSE;
        depthStencil.depthWriteEnable = desc.depthWrite ? VK_TRUE : VK_FALSE;
        depthStencil.depthCompareOp   = ToVkCompareOp(desc.depthCompare);

        VkPipelineColorBlendAttachmentState blendAttachments[kMaxColorAttachments]{};
        for (u32 c = 0; c < desc.colorAttachmentCount; ++c) {
            const auto& cb = desc.colorBlend[c];
            blendAttachments[c].blendEnable         = cb.blendEnable ? VK_TRUE : VK_FALSE;
            blendAttachments[c].srcColorBlendFactor = ToVkBlendFactor(cb.srcColorBlendFactor);
            blendAttachments[c].dstColorBlendFactor = ToVkBlendFactor(cb.dstColorBlendFactor);
            blendAttachments[c].colorBlendOp        = ToVkBlendOp(cb.colorBlendOp);
            blendAttachments[c].srcAlphaBlendFactor = ToVkBlendFactor(cb.srcAlphaBlendFactor);
            blendAttachments[c].dstAlphaBlendFactor = ToVkBlendFactor(cb.dstAlphaBlendFactor);
            blendAttachments[c].alphaBlendOp        = ToVkBlendOp(cb.alphaBlendOp);
            blendAttachments[c].colorWriteMask      = ToVkColorWriteMask(cb.writeMask);
        }

        VkPipelineColorBlendStateCreateInfo colorBlend{};
        colorBlend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlend.attachmentCount = desc.colorAttachmentCount;
        colorBlend.pAttachments    = blendAttachments;

        VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynState{};
        dynState.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynState.dynamicStateCount = 2;
        dynState.pDynamicStates    = dyn;

        // Push constants + layout
        std::vector<VkPushConstantRange> vkPushRanges;
        for (auto& pcRange : desc.pushConstantRanges) {
            VkPushConstantRange vkRange{};
            vkRange.stageFlags = pcRange.stageMask;
            vkRange.offset     = pcRange.offset;
            vkRange.size       = pcRange.size;
            vkPushRanges.push_back(vkRange);
        }
        if (vkPushRanges.empty()) {  // 自动补全避免验证层警告
            VkPushConstantRange r{};
            r.stageFlags = VK_SHADER_STAGE_MESH_BIT_EXT | VK_SHADER_STAGE_TASK_BIT_EXT
                         | VK_SHADER_STAGE_FRAGMENT_BIT;
            r.size = kDefaultPushConstantSize;  // Mesh Shader 回退默认值
            vkPushRanges.push_back(r);
        }

        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount         = static_cast<u32>(descLayouts.size());
        layoutInfo.pSetLayouts            = descLayouts.empty() ? nullptr : descLayouts.data();
        layoutInfo.pushConstantRangeCount = static_cast<u32>(vkPushRanges.size());
        layoutInfo.pPushConstantRanges    = vkPushRanges.empty() ? nullptr : vkPushRanges.data();
        VkPipelineLayout pipelineLayout;
        vkCreatePipelineLayout(device, &layoutInfo, nullptr, &pipelineLayout);

        // Shader stages: [Mesh] + [Task(可选)] + [Pixel(可选)]
        VkPipelineShaderStageCreateInfo meshStages[kMaxMeshShaderStages]{};
        u32 stageCount = 0;
        if (task) {
            meshStages[stageCount].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            meshStages[stageCount].stage  = VK_SHADER_STAGE_TASK_BIT_EXT;
            meshStages[stageCount].module = task;
            meshStages[stageCount].pName  = "main";
            stageCount++;
        }
        meshStages[stageCount].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        meshStages[stageCount].stage  = VK_SHADER_STAGE_MESH_BIT_EXT;
        meshStages[stageCount].module = mesh;
        meshStages[stageCount].pName  = "main";
        stageCount++;
        if (frag) {
            meshStages[stageCount].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            meshStages[stageCount].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
            meshStages[stageCount].module = frag;
            meshStages[stageCount].pName  = "main";
            stageCount++;
        }

        VkGraphicsPipelineCreateInfo pipeInfo{};
        pipeInfo.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipeInfo.stageCount          = stageCount;
        pipeInfo.pStages             = meshStages;
        pipeInfo.pVertexInputState   = &vertexInput;
        pipeInfo.pInputAssemblyState = &inputAssembly;
        pipeInfo.pViewportState      = &viewportState;
        pipeInfo.pRasterizationState = &rasterizer;
        pipeInfo.pMultisampleState   = &ms;
        pipeInfo.pDepthStencilState  = &depthStencil;
        pipeInfo.pColorBlendState    = &colorBlend;
        pipeInfo.pDynamicState       = &dynState;
        pipeInfo.layout              = pipelineLayout;
        pipeInfo.renderPass          = renderPass;
        pipeInfo.subpass             = 0;

        VkPipeline pipeline;
        vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeInfo, nullptr, &pipeline);

        vkDestroyShaderModule(device, mesh, nullptr);
        if (task) vkDestroyShaderModule(device, task, nullptr);
        if (frag) vkDestroyShaderModule(device, frag, nullptr);

        HE_CORE_INFO("Vulkan mesh shader pipeline created (task={}, mesh, frag={})",
                     task != VK_NULL_HANDLE, frag != VK_NULL_HANDLE);
        // ── 插入 PSO 缓存 ──
        if (vulkanDevice) {
            uint64_t hash = HashPipelineStateDesc(desc);
            vulkanDevice->InsertPSOToCache(hash, pipeline, pipelineLayout, renderPass);
            VkPipeline cachedP = VK_NULL_HANDLE; VkPipelineLayout cachedL = VK_NULL_HANDLE;
            VkRenderPass cachedR = VK_NULL_HANDLE;
            auto ref = vulkanDevice->GetCachedPSORef(hash, cachedP, cachedL, cachedR);
            if (ref) {
                return std::make_unique<VulkanPipelineState>(device, pipeline, pipelineLayout,
                    renderPass, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    ref, &vulkanDevice->GetDeferredDestroy());
            }
        }
        return std::make_unique<VulkanPipelineState>(device, pipeline, pipelineLayout,
                                                      renderPass, VK_PIPELINE_BIND_POINT_GRAPHICS);
    }

    // ── 传统 Graphics Pipeline（顶点着色器 + IA）──
    VkShaderModule vert = createShader(desc.vertexShader,   VK_SHADER_STAGE_VERTEX_BIT);
    VkShaderModule frag = createShader(desc.pixelShader,    VK_SHADER_STAGE_FRAGMENT_BIT);

    // 2. Render pass（颜色附件 + 可选的深度附件）
    //    支持 depth-only 模式：colorAttachmentCount=0 时仅创建深度附件
    bool hasColor = (desc.colorAttachmentCount > 0);
    bool hasDepth = (desc.depthFormat != Format::Unknown);

    // 构建颜色附件（支持 MRT：最多 8 个）
    VkAttachmentDescription colorAttachments[kMaxColorAttachments]{};
    VkAttachmentReference   colorRefs[kMaxColorAttachments]{};
    for (u32 c = 0; c < desc.colorAttachmentCount; ++c) {
        colorAttachments[c].format        = ToVkFormat(desc.colorFormats[c]);
        colorAttachments[c].samples       = VK_SAMPLE_COUNT_1_BIT;
        colorAttachments[c].loadOp        = ToVkLoadOp(desc.colorLoadOp);
        colorAttachments[c].storeOp       = VK_ATTACHMENT_STORE_OP_STORE;
        // LOAD 模式下 initialLayout 不能是 UNDEFINED（Vulkan 规范要求）
        colorAttachments[c].initialLayout = (desc.colorLoadOp == LoadOp::Load)
            ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
            : VK_IMAGE_LAYOUT_UNDEFINED;
        colorAttachments[c].finalLayout   = (desc.colorFormats[c] == Format::BGRA8_UNORM ||
                                             desc.colorFormats[c] == Format::BGRA8_SRGB)
                                            ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
                                            : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorRefs[c].attachment = c;
        colorRefs[c].layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }

    VkAttachmentDescription depthAttach{};
    depthAttach.format         = hasDepth ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_UNDEFINED;
    depthAttach.samples        = VK_SAMPLE_COUNT_1_BIT;
    depthAttach.loadOp         = ToVkLoadOp(desc.depthLoadOp);
    depthAttach.storeOp        = VK_ATTACHMENT_STORE_OP_STORE; // 阴影贴图需要 STORE
    depthAttach.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttach.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttach.initialLayout  = (desc.depthLoadOp == LoadOp::Load)
        ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
        : VK_IMAGE_LAYOUT_UNDEFINED;
    depthAttach.finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL; // 阴影贴图后续要采样

    // depth-only 模式下，深度附件在颜色之后
    u32 depthAttachIdx = desc.colorAttachmentCount;  // 深度在颜色附件之后
    VkAttachmentReference depthRef{depthAttachIdx, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount    = desc.colorAttachmentCount;
    subpass.pColorAttachments       = hasColor ? colorRefs : nullptr;
    subpass.pDepthStencilAttachment = hasDepth ? &depthRef : nullptr;

    // 构建附件数组：颜色在前 [0..N-1]，深度在 [N]
    VkAttachmentDescription attachments[kMaxColorAttachments + 1];  // 最多 8 颜色 + 1 深度
    u32 attachmentCount = 0;
    for (u32 c = 0; c < desc.colorAttachmentCount; ++c)
        attachments[attachmentCount++] = colorAttachments[c];
    if (hasDepth) attachments[attachmentCount++] = depthAttach;

    VkSubpassDependency dep{};
    dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass    = 0;
    dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.dstStageMask  = (hasColor ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT : 0u) |
                        (hasDepth ? VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT : 0u);
    dep.dstAccessMask = (hasColor ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT : 0u) |
                        (hasDepth ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : 0u);

    VkRenderPassCreateInfo rpInfo{};
    rpInfo.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpInfo.attachmentCount = attachmentCount;
    rpInfo.pAttachments    = attachments;
    rpInfo.subpassCount    = 1;
    rpInfo.pSubpasses      = &subpass;
    rpInfo.dependencyCount = (hasColor || hasDepth) ? 1u : 0u;
    rpInfo.pDependencies   = (hasColor || hasDepth) ? &dep : nullptr;

    VkRenderPass renderPass;
    vkCreateRenderPass(device, &rpInfo, nullptr, &renderPass);

    // 3. Vertex input（空 layout → SV_VertexID 全屏三角形，无需 VB）
    bool hasVertexInput = (desc.vertexLayout.stride > 0) || (!desc.vertexLayout.attributes.empty());
    VkVertexInputBindingDescription binding{};
    binding.binding   = 0;
    binding.stride    = hasVertexInput ? (desc.vertexLayout.stride > 0 ? desc.vertexLayout.stride : 8) : 0;
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    // VertexFormat -> VkFormat 映射
    auto toVkVertexFormat = [](VertexFormat fmt) -> VkFormat {
        switch (fmt) {
            case VertexFormat::Float:   return VK_FORMAT_R32_SFLOAT;
            case VertexFormat::Float2:  return VK_FORMAT_R32G32_SFLOAT;
            case VertexFormat::Float3:  return VK_FORMAT_R32G32B32_SFLOAT;
            case VertexFormat::Float4:  return VK_FORMAT_R32G32B32A32_SFLOAT;
            case VertexFormat::UByte4_Norm: return VK_FORMAT_R8G8B8A8_UNORM;
            case VertexFormat::Byte4_Norm:  return VK_FORMAT_R8G8B8A8_SNORM;
            case VertexFormat::UInt:    return VK_FORMAT_R32_UINT;
            case VertexFormat::UInt2:   return VK_FORMAT_R32G32_UINT;
            case VertexFormat::UInt4:   return VK_FORMAT_R32G32B32A32_UINT;
            default:                    return VK_FORMAT_R32G32B32_SFLOAT;
        }
    };

    // 根据 desc.vertexLayout 构建 Vulkan 属性列表
    std::vector<VkVertexInputAttributeDescription> vkAttrs;
    if (desc.vertexLayout.attributes.empty()) {
        // 回退：未指定属性时，根据 stride 推导默认格式
        VkVertexInputAttributeDescription defaultAttr{};
        defaultAttr.location = 0;
        defaultAttr.binding  = 0;
        defaultAttr.offset   = 0;
        if (desc.vertexLayout.stride >= 32) defaultAttr.format = VK_FORMAT_R32G32B32_SFLOAT;
        else if (desc.vertexLayout.stride >= 12) defaultAttr.format = VK_FORMAT_R32G32B32_SFLOAT;
        else defaultAttr.format = VK_FORMAT_R32G32_SFLOAT;  // stride=8: vec2
        vkAttrs.push_back(defaultAttr);
    } else {
        for (auto& attr : desc.vertexLayout.attributes) {
            VkVertexInputAttributeDescription va{};
            va.location = attr.location;
            va.binding  = attr.binding;
            va.format   = toVkVertexFormat(attr.format);
            va.offset   = attr.offset;
            vkAttrs.push_back(va);
        }
    }

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount   = hasVertexInput ? 1u : 0u;
    vertexInput.pVertexBindingDescriptions      = hasVertexInput ? &binding : nullptr;
    vertexInput.vertexAttributeDescriptionCount = hasVertexInput ? static_cast<u32>(vkAttrs.size()) : 0u;
    vertexInput.pVertexAttributeDescriptions    = hasVertexInput ? vkAttrs.data() : nullptr;

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
    rasterizer.lineWidth   = 1.0f;
    rasterizer.cullMode    = ToVkCullMode(desc.cullMode);
    rasterizer.frontFace   = ToVkFrontFace(desc.frontFace);
    rasterizer.polygonMode = ToVkFillMode(desc.fillMode);

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // 6. Depth stencil state
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable  = desc.depthTest ? VK_TRUE : VK_FALSE;
    depthStencil.depthWriteEnable = desc.depthWrite ? VK_TRUE : VK_FALSE;
    depthStencil.depthCompareOp   = ToVkCompareOp(desc.depthCompare);
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.stencilTestEnable     = VK_FALSE;

    VkPipelineColorBlendAttachmentState blendAttachments[kMaxColorAttachments]{};  // 支持最多 8 个 MRT
    for (u32 c = 0; c < desc.colorAttachmentCount; ++c) {
        const auto& cb = desc.colorBlend[c];
        blendAttachments[c].blendEnable         = cb.blendEnable ? VK_TRUE : VK_FALSE;
        blendAttachments[c].srcColorBlendFactor = ToVkBlendFactor(cb.srcColorBlendFactor);
        blendAttachments[c].dstColorBlendFactor = ToVkBlendFactor(cb.dstColorBlendFactor);
        blendAttachments[c].colorBlendOp        = ToVkBlendOp(cb.colorBlendOp);
        blendAttachments[c].srcAlphaBlendFactor = ToVkBlendFactor(cb.srcAlphaBlendFactor);
        blendAttachments[c].dstAlphaBlendFactor = ToVkBlendFactor(cb.dstAlphaBlendFactor);
        blendAttachments[c].alphaBlendOp        = ToVkBlendOp(cb.alphaBlendOp);
        blendAttachments[c].colorWriteMask      = ToVkColorWriteMask(cb.writeMask);
    }

    VkPipelineColorBlendStateCreateInfo colorBlend{};
    colorBlend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = desc.colorAttachmentCount;
    colorBlend.pAttachments    = blendAttachments;

    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynState{};
    dynState.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynState.dynamicStateCount = 2;
    dynState.pDynamicStates    = dyn;

    // 构建 push constant ranges（直接使用 stageMask 位掩码）
    std::vector<VkPushConstantRange> vkPushRanges;
    for (auto& pcRange : desc.pushConstantRanges) {
        VkPushConstantRange vkRange{};
        vkRange.stageFlags = pcRange.stageMask;  // 直接使用 Vulkan 兼容的位掩码
        vkRange.offset     = pcRange.offset;
        vkRange.size       = pcRange.size;
        vkPushRanges.push_back(vkRange);
    }
    // 如果着色器使用 push constants 但没有显式声明 range，自动添加默认范围
    // 避免 Vulkan 验证层警告 "push constants but no VkPushConstantRange found"
    if (vkPushRanges.empty()) {
        VkPushConstantRange defaultRange{};
        defaultRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        defaultRange.size       = kDefaultPushConstantSize;  // 最小保证范围
        vkPushRanges.push_back(defaultRange);
    }

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount         = static_cast<u32>(descLayouts.size());
    layoutInfo.pSetLayouts            = descLayouts.empty() ? nullptr : descLayouts.data();
    layoutInfo.pushConstantRangeCount = static_cast<u32>(vkPushRanges.size());
    layoutInfo.pPushConstantRanges    = vkPushRanges.empty() ? nullptr : vkPushRanges.data();
    VkPipelineLayout pipelineLayout;
    vkCreatePipelineLayout(device, &layoutInfo, nullptr, &pipelineLayout);

    // 6. Shader stages
    VkPipelineShaderStageCreateInfo stages[kMaxShaderStages]{};
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
    pipeInfo.pDepthStencilState  = &depthStencil;
    pipeInfo.pColorBlendState    = &colorBlend;
    pipeInfo.pDynamicState       = &dynState;
    pipeInfo.layout              = pipelineLayout;
    pipeInfo.renderPass          = renderPass;
    pipeInfo.subpass             = 0;

    VkPipeline pipeline;
    vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeInfo, nullptr, &pipeline);

    vkDestroyShaderModule(device, vert, nullptr);
    vkDestroyShaderModule(device, frag, nullptr);

    HE_CORE_INFO("Vulkan graphics pipeline created");
    // ── 插入 PSO 缓存 ──
    if (vulkanDevice) {
        uint64_t hash = HashPipelineStateDesc(desc);
        vulkanDevice->InsertPSOToCache(hash, pipeline, pipelineLayout, renderPass);
        VkPipeline cachedP = VK_NULL_HANDLE; VkPipelineLayout cachedL = VK_NULL_HANDLE;
        VkRenderPass cachedR = VK_NULL_HANDLE;
        auto ref = vulkanDevice->GetCachedPSORef(hash, cachedP, cachedL, cachedR);
        if (ref) {
            return std::make_unique<VulkanPipelineState>(device, pipeline, pipelineLayout,
                renderPass, VK_PIPELINE_BIND_POINT_GRAPHICS,
                ref, &vulkanDevice->GetDeferredDestroy());
        }
    }
    return std::make_unique<VulkanPipelineState>(device, pipeline, pipelineLayout,
                                                  renderPass, VK_PIPELINE_BIND_POINT_GRAPHICS);
}

} // namespace he::rhi
