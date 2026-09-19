// DDGI.comp.slang — 动态漫反射全局光照探针更新
// 每线程处理一个探针：球面采样 GBuffer → SH 投影 → 写入探针缓冲
//
// 探针数据布局（RWStructuredBuffer<float4>，每探针 16 float4）：
//   [0..8]  SH 系数 band 0/1/2 (9×float4, RGB 存储)
//   [9..15] 保留

#include "ShaderTypes.slang"  // GPU 常量 (kGPU*) + 结构体
#include "Lumen/ScreenProbe.slang"   // 步骤 31：Screen Probe 的共享布局（与 Lumen 侧逐字段一致）

// GBuffer 输入（只读，CombinedImageSampler）
[[vk::binding(kGPUBinding_GBufferA, kGPUDescSet_PerFrame)]] Texture2D<float4> u_GBufferA;
[[vk::binding(kGPUBinding_GBufferB, kGPUDescSet_PerFrame)]] Texture2D<float4> u_GBufferB;
[[vk::binding(2, 0)]] Texture2D<float>  u_Depth;  // 深度（DDGI 计算描述符集 binding 2，与 GI_DDGI.cpp 布局一致；勿用 kGPUBinding_Depth=3，那是 Deferred GBuffer 的绑定号）
[[vk::binding(0, 0)]] SamplerState      u_PointSampler; // 点采样（GBuffer 读取）

// 探针输出（读写，当前帧结果）
[[vk::binding(3, 0)]] RWStructuredBuffer<float4> u_Probes;

// 上一帧探针历史（只读，时间混合源）
[[vk::binding(5, 0)]] StructuredBuffer<float4> u_ProbeHistory;

// 前帧 HDR 辐射度（CombinedImageSampler, 与 u_LinearSampler 共享 binding slot）
// 屏幕空间来源：视角相关（视锥外采样点被跳过）——仅作 RSM 不可用时的回退
[[vk::binding(6, 0)]] Texture2D<float4> u_PrevHDR;
[[vk::binding(6, 0)]] SamplerState u_LinearSampler;

// RSM 世界辐射度（B 路径：单次反弹 VPL，视角无关——优先来源）
[[vk::binding(7, 0)]] Texture2D<float4> u_RSMPositionMap;
[[vk::binding(8, 0)]] Texture2D<float4> u_RSMRadianceMap;   // VPL 出射辐射度（见 ShaderTypes 的 RSM 通道约定）

// IBL 环境辐照度（Cubemap，按方向采样——世界空间、视角无关；RSM 不可用时的回退）
[[vk::binding(9, 0)]] TextureCube<float4> u_IBLIrradiance;

// 光追 march 的探针射线辐射度（任务 17 / B4，由 DDGI_Trace pass 每帧写入）
// 布局：每探针 numSamples 条，[probeIndex * numSamples + i]；不可用时 u_Flags.w=0
[[vk::binding(10, 0)]] StructuredBuffer<float4> u_TracedRadiance;
// ── 步骤 31（L5）：输入改为 Screen Probe ──
// b11 = Lumen 的探针（世界位置/法线 + 辐射度 SH），b12 = 16×16 单元 → 探针下标，b13 = 收敛统计
[[vk::binding(11, 0)]] StructuredBuffer<ScreenProbe> u_ScreenProbes;
[[vk::binding(12, 0)]] StructuredBuffer<uint>         u_ScreenCellProbe;
[[vk::binding(13, 0)]] RWStructuredBuffer<uint>       u_Stats;   // 0=采纳 1=更新 2=继承历史 3=Σ相对变化(定点)

