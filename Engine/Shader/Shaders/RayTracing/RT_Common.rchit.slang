// RT_Common.rchit.slang — 通用 ClosestHit 着色器
// 用于 Shadow / AO 的命中处理（不需要材质计算的 Pass）
//
// 数据流：
//   Shadow: 命中 → payload.blocked = true（被遮挡）
//   AO:     命中 → payload.blocked = true（被遮蔽）
//   Reflection: 命中 → 使用 RT_PBR.rchit 或 RT_PositionFetch.rchit（需要材质计算）
//
// 使用 RAY_FLAG_SKIP_CLOSEST_HIT_SHADER 可跳过此着色器，仅用 AnyHit 或 miss/hit 检测

struct ShadowPayload {
    bool blocked;  // true=被遮挡, false=可见
};

[shader("closesthit")]
void main(inout ShadowPayload payload) {
    // 命中几何体 → 光线被遮挡 / AO 被遮蔽
    payload.blocked = true;
}
