#include "PSOPrecompileManager.h"
#include "VulkanDevice.h"
#include "RHI/Shader.h"
#include "Core/Log.h"

namespace he::rhi {

PSOPrecompileManager::PSOPrecompileManager() = default;

PSOPrecompileManager::~PSOPrecompileManager() {
    Shutdown();
}

void PSOPrecompileManager::Initialize(VkDevice device, VkPhysicalDevice physical,
                                       VkPipelineCache mainCache) {
    m_Device    = device;
    m_Physical   = physical;
    m_MainCache = mainCache;
    HE_CORE_INFO("PSOPrecompileManager: 初始化完成");
}

void PSOPrecompileManager::Shutdown() {
    m_StopRequested.store(true, std::memory_order_release);
    if (m_WorkerThread.joinable()) {
        m_WorkerThread.join();
    }
    // 【C1】在这里合并（而不是 worker 线程里）：join 之后没有并发的 PSO 创建，
    // `vkMergePipelineCaches` 对主缓存的外部同步要求成立。合并结果随后会被
    // `VulkanDevice::SavePipelineCache` 落盘（设备析构里的调用顺序已相应调整）。
    if (m_WorkerCache != VK_NULL_HANDLE && m_MainCache != VK_NULL_HANDLE &&
        m_CompiledCount.load() > 0) {
        MergeCache();
    }
    if (m_WorkerCache != VK_NULL_HANDLE) {
        vkDestroyPipelineCache(m_Device, m_WorkerCache, nullptr);
        m_WorkerCache = VK_NULL_HANDLE;
    }
    m_Queue.clear();
    m_CompiledCount.store(0);
    m_TotalCount.store(0);
    m_Running.store(false);
    m_StopRequested.store(false);
    HE_CORE_INFO("PSOPrecompileManager: 已关闭");
}

void PSOPrecompileManager::QueuePSO(const PipelineStateDesc& desc, VkPipelineLayout layout) {
    std::lock_guard<std::mutex> lock(m_QueueMutex);
    QueueItem item;
    item.desc   = desc;
    item.layout = layout;   // 借用（生命周期由 VulkanDevice 持有到 Shutdown）
    m_Queue.push_back(item);
    m_TotalCount.store(static_cast<u32>(m_Queue.size()), std::memory_order_release);
}

void PSOPrecompileManager::StartPrecompile() {
    if (m_Running.load(std::memory_order_acquire)) {
        HE_CORE_WARN("PSOPrecompileManager: 预热已在运行中");
        return;
    }
    if (m_Queue.empty()) {
        HE_CORE_WARN("PSOPrecompileManager: 队列为空，跳过预热");
        return;
    }

    // 创建 worker 专用的 VkPipelineCache（从主缓存派生数据，独立编译目标）
    VkPipelineCacheCreateInfo cacheInfo{};
    cacheInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    // 【C1 崩溃根因】派生数据必须活得比 `vkCreatePipelineCache` 久：原先 `cacheData` 声明在
    // `if (m_MainCache != VK_NULL_HANDLE)` 的花括号里，`cacheInfo.pInitialData` 指向它之后
    // 向量就在同一个函数里析构了，而 `vkCreatePipelineCache` 在块**外**才调用 ⇒ 驱动读到已释放的
    // 堆内存（use-after-free）。实测表现：打开 Vulkan 预编译后 02.Cube 在 ParticleRenderer 初始化
    // 之后立刻 0xC0000005（ACCESS_VIOLATION）退出 —— 之前把 Vulkan 预编译一刀切禁掉，正是掩盖了它。
    std::vector<u8> cacheData;
    if (m_MainCache != VK_NULL_HANDLE) {
        size_t dataSize = 0;
        vkGetPipelineCacheData(m_Device, m_MainCache, &dataSize, nullptr);
        if (dataSize > 0) {
            cacheData.resize(dataSize);
            vkGetPipelineCacheData(m_Device, m_MainCache, &dataSize, cacheData.data());
            cacheInfo.initialDataSize = dataSize;
            cacheInfo.pInitialData    = cacheData.data();
        }
    }
    VkResult vr = vkCreatePipelineCache(m_Device, &cacheInfo, nullptr, &m_WorkerCache);
    if (vr != VK_SUCCESS) {
        HE_CORE_ERROR("PSOPrecompileManager: 无法创建 Worker PipelineCache (result={})", static_cast<i32>(vr));
        return;
    }

    m_CompiledCount.store(0, std::memory_order_release);
    m_StopRequested.store(false, std::memory_order_release);
    m_Running.store(true, std::memory_order_release);
    m_WorkerThread = std::thread(&PSOPrecompileManager::WorkerThreadFunc, this);

    HE_CORE_INFO("PSOPrecompileManager: 启动后台预热 — {} 个 PSO", m_TotalCount.load());
}

void PSOPrecompileManager::MergeCache() {
    if (m_WorkerCache == VK_NULL_HANDLE || m_MainCache == VK_NULL_HANDLE) return;
    VkResult vr = vkMergePipelineCaches(m_Device, m_MainCache, 1, &m_WorkerCache);
    if (vr == VK_SUCCESS) {
        HE_CORE_INFO("PSOPrecompileManager: Worker 缓存已合并到主缓存");
    } else {
        HE_CORE_WARN("PSOPrecompileManager: vkMergePipelineCaches 失败 (result={})", static_cast<i32>(vr));
    }
}

// ============================================================
// 辅助函数：为单个 PSO 创建着色器模块
// ============================================================
static VkShaderModule CreateShaderModuleSafe(VkDevice device,
                                              const ShaderBytecode& bytecode) {
    if (bytecode.spirv.empty()) return VK_NULL_HANDLE;
    VkShaderModuleCreateInfo info{};
    info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = bytecode.spirv.size() * sizeof(u32);
    info.pCode    = bytecode.spirv.data();
    VkShaderModule mod = VK_NULL_HANDLE;
    VkResult vr = vkCreateShaderModule(device, &info, nullptr, &mod);
    if (vr != VK_SUCCESS) {
        HE_CORE_WARN("PSOPrecompileManager: vkCreateShaderModule 失败 (stage={}, result={})",
                     static_cast<i32>(bytecode.stage), static_cast<i32>(vr));
    }
    return mod;
}

// ============================================================
// 辅助函数：将 RHI 类型映射到 Vulkan 类型（与 VulkanPipeline.cpp 中一致）
// ============================================================
// 【改名】VulkanConverters.h 里已有全局 ToVkFormat(Format)：同名的本地 static 版本在
// clang 下会被判定为"static 声明跟随非 static 声明"，且调用有歧义，故本地版本改名。
static VkFormat ToVkFormatLocal(Format fmt) {
    switch (fmt) {
    case Format::RGBA8_UNORM:  return VK_FORMAT_R8G8B8A8_UNORM;
    case Format::BGRA8_UNORM:  return VK_FORMAT_B8G8R8A8_UNORM;
    case Format::A2B10G10R10_UNORM_PACK32: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case Format::RGBA16_FLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case Format::RG16_FLOAT:   return VK_FORMAT_R16G16_SFLOAT;
    case Format::R16_FLOAT:    return VK_FORMAT_R16_SFLOAT;
    case Format::D32_FLOAT:    return VK_FORMAT_D32_SFLOAT;
    default:                   return VK_FORMAT_R8G8B8A8_UNORM;
    }
}

static VkShaderStageFlagBits ToVkShaderStage(ShaderStage stage) {
    switch (stage) {
    case ShaderStage::Vertex:            return VK_SHADER_STAGE_VERTEX_BIT;
    case ShaderStage::Pixel:             return VK_SHADER_STAGE_FRAGMENT_BIT;
    case ShaderStage::Compute:           return VK_SHADER_STAGE_COMPUTE_BIT;
    case ShaderStage::Mesh:              return VK_SHADER_STAGE_MESH_BIT_EXT;
    case ShaderStage::Amplification:     return VK_SHADER_STAGE_TASK_BIT_EXT;
    default:                             return VK_SHADER_STAGE_VERTEX_BIT;
    }
}

// ============================================================
// Worker 线程主函数
// ============================================================
void PSOPrecompileManager::WorkerThreadFunc() {
    HE_CORE_INFO("PSOPrecompileManager: Worker 线程启动 (队列大小={})", m_TotalCount.load());

    while (!m_StopRequested.load(std::memory_order_acquire)) {
        // 从队列取一个 PSO（【C1】队列项还带主线程建好的真实 pipeline layout，worker 只借用）
        PipelineStateDesc desc;
        VkPipelineLayout  sharedLayout = VK_NULL_HANDLE;
        {
            std::lock_guard<std::mutex> lock(m_QueueMutex);
            u32 idx = m_CompiledCount.load(std::memory_order_acquire);
            if (idx >= m_Queue.size()) break;  // 队列已清空
            desc         = m_Queue[idx].desc;
            sharedLayout = m_Queue[idx].layout;
        }

        // ── 编译 PSO 到 Worker Cache ──
        if (desc.bindPoint == PipelineBindPoint::Compute) {
            // Compute PSO
            if (!desc.computeShader || desc.computeShader->spirv.empty()) {
                m_CompiledCount.fetch_add(1, std::memory_order_release);
                continue;
            }

            VkShaderModule cs = CreateShaderModuleSafe(m_Device, *desc.computeShader);
            if (cs == VK_NULL_HANDLE) {
                m_CompiledCount.fetch_add(1, std::memory_order_release);
                continue;
            }

            // 【C1】优先用主线程给的**真实**布局（含 desc 的 descriptorSetLayouts）。
            // 没有时退回自建空布局：那只适合不声明任何描述符的着色器，否则校验层会报
            // VUID-VkGraphicsPipelineCreateInfo-layout-07988（shader 有 set=0、布局里 0 个 set）。
            VkPipelineLayout layout    = sharedLayout;
            bool             ownLayout = false;
            if (layout == VK_NULL_HANDLE) {
                VkPipelineLayoutCreateInfo layoutInfo{};
                layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
                layoutInfo.setLayoutCount = 0;
                layoutInfo.pSetLayouts    = nullptr;
                // 处理 push constant（如果有）
                std::vector<VkPushConstantRange> pcr;
                for (auto& r : desc.pushConstantRanges) {
                    VkPushConstantRange range{};
                    range.stageFlags = r.stageMask | VK_SHADER_STAGE_COMPUTE_BIT;
                    range.offset     = r.offset;
                    range.size       = r.size;
                    pcr.push_back(range);
                }
                if (!pcr.empty()) {
                    layoutInfo.pushConstantRangeCount = static_cast<u32>(pcr.size());
                    layoutInfo.pPushConstantRanges    = pcr.data();
                }
                vkCreatePipelineLayout(m_Device, &layoutInfo, nullptr, &layout);
                ownLayout = true;
            }

            VkComputePipelineCreateInfo compInfo{};
            compInfo.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
            compInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            compInfo.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
            compInfo.stage.module = cs;
            // 【C1】入口点名必须与**主创建路径**一致（`VulkanPipeline.cpp` 用的是
            // `kDefaultShaderEntryPoint`）：Slang 编译出的 SPIR-V 里阶段入口恒为 "main"，
            // 而 `ShaderBytecode::entryPoint` 是 Slang 源里的函数名（如 "vertexMain"）。
            // 这里原先用 `desc.entryPoint` ⇒ 校验层报 VUID-VkPipelineShaderStageCreateInfo-pName-00707
            // 且流水线创建失败（预编译对这些 PSO 实际没产出任何缓存）。
            compInfo.stage.pName  = kDefaultShaderEntryPoint;
            compInfo.layout       = layout;

            VkPipeline pipeline = VK_NULL_HANDLE;
            vkCreateComputePipelines(m_Device, m_WorkerCache, 1, &compInfo, nullptr, &pipeline);

            if (pipeline)      vkDestroyPipeline(m_Device, pipeline, nullptr);
            if (ownLayout && layout) vkDestroyPipelineLayout(m_Device, layout, nullptr);
            vkDestroyShaderModule(m_Device, cs, nullptr);
        } else {
            // Graphics PSO（传统 VS+FS 路径）
            VkShaderModule vs = VK_NULL_HANDLE;
            VkShaderModule fs = VK_NULL_HANDLE;
            if (desc.vertexShader && !desc.vertexShader->spirv.empty())
                vs = CreateShaderModuleSafe(m_Device, *desc.vertexShader);
            if (desc.pixelShader && !desc.pixelShader->spirv.empty())
                fs = CreateShaderModuleSafe(m_Device, *desc.pixelShader);

            if (vs == VK_NULL_HANDLE && fs == VK_NULL_HANDLE) {
                m_CompiledCount.fetch_add(1, std::memory_order_release);
                continue;
            }

            std::vector<VkPipelineShaderStageCreateInfo> stages;
            if (vs) {
                VkPipelineShaderStageCreateInfo s{};
                s.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                s.stage  = VK_SHADER_STAGE_VERTEX_BIT;
                s.module = vs;
                s.pName  = kDefaultShaderEntryPoint;   // 【C1】与主路径一致（见计算 PSO 处的说明）
                stages.push_back(s);
            }
            if (fs) {
                VkPipelineShaderStageCreateInfo s{};
                s.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                s.stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
                s.module = fs;
                s.pName  = kDefaultShaderEntryPoint;   // 【C1】与主路径一致（见计算 PSO 处的说明）
                stages.push_back(s);
            }

            // Vertex input（简化：对于全屏三角形等无顶点属性的情况）
            VkPipelineVertexInputStateCreateInfo vi{};
            vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
            std::vector<VkVertexInputBindingDescription> bindings;
            std::vector<VkVertexInputAttributeDescription> attrs;
            if (desc.vertexLayout.stride > 0) {
                VkVertexInputBindingDescription bd{};
                bd.binding   = 0;
                bd.stride    = desc.vertexLayout.stride;
                bd.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
                bindings.push_back(bd);
                for (auto& a : desc.vertexLayout.attributes) {
                    VkVertexInputAttributeDescription ad{};
                    ad.location = a.location;
                    ad.binding  = a.binding;
                    ad.offset   = a.offset;
                    // 简化映射：基于字节数估算格式
                    switch (a.format) {
                    case VertexFormat::Float:  ad.format = VK_FORMAT_R32_SFLOAT;
                    break;
                    case VertexFormat::Float2: ad.format = VK_FORMAT_R32G32_SFLOAT;
                    break;
                    case VertexFormat::Float3: ad.format = VK_FORMAT_R32G32B32_SFLOAT;
                    break;
                    case VertexFormat::Float4: ad.format = VK_FORMAT_R32G32B32A32_SFLOAT;
                    break;
                    case VertexFormat::UByte4: ad.format = VK_FORMAT_R8G8B8A8_UINT;
                    break;
                    default:                   ad.format = VK_FORMAT_R32G32B32_SFLOAT;
                    break;
                    }
                    attrs.push_back(ad);
                }
            }
            vi.vertexBindingDescriptionCount   = static_cast<u32>(bindings.size());
            vi.pVertexBindingDescriptions      = bindings.data();
            vi.vertexAttributeDescriptionCount = static_cast<u32>(attrs.size());
            vi.pVertexAttributeDescriptions    = attrs.data();

            VkPipelineInputAssemblyStateCreateInfo ia{};
            ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
            ia.topology = (desc.topology == PrimitiveTopology::TriangleList)
                          ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
                          : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;

            // Pipeline layout
            // 【C1】同计算路径：优先用主线程给的真实布局（含 desc 的 descriptorSetLayouts）
            VkPipelineLayout layout    = sharedLayout;
            bool             ownLayout = false;
            if (layout == VK_NULL_HANDLE) {
                VkPipelineLayoutCreateInfo layoutInfo{};
                layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
                layoutInfo.setLayoutCount = 0;
                layoutInfo.pSetLayouts    = nullptr;
                std::vector<VkPushConstantRange> pcr;
                for (auto& r : desc.pushConstantRanges) {
                    VkPushConstantRange range{};
                    range.stageFlags = r.stageMask | VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
                    range.offset     = r.offset;
                    range.size       = r.size;
                    pcr.push_back(range);
                }
                if (!pcr.empty()) {
                    layoutInfo.pushConstantRangeCount = static_cast<u32>(pcr.size());
                    layoutInfo.pPushConstantRanges    = pcr.data();
                }
                vkCreatePipelineLayout(m_Device, &layoutInfo, nullptr, &layout);
                ownLayout = true;
            }

            // Render pass（简化：单 RT + 可选深度）
            VkRenderPass rp = VK_NULL_HANDLE;
            std::vector<VkAttachmentDescription> attachments;
            std::vector<VkAttachmentReference>   colorRefs;
            VkAttachmentReference                depthRef{};
            bool hasDepth = false;

            for (u32 i = 0; i < desc.colorAttachmentCount; ++i) {
                VkAttachmentDescription ad{};
                ad.format         = ToVkFormatLocal(desc.colorFormats[i]);
                ad.samples        = VK_SAMPLE_COUNT_1_BIT;
                ad.loadOp         = (desc.colorLoadOp == LoadOp::Clear) ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
                ad.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
                ad.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                ad.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
                ad.initialLayout  = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                ad.finalLayout    = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                attachments.push_back(ad);

                VkAttachmentReference ref{};
                ref.attachment = i;
                ref.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                colorRefs.push_back(ref);
            }
            if (desc.depthFormat != Format::Unknown) {
                hasDepth = true;
                VkAttachmentDescription dd{};
                dd.format         = ToVkFormatLocal(desc.depthFormat);
                dd.samples        = VK_SAMPLE_COUNT_1_BIT;
                dd.loadOp         = (desc.depthLoadOp == LoadOp::Clear) ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
                dd.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
                dd.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                dd.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
                dd.initialLayout  = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                dd.finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                attachments.push_back(dd);

                depthRef.attachment = static_cast<u32>(attachments.size()) - 1;
                depthRef.layout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            }

            VkSubpassDescription subpass{};
            subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
            subpass.colorAttachmentCount    = static_cast<u32>(colorRefs.size());
            subpass.pColorAttachments       = colorRefs.data();
            subpass.pDepthStencilAttachment = hasDepth ? &depthRef : nullptr;

            VkRenderPassCreateInfo rpInfo{};
            rpInfo.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
            rpInfo.attachmentCount = static_cast<u32>(attachments.size());
            rpInfo.pAttachments    = attachments.data();
            rpInfo.subpassCount    = 1;
            rpInfo.pSubpasses      = &subpass;
            vkCreateRenderPass(m_Device, &rpInfo, nullptr, &rp);

            // Rasterization
            VkPipelineRasterizationStateCreateInfo rs{};
            rs.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
            rs.polygonMode = (desc.fillMode == FillMode::Solid) ? VK_POLYGON_MODE_FILL : VK_POLYGON_MODE_LINE;
            rs.cullMode    = VK_CULL_MODE_NONE;
            rs.frontFace   = VK_FRONT_FACE_CLOCKWISE;
            rs.lineWidth   = 1.0f;

            // Depth/Stencil
            VkPipelineDepthStencilStateCreateInfo ds{};
            ds.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
            ds.depthTestEnable  = desc.depthTest ? VK_TRUE : VK_FALSE;
            ds.depthWriteEnable = desc.depthWrite ? VK_TRUE : VK_FALSE;
            ds.depthCompareOp   = (desc.depthCompare == CompareFunc::LessEqual) ? VK_COMPARE_OP_LESS_OR_EQUAL
                                 : (desc.depthCompare == CompareFunc::Less)      ? VK_COMPARE_OP_LESS
                                 :                                                VK_COMPARE_OP_ALWAYS;

            // Multisample
            VkPipelineMultisampleStateCreateInfo ms{};
            ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
            ms.rasterizationSamples = static_cast<VkSampleCountFlagBits>(desc.sampleCount);

            // Viewport (dynamic)
            VkPipelineViewportStateCreateInfo vp{};
            vp.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
            vp.viewportCount = 1;
            vp.scissorCount  = 1;

            // Dynamic states
            VkDynamicState dynStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
            VkPipelineDynamicStateCreateInfo dyn{};
            dyn.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
            dyn.dynamicStateCount = 2;
            dyn.pDynamicStates    = dynStates;

            // Color blend
            VkPipelineColorBlendStateCreateInfo cb{};
            cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
            std::vector<VkPipelineColorBlendAttachmentState> cbAtt;
            for (u32 i = 0; i < desc.colorAttachmentCount; ++i) {
                VkPipelineColorBlendAttachmentState ba{};
                ba.blendEnable         = desc.colorBlend[i].blendEnable ? VK_TRUE : VK_FALSE;
                ba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
                ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                ba.colorBlendOp        = VK_BLEND_OP_ADD;
                ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
                ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
                ba.alphaBlendOp        = VK_BLEND_OP_ADD;
                ba.colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
                cbAtt.push_back(ba);
            }
            cb.attachmentCount = static_cast<u32>(cbAtt.size());
            cb.pAttachments    = cbAtt.data();

            // Create graphics pipeline
            VkGraphicsPipelineCreateInfo pipeInfo{};
            pipeInfo.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
            pipeInfo.stageCount          = static_cast<u32>(stages.size());
            pipeInfo.pStages             = stages.data();
            pipeInfo.pVertexInputState   = &vi;
            pipeInfo.pInputAssemblyState = &ia;
            pipeInfo.pViewportState      = &vp;
            pipeInfo.pRasterizationState = &rs;
            pipeInfo.pDepthStencilState  = &ds;
            pipeInfo.pMultisampleState   = &ms;
            pipeInfo.pColorBlendState    = &cb;
            pipeInfo.pDynamicState       = &dyn;
            pipeInfo.layout              = layout;
            pipeInfo.renderPass          = rp;
            pipeInfo.subpass             = desc.subpassIndex;

            VkPipeline pipeline = VK_NULL_HANDLE;
            vkCreateGraphicsPipelines(m_Device, m_WorkerCache, 1, &pipeInfo, nullptr, &pipeline);

            // 销毁临时 Vulkan 对象（编译结果已保存在 WorkerCache 中）
            if (pipeline) vkDestroyPipeline(m_Device, pipeline, nullptr);
            if (rp)       vkDestroyRenderPass(m_Device, rp, nullptr);
            if (ownLayout && layout) vkDestroyPipelineLayout(m_Device, layout, nullptr);
            if (vs)       vkDestroyShaderModule(m_Device, vs, nullptr);
            if (fs)       vkDestroyShaderModule(m_Device, fs, nullptr);
        }

        m_CompiledCount.fetch_add(1, std::memory_order_release);
    }

    // 【C1】合并**不在这里做**：`vkMergePipelineCaches` 要求调用方对主缓存做外部同步，而此刻主线程
    // 正在用同一个主缓存创建 PSO（首帧起各 pass 惰性建 PSO）⇒ 在 worker 线程里合并是数据竞争。
    // 合并移到 `Shutdown()`（join 之后、销毁 worker cache 之前），此时不再有并发创建。
    HE_CORE_INFO("PSOPrecompileManager: Worker 线程完成 — {}/{} 个 PSO 已编译",
                 m_CompiledCount.load(), m_TotalCount.load());
    m_Running.store(false, std::memory_order_release);
}

} // namespace he::rhi
