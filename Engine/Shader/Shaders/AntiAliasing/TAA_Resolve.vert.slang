// TAA_Resolve.vert — TAA 全屏三角形顶点着色器（无 VB/IB）
// 3 顶点通过 SV_VertexID 生成，覆盖 [0,1] UV 全屏

#include "FullscreenVS.slang"   // VSOutput / VSOutputWorldDir（接口唯一出处）

VSOutput vertexMain(uint vid : SV_VertexID) {
    VSOutput o;
    o.uv = float2((vid << 1) & 2, vid & 2);
    o.position = float4(o.uv * 2.0 - 1.0, 0.0, 1.0);
    o.uv.y = 1.0 - o.uv.y;  // Vulkan 上下翻转
    return o;
}
