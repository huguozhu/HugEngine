#pragma once

#include "Scene/MeshComponent.h"
#include "Math/Math.h"
#include "RHI/FrameRetireQueue.h"

#include <vector>

// ============================================================
// InstancedMeshComponent — 实例化网格（对应 UE5 UInstancedStaticMeshComponent）
//
// 同一网格大量实例（草丛/森林/建筑群），单次 DrawIndexed 渲染 N 个实例。
// MVP 约定：
//   - 网格：内置单位立方体（OnCreate 程序化生成；meshPath 预留 glTF 实例化）
//   - 实例变换：CPU 侧 float4x4 数组（SetInstanceTransforms 置脏），
//     由 ForwardPipeline 每帧检测脏标记 → 更新实例变换 SSBO（容量够就 Map 复用）
//   - 绘制：useInstanceID=2 模式，顶点着色器按 SV_InstanceID 取变换
//   - 逐实例视锥剔除（enableFrustumCull）预留；Deferred/GPU Culling 路径后续扩展
//
// 任务 23：实例变换 SSBO **复用 + 有界退役**（容量够时 Map 原地更新，只在扩容时重建并
// 把旧缓冲放进 N 帧延迟释放队列），不再每次更新都新建 SSBO + 旧缓冲无界保活。
// ============================================================

namespace he {

class InstancedMeshComponent : public MeshComponent {
    HE_COMPONENT()
public:
    /// 生成单位立方体网格（halfExtent=0.5 约定，与 CubeComponent 一致）
    void OnCreate() override;

    String meshPath;                  // 网格资产路径（预留：glTF 实例化；MVP 用内置立方体）
    /// 逐实例视锥剔除开关（任务 25）：开启后由 GPU 逐实例做六平面测试，
    /// 只把可见实例写进压缩列表 + 间接命令计数（原来的路径是"整批实例一起画"）。
    /// 默认关（与原行为一致：先看得到全量，再按需要打开对比）。
    bool   enableFrustumCull = false;

    // --- CPU 实例变换（每实例一个世界矩阵）---
    std::vector<float4x4> instanceTransforms;

    /// 设置实例变换并标记脏（渲染管线下一帧更新 GPU 实例缓冲）
    void SetInstanceTransforms(std::vector<float4x4> transforms);

    u32 GetInstanceCount() const { return (u32)instanceTransforms.size(); }

    /// GPU 实例缓冲容量（可容纳的实例数；ForwardPipeline 管理）
    u32 GetInstanceBufferCapacity() const { return instanceBufferCapacity; }

    /// 帧边界推进退役队列（ForwardPipeline 每帧调用一次）
    void AdvanceRetireQueue() { retiredBuffers.Advance(); }

    /// 退役当前实例缓冲（扩容重建时调用；旧缓冲 N 帧后释放）
    void RetireInstanceBuffer() {
        if (!instanceBuffer) return;
        retiredBuffers.Retire(std::move(instanceBuffer));
    }

    /// 待释放缓冲数（调试/判据：验证"有界"）
    u32 GetRetiredBufferCount() const { return retiredBuffers.GetPendingCount(); }

    // --- GPU 侧状态（ForwardPipeline 管理，勿手动改）---
    bool bTransformsDirty = false;    // CPU 变换已改，待上传
    u32  instanceSSBOHandle = 0;      // bindless SSBO 句柄（容量不变则句柄不变）
    u32  instanceBufferCapacity = 0;  // 已分配缓冲可容纳的实例数
    std::unique_ptr<rhi::IRHIBuffer> instanceBuffer;   // GPU 实例变换缓冲（随组件存活）
    // 退役缓冲（任务 23：**有界** N 帧延迟释放，替代原来的无界 vector 保活）
    rhi::FrameRetireQueue<std::unique_ptr<rhi::IRHIBuffer>> retiredBuffers;

    // --- 逐实例剔除（任务 25）---
    /// 间接绘制命令（20 字节；CPU 填 indexCount/firstIndex/vertexOffset，GPU 原子写 instanceCount）。
    /// **每飞行帧一份**：单份会让"本帧 CPU 清零命令"与"上帧 GPU 仍在读该命令做间接绘制"打架。
    std::unique_ptr<rhi::IRHIBuffer> instanceCullCmd[rhi::kMaxFramesInFlight];
    u32 instanceCullCmdHandle[rhi::kMaxFramesInFlight] = { 0, 0, 0 };   // 命令缓冲的 bindless SSBO 句柄
    /// 上一次读回的可见实例数（GPU 异步写入，可能滞后一帧；仅统计/调试用）
    u32 visibleInstanceCount = 0;
};

} // namespace he
