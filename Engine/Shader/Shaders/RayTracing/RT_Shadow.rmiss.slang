// RT_Shadow.rmiss.slang — 阴影射线 Miss 着色器
// 射线未命中任何几何体 → 像素对该光源可见（未被遮挡）

#include "RTPayloads.slang"   // ShadowPayload / RayPayload / GIPayload / ReflectionPayload（唯一出处）

[shader("miss")]
void main(inout ShadowPayload payload) {
    // 未命中 → 光线到达光源，像素可见
    payload.blocked = false;
}
