#include "RenderGraph.h"
#include "Core/Log.h"

#include <algorithm>
#include <unordered_set>

namespace he::render {

RenderGraph::RenderGraph() {
    HE_CORE_INFO("RenderGraph created");
}

RenderGraph::~RenderGraph() {
    Reset();
}

ResourceHandle RenderGraph::CreateTexture(StringView name, const rhi::TextureDesc& desc) {
    ResourceHandle h = static_cast<ResourceHandle>(m_Resources.size());
    m_Resources.push_back({ResourceType::Texture, desc.width, desc.height, desc.format, rhi::BufferUsage::None, desc.usage});
    HE_CORE_INFO("  RG resource #{}: {} ({}x{})", h, name, desc.width, desc.height);
    return h;
}

ResourceHandle RenderGraph::CreateBuffer(StringView name, const rhi::BufferDesc& desc) {
    ResourceHandle h = static_cast<ResourceHandle>(m_Resources.size());
    m_Resources.push_back({ResourceType::Buffer, 0, 0, rhi::Format::Unknown, desc.usage, rhi::TextureUsage::None});
    HE_CORE_INFO("  RG resource #{}: {} (buffer {}, {} bytes)", h, name,
                 u32(desc.usage), desc.size);
    return h;
}

const ResourceDesc& RenderGraph::GetResourceDesc(ResourceHandle h) const {
    if (h < m_Resources.size())
        return m_Resources[h];
    static ResourceDesc invalid;
    return invalid;
}

ResourceHandle RenderGraph::ImportBackBuffer() {
    if (m_BackBufferHandle == kInvalidHandle) {
        m_BackBufferHandle = static_cast<ResourceHandle>(m_Resources.size());
        m_Resources.push_back({ResourceType::Texture, 1920, 1080, rhi::Format::RGBA8_UNORM, rhi::BufferUsage::None, rhi::TextureUsage::RenderTarget});
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

void RenderGraph::Compile() {
    HE_CORE_INFO("RenderGraph::Compile — {} passes, {} resources",
                 m_Passes.size(), m_Resources.size());

    BuildDependencies();
    TopologicalSort();
    m_Compiled = true;
}

void RenderGraph::BuildDependencies() {
    // 为每个 Pass 推导依赖关系
    // 若 Pass B 读取/写入资源 X，而 Pass A 之前写入了资源 X，则 B 依赖 A
    for (usize i = 0; i < m_Passes.size(); ++i) {
        m_Passes[i].dependencies.clear();
        m_Passes[i].order = 0;
    }

    for (usize i = 0; i < m_Passes.size(); ++i) {
        for (usize j = 0; j < i; ++j) {
            // 检查 Pass[j] 是否写入了 Pass[i] 依赖的资源
            bool depends = false;
            for (auto& r : m_Passes[i].reads) {
                for (auto& w : m_Passes[j].writes) {
                    if (r.handle == w.handle) { depends = true; break; }
                }
                if (depends) break;
            }
            // RAW (Read After Write) — Pass[i] 读 → Pass[j] 写
            for (auto& w : m_Passes[i].writes) {
                for (auto& r : m_Passes[j].reads) {
                    if (w.handle == r.handle) { depends = true; break; }
                }
                // WAW (Write After Write) — 写入顺序依赖
                for (auto& w2 : m_Passes[j].writes) {
                    if (w.handle == w2.handle) { depends = true; break; }
                }
                if (depends) break;
            }

            if (depends) {
                m_Passes[i].dependencies.push_back(&m_Passes[j]);
            }
        }
    }
}

void RenderGraph::TopologicalSort() {
    m_PassOrder.clear();

    // 计算入度
    std::vector<u32> inDegree(m_Passes.size(), 0);
    for (usize i = 0; i < m_Passes.size(); ++i) {
        inDegree[i] = static_cast<u32>(m_Passes[i].dependencies.size());
    }

    // Kahn 算法
    std::vector<PassNode*> queue;
    for (usize i = 0; i < m_Passes.size(); ++i) {
        if (inDegree[i] == 0) queue.push_back(&m_Passes[i]);
    }

    while (!queue.empty()) {
        PassNode* node = queue.back();
        queue.pop_back();
        node->order = static_cast<u32>(m_PassOrder.size());
        m_PassOrder.push_back(node);

        // 减少依赖此节点的 Pass 的入度
        for (usize i = 0; i < m_Passes.size(); ++i) {
            for (auto* dep : m_Passes[i].dependencies) {
                if (dep == node) {
                    if (--inDegree[i] == 0) {
                        queue.push_back(&m_Passes[i]);
                    }
                    break;
                }
            }
        }
    }
}

void RenderGraph::Execute(rhi::IRHICommandList* cmdList, rhi::IRHIDevice* device) {
    if (!m_Compiled) Compile();

    HE_CORE_INFO("RenderGraph::Execute — {} passes in order", m_PassOrder.size());

    // 为每个资源创建 RHI 对象（Phase 1: 简单分配，不做别名）
    TArray<std::unique_ptr<rhi::IRHITexture>> textures;
    TArray<std::unique_ptr<rhi::IRHIBuffer>>  buffers;
    textures.resize(m_Resources.size());
    buffers.resize(m_Resources.size());

    for (usize i = 0; i < m_Resources.size(); ++i) {
        auto& desc = m_Resources[i];
        if (desc.type == ResourceType::Texture && desc.width > 0) {
            rhi::TextureDesc texDesc;
            texDesc.width  = desc.width;
            texDesc.height = desc.height;
            texDesc.format = desc.format;
            texDesc.usage  = desc.textureUsage;
            textures[i] = device->CreateTexture(texDesc);
        } else if (desc.type == ResourceType::Buffer) {
            rhi::BufferDesc bufDesc;
            bufDesc.size  = 0; // TODO: 大小从 desc 获取
            bufDesc.usage = desc.bufferUsage;
            buffers[i] = device->CreateBuffer(bufDesc);
        }
    }

    // 按拓扑顺序执行 Pass
    for (auto* pass : m_PassOrder) {
        cmdList->Begin();
        if (pass->execute) {
            pass->execute(cmdList);
        }
        cmdList->End();
        device->Submit(cmdList);
    }
}

void RenderGraph::Reset() {
    m_Resources.clear();
    m_Passes.clear();
    m_PassOrder.clear();
    m_BackBufferHandle = kInvalidHandle;
    m_Compiled = false;
    HE_CORE_INFO("RenderGraph cleared");
}

} // namespace he::render
