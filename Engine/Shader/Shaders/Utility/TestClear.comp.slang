// 测试计算着色器：将 Storage Image 清零
// 用于验证 Compute PSO + Dispatch 通路

[[vk::binding(0, 0)]] RWTexture2D<float4> u_Output;

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    u_Output[id.xy] = float4(0.0, 0.0, 0.0, 1.0);
}
