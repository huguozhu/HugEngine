// ============================================================
// SplineMeshSystem.cpp — 样条网格重建
// ============================================================

#include "Scene/SplineMeshSystem.h"
#include "Scene/SplineMeshComponent.h"
#include "Scene/SplineComponent.h"
#include "Scene/World.h"
#include "Scene/Entity.h"

namespace he {

void SplineMeshSystem::Update(World& world, rhi::IRHIDevice* device) {
    (void)device;   // 网格上传由 MeshComponent::SetMeshData 内部经 RHI 设备完成（无设备时仅跳过缓冲创建）

    world.ForEach<SplineMeshComponent>([&](Entity e, SplineMeshComponent& sm) {
        if (!sm.enabled) return;                 // 禁用：保留现有网格，不重建

        // 关联样条（0 = 未设置）
        if (sm.splineEntity == 0) return;
        SplineComponent* spline = world.GetComponent<SplineComponent>(Entity{sm.splineEntity});
        if (!spline) return;                     // 关联实体无样条：容错跳过

        // 脏检测：样条数据版本未变则跳过（避免每帧重建 GPU 缓冲）
        if (sm.GetBuiltVersion() == spline->GetVersion()) return;

        sm.RebuildFrom(*spline);
    });
}

} // namespace he
