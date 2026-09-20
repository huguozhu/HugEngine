// ============================================================
// Nanite_MeshTest.frag.slang — §14.8 任务 6 的 mesh 通道片元着色器
//
// 【唯一职责】把 mesh shader 真正输出并光栅化到的图元数记下来（原子加一），
//   同时往模块自建的 1×1 R8 目标写一个非零值。
//   这两件事就是任务 6「能输出非空画面」的两个独立 GPU 读数来源：
//     · `u_MeshTestCount[0]`   → meshlet_outputs（dump 帧读回；本帧应为 2）
//     · 1×1 R8 目标的像素值     → target_max（dump 帧用 CopyTextureToBuffer 读回；应为 255）
//
// 【为什么是"图元数"而不是"片元数"的近似】目标是 1×1 且 mesh shader 输出的是
//   覆盖整个 NDC 的四边形（2 个三角形），每个三角形只覆盖这唯一一个像素 ⇒
//   片元数恰好等于被光栅化的图元数（见 Nanite_MeshTest.mesh.slang 的说明）。
//
// 【绝不能改动可见画面】本 pass 渲染到模块自建的 1×1 R8 小目标（不是 GBuffer 的任何一张），
//   颜色值本身只是为了读回时能看出"确实写了"。
//
// 【§14.3 依赖禁令】只依赖模块自持的计数缓冲，不引用 GI/Lumen/GPUCulling。
// ============================================================

[[vk::binding(0, 0)]] RWStructuredBuffer<uint> u_MeshTestCount;   // [0] = 已光栅化图元数

struct MeshTestFragIn {
    float4 position : SV_Position;
};

[shader("fragment")]
float4 fragmentMain(MeshTestFragIn input) : SV_Target {
    // 每被光栅化一个 mesh 图元加一（1×1 目标 ⇒ 一个图元恰好一个片元）
    InterlockedAdd(u_MeshTestCount[0], 1u);
    // 写入 1.0：R8_UNORM 读回即 255，是"非空"的第二个独立证据
    return float4(1.0, 1.0, 1.0, 1.0);
}
