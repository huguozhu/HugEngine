// RT_GI.rmiss.slang — GI 射线 Miss 着色器
//
// 未命中 → 回退到程序化天空色（天光作为远距离间接漫反射来源）。

struct GIPayload {
    float4 radianceT;  // rgb=辐射度, a=命中距离
};

[shader("miss")]
void main(inout GIPayload payload) {
    float3 dir = WorldRayDirection();
    float t = clamp(dir.y * 0.5 + 0.5, 0.0, 1.0);  // 天顶→地平线
    float3 sky = lerp(float3(0.1, 0.12, 0.2), float3(0.75, 0.78, 0.85), t);
    payload.radianceT = float4(sky * 0.5, -1.0);  // 天空强度缩放
}
