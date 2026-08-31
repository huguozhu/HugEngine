// AITensorScale.comp.slang — AI 张量逐元素缩放（A3.1 GPU 推理最小验证核）
//
// 用途：验证 IAIDevice → GPUBackend → RHI compute 全链路。
// 语义：out[i] = in[i] * scale（纯数据搬移 + 标量乘，无模型依赖）。
// 后续真实模型（ONNX/DirectML）接入后，此核被模型推理替代。

// 输入张量（只读）
[[vk::binding(0, 0)]] StructuredBuffer<float> u_Input;
// 输出张量（可写）
[[vk::binding(1, 0)]] RWStructuredBuffer<float> u_Output;

// 推参：元素个数 + 缩放系数（C++ 侧结构需严格对齐）
[[vk::push_constant]] cbuffer ScaleParams {
    uint  count;    // 元素个数
    float scale;    // 缩放系数
};

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= count) return;
    u_Output[id.x] = u_Input[id.x] * scale;
}
