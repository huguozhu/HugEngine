// ============================================================
// InstancedCull.comp.slang — 逐实例视锥剔除（任务 25）
//
// 与 GPUCull.comp.slang 的区别（那份是**逐物体**剔除：一个物体一条间接命令）：
//   实例化网格是"一个组件 = 上万个实例"。逐物体粒度对它没有意义 —— 只要实例网格的
//   整体 AABB 有一部分在视锥内，整批 10000 个实例就都会被画出来（GPU 白白处理 90% 看不见的实例）。
//   本 Pass 把粒度下推到**每个实例**：按实例的世界矩阵变换局部包围盒 → 视锥测试 →
//   把通过测试的实例索引**压缩**写进可见列表，并原子累加该组件的 DrawIndexedIndirect
//   instanceCount。绘制端用 `DrawIndexedIndirect(cmd, 0, 1, 20)` + 顶点着色器查可见列表。
//
// 【为什么用 bindless SSBO 数组寻址而不是每组件更新描述符集】Vulkan 描述符是**执行时**读取的：
//   同一个命令缓冲里"更新描述符 → dispatch A → 更新描述符 → dispatch B"时，GPU 执行到 A 时
//   描述符已经指向 B 的缓冲了。逐组件传**句柄**（bindless 数组下标）就完全没有这个问题，
//   这也是引擎里实例变换/骨骼矩阵一直用的方式（见 PBR.vert 的 u_InstanceTransforms[handle]）。
//
// 共享可见列表：cull 与 draw 在同一命令缓冲里按组件顺序交替（cull A → draw A → cull B → …），
//   同队列顺序执行 + 一次全局屏障保证 draw A 读到的就是 cull A 写的内容，因此列表可全局共用一份。
// ============================================================

struct IndirectCmd {
    uint indexCount;
    uint instanceCount;
    uint firstIndex;
    int  vertexOffset;
    uint firstInstance;
};

// bindless SSBO 数组（binding 30，与引擎其它 shader 一致），全部按 push constant 里的句柄索引
[[vk::binding(30, 0)]] StructuredBuffer<float4x4>   u_Instances[];
[[vk::binding(30, 0)]] RWStructuredBuffer<uint>     u_VisibleIndices[];
[[vk::binding(30, 0)]] RWStructuredBuffer<IndirectCmd> u_Commands[];

[[vk::push_constant]] cbuffer InstancedCullParams {
    float4 frustumPlanes[6];      // 世界空间视锥平面（已归一化，n·p + d = 0 外侧为正）
    float4 localBoundsMin;        // 网格局部包围盒（实例矩阵变换前）
    float4 localBoundsMax;
    uint   instanceCount;
    uint   instanceBufferHandle;  // u_Instances[] 句柄
    uint   visibleBufferHandle;   // u_VisibleIndices[] 句柄
    uint   commandHandle;         // u_Commands[] 句柄（单元素：本组件的间接命令）
};

// 保守的 AABB-平面测试：取 AABB 沿平面法线的**最远角**（support point）；
// 余量 0.01（平面已归一化 ⇒ 约 1cm 世界空间），避免浮点误差在视锥边界误剔除
bool AABBOutside(float3 aabbMin, float3 aabbMax, float4 plane) {
    float3 n = plane.xyz;
    float3 p;
    p.x = (n.x >= 0.0) ? aabbMax.x : aabbMin.x;
    p.y = (n.y >= 0.0) ? aabbMax.y : aabbMin.y;
    p.z = (n.z >= 0.0) ? aabbMax.z : aabbMin.z;
    return dot(n, p) + plane.w < -0.01;
}

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= instanceCount) return;

    // 1. 实例世界矩阵 → 局部包围盒的 8 个角变换到世界空间求 AABB
    float4x4 M = u_Instances[instanceBufferHandle][id.x];
    float3 mn = float3(1e30, 1e30, 1e30);
    float3 mx = float3(-1e30, -1e30, -1e30);
    [unroll]
    for (int c = 0; c < 8; ++c) {
        float3 corner = float3((c & 1) ? localBoundsMax.x : localBoundsMin.x,
                               (c & 2) ? localBoundsMax.y : localBoundsMin.y,
                               (c & 4) ? localBoundsMax.z : localBoundsMin.z);
        float3 wp = mul(M, float4(corner, 1.0)).xyz;
        mn = min(mn, wp);
        mx = max(mx, wp);
    }

    // 2. 六个平面逐个测试（任一平面外侧 ⇒ 剔除）
    [unroll]
    for (int i = 0; i < 6; ++i)
        if (AABBOutside(mn, mx, frustumPlanes[i])) return;

    // 3. 通过 → 压缩写入可见列表，并原子累加本组件的实例计数
    uint slot;
    InterlockedAdd(u_Commands[commandHandle][0].instanceCount, 1u, slot);
    u_VisibleIndices[visibleBufferHandle][slot] = id.x;
}
