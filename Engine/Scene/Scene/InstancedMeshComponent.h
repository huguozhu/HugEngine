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
//   - 实例变换：CPU 侧 float4x4 数组（SetInstanceTransforms 递增版本号），
//     渲染侧按快照里的**版本号**决定是否重传实例变换 SSBO（容量够就 Map 复用）
//   - 绘制：useInstanceID=2 模式，顶点着色器按 SV_InstanceID 取变换
//   - 逐实例视锥剔除（enableFrustumCull）预留；Deferred/GPU Culling 路径后续扩展
//
// 任务 23：实例变换 SSBO **复用 + 有界退役**（容量够时 Map 原地更新，只在扩容时重建并
// 把旧缓冲放进 N 帧延迟释放队列），不再每次更新都新建 SSBO + 旧缓冲无界保活。
// 阶段 1 第①段（§15.1）：这份"缓冲生命周期"状态已从组件搬到渲染侧
// （`render::InstanceCuller::InstanceState`），组件只留 CPU 数据源与版本号。
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
    /// 【阶段 1 第①段 / §15.1】组件侧**只保留数据源**：逐网格的 GPU 实例缓冲（SSBO、容量、
    /// 脏标记、退役队列）与逐实例剔除的命令缓冲原先都挂在本组件上 —— 那让渲染期必须回读组件
    /// （B1 / 组件指针两项闸门都降不下来）。它们已搬到 `render::InstanceCuller` 的实例状态表
    /// （按 `meshIndex` 索引），数据则由 `FrameSceneSnapshot::instanceTransforms` 按值送达。
    std::vector<float4x4> instanceTransforms;

    /// 设置实例变换并递增版本号（渲染侧据此判断"要不要重传实例缓冲"）
    void SetInstanceTransforms(std::vector<float4x4> transforms);

    u32 GetInstanceCount() const { return (u32)instanceTransforms.size(); }

    /// 变换数据版本号：每次 `SetInstanceTransforms` +1。
    /// 【为什么用版本号而不是脏标记】缓冲状态在渲染侧，渲染侧不该回写/回读组件上的标志位；
    /// 快照把版本号按值带走，渲染侧比较"本帧版本 ≠ 上次上传的版本"决定是否重传 ——
    /// 这个判据跨帧幂等（同一帧被消费两次也不会重复上传），适合另一根线程。
    /// 【约定】绕过 `SetInstanceTransforms` 直接改写 `instanceTransforms` 时，必须一并递增它。
    u32 instanceTransformVersion = 0;
};

} // namespace he
