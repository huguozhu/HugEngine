#pragma once

#include "Pipeline/Material.h"
#include "Pipeline/Camera.h"
#include "RHI/RHI.h"
#include "Core/Types.h"
// 阶段 1 §15.1 第②段：`Prepare` 改为消费快照（`FrameSceneSnapshot`），不再读世界
#include "Threading/FrameSceneSnapshot.h"
#include <vector>

namespace he { class World; class SceneGraph; class MeshComponent; }

namespace he::render {

// ============================================================================
// DrawItem — SceneRenderer::Prepare 输出的单条绘制项
//
// 【阶段 1 §15.1 第②段：不再持有组件指针】`mesh` 字段已删除 —— 顶点/索引缓冲与索引数一律
// 按 `meshIndex` 去 `MeshRegistry` 查（渲染侧自己的表）。这是附录 E"组件指针闸门"
// （`--mesh-ptrs`）的收敛点：渲染侧不再持有任何 `*Component*`。
// ============================================================================
struct DrawItem {
    u32 objectIndex = 0;  // GPUObjectData SSBO 中的索引
    bool bInstanced = false;  // 实例化/骨骼网格：普通绘制循环跳过，由专用 Pass 渲染
    /// 渲染侧 mesh 注册表索引（阶段 1 附录 E / E-3）：快照条目透传。
    /// 0 = 未注册（消费侧 `Find(0)` 返回空并跳过 —— 可见化，而不是指错资源）。
    u32 meshIndex = 0;
    /// 【A4】CPU 视锥剔除结果（只由 `PrepareBatched` 填写；`Prepare` 输出的条目恒为 false）。
    /// 合批顺序的列表**不能**紧凑化（下标 = 间接命令 `firstInstance` = 物体缓冲下标），
    /// 所以剔除结果只能逐条打标记：逐 draw 路径据此跳过不可见物体，GPU 驱动路径忽略它
    /// （那条路径由 GPU 剔除决定画什么，CPU 不参与）。
    bool bCPUCulled = false;
};

// ============================================================================
// SceneRenderer — 通用几何体数据准备器
//
// 只做**视锥剔除 + GPU 数据上传**，不录制 Draw 命令、也不再遍历世界。
// 【阶段 1 §15.1 第②段】收集（遍历 ECS、算材质、算世界矩阵/AABB）已经在游戏线程由
// `SceneSnapshotBuilder` 做完并放进快照；本类只消费快照里的值 ⇒ 签名里的 `World&`/
// `SceneGraph&` 一并消失（附录 B1 因此下降），渲染期与 ECS 彻底解耦。
// 管线自行遍历 DrawItem 列表，使用自己的 PSO/描述符集/RenderPass 录制。
// ============================================================================
class SceneRenderer {
public:
    bool enableFrustumCull = true;  // CPU 视锥剔除开关（默认开启）

    /// 从快照取本帧可见物体 → 剔除 → 上传 GPUObjectData → 返回 DrawList
    /// 【为什么不再需要 excludeDecals】贴花是否进 `draws` 在**收集侧**就由
    /// `SceneSnapshotObjectOptions::excludeDecals` 决定了（快照即口径），消费侧无需再传。
    /// 【为什么不再需要 world/sg】条目的世界矩阵、世界 AABB、材质数据都已在快照里算好。
    /// @param objectBuffer GPUObjectData SSBO（按可见顺序写入，下标 = `DrawItem::objectIndex`）
    std::vector<DrawItem> Prepare(const FrameSceneSnapshot& snapshot,
                                  const CameraData& camera,
                                  rhi::IRHIBuffer* objectBuffer);

    /// 【A4】按**合批顺序**准备物体数据（GPU 驱动的间接绘制路径专用）
    ///
    /// 与 `Prepare` 的区别（`Prepare` 只服务逐 draw 的 CPU 路径，契约不变）：
    ///   · 条目顺序 = 合批顺序，**不紧凑化**（CPU 剔除只打 `bCPUCulled` 标记）——
    ///     因为 GPU 剔除写出的间接命令用 `firstInstance = 合批序号`，顶点着色器再用它索引
    ///     `GPUObjectData[]`；两处必须是同一个序号（04 的 GPU GBuffer 已按这条契约修过，
    ///     Forward 此前没修：它把快照顺序的对象表与可见顺序的物体缓冲混着用 ⇒ 画到别的物体）。
    ///   · 上传**全部**合批物体（不只是 CPU 可见的那些）：GPU 剔除可能保留 CPU 判为不可见的
    ///     物体（它的判据更保守），若物体缓冲只写可见子集，`u_Objects[firstInstance]` 会读到
    ///     未写入的尾部（脏矩阵 ⇒ 三角形被拉到屏幕外/糊满全屏，实测就是 A4 的画面差）。
    ///   · 合批收集集**之外**的条目（Spline / Skeletal：顶点布局不同，不并入合批）追加在
    ///     合批区间之后，让它们仍能走各自的专用路径（`objectIndex` 同样指向有效槽位）。
    ///
    /// 契约（四处同一个序号，与 `GBufferRenderer_GPU::BuildBatchDrawItemsAndUpload` 一致）：
    ///   合批序号 = GPUScene 对象下标（`CollectBatchedFromSnapshot`）= 间接命令 `firstInstance`
    ///            = 物体缓冲下标（本函数写入的位置）
    ///
    /// @param batchSnapshotIndices 合批命令 → 快照条目下标（`MeshBatcher::GetBatchSnapshotIndices`）
    /// @return 与合批命令同序的绘制项（含追加的 Spline / Skeletal 条目）；
    ///         `objectBuffer` 为空 / 映射失败 / 合批为空时返回空列表（调用方回退逐 draw）
    std::vector<DrawItem> PrepareBatched(const FrameSceneSnapshot& snapshot,
                                         const CameraData& camera,
                                         rhi::IRHIBuffer* objectBuffer,
                                         const std::vector<u32>& batchSnapshotIndices);
};

} // namespace he::render
