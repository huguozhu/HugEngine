// RT_Shadow.rmiss.slang — 阴影射线 Miss 着色器
// 射线未命中任何几何体 → 像素对该光源可见（未被遮挡）

struct ShadowPayload {
    bool blocked;  // true=被遮挡, false=可见
};

[shader("miss")]
void main(inout ShadowPayload payload) {
    // 未命中 → 光线到达光源，像素可见
    payload.blocked = false;
}
