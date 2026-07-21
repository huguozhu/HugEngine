#include "RenderGraph.h"
#include "Profiler/ProfilerManager.h"
#include "Core/Log.h"

#include <algorithm>
#include <unordered_set>

namespace he::render {

RenderGraph::RenderGraph() {
    HE_CORE_INFO("RenderGraph v2 created (auto-barrier + aliasing)");
}

RenderGraph::~RenderGraph() {
    Reset();
}

ResourceHandle RenderGraph::CreateTexture(StringView name, const rhi::TextureDesc& desc) {
    ResourceHandle h = static_cast<ResourceHandle>(m_Resources.size());
    ResourceDesc rd;
    rd.type         = ResourceType::Texture;
    rd.width        = desc.width;
    rd.height       = desc.height;
    rd.format       = desc.format;
    rd.textureUsage = desc.usage;
    rd.name         = String(name);
    m_Resources.push_back(rd);
    m_ResourceStates.push_back({rhi::ResourceState::Undefined, false});
    m_AliasInfo.push_back({});
    HE_CORE_INFO("  RG tex #{}: {} ({}x{})", h, name, desc.width, desc.height);
    return h;
}

ResourceHandle RenderGraph::CreateBuffer(StringView name, const rhi::BufferDesc& desc) {
    ResourceHandle h = static_cast<ResourceHandle>(m_Resources.size());
    ResourceDesc rd;
    rd.type        = ResourceType::Buffer;
    rd.bufferUsage = desc.usage;
    rd.bufferSize  = desc.size;
    rd.name        = String(name);
    m_Resources.push_back(rd);
    m_ResourceStates.push_back({rhi::ResourceState::Undefined, false});
    m_AliasInfo.push_back({});
    HE_CORE_INFO("  RG buf #{}: {} ({} bytes)", h, name, desc.size);
    return h;
}

ResourceHandle RenderGraph::ImportTexture(StringView name, rhi::IRHITexture* external) {
    ResourceHandle h = static_cast<ResourceHandle>(m_Resources.size());
    ResourceDesc rd;
    rd.type   = ResourceType::Texture;
    rd.width  = external->GetWidth();
    rd.height = external->GetHeight();
    rd.format = external->GetFormat();
    rd.name   = String(name);
    m_Resources.push_back(rd);
    m_ResourceStates.push_back({rhi::ResourceState::Undefined, false});
    m_AliasInfo.push_back({});
    m_ImportedTextures[h] = external;
    HE_CORE_INFO("  RG import #{}: {}", h, name);
    return h;
}

const ResourceDesc& RenderGraph::GetResourceDesc(ResourceHandle h) const {
    if (h < m_Resources.size()) return m_Resources[h];
    static ResourceDesc invalid;
    return invalid;
}

ResourceHandle RenderGraph::ImportBackBuffer() {
    if (m_BackBufferHandle == kInvalidHandle) {
        m_BackBufferHandle = static_cast<ResourceHandle>(m_Resources.size());
        ResourceDesc rd;
        rd.type         = ResourceType::Texture;
        rd.width        = rhi::kDefaultBackBufferWidth;
        rd.height       = rhi::kDefaultBackBufferHeight;
        rd.format       = rhi::Format::RGBA8_UNORM;
        rd.textureUsage = rhi::TextureUsage::RenderTarget;
        rd.name         = "BackBuffer";
        m_Resources.push_back(rd);
        m_ResourceStates.push_back({rhi::ResourceState::Undefined, false});
        m_AliasInfo.push_back({});
    }
    return m_BackBufferHandle;
}

PassNode* RenderGraph::AddPass(StringView name,
                                std::vector<PassResource> reads,
                                std::vector<PassResource> writes,
                                PassExecuteFunc execute,
                                RGPassQueue queueHint) {
    PassNode pass;
    pass.name      = String(name);
    pass.reads     = std::move(reads);
    pass.writes    = std::move(writes);
    pass.execute   = std::move(execute);
    pass.queueHint = queueHint;
    m_Passes.push_back(std::move(pass));
    m_Compiled = false;
    HE_CORE_INFO("  RG pass: {} [{}]", name,
                 queueHint == RGPassQueue::Compute ? "Compute" :
                 queueHint == RGPassQueue::Graphics ? "Graphics" : "Default");
    return &m_Passes.back();
}

rhi::ResourceState RenderGraph::AccessToState(ResourceAccess access, bool isDepth) const {
    switch (access) {
        case ResourceAccess::Read:
            return isDepth ? rhi::ResourceState::DepthStencilRead : rhi::ResourceState::ShaderResource;
        case ResourceAccess::Write:
            return isDepth ? rhi::ResourceState::DepthStencilWrite : rhi::ResourceState::RenderTarget;
        case ResourceAccess::ReadWrite:
            return isDepth ? rhi::ResourceState::DepthStencilWrite : rhi::ResourceState::RenderTarget;
        default:
            return rhi::ResourceState::Undefined;
    }
}

// ============================================================
// 编译阶段
// ============================================================

void RenderGraph::Compile() {
    HE_CORE_INFO("RenderGraph::Compile — {} passes, {} resources", m_Passes.size(), m_Resources.size());

    BuildDependencies();
    TopologicalSort();
    DeriveBarriers();
    CullDeadPasses();
    ApplyAliasing();

    // 异步调度分析（在 Barrier 推导之后，因为需要知道资源状态）
    if (m_AsyncComputeEnabled) {
        ScheduleAsyncPasses();
    }

    m_Compiled = true;
}

void RenderGraph::BuildDependencies() {
    for (auto& p : m_Passes) { p.dependencies.clear(); p.order = 0; }

    for (usize i = 0; i < m_Passes.size(); ++i) {
        for (usize j = 0; j < i; ++j) {
            bool depends = false;
            // RAW: Pass[i] reads what Pass[j] wrote
            for (auto& r : m_Passes[i].reads)
                for (auto& w : m_Passes[j].writes)
                    if (r.handle == w.handle) { depends = true; break; }
            if (depends) { m_Passes[i].dependencies.push_back(&m_Passes[j]); continue; }
            // WAW: Pass[i] writes what Pass[j] wrote
            for (auto& w1 : m_Passes[i].writes)
                for (auto& w2 : m_Passes[j].writes)
                    if (w1.handle == w2.handle) { depends = true; break; }
            if (depends) { m_Passes[i].dependencies.push_back(&m_Passes[j]); continue; }
            // WAR: Pass[i] writes what Pass[j] reads (ordering constraint)
            for (auto& w : m_Passes[i].writes)
                for (auto& r : m_Passes[j].reads)
                    if (w.handle == r.handle) { depends = true; break; }
            if (depends) m_Passes[i].dependencies.push_back(&m_Passes[j]);
        }
    }
}

void RenderGraph::TopologicalSort() {
    m_PassOrder.clear();
    std::vector<u32> inDegree(m_Passes.size(), 0);
    for (auto& p : m_Passes)
        inDegree[p.order] = static_cast<u32>(p.dependencies.size());

    // Recalculate inDegree properly
    for (usize i = 0; i < m_Passes.size(); ++i)
        inDegree[i] = static_cast<u32>(m_Passes[i].dependencies.size());

    std::vector<PassNode*> queue;
    for (usize i = 0; i < m_Passes.size(); ++i)
        if (inDegree[i] == 0) queue.push_back(&m_Passes[i]);

    while (!queue.empty()) {
        PassNode* node = queue.back();
        queue.pop_back();
        node->order = static_cast<u32>(m_PassOrder.size());
        m_PassOrder.push_back(node);

        for (usize i = 0; i < m_Passes.size(); ++i) {
            for (auto* dep : m_Passes[i].dependencies) {
                if (dep == node && --inDegree[i] == 0)
                    queue.push_back(&m_Passes[i]);
            }
        }
    }
}

void RenderGraph::DeriveBarriers() {
    HE_CORE_INFO("RenderGraph::DeriveBarriers — {} passes in order", m_PassOrder.size());

    // 初始化所有资源状态为 Undefined
    for (auto& s : m_ResourceStates)
        s = {rhi::ResourceState::Undefined, false};

    // 标记深度资源
    for (usize i = 0; i < m_Resources.size(); ++i) {
        bool isDepth = (u32(m_Resources[i].textureUsage) & u32(rhi::TextureUsage::DepthStencil)) != 0;
        m_ResourceStates[i].isDepth = isDepth;
    }

    // 按执行顺序遍历 Pass，推导 Barrier
    for (auto* pass : m_PassOrder) {
        pass->preBarriers.clear();

        // 为每个读/写资源检查当前状态
        auto checkResource = [&](ResourceHandle h, ResourceAccess access) {
            if (h >= m_ResourceStates.size()) return;
            auto& cur = m_ResourceStates[h];
            rhi::ResourceState needed = AccessToState(access, cur.isDepth);
            if (cur.layout == rhi::ResourceState::Undefined) {
                // 首次使用：从 Undefined 过渡，不需要显式 Barrier
                cur.layout = needed;
                return;
            }
            if (cur.layout != needed) {
                // 需要 Barrier
                BarrierRecord br;
                br.handle   = h;
                br.srcState = cur.layout;
                br.dstState = needed;
                // 推导合适的管线阶段
                if (cur.layout == rhi::ResourceState::DepthStencilWrite ||
                    cur.layout == rhi::ResourceState::RenderTarget)
                    br.srcStage = rhi::PipelineStage::ColorAttachmentOutput;
                else
                    br.srcStage = rhi::PipelineStage::FragmentShader;
                br.dstStage = rhi::PipelineStage::FragmentShader;
                if (needed == rhi::ResourceState::RenderTarget ||
                    needed == rhi::ResourceState::DepthStencilWrite)
                    br.dstStage = rhi::PipelineStage::ColorAttachmentOutput;

                pass->preBarriers.push_back(br);
                cur.layout = needed;
            }
        };

        for (auto& r : pass->reads)  checkResource(r.handle, r.access);
        for (auto& w : pass->writes) checkResource(w.handle, w.access);
    }
}

void RenderGraph::CullDeadPasses() {
    // 标记被后续 Pass 消费的资源（导入资源 + BackBuffer 永不被裁剪）
    std::unordered_set<ResourceHandle> consumed;
    for (auto* pass : m_PassOrder)
        for (auto& r : pass->reads)
            consumed.insert(r.handle);
    for (auto& [h, _] : m_ImportedTextures) consumed.insert(h);
    if (m_BackBufferHandle != kInvalidHandle) consumed.insert(m_BackBufferHandle);

    // 移除输出未被消费的 Pass
    std::vector<PassNode*> alive;
    for (auto* pass : m_PassOrder) {
        bool hasOutput = false;
        for (auto& w : pass->writes) {
            if (consumed.count(w.handle)) { hasOutput = true; break; }
        }
        if (hasOutput || pass->writes.empty())
            alive.push_back(pass);
        else
            HE_CORE_INFO("  RG cull dead pass: {}", pass->name);
    }
    m_PassOrder = std::move(alive);
}

void RenderGraph::ApplyAliasing() {
    HE_CORE_INFO("RenderGraph::ApplyAliasing — {} resources", m_Resources.size());

    // 简化版贪心别名：计算每个资源的首/末使用 pass
    struct Lifetime { u32 first = ~0u; u32 last = 0; };
    std::vector<Lifetime> lifetimes(m_Resources.size());

    for (auto* pass : m_PassOrder) {
        u32 ord = pass->order;
        for (auto& r : pass->reads) {
            lifetimes[r.handle].first = std::min(lifetimes[r.handle].first, ord);
            lifetimes[r.handle].last  = std::max(lifetimes[r.handle].last, ord);
        }
        for (auto& w : pass->writes) {
            lifetimes[w.handle].first = std::min(lifetimes[w.handle].first, ord);
            lifetimes[w.handle].last  = std::max(lifetimes[w.handle].last, ord);
        }
    }

    // 贪心合并非重叠区间
    struct Pool { u64 size = 0; std::vector<std::pair<u32,u32>> used; };
    std::vector<Pool> pools;
    for (usize i = 0; i < m_Resources.size(); ++i) {
        if (lifetimes[i].first == ~0u) continue;  // 未使用
        u64 needed = m_Resources[i].type == ResourceType::Buffer
                     ? m_Resources[i].bufferSize
                     : u64(m_Resources[i].width) * m_Resources[i].height * 8;  // 粗略估算

        bool placed = false;
        for (auto& pool : pools) {
            bool overlap = false;
            for (auto& [a, b] : pool.used)
                if (!(lifetimes[i].last < a || lifetimes[i].first > b))
                    { overlap = true; break; }
            if (!overlap) {
                m_AliasInfo[i].offset = pool.size;
                m_AliasInfo[i].poolId = static_cast<u32>(&pool - &pools[0]) + 1;
                pool.used.push_back({lifetimes[i].first, lifetimes[i].last});
                pool.size = std::max(pool.size, pool.size + needed);
                placed = true;
                break;
            }
        }
        if (!placed) {
            Pool p;
            p.used.push_back({lifetimes[i].first, lifetimes[i].last});
            p.size = needed;
            pools.push_back(p);
            m_AliasInfo[i].offset = 0;
            m_AliasInfo[i].poolId = static_cast<u32>(pools.size());
        }
    }

    u64 totalNaive = 0, totalAliased = 0;
    for (usize i = 0; i < m_Resources.size(); ++i) {
        if (lifetimes[i].first != ~0u) {
            totalNaive += m_Resources[i].type == ResourceType::Buffer
                          ? m_Resources[i].bufferSize
                          : u64(m_Resources[i].width) * m_Resources[i].height * 8;
        }
    }
    for (auto& p : pools) totalAliased += p.size;
    HE_CORE_INFO("  RG alias: {} pools, {}→{} bytes saved ({:.0f}%)",
                 pools.size(), totalNaive, totalAliased,
                 totalNaive > 0 ? (1.0 - double(totalAliased)/totalNaive) * 100 : 0.0);
}

// ============================================================
// 执行阶段
// ============================================================

void RenderGraph::Execute(rhi::IRHICommandList* cmdList, rhi::IRHIDevice* device) {
    if (!m_Compiled) Compile();

    HE_CORE_INFO("RenderGraph::Execute — {} passes", m_PassOrder.size());

    // 每帧更新 SwapChain BackBuffer
    if (m_SwapChain && m_BackBufferHandle != kInvalidHandle) {
        u32 idx = m_SwapChain->GetCurrentBackBufferIndex();
        void* bbView = m_SwapChain->GetCurrentBackBufferView();
        m_Resources[m_BackBufferHandle].width  = m_SwapChain->GetWidth();
        m_Resources[m_BackBufferHandle].height = m_SwapChain->GetHeight();
        // Store backbuffer view for barrier/pass use
        m_ImportedTextures[m_BackBufferHandle] = reinterpret_cast<rhi::IRHITexture*>(bbView);
    }

    TArray<std::unique_ptr<rhi::IRHITexture>> textures(m_Resources.size());
    TArray<std::unique_ptr<rhi::IRHIBuffer>>  buffers(m_Resources.size());

    for (usize i = 0; i < m_Resources.size(); ++i) {
        if (m_ImportedTextures.count(static_cast<ResourceHandle>(i))) continue;
        auto& desc = m_Resources[i];
        if (desc.type == ResourceType::Texture && desc.width > 0) {
            rhi::TextureDesc texDesc;
            texDesc.width  = desc.width;
            texDesc.height = desc.height;
            texDesc.format = desc.format;
            texDesc.usage  = desc.textureUsage;
            textures[i] = device->CreateTexture(texDesc);
        } else if (desc.type == ResourceType::Buffer && desc.bufferSize > 0) {
            rhi::BufferDesc bufDesc;
            bufDesc.size  = desc.bufferSize;
            bufDesc.usage = desc.bufferUsage;
            buffers[i] = device->CreateBuffer(bufDesc);
        }
    }

    // 按队列类型分支执行
    if (m_AsyncComputeEnabled && HasComputePasses()) {
        // AsyncCompute 多阶段提交：自动拆分 Graphics/Compute 到独立命令列表
        ExecuteWithAsyncCompute(cmdList, device, textures, buffers);
    } else {
        // 单队列执行（传统路径）
        if (m_Profiler) m_Profiler->BeginFrame(cmdList);
        u32 passIdx = 0;

        for (auto* pass : m_PassOrder) {
            // 调试标签：每个 Pass 包裹 begin/end label（RenderDoc 中可见）
            cmdList->BeginDebugLabel(pass->name.c_str());

            for (auto& br : pass->preBarriers) {
                if (br.srcState == br.dstState) continue;
                u32 idx = br.handle;
                rhi::IRHITexture* tex = (idx < textures.size()) ? textures[idx].get() : nullptr;
                if (!tex && m_ImportedTextures.count(br.handle))
                    tex = m_ImportedTextures[br.handle];
                if (tex) {
                    cmdList->PipelineBarrier(br.srcStage, br.dstStage, br.srcState, br.dstState, tex);
                }
            }
            if (m_Profiler) m_Profiler->BeginPass(cmdList, passIdx, pass->name);
            if (pass->execute) pass->execute(cmdList);
            if (m_Profiler) m_Profiler->EndPass(cmdList, passIdx);

            cmdList->EndDebugLabel();
            passIdx++;
        }

        if (m_Profiler) m_Profiler->EndFrame(device);
    }
}

// ============================================================
// AsyncCompute 辅助方法
// ============================================================

bool RenderGraph::HasComputePasses() const {
    for (auto* pass : m_PassOrder) {
        if (pass->queueHint == RGPassQueue::Compute) return true;
    }
    return false;
}

// ============================================================
// ExecuteWithAsyncCompute — 多阶段提交
// 将 Pass 按队列拆分为三段：
//   Phase 1: Graphics 命令列表 #1（Shadow + 前期 Pass）
//   Phase 2: Compute 命令列表（GPUCull, SSAO, AutoExposure 等）
//   Phase 3: Graphics 命令列表 #2（GBuffer, Lighting, PostProcess）
// ============================================================

void RenderGraph::ExecuteWithAsyncCompute(rhi::IRHICommandList* mainCmd,
    rhi::IRHIDevice* device,
    const TArray<std::unique_ptr<rhi::IRHITexture>>& textures,
    const TArray<std::unique_ptr<rhi::IRHIBuffer>>& /*buffers*/) {

    HE_CORE_INFO("RenderGraph::ExecuteWithAsyncCompute — {} passes", m_PassOrder.size());

    // 辅助: 根据资源 handle 获取纹理指针
    auto getTexture = [&](ResourceHandle h) -> rhi::IRHITexture* {
        if (h < textures.size() && textures[h]) return textures[h].get();
        if (m_ImportedTextures.count(h)) return m_ImportedTextures[h];
        return nullptr;
    };

    u64 timelineValue = m_TimelineBase;

    // ============================================================
    // 简单双命令列表方案：
    // - computeCmd: 收集所有可异步执行的 Compute Pass（仅读上帧数据）
    // - mainCmd:   收集所有 Graphics Pass + 不可异步的 Compute Pass
    //
    // Timeline Semaphore: computeCmd 完成后 signal，mainCmd 提交时 wait
    // mainCmd 上的所有 Pass 完全保持与单队列路径一致的录制顺序，
    // 仅 GPU_Cull 等安全 Pass 被移到异步队列执行。
    // ============================================================

    // 先收集异步 Compute Pass 列表
    std::vector<PassNode*> asyncComputePasses;
    std::vector<PassNode*> mainPasses;
    bool crossedCompute = false;
    for (auto* pass : m_PassOrder) {
        if (pass->queueHint == RGPassQueue::Compute && !crossedCompute) {
            asyncComputePasses.push_back(pass);
        } else {
            crossedCompute = true;  // 第一个非 Compute Pass 之后，所有 Pass 走 mainCmd
            mainPasses.push_back(pass);
        }
    }

    // 创建 Compute 命令列表，录制所有异步 Compute Pass
    auto computeCmd = device->CreateCommandList(rhi::QueueType::Compute);
    computeCmd->Begin();

    u32 passIdx = 0;
    for (auto* pass : asyncComputePasses) {
        computeCmd->BeginDebugLabel(pass->name.c_str());
        for (auto& br : pass->crossQueueAcquire) {
            rhi::IRHITexture* tex = getTexture(br.handle);
            if (!tex) continue;
            computeCmd->QueueOwnershipTransfer(tex,
                rhi::QueueType::Graphics, rhi::QueueType::Compute,
                br.srcState, br.dstState);
        }

        for (auto& br : pass->preBarriers) {
            if (br.srcState == br.dstState) continue;
            rhi::IRHITexture* tex = getTexture(br.handle);
            if (tex) computeCmd->PipelineBarrier(br.srcStage, br.dstStage, br.srcState, br.dstState, tex);
        }

        if (m_Profiler) m_Profiler->BeginPass(computeCmd.get(), passIdx, pass->name);
        if (pass->execute) pass->execute(computeCmd.get());
        if (m_Profiler) m_Profiler->EndPass(computeCmd.get(), passIdx);

        for (auto& br : pass->crossQueueRelease) {
            rhi::IRHITexture* tex = getTexture(br.handle);
            if (!tex) continue;
            computeCmd->QueueOwnershipTransfer(tex,
                rhi::QueueType::Compute, rhi::QueueType::Graphics,
                br.srcState, br.dstState);
            mainCmd->QueueOwnershipTransfer(tex,
                rhi::QueueType::Compute, rhi::QueueType::Graphics,
                br.srcState, br.dstState);
        }
        computeCmd->EndDebugLabel();
        passIdx++;
    }

    // 设置 Compute → Graphics 同步点
    if (!asyncComputePasses.empty()) {
        computeCmd->SetTimelineSignal(m_CrossQueueFence, timelineValue);
        mainCmd->SetTimelineWait(m_CrossQueueFence, timelineValue);
    }

    computeCmd->End();
    computeCmd->Submit();  // 异步提交到 Compute 队列

    // ============================================================
    // 所有主 Pass 录制在 mainCmd 上（与单队列路径完全一致）
    // ============================================================
    if (m_Profiler) m_Profiler->BeginFrame(mainCmd);

    for (auto* pass : mainPasses) {
        mainCmd->BeginDebugLabel(pass->name.c_str());
        for (auto& br : pass->preBarriers) {
            if (br.srcState == br.dstState) continue;
            rhi::IRHITexture* tex = getTexture(br.handle);
            if (tex) mainCmd->PipelineBarrier(br.srcStage, br.dstStage, br.srcState, br.dstState, tex);
        }

        if (m_Profiler) m_Profiler->BeginPass(mainCmd, passIdx, pass->name);
        if (pass->execute) pass->execute(mainCmd);
        if (m_Profiler) m_Profiler->EndPass(mainCmd, passIdx);
        mainCmd->EndDebugLabel();
        passIdx++;
    }

    if (m_Profiler) m_Profiler->EndFrame(device);
    // 注意：mainCmd 由外部框架管理 Begin/End/Submit，此处不操作
}

// ============================================================
// AsyncCompute 调度分析
// ============================================================

void RenderGraph::ScheduleAsyncPasses() {
    HE_CORE_INFO("RenderGraph::ScheduleAsyncPasses — analyzing async compute candidates");

    for (auto* pass : m_PassOrder) {
        if (pass->queueHint != RGPassQueue::Compute) continue;

        // 检查资源依赖: 该 Pass 的输出是否被后面的 Graphics Pass 读取
        bool canAsync = true;
        for (auto& write : pass->writes) {
            for (auto* subsequentPass : GetSubsequentPasses(pass)) {
                if (subsequentPass->queueHint == RGPassQueue::Compute) continue; // Comp-to-Comp 无需转移
                for (auto& read : subsequentPass->reads) {
                    if (write.handle == read.handle) {
                        canAsync = false; // 有 RAW 依赖，不可完全异步
                        break;
                    }
                }
                if (!canAsync) break;
            }
        }

        if (canAsync) {
            pass->asyncSchedule = true;
            // 自动插入跨队列 Barrier
            InsertCrossQueueBarrier(pass,
                rhi::QueueType::Graphics, rhi::QueueType::Compute);
            HE_CORE_INFO("  Async pass '{}': 可并行", pass->name);
        } else {
            pass->requiresSync = true;
            HE_CORE_INFO("  Async pass '{}': 存在依赖，需同步点", pass->name);
        }
    }
}

std::vector<PassNode*> RenderGraph::GetSubsequentPasses(PassNode* pass) {
    std::vector<PassNode*> result;
    bool found = false;
    for (auto* p : m_PassOrder) {
        if (found) result.push_back(p);
        if (p == pass) found = true;
    }
    return result;
}

void RenderGraph::InsertCrossQueueBarrier(PassNode* pass,
                                           rhi::QueueType srcQueue,
                                           rhi::QueueType dstQueue) {
    // 为 Pass 的 reads 插入 Acquire：从 Graphics 获取资源所有权
    for (auto& r : pass->reads) {
        if (r.handle >= m_ResourceStates.size()) continue;
        auto& state = m_ResourceStates[r.handle];
        if (state.layout == rhi::ResourceState::Undefined) continue;

        BarrierRecord br;
        br.handle   = r.handle;
        br.srcState = state.layout;
        br.dstState = state.layout;  // 保持相同状态，只转移所有权
        br.srcStage = rhi::PipelineStage::BottomOfPipe;
        br.dstStage = rhi::PipelineStage::ComputeShader;
        pass->crossQueueAcquire.push_back(br);
    }

    // 为 Pass 的 writes 插入 Release：释放回 Graphics
    for (auto& w : pass->writes) {
        if (w.handle >= m_ResourceStates.size()) continue;
        auto& state = m_ResourceStates[w.handle];

        // 推导写入后的状态
        rhi::ResourceState writtenState = AccessToState(w.access, state.isDepth);

        BarrierRecord br;
        br.handle   = w.handle;
        br.srcState = writtenState;
        br.dstState = writtenState;  // 保持相同状态，只转移所有权
        br.srcStage = rhi::PipelineStage::ComputeShader;
        br.dstStage = rhi::PipelineStage::TopOfPipe;
        pass->crossQueueRelease.push_back(br);

        // 更新追踪状态
        state.layout = writtenState;
    }
}

void RenderGraph::Reset() {
    m_Resources.clear();
    m_ResourceStates.clear();
    m_AliasInfo.clear();
    m_Passes.clear();
    m_PassOrder.clear();
    m_ImportedTextures.clear();
    m_BackBufferHandle = kInvalidHandle;
    m_Compiled = false;
    m_AsyncComputeEnabled = false;
    HE_CORE_INFO("RenderGraph cleared");
}

} // namespace he::render
