// SMAA_BlendWeight.frag.slang — SMAA 混合权重计算 Pass
// 沿边缘方向搜索端点距离 → 解析计算混合权重 → 输出 RGBA8 四方向权重
//   R=左, G=右, B=上, A=下

struct VSOutput {
    float4 position : SV_Position;
    [[vk::location(0)]] float2 uv;
};

[[vk::binding(0, 0)]] Texture2D<float4> u_EdgeTex;    // R8 边缘类型（Point 采样）
[[vk::binding(0, 0)]] SamplerState      u_EdgeSampler; // Nearest-neighbor

[[vk::push_constant]] cbuffer BlendParams {
    float2 invScreenSize;
    float2 _pad;
};

// 最大搜索步数（沿边缘方向）
static const int kMaxSearchSteps = 16;

// 沿给定方向搜索边缘端点，返回归一化距离 [0, 1]
float SearchEdgeDistance(float2 uv, float2 dir) {
    for (int i = 1; i <= kMaxSearchSteps; i++) {
        float2 offset = dir * float(i);
        float e = u_EdgeTex.Sample(u_EdgeSampler, uv + offset * invScreenSize).r;
        // 遇到非边缘像素，返回当前位置
        if (e < 0.01) return float(i - 1) / float(kMaxSearchSteps);
    }
    return 1.0;  // 搜索到底
}

float4 fragmentMain(VSOutput input) : SV_Target0 {
    float2 uv = input.uv;

    // 读取当前像素边缘类型
    float edge = u_EdgeTex.Sample(u_EdgeSampler, uv).r;
    if (edge < 0.01)
        return float4(0.0, 0.0, 0.0, 0.0);  // 非边缘像素，无混合

    // 判断边缘方向：1.0 = 水平边缘（左右混合），0.5 = 垂直边缘（上下混合）
    bool isHorizontal = (edge > 0.75);

    float2 searchDir = isHorizontal ? float2(1.0, 0.0) : float2(0.0, 1.0);

    // 向两侧搜索边缘端点
    float distNeg = SearchEdgeDistance(uv, -searchDir);
    float distPos = SearchEdgeDistance(uv,  searchDir);

    // 解析计算混合权重：
    // 当前像素越接近边缘中心，权重越小（保留原始颜色更多）
    // 当前像素越接近边缘端点，权重越大（向端点方向混合更多）
    float totalDist = distNeg + distPos + 0.001;  // 避免除零
    float centerRatio = abs(distNeg - distPos) / totalDist;  // [0,1]，0=居中, 1=在端点

    // 边缘端点处的像素向另一端混合，中心像素保持不变
    float blendAmount = centerRatio * 0.5;  // 最大混合 50%

    if (isHorizontal) {
        // 水平边缘：左右方向
        float wLeft  = (distNeg > 0.0) ? blendAmount * (distNeg / totalDist) : 0.0;
        float wRight = (distPos > 0.0) ? blendAmount * (distPos / totalDist) : 0.0;
        return float4(wLeft, wRight, 0.0, 0.0);
    } else {
        // 垂直边缘：上下方向
        float wUp   = (distNeg > 0.0) ? blendAmount * (distNeg / totalDist) : 0.0;
        float wDown = (distPos > 0.0) ? blendAmount * (distPos / totalDist) : 0.0;
        return float4(0.0, 0.0, wUp, wDown);
    }
}
