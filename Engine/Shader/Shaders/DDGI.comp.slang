// DDGI.comp.slang — 动态漫反射全局光照探针更新
// 每线程处理一个探针：球面采样 GBuffer → SH 投影 → 写入探针缓冲
//
// 探针数据布局（RWStructuredBuffer<float4>，每探针 16 float4）：
//   [0..8]  SH 系数 band 0/1/2 (9×float4, RGB 存储)
//   [9..15] 保留

#include "ShaderTypes.slang"  // GPU 常量 (kGPU*) + 结构体

// GBuffer 输入（只读，CombinedImageSampler）
[[vk::binding(kGPUBinding_GBufferA, kGPUDescSet_PerFrame)]] Texture2D<float4> u_GBufferA;
[[vk::binding(kGPUBinding_GBufferB, kGPUDescSet_PerFrame)]] Texture2D<float4> u_GBufferB;
[[vk::binding(kGPUBinding_Depth, kGPUDescSet_PerFrame)]] Texture2D<float>  u_Depth;
[[vk::binding(0, 0)]] SamplerState      u_PointSampler; // 点采样（GBuffer 读取）

// 探针输出（读写，当前帧结果）
[[vk::binding(3, 0)]] RWStructuredBuffer<float4> u_Probes;

// 上一帧探针历史（只读，时间混合源）
[[vk::binding(5, 0)]] StructuredBuffer<float4> u_ProbeHistory;

// 前帧 HDR 辐射度（CombinedImageSampler, 与 u_LinearSampler 共享 binding slot）
// 替代之前的 albedo * 0.3 近似，提供探针采样的真实光照
[[vk::binding(6, 0)]] Texture2D<float4> u_PrevHDR;
[[vk::binding(6, 0)]] SamplerState u_LinearSampler;

// 探针网格 + 相机参数
[[vk::binding(4, 0)]] cbuffer DDGIParams {
    float4   u_GridOrigin;   // xyz=世界空间原点
    float4   u_GridSize;     // xyz=探针数量, w=cellSize
    float4   u_CameraPos;    // xyz=相机世界位置
    float4   u_Params;       // x=intensity, y=numSamples, z=blendAlpha, w=historyValid
    float4x4 u_ViewProj;     // 相机 View→Proj（世界→裁剪空间）
};

// 每探针 float4 数量（与 C++ kFloats4PerProbe 保持一致）
static const uint kStride = 16;

// ---- SH 基函数常量 ----
static const float kSH_Y00 = 0.28209479177387814;  // l=0: sqrt(1/(4π))

// Band 1 (l=1): 3 个系数
float3 SH_Band1(float3 dir) {
    return dir * 0.4886025119029199;  // sqrt(3/(4π))
}

// Band 2 (l=2): 5 个系数
void SH_Band2(float3 dir, out float sh[5]) {
    float xx = dir.x * dir.x, yy = dir.y * dir.y, zz = dir.z * dir.z;
    float xy = dir.x * dir.y, yz = dir.y * dir.z, xz = dir.x * dir.z;
    sh[0] = 1.0925484305920792 * xy;                   // l=2,m=-2: sqrt(15/(4π))*xy
    sh[1] = 1.0925484305920792 * yz;                   // l=2,m=-1: sqrt(15/(4π))*yz
    sh[2] = 0.31539156525252005 * (3.0 * zz - 1.0);    // l=2,m=0:  sqrt(5/(16π))*(3z²-1)
    sh[3] = 1.0925484305920792 * xz;                   // l=2,m=1:  sqrt(15/(4π))*xz
    sh[4] = 0.5462742152960396 * (xx - yy);             // l=2,m=2:  sqrt(15/(16π))*(x²-y²)
}

// 将方向 dir 投影为 9 个 SH 系数（bands 0/1/2）
void SHBasis(float3 dir, out float sh[9]) {
    sh[0] = kSH_Y00;                                    // l=0,m=0
    float3 b1 = SH_Band1(dir);
    sh[1] = b1.y; sh[2] = b1.z; sh[3] = b1.x;          // l=1: m=-1,0,1
    // l=2: 5 个系数
    float s2[5];
    SH_Band2(dir, s2);
    sh[4] = s2[0]; sh[5] = s2[1]; sh[6] = s2[2];
    sh[7] = s2[3]; sh[8] = s2[4];
}

// Fibonacci 球面：均匀分布在单位球面上的第 i 个方向（共 n 个）
float3 FibonacciSphere(int i, int n) {
    float phi   = acos(1.0 - 2.0 * (float(i) + 0.5) / float(n));
    float theta = 3.14159265359 * (1.0 + sqrt(5.0)) * float(i);
    return float3(sin(phi) * cos(theta), cos(phi), sin(phi) * sin(theta));
}

