#include "RenderGraph.h"
#include "Profiler/ProfilerManager.h"
#include "Core/Log.h"
#include "RHI/TextureLayoutTracker.h"   // 查询导入纹理的真实布局（跨帧持久资源）

#include <cstdlib>   // std::getenv（HE_TRACE_PASSES / HE_CPU_PASSES 诊断开关）
#include <chrono>    // 步骤 37：逐 pass CPU 录制耗时
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include <algorithm>
#include <unordered_set>

// 【日志级别】RenderGraph 每帧都会重建（创建 → 声明资源/pass → Compile → 屏障推导 → 别名 → 执行），
// 这些过程日志此前用 INFO，每帧刷约 70 行，淹没其它日志且拖慢帧率；改为 TRACE，排查时把日志级别调到 Trace 即可看到
namespace he::render {

// 【P5】把 RHI 资源状态转成能进日志的短名（异步计算的"交接状态"读数要用）
static const char* StateName(rhi::ResourceState s) {
    switch (s) {
        case rhi::ResourceState::Undefined:               return "UNDEFINED";
        case rhi::ResourceState::Common:                  return "COMMON";
        case rhi::ResourceState::VertexAndConstantBuffer: return "VB_CB";
        case rhi::ResourceState::IndexBuffer:             return "IB";
        case rhi::ResourceState::RenderTarget:            return "RT";
        case rhi::ResourceState::DepthStencilRead:        return "DEPTH_READ";
        case rhi::ResourceState::DepthStencilWrite:       return "DEPTH_WRITE";
        case rhi::ResourceState::ShaderResource:          return "SRV";
        case rhi::ResourceState::UnorderedAccess:         return "UAV";
        case rhi::ResourceState::CopySrc:                 return "COPY_SRC";
        case rhi::ResourceState::CopyDst:                 return "COPY_DST";
        case rhi::ResourceState::Present:                 return "PRESENT";
        case rhi::ResourceState::IndirectArgument:        return "INDIRECT";
        default:                                          return "?";
    }
}

RenderGraph::RenderGraph() {
    HE_CORE_TRACE("RenderGraph v2 created (auto-barrier + aliasing)");
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
    HE_CORE_TRACE("  RG tex #{}: {} ({}x{})", h, name, desc.width, desc.height);
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
    HE_CORE_TRACE("  RG buf #{}: {} ({} bytes)", h, name, desc.size);
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
    HE_CORE_TRACE("  RG import #{}: {}", h, name);
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
    HE_CORE_TRACE("  RG pass: {} [{}]", name,
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
        case ResourceAccess::UAV:
            // RT/Compute 效果输出：存储图像 UAV（非深度资源）
            return isDepth ? rhi::ResourceState::DepthStencilWrite : rhi::ResourceState::UnorderedAccess;
        default:
            return rhi::ResourceState::Undefined;
    }
}

// ============================================================
// 编译阶段
// ============================================================

void RenderGraph::Compile() {
    HE_CORE_TRACE("RenderGraph::Compile — {} passes, {} resources", m_Passes.size(), m_Resources.size());

    BuildDependencies();
    TopologicalSort();
    // 【P5】屏障推导之前先分组：计算列表上的 pass 会**先于**全部图形 pass 执行，屏障必须按
    // 这个真实执行顺序推导（否则提前执行的那几个 pass 会拿到"上一个图形 pass 之后"的起始状态，
    // 与它真正看到的状态不符）。
    MarkAsyncQueueGroups();
    DeriveBarriers();
    CullDeadPasses();
    ApplyAliasing();

    // 异步调度分析（在 Barrier 推导之后，因为需要知道资源状态）
    if (m_AsyncComputeEnabled) {
        ScheduleAsyncPasses();
    }

    m_Compiled = true;
}

// ============================================================
// 【P5】MarkAsyncQueueGroups — 按队列归属分组
//
// 旧实现在 ExecuteWithAsyncCompute 里只从 `m_PassOrder` **头部**切连续的 Compute pass，
// 而编译后第一个 pass 是图形 hint 的 ToneMap（它的 reads 声明为空，内部自己发屏障 ⇒
// 拓扑序上没有依赖）⇒ 「异步前缀」恒为空、异步 pass 数恒为 0。
//
// 这里改成先做分组决定（不只看头部），一个 Compute hint 的 pass 只有在同时满足下面两条
// 时才被提到计算列表：
//
//   ① **依赖闭包全在异步组内**：它的所有前驱 pass 也都在异步组里。等价于"它的输入要么来自
//      上一帧，要么来自另一个异步 pass"，因此把它挪到本帧**最前面**执行读到的内容与原地
//      执行一致。第一个被选中的 pass 必然没有前驱 ⇒ 天然从"无依赖"的那些开始。
//      这条同时挡住了 WAR 方向的问题：如果有图形 pass 在本帧读了它写的资源，BuildDependencies
//      会把它记成前驱，于是它进不了异步组（不会被提前到那个图形 pass 之前写）。
//
//   ② **只碰跨帧持久资源**（导入纹理）：帧图自己创建的资源是**瞬态**的，D3D12 后端会把
//      生命周期不重叠的瞬态纹理放在同一段内存上（内存别名，见 ApplyAliasing / CreateTransientTexture）。
//      把这类 pass 提前到帧首，生命周期顺序就与别名分配时用的拓扑序不符 ⇒ 可能和别的 pass
//      共用内存而互相踩。持久资源（GBuffer 深度、光照 HDR、交换链）不参与别名，提前用是安全的。
//
// 另外，被选中的 pass 只读上一帧的内容，所以计算列表提交前必须让计算队列先等"此前的图形
// 工作完成"（见 D3D12Device::SubmitQueueHandoff），这一点由 RHI 侧保证。
// ============================================================
void RenderGraph::MarkAsyncQueueGroups() {
    // 每帧重新决定（RenderGraph 本身每帧重建，但 Compile 也可能被重入）
    for (auto* p : m_PassOrder) p->hoistedAsync = false;
    if (!m_AsyncComputeEnabled) return;

    std::vector<PassNode*> chosen;
    auto inChosen = [&chosen](PassNode* p) {
        return std::find(chosen.begin(), chosen.end(), p) != chosen.end();
    };

    for (auto* pass : m_PassOrder) {
        if (pass->queueHint != RGPassQueue::Compute) continue;

        // ① 依赖闭包必须在异步组内
        bool ok = true;
        for (auto* dep : pass->dependencies) {
            if (!inChosen(dep)) { ok = false; break; }
        }
        if (!ok) {
            HE_CORE_TRACE("  Async 分组: '{}' 留在图形列表（前驱不在异步组内）", pass->name);
            continue;
        }

        // ② 读写的资源必须全是跨帧持久资源
        for (auto& r : pass->reads) {
            if (!IsPersistentResource(r.handle)) { ok = false; break; }
        }
        if (ok) {
            for (auto& w : pass->writes) {
                if (!IsPersistentResource(w.handle)) { ok = false; break; }
            }
        }
        if (!ok) {
            HE_CORE_TRACE("  Async 分组: '{}' 留在图形列表（使用了帧图瞬态资源）", pass->name);
            continue;
        }

        pass->hoistedAsync = true;
        chosen.push_back(pass);
        HE_CORE_TRACE("  Async 分组: '{}' → 计算列表", pass->name);
    }
}

bool RenderGraph::IsPersistentResource(ResourceHandle handle) const {
    // 导入纹理 = 跨帧持久（交换链 / GBuffer / 光照目标等）；帧图 CreateTexture 出来的是瞬态
    return m_ImportedTextures.count(handle) != 0;
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
    HE_CORE_TRACE("RenderGraph::DeriveBarriers — {} passes in order", m_PassOrder.size());

    // 初始化所有资源状态为 Undefined
    for (auto& s : m_ResourceStates)
        s = {rhi::ResourceState::Undefined, false};

    // 标记深度资源
    for (usize i = 0; i < m_Resources.size(); ++i) {
        bool isDepth = (u32(m_Resources[i].textureUsage) & u32(rhi::TextureUsage::DepthStencil)) != 0;
        m_ResourceStates[i].isDepth = isDepth;
    }

    // 单个 Pass 的屏障推导：从"当前资源状态"推到本 pass 需要的状态
    auto deriveOne = [&](PassNode* pass) {
        pass->preBarriers.clear();

        // 为每个读/写资源检查当前状态
        auto checkResource = [&](ResourceHandle h, ResourceAccess access) {
            if (h >= m_ResourceStates.size()) return;
            auto& cur = m_ResourceStates[h];
            rhi::ResourceState needed = AccessToState(access, cur.isDepth);
            if (cur.layout == rhi::ResourceState::Undefined) {
                // ── 本帧首次使用 ──
                // 导入纹理是**跨帧持久**资源（GBuffer depth、Lighting HDR depth 等）：
                // 它们上一帧结束时停在某个布局（引擎的 render pass 会把深度留在
                // DEPTH_STENCIL_READ_ONLY），此时若直接假设 Undefined 并跳过 barrier，
                // 真实布局与本帧记录就会分歧 —— 实测每帧各 1 次
                // VUID-...-initialLayout-00900 与 VUID-...-oldLayout-01197。
                // 因此先向 RHI 查询该视图记录的真实布局，只有真正没记录过（新资源）才按
                // Undefined 处理。
                rhi::ResourceState tracked;
                if (m_ImportedTextures.count(h)) {
                    rhi::IRHITexture* tex = m_ImportedTextures[h];
                    if (tex && rhi::QueryTrackedTextureLayout(tex->GetNativeHandle(), tracked) &&
                        tracked != rhi::ResourceState::Undefined) {
                        cur.layout = tracked;   // 用真实布局继续走下面的"需要 Barrier"分支
                    }
                }
                if (cur.layout == rhi::ResourceState::Undefined) {
                    // 真正首次使用（新建纹理 / 从未被转换过）。
                    // 【§0.6.2 校验修复】读用途**必须**补一次 Undefined→needed 的转换：
                    // 导入纹理在窗口尺寸变化后是全新创建的，真实布局就是 UNDEFINED，而此前
                    // "追踪器没记录 ⇒ 假设已在目标布局 ⇒ 跳过 barrier"的优化会让采样它的
                    // draw 报 VUID-vkCmdDraw-None-09600（实测：04/06 换尺寸后的第一帧，
                    // 新建的 GBuffer 深度被 SSGI/Decal/Lighting 采样，各 1 条）。
                    // 写用途仍交给 render pass / UAV 自己初始化（语义上也不需要保留旧内容）。
                    if (access != ResourceAccess::Read) {
                        cur.layout = needed;
                        return;
                    }
                    // 读用途：落到下面统一补 barrier（srcState=Undefined，从 UNDEFINED 转换永远合法）
                }
            }
            if (cur.layout != needed) {
                // 需要 Barrier
                BarrierRecord br;
                br.handle   = h;
                br.srcState = cur.layout;
                br.dstState = needed;
                // 推导合适的管线阶段（UAV 资源由 RT/Compute 着色器读写 → RayTracingShader 阶段）
                bool srcIsUAV = (cur.layout == rhi::ResourceState::UnorderedAccess);
                bool dstIsUAV = (needed == rhi::ResourceState::UnorderedAccess);
                if (cur.layout == rhi::ResourceState::DepthStencilWrite ||
                    cur.layout == rhi::ResourceState::RenderTarget)
                    br.srcStage = rhi::PipelineStage::ColorAttachmentOutput;
                else if (srcIsUAV)
                    br.srcStage = rhi::PipelineStage::RayTracingShader;
                else
                    br.srcStage = rhi::PipelineStage::FragmentShader;
                br.dstStage = rhi::PipelineStage::FragmentShader;
                if (needed == rhi::ResourceState::RenderTarget ||
                    needed == rhi::ResourceState::DepthStencilWrite)
                    br.dstStage = rhi::PipelineStage::ColorAttachmentOutput;
                else if (dstIsUAV)
                    br.dstStage = rhi::PipelineStage::RayTracingShader;

                pass->preBarriers.push_back(br);
                cur.layout = needed;
            }
        };

        for (auto& r : pass->reads)  checkResource(r.handle, r.access);
        for (auto& w : pass->writes) checkResource(w.handle, w.access);

        // ── 深度附件写之后的真实布局修正 ──
        // 引擎创建 render pass 时把深度附件的 finalLayout 声明为
        // DEPTH_STENCIL_READ_ONLY_OPTIMAL（"写完即可采样"，见 VulkanPipeline.cpp），
        // 因此一个写深度的 pass 结束后，该图的真实布局是 READ_ONLY 而不是 ATTACHMENT。
        // 若模型仍记为 ATTACHMENT，后续 barrier 会声明错误的 oldLayout，实测产生：
        //   · VUID-VkImageMemoryBarrier-oldLayout-01197（barrier 声明 oldLayout 不符）
        //   · VUID-vkCmdBeginRenderPass-initialLayout-00900（下次作为深度附件时布局不符）
        // 这里让模型与 render pass 的实际 finalLayout 保持一致。
        for (auto& w : pass->writes) {
            if (w.handle >= m_ResourceStates.size()) continue;
            auto& st = m_ResourceStates[w.handle];
            if (st.isDepth && st.layout == rhi::ResourceState::DepthStencilWrite) {
                st.layout = rhi::ResourceState::DepthStencilRead;
            }
        }
    };

    // ── 【P5】按真实执行顺序推两遍：先计算列表的 pass，再图形列表的 pass ──
    // 计算列表先提交、图形列表经时间线围栏等它，所以"异步 pass 在前"就是真实顺序；
    // 提前执行的那几个 pass 因此会拿到"本帧首次使用"的起始状态（导入纹理从 RHI 记录的
    // 上一帧布局出发），而不是"某个图形 pass 之后"的状态。
    for (auto* pass : m_PassOrder) {
        if (pass->hoistedAsync) deriveOne(pass);
    }
    for (auto* pass : m_PassOrder) {
        if (!pass->hoistedAsync) deriveOne(pass);
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
            HE_CORE_TRACE("  RG cull dead pass: {}", pass->name);
    }
    m_PassOrder = std::move(alive);
}

void RenderGraph::ApplyAliasing() {
    HE_CORE_TRACE("RenderGraph::ApplyAliasing — {} resources", m_Resources.size());

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

    // 记下生命周期：后端（D3D12）据此按真实大小把生命周期不重叠的瞬态纹理放在同一段内存上
    for (usize i = 0; i < m_Resources.size() && i < m_AliasInfo.size(); ++i) {
        m_AliasInfo[i].firstPass = lifetimes[i].first;
        m_AliasInfo[i].lastPass  = lifetimes[i].last;
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
    HE_CORE_TRACE("  RG alias: {} pools, {}→{} bytes saved ({:.0f}%)",
                 pools.size(), totalNaive, totalAliased,
                 totalNaive > 0 ? (1.0 - double(totalAliased)/totalNaive) * 100 : 0.0);
}

// ============================================================
// 执行阶段
// ============================================================

void RenderGraph::Execute(rhi::IRHICommandList* cmdList, rhi::IRHIDevice* device) {
    if (!m_Compiled) Compile();

    HE_CORE_TRACE("RenderGraph::Execute — {} passes", m_PassOrder.size());

    // 帧首：推进瞬态资源分配器（切换 Heap，回收上上帧内存）
    device->AdvanceTransientResources();

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

            // 别名资源：优先使用瞬态内存池（帧内共享 Heap，帧末回收）
            // 非别名资源（如导入/持久资源）：使用独占 VMA 分配
            bool isAliased = (i < m_AliasInfo.size() && m_AliasInfo[i].poolId > 0);
            if (isAliased) {
                // 带上生命周期：支持内存别名的后端让生命周期不重叠的纹理共用内存
                textures[i] = device->CreateTransientTexture(texDesc, m_AliasInfo[i].firstPass,
                                                             m_AliasInfo[i].lastPass);
            }
            // 回退：瞬态分配失败或不支持时使用普通纹理
            if (!textures[i]) {
                textures[i] = device->CreateTexture(texDesc);
            }
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
    // 【P5】返回的是"本帧真的有 pass 会被放到计算列表上"（编译期分组的结果），
    // 而不是"声明了 Compute hint 的 pass 是否存在"——后者为真但都被分组条件挡下时，
    // 走单队列路径即可，不必白建一条空的 Compute 命令列表。
    for (auto* pass : m_PassOrder) {
        if (pass->hoistedAsync) return true;
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

    HE_CORE_TRACE("RenderGraph::ExecuteWithAsyncCompute — {} passes", m_PassOrder.size());

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
    // 【P5】按编译期的分组决定（pass->hoistedAsync）切分，不再只看头部连续段：
    // 计算列表 = 那些"依赖闭包全在异步组内 + 只碰持久资源"的 Compute hint pass，
    // 图形列表 = 其余全部，且保持与单队列路径完全一致的相对顺序。
    std::vector<PassNode*> asyncComputePasses;
    std::vector<PassNode*> mainPasses;
    for (auto* pass : m_PassOrder) {
        if (pass->hoistedAsync) asyncComputePasses.push_back(pass);
        else                    mainPasses.push_back(pass);
    }

    // 创建 Compute 命令列表，录制所有异步 Compute Pass
    auto computeCmd = device->CreateCommandList(rhi::QueueType::Compute);
    computeCmd->BeginLightweight();  // 轻量 Begin：不推进帧计数器，避免延迟销毁提前触发

    // 【P5 可观测性】把切分结果、真实执行顺序、跨队列交接状态与同步点一次性打出来。
    // 前三帧与每 120 帧一次，便于脚本抓读数。
    {
        static u32 s_logCount = 0;
        ++s_logCount;
        if (s_logCount <= 3u || (s_logCount % 120u) == 0u) {
            std::string names;
            for (auto* p : asyncComputePasses) {
                if (!names.empty()) names += " / ";
                names += p->name;
            }
            HE_CORE_INFO("[AsyncCompute] 异步计算 pass {} 个（主列表 pass {} 个）：{}",
                         asyncComputePasses.size(), mainPasses.size(),
                         names.empty() ? "(无)" : names);

            // 真实执行顺序：计算列表的 pass 在前（它们先提交、图形列表等它们），随后是图形列表
            std::string order;
            for (auto* p : asyncComputePasses) {
                if (!order.empty()) order += " → ";
                order += p->name;
                order += "[C]";
            }
            for (auto* p : mainPasses) {
                if (!order.empty()) order += " → ";
                order += p->name;
                order += "[G]";
            }
            if (s_logCount <= 3u) HE_CORE_INFO("[AsyncCompute] 执行顺序：{}", order);

            // 跨队列交接状态：把每个异步 pass 的 acquire/release 记录打出来
            //（RHI 状态名；D3D12 侧实际下发的图形专用状态 → 计算可用状态由 RHI 打印）
            for (auto* p : asyncComputePasses) {
                std::string ho;
                auto add = [&](const BarrierRecord& br, const char* dir) {
                    if (!ho.empty()) ho += "；";
                    ho += dir;
                    const char* resName =
                        (br.handle < m_Resources.size() && !m_Resources[br.handle].name.empty())
                        ? m_Resources[br.handle].name.c_str() : "?";
                    ho += resName;
                    char buf[96];
                    std::snprintf(buf, sizeof(buf), " %s→%s",
                                  StateName(br.srcState), StateName(br.dstState));
                    ho += buf;
                };
                for (auto& br : p->crossQueueAcquire) add(br, "读");
                for (auto& br : p->crossQueueRelease) add(br, "写");
                for (auto& br : p->preBarriers)      add(br, "屏障");
                HE_CORE_INFO("[AsyncCompute] 交接 '{}'：{}", p->name,
                             ho.empty() ? "(无资源交接)" : ho);
            }
            HE_CORE_INFO("[AsyncCompute] 图形队列等待计算队列：时间线围栏值 {}（计算列表先提交、信号在末尾）",
                         static_cast<unsigned long long>(timelineValue));
            // 【Profiler 读数】有异步 pass 时 BeginFrame（重置本帧时间戳查询池 + 读回 2 帧前结果）
            // 必须放在计算列表上：计算列表先提交、先写异步 pass 的时间戳，若主列表随后再重置就把它清掉了。
            HE_CORE_INFO("[AsyncCompute] Profiler 查询池重置落在：{}",
                         m_Profiler ? (asyncComputePasses.empty() ? "图形列表" : "计算列表")
                                    : "（未启用 Profiler）");
        }
    }

    // 【Profiler 帧开始放在计算列表上】BeginFrame 会重置本帧查询池。此前它在主列表上调用，而计算列表
    // 先提交、先写入异步计算 pass 的时间戳，主列表随后的重置把这些时间戳清掉（异步 pass 的耗时恒为 0/无效）。
    // 有异步 pass 时改在计算列表开头重置：主列表经时间线等待排在计算列表之后，写入顺序成立；
    // 没有异步 pass 时两条列表之间没有同步，仍在主列表上重置
    const bool profilerOnCompute = !asyncComputePasses.empty();
    if (m_Profiler && profilerOnCompute) m_Profiler->BeginFrame(computeCmd.get());

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
        // 【诊断】HE_TRACE_PASSES=1 时打印每个 pass 的开始：把 pass 名与校验层报错在时间上对齐，
        // 用于定位"是哪个 pass 触发了布局类校验错误"（见开发计划 §1.3.4）
        if (std::getenv("HE_TRACE_PASSES")) HE_CORE_WARN("[PASS] {}", pass->name);
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
    if (m_Profiler && !profilerOnCompute) m_Profiler->BeginFrame(mainCmd);

    // 【步骤 37】逐 pass 的 **CPU 录制耗时**（与 GPU 耗时无关）：1080p 实测"执行(录制+提交)"一帧要
    // 34~48 ms，而 GPU 各 pass 合计只有 14 ms ⇒ 瓶颈在 CPU 录制这一侧，而"整帧 CPU 时长"定位不到
    // 具体是哪个 pass。故在录制循环里逐 pass 计时，由 `HE_CPU_PASSES=1` 打开、每 120 帧打最重的几个。
    static const bool s_logCpuPasses = (std::getenv("HE_CPU_PASSES") != nullptr);
    static u32  s_cpuFrame = 0;
    static std::vector<std::pair<std::string, double>> s_cpuAcc;
    static double s_cpuTotal = 0.0;

    for (auto* pass : mainPasses) {
        mainCmd->BeginDebugLabel(pass->name.c_str());
        for (auto& br : pass->preBarriers) {
            if (br.srcState == br.dstState) continue;
            rhi::IRHITexture* tex = getTexture(br.handle);
            if (tex) mainCmd->PipelineBarrier(br.srcStage, br.dstStage, br.srcState, br.dstState, tex);
        }

        if (m_Profiler) m_Profiler->BeginPass(mainCmd, passIdx, pass->name);
        // 【诊断】HE_TRACE_PASSES=1：打印 pass 开始，用于与校验层报错对齐定位（见 §1.3.4）
        if (std::getenv("HE_TRACE_PASSES")) HE_CORE_WARN("[PASS] {}", pass->name);
        const auto cpuT0 = std::chrono::steady_clock::now();
        if (pass->execute) pass->execute(mainCmd);
        if (s_logCpuPasses) {
            const double ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - cpuT0).count();
            s_cpuTotal += ms;
            bool found = false;
            for (auto& e : s_cpuAcc) if (e.first == pass->name) { e.second += ms; found = true; break; }
            if (!found) s_cpuAcc.emplace_back(pass->name, ms);
        }
        if (m_Profiler) m_Profiler->EndPass(mainCmd, passIdx);
        mainCmd->EndDebugLabel();
        passIdx++;
    }

    if (s_logCpuPasses && (++s_cpuFrame % 120u) == 0u) {
        std::sort(s_cpuAcc.begin(), s_cpuAcc.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        std::string top;
        char buf[128];
        const u32 kTop = std::min<u32>(6u, (u32)s_cpuAcc.size());
        for (u32 i = 0; i < kTop; ++i) {
            std::snprintf(buf, sizeof(buf), "%s %.2f", s_cpuAcc[i].first.c_str(),
                          s_cpuAcc[i].second / 120.0);
            if (!top.empty()) top += " / ";
            top += buf;
        }
        HE_CORE_INFO("CPU 录制耗时（步骤 37）: 主命令列表每帧合计 {:.3f} ms；最重的 {} 个: {}",
                     s_cpuTotal / 120.0, kTop, top);
        s_cpuAcc.clear();
        s_cpuTotal = 0.0;
    }

    if (m_Profiler) m_Profiler->EndFrame(device);
    // 注意：mainCmd 由外部框架管理 Begin/End/Submit，此处不操作
}

// ============================================================
// AsyncCompute 调度分析
// ============================================================

void RenderGraph::ScheduleAsyncPasses() {
    HE_CORE_TRACE("RenderGraph::ScheduleAsyncPasses — analyzing async compute candidates");

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
            HE_CORE_TRACE("  Async pass '{}': 可并行", pass->name);
        } else {
            pass->requiresSync = true;
            HE_CORE_TRACE("  Async pass '{}': 存在依赖，需同步点", pass->name);
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
    HE_CORE_TRACE("RenderGraph cleared");
}

} // namespace he::render
