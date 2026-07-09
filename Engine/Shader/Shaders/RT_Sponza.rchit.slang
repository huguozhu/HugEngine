// RT_Sponza.rchit.slang — Phase 2 命中着色（终版）
// 法线近似 = -WorldRayDirection()，对正对相机的面准确，掠射角有误差

struct RayPayload {
    float3 color;
    float  hitT;
};

[shader("closesthit")]
void main(inout RayPayload payload) {
    uint id = InstanceID();

    // 法线近似：光线反方向
    float3 N = -WorldRayDirection();

    // 主方向光（上偏右，暖色）
    float dotL0 = max(dot(N, normalize(float3(0.5, -0.9, 0.3))), 0.0);
    float3 keyLight = float3(1.0, 0.92, 0.78) * dotL0 * 0.55;

    // 补光（下偏左，冷色）— 减少暗部
    float dotL1 = max(dot(N, normalize(float3(-0.3, 0.4, -0.2))), 0.0);
    float3 fillLight = float3(0.45, 0.50, 0.58) * dotL1 * 0.35;

    // 环境光（提高以补偿掠射角误差）
    float3 ambient = float3(0.22, 0.23, 0.26);
    // 半球光：上方偏亮
    float hemi = (N.y * 0.5 + 0.5) * 0.12;

    float3 lighting = ambient + hemi + keyLight + fillLight;

    // InstanceID → 暖石色
    uint h = id * 2654435761u;
    float3 baseColor = float3(
        0.52 + float((h >> 0)  & 0xFF) / 255.0 * 0.42,
        0.36 + float((h >> 8)  & 0xFF) / 255.0 * 0.38,
        0.20 + float((h >> 16) & 0xFF) / 255.0 * 0.30);

    payload.color = baseColor * lighting;
}
