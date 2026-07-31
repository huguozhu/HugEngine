// RT_Shadow.rahit.slang — 阴影射线 AnyHit 着色器
//
// 用途：阴影/AO 遮挡检测。光线命中任一三角形即认为被遮挡。
// 配合 RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH：
//   - 此着色器置 payload.blocked = true（不调用 IgnoreHit）
//   - ACCEPT_FIRST_HIT 使遍历在第一个被接受命中处立即终止
//   - 配合 RAY_FLAG_SKIP_CLOSEST_HIT_SHADER 跳过材质计算，性能最优
//
// 注意：不能用 ClosestHit 置 blocked 配合 SKIP_CLOSEST_HIT——
// 那会导致 payload 永不置位、阴影恒为 1.0（无阴影）。这是旧版 bug 根源。

struct ShadowPayload {
    bool blocked;  // true=被遮挡, false=可见
};

[shader("anyhit")]
void main(inout ShadowPayload payload, in BuiltInTriangleIntersectionAttributes attr) {
    // 命中几何体 → 光线被遮挡（接受命中，ACCEPT_FIRST_HIT 终止遍历）
    payload.blocked = true;
}
