// AITensorAdd.comp.slang — 外部核示例：逐元素加法 out = in + add
//
// 用途：验证 GPUBackend 的「外部 SPIR-V 即插即用」路径 ——
// 该 shader 的 SPIR-V 由运行时经 InferenceRequest::customSpirv 传入
// （对 GPUBackend 而言是外部字节，非内置注册表），
// 按需构建 PSO 并缓存。
// 描述符布局与 AITensorScale 相同：2 个 StorageBuffer + {count, add} 推参。

[[vk::binding(0, 0)]] StructuredBuffer<float> u_Input;
[[vk::binding(1, 0)]] RWStructuredBuffer<float> u_Output;

[[vk::push_constant]] cbuffer AddParams {
    uint  count;    // 元素个数
    float add;      // 加数
};

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= count) return;
    u_Output[id.x] = u_Input[id.x] + add;
}
