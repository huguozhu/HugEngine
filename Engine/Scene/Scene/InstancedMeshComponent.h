#pragma once

#include "Scene/MeshComponent.h"
#include "Math/Math.h"

#include <vector>

// ============================================================
// InstancedMeshComponent — 实例化网格（对应 UE5 UInstancedStaticMeshComponent）
//
// 同一网格大量实例（草丛/森林/建筑群），单次 DrawIndexed 渲染 N 个实例。
// MVP 约定：
//   - 网格：内置单位立方体（OnCreate 程序化生成；meshPath 预留 glTF 实例化）
//   - 实例变换：CPU 侧 float4x4 数组（SetInstanceTransforms 置脏），
//     由 ForwardPipeline 每帧检测脏标记 → 重建实例变换 SSBO → 注册 bindless
//   - 绘制：useInstanceID=2 模式，顶点着色器按 SV_InstanceID 取变换
//   - 逐实例视锥剔除（enableFrustumCull）预留；Deferred/GPU Culling 路径后续扩展
// ============================================================

namespace he {

class InstancedMeshComponent : public MeshComponent {
    HE_COMPONENT()
public:
    /// 生成单位立方体网格（halfExtent=0.5 约定，与 CubeComponent 一致）
    void OnCreate() override;

    String meshPath;                  // 网格资产路径（预留：glTF 实例化；MVP 用内置立方体）
    bool   enableFrustumCull = false; // 逐实例 CPU 视锥剔除（预留，MVP 未接）

    // --- CPU 实例变换（每实例一个世界矩阵）---
    std::vector<float4x4> instanceTransforms;

    /// 设置实例变换并标记脏（渲染管线下一帧重建 GPU 实例缓冲）
    void SetInstanceTransforms(std::vector<float4x4> transforms);

    u32 GetInstanceCount() const { return (u32)instanceTransforms.size(); }

    // --- GPU 侧状态（ForwardPipeline 管理，勿手动改）---
    bool bTransformsDirty = false;    // CPU 变换已改，待上传
    u32  instanceSSBOHandle = 0;      // bindless SSBO 句柄
    std::unique_ptr<rhi::IRHIBuffer> instanceBuffer;   // GPU 实例变换缓冲（随组件存活）
    // 退役缓冲（bindless 堆 append-only：旧缓冲不销毁避免悬垂指针；随组件析构释放）
    std::vector<std::unique_ptr<rhi::IRHIBuffer>> retiredBuffers;
};

} // namespace he
