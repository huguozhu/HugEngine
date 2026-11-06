#pragma once

#include <cstdlib>

// ============================================================
// GBufferMeshMode.h — 【P9】GBuffer 网格着色器路径开关
//
// 环境变量 `HE_GBUFFER_MESH=1` 打开：GPU 驱动的 GBuffer 绘制改用
// `GBuffer.mesh.slang`（网格着色器从合并顶点/索引 SSBO 读几何）+ `DrawMeshTasks`，
// 片元着色器仍是 `GBuffer.frag`。默认关闭 ⇒ 所有既有路径逐字节不变。
//
// 【为什么只读一次】开关在建 PSO / 建合批缓冲 / 录制绘制三处都要问，必须给同一个答案
//（若三处各读一次环境变量，程序中途改环境就可能在"有 PSO 没几何"的状态下绘制）。
// ============================================================
namespace he::render {

/// 网格着色器 GBuffer 路径是否开启（`HE_GBUFFER_MESH` 非零即开启，默认关闭）
inline bool GBufferMeshPathEnabled() {
    static const bool s_Enabled = [] {
        const char* v = std::getenv("HE_GBUFFER_MESH");
        return v != nullptr && std::atoi(v) != 0;
    }();
    return s_Enabled;
}

/// 每个网格工作组负责的三角形数（= `GBuffer.mesh.slang` 的 kMeshGroupMaxTriangles）
/// 【必须与 shader 一致】shader 里 192 输出顶点 / 64 图元的容量按它算；CPU 侧的
/// `DrawMeshTasks` 组数 = ceil(三角形数 / 本值)，改了这里而没改 shader 会漏画三角形。
constexpr unsigned kGBufferMeshTrianglesPerGroup = 64u;

} // namespace he::render
