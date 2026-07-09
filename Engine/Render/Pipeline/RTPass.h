#pragma once

#include "RHI/RHI.h"
#include "Scene/MeshComponent.h"
#include "Scene/Transform.h"
#include "Core/Types.h"

#include <memory>
#include <vector>
#include <unordered_map>

namespace he { class World; class SceneGraph; }
namespace he::rhi { class IRHIDevice; class IRHICommandList; }

namespace he::render {

// ============================================================
// RTPass — Ray Tracing Pass 管理器
//
// 职责：
//   1. 为每个 MeshComponent 创建/更新 BLAS
//   2. 每帧构建 TLAS（收集所有 Mesh 的 world transform）
//   3. 管理 RT Pipeline State + SBT
//   4. 提供便捷的 TraceRays 调度接口
//
// 用法：
//   RTPass rt;
//   rt.Initialize(device);
//   rt.BuildAS(cmd, world, sg);        // 构建/更新 BLAS + TLAS
//   rt.BindPipeline(cmd);              // 绑定 RT PSO
//   rt.TraceRays(cmd, sbt, w, h);      // 发射光线
// ============================================================
class RTPass {
    HE_DECLARE_NON_COPYABLE(RTPass);

public:
    RTPass();
    ~RTPass();

    // 初始化：创建 RT PSO + SBT
    bool Initialize(rhi::IRHIDevice* device,
                    const std::vector<rhi::ShaderBytecode>& rtShaders,
                    const std::vector<rhi::RTShaderGroup>& shaderGroups);

    // 为 BLAS 绑定 SRV（在 Initialize 之后调用）
    bool CreateSBT(rhi::IRHIDevice* device);

    void Shutdown();

    // 每帧调用：构建/更新 BLAS（仅当几何变更时） + 构建 TLAS
    void BuildAS(rhi::IRHICommandList* cmd,
                 he::World& world, he::SceneGraph& sg);

    // 绑定 RT 管线
    void BindPipeline(rhi::IRHICommandList* cmd);

    // 发射光线（width/height 通常对应屏幕分辨率或半分辨率）
    void TraceRays(rhi::IRHICommandList* cmd, u32 width, u32 height);

    // 状态查询
    bool IsValid() const { return m_RTPipeline != nullptr && m_TLAS != nullptr; }
    rhi::IRHIAccelerationStructure* GetTLAS() const { return m_TLAS.get(); }

    // Shader 热重载
    int ReloadShader(StringView shaderName, const std::vector<u32>& newSpirv);

private:
    // 检查几何是否变更（用于增量更新 BLAS）
    bool HasGeometryChanged(he::MeshComponent* mesh);

    rhi::IRHIDevice* m_Device = nullptr;

    // RT Pipeline + SBT
    std::unique_ptr<rhi::IRHIRayTracingPipelineState> m_RTPipeline;
    rhi::SBTDesc m_SBT;
    std::unique_ptr<rhi::IRHIBuffer> m_SBTBuffer;

    // AS
    std::unique_ptr<rhi::IRHIAccelerationStructure> m_TLAS;
    std::unique_ptr<rhi::IRHIBuffer> m_TLASScratch;
    std::unique_ptr<rhi::IRHIBuffer> m_TLASInstanceBuffer;

    // BLAS 映射: MeshComponent* → BLAS
    struct BLASEntry {
        std::unique_ptr<rhi::IRHIAccelerationStructure> blas;
        std::unique_ptr<rhi::IRHIBuffer> scratchBuffer;
        u64 geometryHash = 0;  // 用于检测几何变更
    };
    std::unordered_map<he::MeshComponent*, BLASEntry> m_BLASMap;

    // 着色器字节码（热重载用）
    std::vector<rhi::ShaderBytecode> m_Shaders;
    std::vector<rhi::RTShaderGroup>  m_ShaderGroups;

    // 描述符集布局
    rhi::DescriptorSetLayoutHandle m_DescLayout = rhi::kInvalidLayout;

    u32 m_MaxInstanceCount = 4096;
    bool m_Initialized = false;
};

} // namespace he::render