// 写入 SH 系数到探针缓冲
void WriteSH(uint probeIndex, float4 sh[9]) {
    uint base = probeIndex * kStride;
    u_Probes[base + 0] = sh[0];
    u_Probes[base + 1] = sh[1];
    u_Probes[base + 2] = sh[2];
    u_Probes[base + 3] = sh[3];
    u_Probes[base + 4] = sh[4];
    u_Probes[base + 5] = sh[5];
    u_Probes[base + 6] = sh[6];
    u_Probes[base + 7] = sh[7];
    u_Probes[base + 8] = sh[8];
    for (uint i = 9; i < kStride; i++) {
        u_Probes[base + i] = float4(0, 0, 0, 0);
    }
}

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint probeIndex = id.x;
    uint gridX = uint(u_GridSize.x);
    uint gridY = uint(u_GridSize.y);
    uint gridZ = uint(u_GridSize.z);
    uint probeCount = gridX * gridY * gridZ;
    int  numSamples = int(u_Params.y);
    if (probeIndex >= probeCount || numSamples <= 0) return;

    // 计算探针世界空间位置
    uint gx = probeIndex % gridX;
    uint gy = (probeIndex / gridX) % gridY;
    uint gz = probeIndex / (gridX * gridY);
    float cellSize = u_GridSize.w;
    float3 probePos = u_GridOrigin.xyz + float3(gx, gy, gz) * cellSize;

    // 采样步长（距探针中心的距离，用于 GBuffer 采样）
    float stepDist = cellSize * 0.4;

    // 初始化 SH 累积
    float4 sh[9];
    for (int j = 0; j < 9; j++) sh[j] = float4(0, 0, 0, 0);
    float validSamples = 0.0;

    // 球面采样 → GBuffer → SH 投影
    for (int i = 0; i < numSamples; i++) {
        // Fibonacci 球面方向
        float3 dir = FibonacciSphere(i, numSamples);

        // 沿方向步进到采样点（世界空间）
        float3 samplePos = probePos + dir * stepDist;

        // 世界 → 屏幕投影
        float4 clipPos = mul(u_ViewProj, float4(samplePos, 1.0));
        float3 ndc = clipPos.xyz / max(clipPos.w, 0.0001);
        float2 uv = ndc.xy * 0.5 + 0.5;

        // 检查是否在屏幕范围内（Vulkan NDC: x,y ∈ [-1,1], z ∈ [0,1]）
        if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || ndc.z < 0.0 || ndc.z > 1.0) {
            continue;
        }

        // 深度可见性测试：比较采样点投影深度与 GBuffer 深度（SampleLevel 避免 compute derivative）
        float gbufDepth = u_Depth.SampleLevel(u_PointSampler, uv, 0).r;
        // Reverse-Z 约定：近=1, 远=0。ndc.z < gbufDepth 表示在几何体前方（被遮挡）
        // 允许小阈值避免浮点误差
        if (ndc.z > gbufDepth + 0.003) {
            continue;  // 采样点被遮挡
        }

        // 可见：读取前帧 HDR 作为真实辐射度（包含完整 PBR+IBL+阴影）
        // 灰度兜底：首帧 HDR 可能为全零，用 0.03 防止全黑
        float3 radiance = u_PrevHDR.SampleLevel(u_LinearSampler, uv, 0).rgb;
        if (dot(radiance, radiance) < 0.0001) {
            // 首帧或采样点屏幕外：回退到 albedo 近似
            float3 albedo = u_GBufferA.SampleLevel(u_PointSampler, uv, 0).rgb;
            radiance = albedo * 0.03;  // 极弱光兜底，首帧后会被 HDR 覆盖
        }

        // SH 投影
        float basis[9];
        SHBasis(dir, basis);
        for (int j = 0; j < 9; j++) {
            sh[j].rgb += radiance * basis[j];
        }
        validSamples += 1.0;
    }

    // 归一化：蒙特卡洛积分缩放因子 4π/N
    if (validSamples > 0.0) {
        float scale = (4.0 * 3.14159265359) / validSamples;
        for (int j = 0; j < 9; j++) {
            sh[j].rgb *= scale;
        }
    } else {
        // 完全无可见采样 → 弱环境光兜底
        sh[0] = float4(0.02, 0.03, 0.08, 0.0);
    }

    // 时间混合：与上一帧历史 SH 系数 lerp（减少闪烁，加速收敛）
    float blendAlpha  = u_Params.z;
    bool  historyValid = u_Params.w > 0.5;
    if (historyValid && blendAlpha > 0.0) {
        uint base = probeIndex * kStride;
        for (int j = 0; j < 9; j++) {
            float4 hist = u_ProbeHistory[base + j];
            sh[j].rgb = lerp(sh[j].rgb, hist.rgb, blendAlpha);
        }
    }

    WriteSH(probeIndex, sh);
}