// 探针网格 + 相机参数
[[vk::binding(4, 0)]] cbuffer DDGIParams {
    float4   u_GridOrigin;   // xyz=世界空间原点
    float4   u_GridSize;     // xyz=探针数量, w=cellSize
    float4   u_CameraPos;    // xyz=相机世界位置
    float4   u_Params;       // x=intensity, y=numSamples, z=blendAlpha, w=historyValid
    float4x4 u_ViewProj;     // 相机 View→Proj（世界→裁剪空间）
    float4x4 u_RSMLightViewProj;  // RSM 光源 VP（世界→RSM 光源空间投影）
    float4   u_Flags;        // x=useRSM；y=updateStride；z=updatePhase；w=光追 march 可用
    float4   u_ScreenProbeDims;   // 步骤 31：x=cellsX, y=cellsY, z=屏宽, w=屏高
    float4   u_ScreenProbeFlags;  // 步骤 31：x=启用, y=距离门限, z=单侧→整球外推系数, w=未用
};

// 每探针 float4 数量（与 C++ kFloats4PerProbe 保持一致）
static const uint kStride = 4;   // 步骤 30：每探针 4 个 SH 系数（16 → 4）

// ---- SH 基函数常量 ----
static const float kSH_Y00 = 0.28209479177387814;  // l=0: sqrt(1/(4π))

// Band 1 (l=1): 3 个系数
float3 SH_Band1(float3 dir) {
    return dir * 0.4886025119029199;  // sqrt(3/(4π))
}

// Band 2 (l=2) 在步骤 30 被移除：二阶 4 系数即可（§7 Radiance Cache 的表示）。
// 保留说明是为了让"为什么少了一档"这件事在代码里可见，而不是看起来像漏写。

// 将方向 dir 投影为 **4** 个 SH 系数（bands 0/1）
void SHBasis(float3 dir, out float sh[4]) {
    sh[0] = kSH_Y00;                                    // l=0,m=0
    float3 b1 = SH_Band1(dir);
    sh[1] = b1.y; sh[2] = b1.z; sh[3] = b1.x;          // l=1: m=-1,0,1
}

// Fibonacci 球面：均匀分布在单位球面上的第 i 个方向（共 n 个）
float3 FibonacciSphere(int i, int n) {
    float phi   = acos(1.0 - 2.0 * (float(i) + 0.5) / float(n));
    float theta = 3.14159265359 * (1.0 + sqrt(5.0)) * float(i);
    return float3(sin(phi) * cos(theta), cos(phi), sin(phi) * sin(theta));
}

// 写入 SH 系数到探针缓冲（4 个系数，各占一个 float4 的 .rgb；其余槽位清零）
void WriteSH(uint probeIndex, float4 sh[4]) {
    const uint base = probeIndex * kStride;
    for (uint i = 0u; i < kStride; i++) {
        u_Probes[base + i] = (i < 4u) ? sh[i] : float4(0, 0, 0, 0);
    }
}

