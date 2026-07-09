// RT_Sponza.rmiss.slang — 天空渐变背景
struct RayPayload { float3 color; float hitT; };

[shader("miss")]
void main(inout RayPayload payload) {
    float3 rayDir = WorldRayDirection();
    float t = clamp(rayDir.y * 0.5 + 0.5, 0.0, 1.0);
    float3 skyBottom = float3(0.55, 0.45, 0.35);   // 暖色地面
    float3 skyTop    = float3(0.25, 0.45, 0.7);    // 蓝色天顶
    payload.color = lerp(skyBottom, skyTop, t);
}
