// AITextureGen.comp.slang — GPU 侧程序化纹理生成核（G2.2 本地资产生成）
//
// 用途：把纹理生成搬到 GPU（经 IAIDevice → GPUBackend 执行），
// 与 CPU 版 TextureGenerator 同语义（solid/gradient/checker/noise）。
// 布局：binding 0 = 输出缓冲（RWStructuredBuffer<float4>，w*h 个像素）。
// 推参：{width,height,pattern,seed,colorA(float4),colorB(float4)}（48 字节）。

[[vk::binding(0, 0)]] RWStructuredBuffer<float4> u_Output;

[[vk::push_constant]] cbuffer GenParams {
    uint   width;     // 纹理宽
    uint   height;    // 纹理高
    uint   pattern;   // 0=solid 1=gradient 2=checker 3=noise
    uint   seed;      // 噪声种子
    float4 colorA;    // 主色 / 渐变起点 / 棋盘色1
    float4 colorB;    // 副色 / 渐变终点 / 棋盘色2
};

// 整数哈希（噪声用，可复现）
uint Hash(uint s) {
    s ^= s >> 16; s *= 0x7feb352du;
    s ^= s >> 15; s *= 0x846ca68bu;
    s ^= s >> 16;
    return s;
}

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= width * height) return;
    uint x = id.x % width;
    uint y = id.x / width;

    float4 c = colorA;   // 默认 solid
    if (pattern == 1) {
        // gradient：左→右 colorA→colorB
        float t = (width <= 1) ? 0.0f : float(x) / float(width - 1);
        c = lerp(colorA, colorB, t);
    } else if (pattern == 2) {
        // checker：8×8 棋盘
        uint cell = max(width / 8, 1u);
        bool on = ((x / cell) + (y / cell)) % 2 == 0;
        c = on ? colorA : colorB;
    } else if (pattern == 3) {
        // noise：灰度随机（colorA 亮度基底混合 30%）
        uint r = Hash(y * width + x + seed);
        float n = float(r & 255) / 255.0;
        c = float4(float3(n) * 0.7 + colorA.rgb * 0.3, 1.0);
    }
    c.a = 1.0;
    u_Output[id.x] = c;
}
