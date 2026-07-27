// RT_Common.rmiss.slang — 通用 Miss 着色器
// 用于 Shadow / AO / Reflection 的未命中处理
//
// 数据流：
//   Shadow:     未命中 → payload.blocked = false（光线到达光源，可见）
//   AO:         未命中 → payload.blocked = false（未遮蔽）
//   Reflection: 未命中 → 回退到 IBL / 天空（使用专用 Miss 如 RT_Sponza.rmiss）

struct ShadowPayload {
    bool blocked;  // true=被遮挡, false=可见
};

[shader("miss")]
void main(inout ShadowPayload payload) {
    // 未命中 = 光线未碰到任何几何体 → 像素对该光源可见
    payload.blocked = false;
}
