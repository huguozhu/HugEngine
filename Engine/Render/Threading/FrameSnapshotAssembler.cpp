// ============================================================
// FrameSnapshotAssembler.cpp — 本帧渲染输入的装配（阶段 1 §15.1 第③段第 4 批）
//
// 本文件是 `Engine/Render/Threading/` 白名单层的一部分：**渲染侧唯一允许读 World/SceneGraph 的
// 地方**。三条管线的帧入口因此可以完全不收世界，各管线的口径差异则通过
// `FrameSnapshotAssemblySettings` 显式表达（而不是各自抄一份遍历代码）。
// ============================================================

#include "Threading/FrameSnapshotAssembler.h"
#include "Threading/SceneSnapshotBuilder.h"

#include "Pipeline/Camera.h"
#include "Scene/SceneGraph.h"
#include "Scene/Transform.h"
#include "Scene/World.h"
#include "Scene/MeshComponent.h"
#include "Scene/PhysicalSkyComponent.h"   // SyncPhysicalSkyToSun（必须在收集之前的世界写）
#include "Core/Log.h"

#include <algorithm>

namespace he::render {

void FrameSnapshotAssembler::AssembleScene(he::World& world, he::SceneGraph& sg,
                                           const CameraData& camera) {
    if (!m_Out || !m_Registry) {
        HE_CORE_WARN("FrameSnapshotAssembler: 未绑定快照/注册表（Bind 未调用？）");
        return;
    }
    if (m_Assembled) return;   // 本帧已装配过（样例为了阴影收集先调过一次）
    m_Assembled = true;
    FrameSceneSnapshot& out = *m_Out;

    // 0) 收集之前的**世界写**必须先做，否则快照里是"上一状态"的数据：
    //    · 物理天空 → 方向光同步（`SyncPhysicalSkyToSun`）：阴影/光照都读方向光的照度与方向，
    //      旧代码把它放在渲染期（Forward 的 Render / 帧图开头），而快照在那之前构建 ⇒ 慢一拍；
    //    · 世界矩阵刷新（`UpdateTransforms`）：同帧被改脏的变换否则会让快照旧一帧。
    he::SyncPhysicalSkyToSun(world);
    sg.UpdateTransforms();

    // 1) 网格注册（回填组件 meshIndex）——必须在一切按 meshIndex 寻址的收集之前
    SceneSnapshotBuilder::RegisterMeshes(world, *m_Registry);

    // 2) 环境 / 天空盒 / 材质（顺序与旧管线一致：天空盒必须早于帧图读取）
    if (m_Settings.buildEnvironment) SceneSnapshotBuilder::BuildEnvironment(world, out);
    if (m_Settings.buildSkybox)      SceneSnapshotBuilder::BuildSkybox(world, out);
    if (m_Settings.buildMaterials)   SceneSnapshotBuilder::BuildMaterials(world, out);

    // 3) 物体（含蒙皮矩阵切片、阴影投射者标记、网格类别）→ 实例变换 → 阴影投射光源
    // 【物体收集恒做】它是其它一切的基础：实例化绘制、骨骼绘制、阴影绘制、GPU 剔除都按
    // `draws` 里的 `meshIndex` 对齐 ⇒ 不提供开关（关掉它没有任何消费侧能工作）。
    SceneSnapshotBuilder::BuildObjects(world, sg, camera, m_Settings.objectOptions, nullptr, out);
    if (m_Settings.buildInstances)    SceneSnapshotBuilder::BuildInstances(world, out);
    if (m_Settings.buildShadowLights) SceneSnapshotBuilder::BuildShadowLights(world, sg, out);
    if (m_Settings.buildDecals)       SceneSnapshotBuilder::BuildDecals(world, sg, out);
    if (m_Settings.buildParticles)    SceneSnapshotBuilder::BuildParticles(world, out);

    // 4) 场景包围盒（RSM 固定光锥用）：按**既有公式**算 —— 网格包围盒 × 组件**局部**变换。
    //    【注意】这里刻意照抄旧实现的 `GetLocalMatrix()`（而不是世界矩阵）：它是 RSM 光锥拟合的
    //    既有口径，本批只搬位置不改数值（口径是否该改成世界矩阵是另一件事，需单独裁决与验证）。
    {
        he::AABB bounds;
        world.ForEach<he::MeshComponent>([&](he::Entity e, he::MeshComponent& mesh) {
            if (auto* tf = world.GetComponent<TransformComponent>(e)) {
                bounds.Expand(mesh.GetBounds().Transform(tf->GetLocalMatrix()));
            }
        });
        if (bounds.IsValid()) {
            out.sceneBoundsMin = float4(bounds.min, 0.0f);
            out.sceneBoundsMax = float4(bounds.max, 0.0f);
        } else {
            out.sceneBoundsMin = float4(0.0f);
            out.sceneBoundsMax = float4(0.0f);   // min > max ⇒ 消费侧判为"无包围盒"
        }
    }

    // 5) 光源（**阴影下标留 -1**：由 `ResolveLightShadowIndices` 在阴影收集之后补齐）
    SceneSnapshotResolvers resolvers;
    resolvers.physicalUnitsEnabled = m_Settings.physicalUnitsEnabled;
    // 故意不设置 `resolvers.shadowIndex`：装配阶段拿不到本帧的阴影映射（见头文件顺序契约）
    SceneSnapshotBuilder::BuildLights(world, sg, resolvers, out, m_Settings.lightOptions);
}

void FrameSnapshotAssembler::ResolveLightShadowIndices(const std::function<i32(he::Entity)>& resolver) {
    if (!m_Out || !resolver) return;
    FrameSceneSnapshot& out = *m_Out;
    const usize n = std::min(out.lights.size(), out.lightSourceEntities.size());
    for (usize i = 0; i < n; ++i) {
        out.lights[i].shadowIndex = resolver(he::Entity{out.lightSourceEntities[i]});
    }
}

void FrameSnapshotAssembler::ReserveOnce() {
    if (m_Reserved || !m_Out) return;
    FrameSceneSnapshot& out = *m_Out;
    // 乘 2 + 常数余量：留出场景增长的余量；超出后仍会自然扩容（正确性不受影响）
    out.Reserve(static_cast<u32>(out.draws.size()) * 2u + 64u,
                static_cast<u32>(out.lights.size()) * 2u + 64u,
                static_cast<u32>(out.skinMatrices.size()) * 2u + 256u,
                static_cast<u32>(out.particles.size()) * 2u + 8u,
                static_cast<u32>(out.instances.size()) * 2u + 8u,
                static_cast<u32>(out.instanceTransforms.size()) * 2u + 1024u);
    m_Reserved = true;
}

} // namespace he::render
