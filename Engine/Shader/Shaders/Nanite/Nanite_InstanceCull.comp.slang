// ============================================================
// Nanite_InstanceCull.comp.slang — 实例剔除（§14.8 任务 13，设计 §5.1 Phase 1 的视锥粗筛）
//
// 【目标】逐实例做世界空间视锥剔除，把可见实例**压缩**写入可见列表，并原子累加可见计数。
//   这是 Phase 1（Instance Culling）的视锥部分；Hi-Z 遮挡（Phase 1 的后半）与 Phase 2/3 的
//   簇 BVH/LOD 属任务 14/15，本文件不涉及。
//
// 【与 C++ 侧的布局契约】本文件的每个结构体都必须与 `Engine/Render/Nanite/NaniteTypes.h`
//   的“§14.8 任务 13”小节逐字段一致（那边用 sizeof + offsetof 的 static_assert 钉住）：
//     · `InstanceObject` —— **GPUSceneObject（128B）契约**的镜像；真身在
//       `Engine/Render/Pipeline/GPUScene.h:26-40`，`NaniteCull.cpp` 另有逐字段交叉 static_assert。
//     · `InstanceSphere` —— 每实例包围球（CPU 侧从 128B 的 boundsMin/boundsMax 推一次并上传；
//       shader 不现推，理由见 NaniteTypes.h 里该结构体的注释：避免 GPU/CPU 浮点末位差异
//       在“球正好切在平面上”的实例上翻转可见性）。
//     · push constant —— 6 个世界空间视锥平面 + 实例数。
//
// 【判据（必须与 CPU 参考逐字一致）】平面 `planes[i] = (n.xyz, d)`，`dot(n, p) + d >= 0` 在内侧；
//   任一平面满足 `dot(n, c) + d < -radius` ⇒ 不可见（**不加 epsilon**）。顺序
//   `[左, 右, 下, 上, 近, 远]`，法线已归一化（CPU 侧提取函数负责）。
//
// 【可见列表的顺序】用“原子取槽位再写”压缩 ⇒ 槽位分配顺序**不定**（GPU 线程调度相关）。
//   CPU 参考按升序紧凑写入；验收读回会把 GPU 列表排序后再逐项比较（见 NaniteRenderer.cpp）。
//   本任务没有消费者按顺序读它，任务 14 起也不依赖顺序（可见集合才是语义）。
//
// 【§14.3 依赖禁令】只依赖本模块自己的 POD，不引用 GI/Lumen/GPUCulling。
// ============================================================

// GPUSceneObject（128B）的 Slang 镜像：字段顺序/类型必须与 NaniteInstanceGpuObject 一致
struct InstanceObject {
    float4x4 localToWorld;    // 偏移 0
    float4   boundsMin;       // 偏移 64：世界空间 AABB 下界（w 不用）
    float4   boundsMax;       // 偏移 80：世界空间 AABB 上界（w 不用）
    uint     meshIndex;       // 偏移 96
    uint     materialIndex;   // 偏移 100
    uint     objectID;        // 偏移 104
    uint     visibilityFlags; // 偏移 108
    uint     indexCount;      // 偏移 112：0 ⇒ 空实例，跳过（与 CPU 参考同规则）
    uint     firstIndex;      // 偏移 116
    int      vertexOffset;    // 偏移 120
    uint     _pad;            // 偏移 124
};

// 每实例包围球（世界空间；与 NaniteInstanceSphere 一致：3×float + radius = 16B）
//
// 【为什么写成 float4 而不是 float3 + float】Slang/SPIR-V 的 std430 下 `float3` 成员按
//   16B 对齐，`float3 center; float radius;` 的偏移会退化成 16（步长 32），与 C++ 侧
//   16B 的契约对不上。写成 `float4(xyz=球心, w=半径)` 后 C++ 的
//   `float center[3]; float radius;` 与它逐字节同布局，风险归零。
struct InstanceSphere {
    float4 centerRadius;   // xyz = 世界空间球心；w = 半径（≥ 0）
};

// 显式描述符绑定（set=0）：
//   0 = 实例表（128B 契约，只读；本任务读 indexCount 判空实例）
//   1 = 包围球表（只读）
//   2 = 可见实例列表（读写；原子压缩写入）
//   3 = 可见实例计数（读写；单个 uint，CPU 每帧清零）
[[vk::binding(0, 0)]] StructuredBuffer<InstanceObject> u_Instances;
[[vk::binding(1, 0)]] StructuredBuffer<InstanceSphere> u_Spheres;
[[vk::binding(2, 0)]] RWStructuredBuffer<uint>          u_VisibleInstances;
[[vk::binding(3, 0)]] RWStructuredBuffer<uint>          u_VisibleCount;

[[vk::push_constant]] cbuffer NaniteInstanceCullParams {
    float4 planes[6];      // 偏移 0：世界空间视锥六平面（[左,右,下,上,近,远]，n 已归一化）
    uint   instanceCount;  // 偏移 96：本帧实例数（= cfg 键 nanite_instance_test_count，已钳制）
    uint   _pad0;          // 偏移 100：填充到 16B 对齐（与 C++ 侧 sizeof == 112 一致）
    uint   _pad1;          // 偏移 104
    uint   _pad2;          // 偏移 108
};

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= instanceCount) return;

    // ① 空实例（无索引可画）不参与 —— 与 CPU 参考的 indexCount == 0 跳过逐条对应。
    //    这一读也让“实例表就是 GPUSceneObject 的 128B 契约”在 shader 侧真正被消费。
    InstanceObject instance = u_Instances[id.x];
    if (instance.indexCount == 0u) return;

    // ② 逐平面球测试：任一平面外侧 ⇒ 不可见（与 NaniteSphereVisibleInFrustum 同判据）
    InstanceSphere sphere = u_Spheres[id.x];
    for (int i = 0; i < 6; ++i) {
        float distance = dot(planes[i].xyz, sphere.centerRadius.xyz) + planes[i].w;
        if (distance < -sphere.centerRadius.w) return;
    }

    // ③ 原子取槽位并计数：计数缓冲的最终值 = 实际写入的可见实例数（压缩列表）
    uint slot;
    InterlockedAdd(u_VisibleCount[0], 1u, slot);
    u_VisibleInstances[slot] = id.x;
}
