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

    /// 上传物体 AABB 到 GPU
    void UploadBounds(rhi::IRHIDevice* device,
                      const std::vector<CullObjectBounds>& bounds);

    /// 调度 Compute Shader（在 command list 中）
    void Dispatch(rhi::IRHICommandList* cmd, const float4x4& viewProj, u32 objectCount);

    /// 从 GPU 读回可见物体索引列表（CPU 同步）
    void Readback(rhi::IRHIDevice* device, std::vector<u32>& outVisibleIndices);

    u32 GetLastVisibleCount() const { return m_LastVisibleCount; }

private:
    bool m_Initialized = false;
    rhi::ShaderBytecode m_CS;

    // GPU 缓冲
    std::unique_ptr<rhi::IRHIBuffer> m_BoundsBuffer;       // binding 0
    std::unique_ptr<rhi::IRHIBuffer> m_VisibleIndicesBuf;  // binding 1
    std::unique_ptr<rhi::IRHIBuffer> m_DrawCountBuf;       // binding 2
    std::unique_ptr<rhi::IRHIPipelineState> m_PSO;
    rhi::DescriptorSetLayoutHandle m_DescLayout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_DescSet    = rhi::kInvalidSet;

    u32 m_LastVisibleCount = 0;
};

} // namespace he::render
