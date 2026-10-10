// FXAA.vert.slang — 全屏三角形
#include "FullscreenVS.slang"   // VSOutput / VSOutputWorldDir（接口唯一出处）

VSOutput vertexMain(uint vid : SV_VertexID) {
    float2 pos[3] = { float2(-1,-1), float2(3,-1), float2(-1,3) };
    VSOutput o;
    o.position = float4(pos[vid], 0, 1);
    o.uv = float2(pos[vid].x*0.5+0.5, 0.5-pos[vid].y*0.5);
    return o;
}