// ── 步骤 31：把 DDGI 探针的位置投到屏幕，取那个 16×16 单元归属的 Screen Probe ──
// 返回值：可用的屏幕探针下标；0xFFFFFFFF = 不可用（未启用 / 投到屏外 / 距离门限不过）。
// 【为什么要距离门限】屏幕探针是"某像素的表面"上的，把 DDGI 探针投过去可能落在完全不同的物体上
//（相机一转，单元归属就换）。不加门限会把远处表面的辐射度灌进这个探针 —— 那正是"拖影"的来源。
// 返回该探针可用的 Screen Probe 下标；0xFFFFFFFF = 不可用。
uint FetchScreenProbe(float3 probePos) {
    if (u_ScreenProbeFlags.x < 0.5) return 0xFFFFFFFFu;
    if (u_ScreenProbeDims.x < 1.0 || u_ScreenProbeDims.y < 1.0) return 0xFFFFFFFFu;
    InterlockedAdd(u_Stats[4], 1u);   // 4 = 进入了抓取（诊断）

    const float4 clip = mul(u_ViewProj, float4(probePos, 1.0));
    if (clip.w <= 1e-4) return 0xFFFFFFFFu;
    const float2 ndc = clip.xy / clip.w;
    const float2 uv  = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);   // Vulkan NDC：y 向下翻转
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) return 0xFFFFFFFFu;
    InterlockedAdd(u_Stats[5], 1u);   // 5 = 投到屏幕内

    const uint cx = min(uint(uv.x * u_ScreenProbeDims.z / 16.0), uint(u_ScreenProbeDims.x) - 1u);
    const uint cy = min(uint(uv.y * u_ScreenProbeDims.w / 16.0), uint(u_ScreenProbeDims.y) - 1u);
    const uint cell = cy * uint(u_ScreenProbeDims.x) + cx;
    const uint idx  = u_ScreenCellProbe[cell];
    if (idx == 0xFFFFFFFFu) return 0xFFFFFFFFu;
    InterlockedAdd(u_Stats[6], 1u);   // 6 = 该单元有探针

    const ScreenProbe sp = u_ScreenProbes[idx];
    if (sp.position.w < 0.5) return 0xFFFFFFFFu;
    // 【深度一致性门限（这才是"重投影"该有的判据）】屏幕上看到的表面与 DDGI 探针必须在**同一条
    // 相机射线上的相近深度**上。第一版用的是"屏幕探针与探针的世界距离 < 1 个格距"—— 对粗网格
    //（本场景格距 248 单位）几乎全部被拒：网格点常常离任何可见表面都有几百单位。
    // 改成比较"到相机的距离"：两者同深度 ⇒ 大致是同一表面；差得远 ⇒ 那是另一个物体，不采纳。
    {
        const float dProbe = distance(u_CameraPos.xyz, probePos);
        const float dSample = distance(u_CameraPos.xyz, sp.position.xyz);
        const float tol = u_ScreenProbeFlags.y + 0.2 * dProbe;   // 绝对门限 + 20% 相对松弛
        if (abs(dProbe - dSample) > tol) return 0xFFFFFFFFu;
    }
    InterlockedAdd(u_Stats[7], 1u);   // 7 = 通过深度门限
    return idx;
}

