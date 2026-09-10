#pragma once

// ============================================================
// SplineMeshSystem — 样条网格重建系统（静态系统，与 TextRenderSystem 同模式）
//
// 每帧遍历 SplineMeshComponent：
//   关联样条缺失 / enabled=false → 跳过
//   样条版本变化 → 调用 RebuildFrom 重建条带网格（版本未变则零成本）
// ============================================================

namespace he {

class World;
class SplineMeshComponent;

namespace rhi { class IRHIDevice; }

class SplineMeshSystem {
public:
    /// 每帧调用：按需重建所有样条网格。
    /// 在渲染前调用（与 SkeletalMeshSystem 等同类系统一致）。
    static void Update(World& world, rhi::IRHIDevice* device = nullptr);
};

} // namespace he
