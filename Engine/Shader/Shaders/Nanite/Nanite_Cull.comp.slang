// ============================================================
// Nanite_Cull.comp.slang — 假簇 → 间接命令（§14.8 任务 3 的「计数」端）
//
// 【目标】建立模块自持的「计数 → 间接绘制」链的最小闭环：
//   输入 = N 个假簇（1 个实例、N 个簇）→ 每簇写一条间接命令 →
//   把「实际写入的命令条数」原子累加进计数缓冲。绘制端（Nanite_Raster）用
//   `DrawIndexedIndirectCount` 直接消费该计数，CPU 全程不读回、不参与条数决定。
//
// 【为什么用「原子取槽位再写命令」而不是按 id 直接索引】
//   这样命令缓冲是**压缩**的：计数缓冲的值恰好等于有效命令条数，
//   任务 3 的验收（count_buffer == indirect_cmds == rasterized_clusters）才有意义。
//
// 【与 C++ 侧的布局契约】本文件的 `FakeCluster` / `IndirectCmd` 必须与
//   `Engine/Render/Nanite/NaniteTypes.h` 的 `NaniteFakeCluster` / `NaniteIndirectCommand`
//   逐字段一致（后者用 static_assert 钉住尺寸与偏移）。
//
// 【§14.3 依赖禁令】本文件只依赖本模块自己的 POD，不引用 GI/Lumen/GPUCulling。
// ============================================================

// 假簇条目（与 NaniteFakeCluster 一致：4×u32 = 16 B）
struct FakeCluster {
    uint clusterId;        // 簇编号（假数据 = 顺序编号）
    uint instanceId;       // 所属实例（任务 3 固定为 0，即"1 个实例"）
    uint triangleCount;    // 该簇的三角形数（假数据固定 1）
    uint _pad;             // 对齐填充：保持 16 B（便于 CPU 侧按 4 字节对齐读回）
};

// 间接绘制命令（必须与 VkDrawIndexedIndirectCommand 二进制兼容：5×4 = 20 B）
struct IndirectCmd {
    uint indexCount;
    uint instanceCount;
    uint firstIndex;
    int  vertexOffset;
    uint firstInstance;
};

// 显式描述符绑定（set=0）：0=假簇输入（只读）、1=间接命令（读写）、2=命令条数（读写）
[[vk::binding(0, 0)]] StructuredBuffer<FakeCluster>   u_FakeClusters;
[[vk::binding(1, 0)]] RWStructuredBuffer<IndirectCmd> u_Commands;
[[vk::binding(2, 0)]] RWStructuredBuffer<uint>        u_Counts;

[[vk::push_constant]] cbuffer NaniteCullParams {
    uint clusterCount;           // 本帧假簇数量（= cfg 键 nanite_fake_clusters）
    uint vertexCountPerCluster;  // 每条命令的 indexCount（假数据：3 = 一个三角形）
};

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= clusterCount) return;

    FakeCluster c = u_FakeClusters[id.x];

    // ① 原子取槽位并把计数 +1：计数缓冲的最终值 = 实际写入的命令条数
    uint slot;
    InterlockedAdd(u_Counts[0], 1u, slot);

    // ② 写间接命令。instanceCount 恒为 1：每个簇画 1 个三角形（绘制端据此验证"恰好画 k 次"）。
    IndirectCmd cmd;
    cmd.indexCount    = vertexCountPerCluster;
    cmd.instanceCount = 1u;
    cmd.firstIndex    = 0u;                 // 假数据不关心真正的索引来源
    cmd.vertexOffset  = 0;
    cmd.firstInstance = c.clusterId;        // 传递簇号，绘制端用 SV_InstanceID 接收
    u_Commands[slot]  = cmd;
}