// 用 Screen Probe 的 4 系数**辐射度** SH 在方向 dir 上求值（L(d) = Σ l_i Y_i(d)），
// 再乘"单侧→整球"的对称外推系数。负值截断：SH 带限表示会有振铃，负辐射度没有物理意义。
float3 EvalScreenProbeRadiance(uint idx, float3 dir) {
    const ScreenProbe sp = u_ScreenProbes[idx];
    float3 l0 = float3(sp.shR.x, sp.shG.x, sp.shB.x);
    float3 l1 = float3(sp.shR.y, sp.shG.y, sp.shB.y);
    float3 l2 = float3(sp.shR.z, sp.shG.z, sp.shB.z);
    float3 l3 = float3(sp.shR.w, sp.shG.w, sp.shB.w);
    const float A0 = 0.2820947918, A1 = 0.4886025119;
    float3 r = l0 * A0 + (l1 * dir.y + l2 * dir.z + l3 * dir.x) * A1;
    return max(r, 0.0) * u_ScreenProbeFlags.z;
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

    // ── 时间维分摊（任务 12 · AMORTIZE）──
    // stride > 1 时每帧只更新一批探针（probeIndex % stride == phase），其余探针**原样继承
    // 上一次的结果**。这里必须显式把历史拷到当前缓冲：两个探针缓冲是逐帧 ping-pong 的，
    // 本帧的"当前"缓冲里装的是**两帧前**的值，而"历史"缓冲才是最近一次的结果（见
    // GI_DDGI::Render 末尾的 swap）。不拷贝就会让未轮到的探针每帧倒退一代。
    // 探针本来就是"每帧新估计与历史做 lerp"，所以降频只是把同一个时间常数拉长为 N 倍帧数。
    uint updateStride = uint(u_Flags.y + 0.5);
    uint updatePhase  = uint(u_Flags.z + 0.5);
    if (updateStride > 1u && (probeIndex % updateStride) != updatePhase) {
        uint carryBase = probeIndex * kStride;
        for (uint i = 0u; i < kStride; i++) {
            u_Probes[carryBase + i] = u_ProbeHistory[carryBase + i];
        }
        return;
    }

    // 计算探针世界空间位置
    uint gx = probeIndex % gridX;
    uint gy = (probeIndex / gridX) % gridY;
    uint gz = probeIndex / (gridX * gridY);
    float cellSize = u_GridSize.w;
    float3 probePos = u_GridOrigin.xyz + float3(gx, gy, gz) * cellSize;

    // 采样步长（距探针中心的距离，用于 GBuffer 采样）
    float stepDist = cellSize * 0.4;

    // 初始化 SH 累积
    float4 sh[4];
    for (int j = 0; j < 4; j++) sh[j] = float4(0, 0, 0, 0);
    float validSamples = 0.0;

    // ── 步骤 31：先确定本探针归属哪个 Screen Probe（与采样方向无关，故循环外取一次）──
    // 0xFFFFFFFF = 没有可用屏幕探针（未启用 / 投到屏外 / 距离门限不过）。
    uint spIndex = 0xFFFFFFFFu;
    if (u_ScreenProbeFlags.x >= 0.5) spIndex = FetchScreenProbe(probePos);
    uint screenAccepted = 0u;

    // 球面采样 → GBuffer → SH 投影
    for (int i = 0; i < numSamples; i++) {
        // Fibonacci 球面方向
        float3 dir = FibonacciSphere(i, numSamples);

        // 沿方向步进到采样点（世界空间）
        float3 samplePos = probePos + dir * stepDist;

        float3 radiance = float3(0.0);
        bool   valid    = false;

        if (spIndex != 0xFFFFFFFFu) {
            // ── 步骤 31 的主路径：输入来自 **Screen Probe** ──
            // 用它的 4 系数辐射度 SH 沿本探针的采样方向求值，再按"单侧→整球"的对称近似补齐立体角。
            // 【投影流程一行没动】只是把"逐样本辐射度从哪来"换掉了 —— 这正是把两边收敛到同一表示
            //（步骤 30 的 2 阶 4 系数）之后的收益。
            radiance = EvalScreenProbeRadiance(spIndex, dir);
            valid    = dot(radiance, radiance) > 0.0001;
            if (valid) screenAccepted += 1u;
        } else if (u_Flags.w > 0.5) {
            // ── A 路径（任务 17 / B4）：光追 march 给出的真实辐射度 ──
            // 每条探针射线由 DDGI_Trace pass 用硬件光追求解：命中 → 命中点出射辐射度，
            // 未命中 → 该方向的 IBL 辐照度。缓冲布局 = [probeIndex * numSamples + i]。
            // 【为什么保留】Screen Probe 没覆盖到的探针（屏外/距离门限不过）用它兜底；
            // 它同样满足"世界空间、视角无关"。
            float4 traced = u_TracedRadiance[probeIndex * uint(numSamples) + uint(i)];
            radiance = traced.rgb;
            valid    = dot(radiance, radiance) > 0.0001;
        } else if (u_Flags.x > 0.5) {
            // ── B 路径：RSM 世界辐射度（视角无关，单次反弹 VPL）──
            float4 rsmClip = mul(u_RSMLightViewProj, float4(samplePos, 1.0));
            if (rsmClip.w > 0.0001) {
                float2 rsmUV = rsmClip.xy / rsmClip.w * 0.5 + 0.5;
                if (rsmUV.x >= 0.0 && rsmUV.x <= 1.0 && rsmUV.y >= 0.0 && rsmUV.y <= 1.0) {
                    // RSM 贴图：一个附件一个量（任务 30）。这里要的是该 VPL 的**出射辐射度**，
                    // 与 RSM_Indirect 读的是同一个通道 —— 此前这张图把编码法线放在 rgb、
                    // 把标量通量放在 a，探针端曾把编码法线当辐射度用（§9.2-AA ②）。
                    float4 rad = u_RSMRadianceMap.SampleLevel(u_LinearSampler, rsmUV, 0);
                    radiance = rad.rgb;
                    valid = dot(radiance, radiance) > 0.0001;
                }
            }
            // 【逐样本回退】RSM 采不到（未渲染／不在光源视锥内／该处无 VPL）时退回 IBL 辐照度。
            // 否则一旦 useRSM 被置位而 RSM 实际不可用，**整片探针**都会采不到数据，
            // 全部落入下方的硬编码兜底常数 —— DDGI 会在"看起来正常"的情况下静默失去全部 GI。
            if (!valid) {
                radiance = u_IBLIrradiance.SampleLevel(u_LinearSampler, dir, 0).rgb;
                valid = dot(radiance, radiance) > 0.0001;
            }
        } else {
            // ── 回退：IBL 环境辐照度（世界空间、视角无关）──
            // 原屏幕 HDR 回退依赖视锥（视锥外采样点被跳过）→ 探针随相机位置/视角变化，
            // 改用天空盒辐照度（按采样方向查询），彻底移除屏幕空间依赖
            radiance = u_IBLIrradiance.SampleLevel(u_LinearSampler, dir, 0).rgb;
            valid = dot(radiance, radiance) > 0.0001;
        }

        if (!valid) {
            continue;
        }

        // SH 投影
        float basis[4];
        SHBasis(dir, basis);
        for (int j = 0; j < 4; j++) {
            sh[j].rgb += radiance * basis[j];
        }
        validSamples += 1.0;
    }

    // 归一化：蒙特卡洛积分缩放因子 4π/N × 强度（u_Params.x）
    if (validSamples > 0.0) {
        float scale = (4.0 * 3.14159265359) / validSamples * u_Params.x;
        for (int j = 0; j < 4; j++) {
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
        for (int j = 0; j < 4; j++) {
            float4 hist = u_ProbeHistory[base + j];
            sh[j].rgb = lerp(sh[j].rgb, hist.rgb, blendAlpha);
        }
    }

    // ── 步骤 31：统计 + "世界空间重投影" ──
    // 【重投影在**世界空间**缓存里的含义】DDGI 的探针是钉在世界空间的，相机移动时它不需要
    // 屏幕空间那种重投影；真正需要防的是：Screen Probe 是屏幕空间的，相机一转，某个探针可能
    // 投到完全不同的表面上。距离门限（FetchScreenProbe 里）+ 下面的"没有可用屏幕探针就**整条
    // 继承历史**"合起来就是这件事的答案 —— 与其吸收一个不相干的表面，不如保持自己的旧值。
    {
        const uint base = probeIndex * kStride;
        if (spIndex != 0xFFFFFFFFu) {
            InterlockedAdd(u_Stats[0], 1u);
            if (screenAccepted == 0u && historyValid) {
                // 采纳了屏幕探针，但它的估计在我们所有采样方向上都是 0（例如法线朝墙内）⇒
                // 与其写入一个"接近零"的值，不如继承历史，避免探针被清空。
                for (int j = 0; j < 4; j++) u_Probes[base + j] = u_ProbeHistory[base + j];
                InterlockedAdd(u_Stats[2], 1u);
                return;
            }
        }
        InterlockedAdd(u_Stats[1], 1u);
        // 相对变化（定点 ×65536）：Σ|new − hist| / Σ|new|，供 CPU 侧判"是否收敛"
        if (historyValid) {
            float sumNew = 0.0, sumDiff = 0.0;
            for (int j = 0; j < 4; j++) {
                const float3 n = sh[j].rgb;
                const float3 h = u_ProbeHistory[base + j].rgb;
                sumNew  += dot(abs(n), float3(1, 1, 1));
                sumDiff += dot(abs(n - h), float3(1, 1, 1));
            }
            if (sumNew > 1e-6) {
                InterlockedAdd(u_Stats[3], (uint)(clamp(sumDiff / sumNew, 0.0, 1.0) * 65536.0 + 0.5));
            }
        }
    }

    WriteSH(probeIndex, sh);
}
