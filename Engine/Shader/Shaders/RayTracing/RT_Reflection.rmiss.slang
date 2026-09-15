// RT_Reflection.rmiss.slang — 反射射线 Miss 着色器
//
// 未命中 → 回退到程序化天空色（水平线渐变，按射线方向插值）。
// 对镜面反射，天空反射是正确的结果；对粗糙反射，作为环境回退。

struct ReflectionPayload {
    float4 colorT;  // rgb=反射颜色, a=命中距离
};

[shader("miss")]
void main(inout ReflectionPayload payload) {
    float3 dir = WorldRayDirection();
    float t = clamp(dir.y * 0.5 + 0.5, 0.0, 1.0);  // 天顶→地平线
    float3 sky = lerp(float3(0.1, 0.12, 0.2), float3(0.75, 0.78, 0.85), t);
    payload.colorT = float4(sky, -1.0);  // a=-1 标记未命中
}
