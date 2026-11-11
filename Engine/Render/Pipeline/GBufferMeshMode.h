#pragma once

// ============================================================
//  GBufferMeshMode.h — 【P9】GBuffer 网格着色器路径开关（**T9 起废弃**）
//
//  【为什么废弃】引擎里曾有两条 mesh 版 GBuffer 路线：
//    ① Nanite 硬光栅（`Nanite_HardRaster.mesh`，由 `NaniteRenderer` 驱动）；
//    ② 本文件对应的 `HE_GBUFFER_MESH=1`（整网格按 64 三角形分组、从合并 VB/IB SSBO 读几何）。
//  两者都写 GBuffer、都在 `supportsMeshShaders` 为真时创建，但互不相通，导致：
//    · 同一份代码有两个 mesh 几何入口，质量与覆盖率口径不一致；
//    · 调试时要记住"这次开的是哪条"，转储基线随环境变量漂移。
//  T9 决定收敛为一条：GBuffer 的 mesh 路线只保留 Nanite 硬光栅；
//  本开关恒为 false（保留符号只为减少调用点的机械改动），后续清理可整体删除。
//
//  【Nanite 硬光栅在哪】`Engine/Render/Nanite/NaniteRaster.cpp` 的硬光栅分流
//  （`NaniteSettings::hardRaster`，T9 起默认开启）。
// ============================================================

namespace he::render {

/// 网格着色器 GBuffer 路径是否开启 —— **T9 起恒为 false（已废弃）**。
///
/// 保留函数签名而不删调用点：调用点散落在"建 PSO / 建合批缓冲 / 录制绘制"三处，
/// 一次删除会牵动三个文件的缩进与分支结构；恒 false 后那三处自然退化为 IA 路径，
/// 与"从未打开"完全一致（默认档的既有测试因此逐字节不变）。
inline bool GBufferMeshPathEnabled() {
    return false;
}

/// 每个网格工作组负责的三角形数（`GBuffer.mesh.slang` 的容量口径）
///
/// 【为什么留着】该 shader 仍参与编译与绑定对账（`AuditShaderBindings`），常量必须与它同源；
/// 只是本引擎不再创建这条 PSO。
constexpr unsigned kGBufferMeshTrianglesPerGroup = 64u;

} // namespace he::render
