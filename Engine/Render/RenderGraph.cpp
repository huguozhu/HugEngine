#include "RenderGraph.h"
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
                                PassExecuteFunc execute) {
    PassNode pass;
    pass.name    = String(name);
    pass.reads   = std::move(reads);
    pass.writes  = std::move(writes);
    pass.execute = std::move(execute);
    m_Passes.push_back(std::move(pass));
    m_Compiled = false;
    HE_CORE_INFO("  RG pass: {}", name);
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
        if (pass->execute) pass->execute(cmdList);
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
    HE_CORE_INFO("RenderGraph cleared");
}

} // namespace he::render
