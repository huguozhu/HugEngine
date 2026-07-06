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
        rd.width        = 1920;
        rd.height       = 1080;
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

    // GPU Profiler
    if (m_Profiler) m_Profiler->BeginFrame(cmdList);
    u32 passIdx = 0;

    for (auto* pass : m_PassOrder) {
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
        passIdx++;
    }

    if (m_Profiler) m_Profiler->EndFrame(device);
}

// ============================================================
// 双队列 Execute（AsyncCompute 模式）
// ============================================================

void RenderGraph::Execute(rhi::IRHICommandList* graphicsCmd,
                           rhi::IRHICommandList* computeCmd,
                           rhi::IRHIDevice* device) {
    if (!m_Compiled) Compile();

    HE_CORE_INFO("RenderGraph::Execute (Async) — {} passes", m_PassOrder.size());

    // 每帧更新 SwapChain BackBuffer
    if (m_SwapChain && m_BackBufferHandle != kInvalidHandle) {
        u32 idx = m_SwapChain->GetCurrentBackBufferIndex();
        void* bbView = m_SwapChain->GetCurrentBackBufferView();
        m_Resources[m_BackBufferHandle].width  = m_SwapChain->GetWidth();
        m_Resources[m_BackBufferHandle].height = m_SwapChain->GetHeight();
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

    // 辅助: 根据资源 handle 获取纹理指针
    auto getTexture = [&](ResourceHandle h) -> rhi::IRHITexture* {
        if (h < textures.size() && textures[h]) return textures[h].get();
        if (m_ImportedTextures.count(h)) return m_ImportedTextures[h];
        return nullptr;
    };

    // GPU Profiler（仍在 Graphics 命令列表上打标签）
    if (m_Profiler) m_Profiler->BeginFrame(graphicsCmd);
    u32 passIdx = 0;

    for (auto* pass : m_PassOrder) {
        bool isComputePass = (pass->queueHint == RGPassQueue::Compute && m_AsyncComputeEnabled);
        rhi::IRHICommandList* cmdList = isComputePass ? computeCmd : graphicsCmd;

        // 1. 跨队列 Acquire（Graphics → Compute）
        //    Vulkan QFOT 需要两个 Barrier:
        //      a) RELEASE 在源队列 (Graphics) 上录制
        //      b) ACQUIRE 在目标队列 (Compute) 上录制
        if (isComputePass) {
            for (auto& br : pass->crossQueueAcquire) {
                if (br.srcState == br.dstState) continue;
                rhi::IRHITexture* tex = getTexture(br.handle);
                if (!tex) continue;
                // RELEASE on Graphics: 释放所有权，Compute 队列可以获取
                graphicsCmd->QueueOwnershipTransfer(tex,
                    rhi::QueueType::Graphics, rhi::QueueType::Compute,
                    br.srcState, br.dstState);
                // ACQUIRE on Compute: 获取所有权，准备使用
                computeCmd->QueueOwnershipTransfer(tex,
                    rhi::QueueType::Graphics, rhi::QueueType::Compute,
                    br.srcState, br.dstState);
            }
        }

        // 2. 常规 Barrier（布局转换）— 在当前 Pass 的命令列表上录制
        for (auto& br : pass->preBarriers) {
            if (br.srcState == br.dstState) continue;
            rhi::IRHITexture* tex = getTexture(br.handle);
            if (tex) {
                cmdList->PipelineBarrier(br.srcStage, br.dstStage,
                                         br.srcState, br.dstState, tex);
            }
        }

        // 3. 执行 Pass
        if (m_Profiler) m_Profiler->BeginPass(cmdList, passIdx, pass->name);
        if (pass->execute) pass->execute(cmdList);
        if (m_Profiler) m_Profiler->EndPass(cmdList, passIdx);
        passIdx++;

        // 4. 跨队列 Release（Compute → Graphics）
        //    QFOT 两个 Barrier: RELEASE on Compute + ACQUIRE on Graphics
        if (isComputePass) {
            for (auto& br : pass->crossQueueRelease) {
                if (br.srcState == br.dstState) continue;
                rhi::IRHITexture* tex = getTexture(br.handle);
                if (!tex) continue;
                // RELEASE on Compute: 释放所有权回 Graphics
                computeCmd->QueueOwnershipTransfer(tex,
                    rhi::QueueType::Compute, rhi::QueueType::Graphics,
                    br.srcState, br.dstState);
                // ACQUIRE on Graphics: 重新获取所有权
                graphicsCmd->QueueOwnershipTransfer(tex,
                    rhi::QueueType::Compute, rhi::QueueType::Graphics,
                    br.srcState, br.dstState);
            }
        }
    }

    if (m_Profiler) m_Profiler->EndFrame(device);
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
