#pragma once

#include "RHI/RHI.h"
#include "RHI/Buffer.h"
#include "RHI/SwapChain.h"
#include "Containers/Array.h"
#include "Core/Types.h"

#include <functional>
#include <string>
#include <vector>

// ============================================================
// Render Graph — 帧级渲染资源编排器
//
// Phase 1: 手动添加 Pass → Compile (拓扑排序 + Barrier) → Execute
// Phase 2+: 自动 Barrier 推导、资源别名、Async Compute
// ============================================================

namespace he::render {

class IRHICommandList; // 避免循环依赖

// --- 资源句柄 ---
using ResourceHandle = u32;
constexpr ResourceHandle kInvalidHandle = ~0u;

// --- 资源描述 ---
enum class ResourceType : u8 {
    Texture,
    Buffer,
};

struct ResourceDesc {
    ResourceType type = ResourceType::Texture;
    u32          width  = 1;
    u32          height = 1;
    rhi::Format       format = rhi::Format::RGBA8_UNORM;
    rhi::BufferUsage  bufferUsage = rhi::BufferUsage::None;
    rhi::TextureUsage textureUsage = rhi::TextureUsage::ShaderResource;
};

// --- 资源访问信息 ---
enum class ResourceAccess : u8 {
    None,
    Read,
    Write,
    ReadWrite,
};

struct PassResource {
    ResourceHandle handle;
    ResourceAccess access;
};

// --- 渲染 Pass ---
using PassExecuteFunc = std::function<void(rhi::IRHICommandList* cmdList)>;

struct PassNode {
    String                      name;
    std::vector<PassResource>   reads;
    std::vector<PassResource>   writes;
    PassExecuteFunc             execute;
    // 编译后填充
    std::vector<PassNode*>      dependencies;  // 本 Pass 依赖的前置 Pass
    u32                         order = 0;     // 执行顺序
};

// --- Render Graph ---
class RenderGraph {
public:
    RenderGraph();
    ~RenderGraph();

    // --- 资源管理 ---
    ResourceHandle CreateTexture(StringView name, const rhi::TextureDesc& desc);
    ResourceHandle CreateBuffer(StringView name, const rhi::BufferDesc& desc);
    const ResourceDesc& GetResourceDesc(ResourceHandle h) const;

    // --- Pass 构建 ---
    /// 添加渲染 Pass，声明读/写资源
    PassNode* AddPass(StringView name,
                      std::vector<PassResource> reads  = {},
                      std::vector<PassResource> writes = {},
                      PassExecuteFunc execute = nullptr);

    // --- 导入外部资源 ---
    /// 导入 SwapChain 的 BackBuffer 作为输出资源
    ResourceHandle ImportBackBuffer();

    // --- 编译与执行 ---
    /// 编译图：拓扑排序 → 推导 Barrier
    void Compile();

    /// 按编译顺序执行所有 Pass
    void Execute(rhi::IRHICommandList* cmdList, rhi::IRHIDevice* device);

    /// 清空图（下一帧重建）
    void Reset();

    const std::vector<PassNode*>& GetPassOrder() const { return m_PassOrder; }

private:
    void BuildDependencies();
    void TopologicalSort();

    TArray<ResourceDesc>     m_Resources;
    std::vector<PassNode>    m_Passes;
    std::vector<PassNode*>   m_PassOrder;

    ResourceHandle m_BackBufferHandle = kInvalidHandle;
    bool           m_Compiled = false;
};

// 便捷宏：声明 Pass 读资源
#define RG_READ(h)   PassResource{h, ResourceAccess::Read}
#define RG_WRITE(h)  PassResource{h, ResourceAccess::Write}
#define RG_RW(h)     PassResource{h, ResourceAccess::ReadWrite}

} // namespace he::render
