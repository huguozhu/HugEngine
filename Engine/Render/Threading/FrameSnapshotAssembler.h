#pragma once

#include "Threading/FrameSceneSnapshot.h"
// 口径结构（`SceneSnapshotLightOptions` / `SceneSnapshotObjectOptions`）定义在收集器头里
#include "Threading/SceneSnapshotBuilder.h"
#include "Threading/MeshRegistry.h"

#include <functional>

namespace he {
class World;
class SceneGraph;
} // namespace he

// ============================================================
// FrameSnapshotAssembler — 本帧渲染输入的"装配器"（阶段 1 §15.1 第③段第 4 批）
//
// 【它解决什么问题】快照必须在**游戏线程**取齐（方案 §4.2），而取齐天然要收
// `World&` / `SceneGraph&`。把这段代码放进 `Engine/Render/Threading/`（附录 B1 的**白名单层** ——
// 它就是"渲染侧唯一允许读世界的地方"），而**管线的帧入口就彻底不收世界**：
//   · 管线在 `Initialize` 里把**口径**配置进来（光源/物体选项、要构建哪些数组、注册表与输出）；
//   · 样例每帧调 `AssembleScene(world, sg, camera)`（游戏线程）；
//   · 阴影收集（`ShadowSystem::Update`）之后调 `ResolveLightShadowIndices(...)`（**不收世界**）。
//
// 【顺序契约（必须遵守）】
//   1. `AssembleScene`：注册网格 → 天空盒/环境/材质 → 物体 → 实例 → 阴影光源 → 光源
//      （此时 `SnapshotLight::shadowIndex` 一律 **-1**）；
//   2. `ShadowSystem::Update`（它建立"实体 → 阴影下标"的映射）；
//   3. `ResolveLightShadowIndices`：用该映射补齐 `shadowIndex`；
//   4. 管线 `Render(cmd, snapshot, camera, dt)`。
//   【为什么光源不能在 1 里就解析阴影下标】那时映射还是上一帧的（甚至没有）——
//   旧代码把 `CollectLights` 放在阴影收集之后正是为了拿本帧的映射。
//
// 【为什么把"场景包围盒"也放进来】`ForwardPipeline::RefreshRSMFrustum` 原先自己遍历世界算它
//（每 30 帧一次），那是渲染期读世界的一处；改由本装配器按**同一公式**（网格包围盒 × **局部**变换）
// 算好带进快照。
// ============================================================

namespace he::render {

class MeshRegistry;
struct CameraData;

/// 装配口径（由管线在 `Initialize` 时配置一次；`physicalUnitsEnabled` 每帧由管线刷新）
struct FrameSnapshotAssemblySettings {
    /// 光源收集口径（三条管线各有历史差异，默认值 = Deferred 现状）
    SceneSnapshotLightOptions  lightOptions{};
    /// 物体收集口径（Deferred 排除贴花卡片）
    SceneSnapshotObjectOptions objectOptions{};
    /// 全局物理光开关（`r.Light.PhysicalUnits`）：由管线每帧传入，避免装配器读全局 CVar
    bool physicalUnitsEnabled = false;

    // ── 要构建哪些数组（各管线需要的内容不同）──
    bool buildSkybox       = true;
    bool buildEnvironment  = true;
    bool buildMaterials    = false;   // bindless 材质数组（Forward 的 `UploadMaterialBindless` 要）
    bool buildInstances    = true;
    bool buildShadowLights = true;
    bool buildDecals       = false;   // Deferred 的 `DecalPass` 要
    bool buildParticles    = false;   // 粒子模拟循环要
};

class FrameSnapshotAssembler {
public:
    /// 配置口径（管线 `Initialize` 时调用；可反复调用以改开关）
    void Configure(const FrameSnapshotAssemblySettings& settings) { m_Settings = settings; }
    [[nodiscard]] FrameSnapshotAssemblySettings&       Settings()       { return m_Settings; }
    [[nodiscard]] const FrameSnapshotAssemblySettings& Settings() const { return m_Settings; }

    /// 绑定输出快照与网格注册表（管线 `Initialize` 时调用一次）
    void Bind(FrameSceneSnapshot* out, MeshRegistry* registry) { m_Out = out; m_Registry = registry; }

    /// 帧边界：允许本帧重新装配一次（管线 `NextFrame()` 调用）
    void BeginFrame() { m_Assembled = false; }

    /// 本帧是否已经装配过（样例可能为了阴影收集先装配一次 ⇒ 管线 `Render` 不必重复收集）
    [[nodiscard]] bool AssembledThisFrame() const { return m_Assembled; }

    /// **游戏线程**：把本帧渲染输入取齐（每帧第一次调用真正干活，之后为空操作）。
    /// 【内部先做】`sg.UpdateTransforms()`：世界矩阵必须是最新的才能取快照。
    /// 旧代码把这一步放在渲染期（`ForwardPipeline::RenderScene` 开头），而快照在它**之前**构建，
    /// 于是"同帧内被改脏的变换"会让快照旧一帧 —— 现在顺序反过来了（先刷新、再取快照）。
    void AssembleScene(he::World& world, he::SceneGraph& sg, const CameraData& camera);

    /// 阴影下标解析（**必须在 `ShadowSystem::Update` 之后**调用；无 world 参数）
    void ResolveLightShadowIndices(const std::function<i32(he::Entity)>& resolver);

    /// 首帧按实际规模自校准预留一次容量（幂等；稳态下快照数组不再重分配）
    void ReserveOnce();

    [[nodiscard]] bool IsBound() const { return m_Out != nullptr; }

private:
    FrameSnapshotAssemblySettings m_Settings{};
    FrameSceneSnapshot* m_Out      = nullptr;
    MeshRegistry*       m_Registry = nullptr;
    bool                m_Reserved = false;
    bool                m_Assembled = false;   // 本帧是否已装配（`BeginFrame` 复位）
};

} // namespace he::render
