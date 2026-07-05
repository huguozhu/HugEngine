#pragma once

#include "RHI/RHI.h"
#include "RHI/Shader.h"
#include "Math/Math.h"
#include "Math/Geometry.h"
#include <vector>
#include <memory>

// ============================================================
// GPUCulling — Compute Shader 视锥剔除
//
// 用法（每帧）:
//   gpuCull.UploadBounds(device, bounds, count);
//   gpuCull.Dispatch(cmd, viewProj, count);
//   gpuCull.Readback(device, visibleIndices);
//   // 然后遍历 visibleIndices 发出 draw call
// ============================================================

namespace he::render {

// 每个物体的 AABB + GPUObjectData 索引
struct alignas(16) CullObjectBounds {
    float4 minPoint;    // AABB min（世界空间）
    float4 maxPoint;    // AABB max
    u32    objectIndex; // GPUObjectData[] 中的索引
    u32    _pad[3];
};

class GPUCulling {
public:
    static constexpr u32 kMaxObjects = 2048;

    bool enabled = true;

    /// 初始化：创建 SSBO + Compute PSO
    bool Initialize(rhi::IRHIDevice* device);
    /// 清理 GPU 资源
    void Shutdown(rhi::IRHIDevice* device);

    /// 设置 GPUScene SSBO + 深度缓冲输入
    void SetSceneBuffer(rhi::IRHIDevice* device, rhi::IRHIBuffer* gpuSceneSSBO);
    void SetDepthTexture(rhi::IRHIDevice* device, rhi::IRHITexture* depthTex, u32 width, u32 height);

    /// 调度 Compute Shader（含 Hi-Z 金字塔构建）
    void Dispatch(rhi::IRHICommandList* cmd, const float4x4& viewProj, u32 objectCount, u32 screenW, u32 screenH);

    /// 从 GPU 读回可见物体索引列表（CPU 同步）
    void Readback(rhi::IRHIDevice* device, std::vector<u32>& outVisibleIndices);

    u32 GetLastVisibleCount() const { return m_LastVisibleCount; }

private:
    bool m_Initialized = false;
    rhi::ShaderBytecode m_CS;

    // GPU 缓冲
    std::unique_ptr<rhi::IRHIBuffer> m_IndirectCmdBuf;     // binding 1: IndirectDrawCommand[]
    std::unique_ptr<rhi::IRHIBuffer> m_DrawCountBuf;       // binding 2
    std::unique_ptr<rhi::IRHIPipelineState> m_PSO;
    rhi::DescriptorSetLayoutHandle m_DescLayout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_DescSet    = rhi::kInvalidSet;

    // Hi-Z 金字塔
    static constexpr u32 kHiZMips = 8;  // 最多 8 级（256→128→...→2）
    std::unique_ptr<rhi::IRHITexture> m_HiZTexture;
    std::unique_ptr<rhi::IRHISampler> m_HiZSampler;
    std::unique_ptr<rhi::IRHIPipelineState> m_HiZ_PSO;
    rhi::DescriptorSetLayoutHandle m_HiZLayout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_HiZSet0   = rhi::kInvalidSet;  // level 0→1
    rhi::DescriptorSetHandle       m_HiZSet1   = rhi::kInvalidSet;  // level 1→2 (ping-pong)
    u32 m_HiZMipCount = 0;
    rhi::IRHITexture* m_DepthInput = nullptr;

    u32 m_LastVisibleCount = 0;
};

} // namespace he::render
