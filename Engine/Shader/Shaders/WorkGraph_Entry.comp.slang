// WorkGraph_Entry.comp.slang — Work Graph 入口节点 Compute Shader
//
// 读取输入 WGRecord 数组，执行视锥剔除（当前简化：pass-through），
// 输出可见物体对应的 WGRecord 数组供下游 Draw 节点使用。
//
// 布局（set=0）：
//   binding 0: StructuredBuffer<WGRecord>  输入 Record（CPU PushEntryWork）
//   binding 1: RWStructuredBuffer<WGRecord> 输出 Record（传递给下游）
//   binding 2: RWStructuredBuffer<uint>    输出 Record 计数器（原子操作）
//
// Push Constants：
//   targetNodeID: 输出 Record 的目标节点 ID
//   maxOutputs:   最大输出 Record 数
//   inputCount:   输入 Record 数
//   _pad:         填充

struct WGRecord {
    uint nodeID;       // 目标节点 ID
    uint payload[7];   // 负载数据（objectIndex 等）
};

[[vk::binding(0, 0)]] StructuredBuffer<WGRecord>    u_Input;
[[vk::binding(1, 0)]] RWStructuredBuffer<WGRecord>  u_Output;
[[vk::binding(2, 0)]] RWStructuredBuffer<uint>      u_Counter;

[[vk::push_constant]] cbuffer WorkGraphParams {
    uint targetNodeID;   // 输出 Record 的目标节点 ID
    uint maxOutputs;     // 最大输出 Record 数
    uint inputCount;     // 输入 Record 数
    uint _pad;           // 16 字节对齐
};

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    // 边界检查
    if (id.x >= inputCount) return;

    // 读取输入 Record
    WGRecord input = u_Input[id.x];

    // ── 视锥剔除模拟 ──
    // 简化版：所有物体通过剔除，直接输出 Record。
    // 生产代码中在此处执行完整视锥+遮挡剔除：
    //   1. 从 input.payload[0] 获取物体索引
    //   2. 通过 bindless 纹理或额外 SSBO 读取物体 AABB
    //   3. 执行视锥平面测试 + Hi-Z 遮挡测试
    //   4. 仅对可见物体写入输出 Record

    // 原子递增输出计数器
    uint idx;
    InterlockedAdd(u_Counter[0], 1, idx);

    // 边界检查：不超过最大输出数
    if (idx >= maxOutputs) return;

    // 构建输出 Record
    WGRecord output;
    output.nodeID       = targetNodeID;                  // 下游节点 ID
    output.payload[0]   = input.payload[0];              // 传递物体索引
    output.payload[1]   = 1;                             // 可见标志
    output.payload[2]   = 0;
    output.payload[3]   = 0;
    output.payload[4]   = 0;
    output.payload[5]   = 0;
    output.payload[6]   = 0;

    // 写入输出
    u_Output[idx] = output;
}
