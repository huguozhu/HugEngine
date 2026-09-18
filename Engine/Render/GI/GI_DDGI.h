#pragma once

#include "GI/GIRadianceHistory.h"   // 前帧 HDR 辐射度（共享组件）
#include "GI/GlobalIllumination.h"
#include "RHI/RHI.h"
#include "Math/Math.h"
#include <memory>

namespace he::render {

// ============================================================
// GI_DDGI — 动态漫反射全局光照（探针网格 + SH 辐照度）
//
// 3D 探针网格 → Compute Shader 每帧采样 GBuffer 更新 SH
// → Lighting shader 三线性插值采样间接漫反射。
//
// 探针数据布局（StructuredBuffer，每探针 16 float4）：
//   [0..8]  SH 系数 (band 0/1/2, 9×float4)
//   [9..15] 保留（深度/可见性/偏移等，暂未使用）
// ============================================================
class GI_DDGI : public IGlobalIllumination {
public:
    GI_DDGI() = default;

    bool Initialize(rhi::IRHIDevice* device, u32 width, u32 height) override;
    void Shutdown() override;
    void Update(const SubsystemContext& ctx) override;
    void Render(rhi::IRHICommandList* cmd) override;
    void Bind(rhi::IRHICommandList*) const override {}
    void OnResize(u32 w, u32 h) override;
    const char* GetName() const override { return "GI_DDGI"; }
    bool IsReady() const override { return m_Ready; }
    bool IsEnabled() const override { return m_Settings.enabled; }
    void SetEnabled(bool e) override { m_Settings.enabled = e; }

    GIMode GetMode() const override { return GIMode::DDGI; }
    rhi::IRHITexture* GetIndirectDiffuseTexture() const override { return nullptr; }

    void SetGBufferInputs(rhi::IRHITexture* depth, rhi::IRHITexture* normal, rhi::IRHITexture* albedo);

    // 设置 RSM 世界辐射度输入（B 路径：探针从 RSM 采样单次反弹辐射度，视角无关）
    // 任务 30 起第二张图是 **VPL 出射辐射度**（此前是"编码法线.rgb + 通量.a"的打包图，
    // 探针端必须知道解包规则才拿得到通量，§9.2-AA ②）。
    // pos/radiance 为空时回退 IBL 辐照度
    void SetRSM(rhi::IRHITexture* pos, rhi::IRHITexture* radiance, const float4x4& lightViewProj);

    // 清除 RSM 输入，回退到 IBL 辐照度（useRSM 归零）。
    // 【为什么需要显式清除】SetRSM 只有一个"置位"方向，而 useRSM 是由
    // m_RSMPositionMap/m_RSMRadianceMap 是否为空**推导**出来的：帧图若只是"本帧不再调用 SetRSM"，
    // 成员仍非空 ⇒ useRSM 恒为 1，探针会一直消费过期甚至已被重建的 RSM 纹理。
    // 因此帧图每帧都要给出明确结论：注册了就 SetRSM，没注册就 ClearRSM（§9.2-F/R）。
    void ClearRSM();

    // 把共享组件的纹理绑到本源的 binding 6；仅当组件代次变化（resize 重建）时才实际重绑
    void BindRadianceHistory();

    // 设置 IBL 辐照度（Cubemap）：RSM 不可用时的回退来源（世界空间、视角无关）
    void SetIBL(rhi::IRHITexture* irradiance, rhi::IRHISampler* sampler);

    /// 注入共享的前帧 HDR 辐射度组件（非拥有）。须在 Initialize 之前调用。
    void SetRadianceHistory(GIRadianceHistory* radiance) { m_Radiance = radiance; }
    // 探针数据缓冲（供 Lighting Pass 绑定，每帧更新后为最新 blend 结果）
    rhi::IRHIBuffer* GetProbeBuffer() const { return m_ProbeBuffer.get(); }

    /// 每个探针的球面采样数（DDGI.comp 的 SH 投影次数）。
    /// 必须与 DDGI_Trace pass 的采样数一致：光追 march 的射线结果按
    /// `[probeIndex * kSamplesPerProbe + i]` 索引，二者不一致就会读错。
    static constexpr u32 kSamplesPerProbe = 32;

    /// 注入本帧光追 march 的探针射线辐射度（任务 17 / B4）。
    /// 传 nullptr 或缺省采样数即回退到 RSM/IBL 路径（不支持光追的设备）。
    void SetTracedRadiance(rhi::IRHIBuffer* radiance, u32 samplesPerProbe) {
        m_TracedRadiance = radiance;
        m_TracedSamples  = samplesPerProbe;
    }

    // 探针网格参数 UBO（供 Lighting Pass / RT GI 采样 DDGI 时读取网格参数，替代 shader 硬编码常量）
    rhi::IRHIBuffer* GetGridUniform() const { return m_GridUniform.get(); }

    // 探针网格参数
    u32 gridX = 8, gridY = 4, gridZ = 8;
    float3 gridOrigin = float3(-10, -2, -10);
    float  cellSize     = 3.0f;
    float  blendAlpha   = 0.85f;   // 时间混合：历史保留比例（0=无历史, 1=完全历史）
    float  debugScale   = 0.5f;    // 调试：DDGI 贡献缩放
    /// 时间维分摊：每 N 帧更新一轮探针（任务 12 · AMORTIZE）。
    /// 1 = 每帧全量更新（既有行为，默认）；N &gt; 1 时每帧只更新 `probeIndex % N == phase`
    /// 的那一批（phase 逐帧轮转），未轮到的探针**原样继承上一次的结果**。
    /// 与 `blendAlpha` 天然配合：探针本来就是"每帧新估计与历史做 lerp"，
    /// 把更新频率降到 1/N 只是让同一个时间常数以 N 倍帧数走完。
    /// 代价：每帧的样本量降为 1/N；代价的另一面是收敛所需帧数变为 N 倍。
    u32 updateStride = 1;
    /// 当前轮转相位（每帧自增，渲染时对 updateStride 取模）
    u32 updatePhase  = 0;

    /// 是否按场景包围盒自动拟合探针网格（任务 14 / §9.2-K）。
    ///
    /// 【为什么需要】网格是固定参数时，覆盖不到的区域会被 `SampleDDGI` 的 clamp 变成
    /// **贴边常数外推**：开着 DDGI、却在大半屏幕上拿到同一个常数，且没有任何标记。
    /// 实测（本仓库 GILab/Sponza）：默认网格 8×4×8 格距 3 ⇒ 只覆盖 21×9×21 世界单位，
    /// 而场景包围盒是 3720.9×1555.9×2288.2 —— 加上覆盖语义（`kGIConfProbeGrid`）之后
    /// DDGI 的贡献直接归零，即此前测到的"DDGI 贡献"**全部**来自那次 clamp。
    /// 自动拟合让网格真正罩住场景；覆盖语义负责把仍在外面的部分标记为无效。
    bool autoFitGrid = true;
    /// 自动拟合时**最长边**上的探针数（其余两轴按同一格距推出各自需要的探针数）。
    /// 格距 `cellSize = maxExtent / (fitCellsMax - 1)`，三轴共用同一个格距，
    /// 因此不会出现各向异性拉伸；每轴探针数取"刚好罩住该轴"，
    /// 不按最长边统一取值（否则短轴会白铺一半以上的探针）。
    u32 fitCellsMax = 16;

    /// 用场景包围盒拟合网格：格距由**最长边**推出，每轴独立取刚好罩住该轴的探针数，
    /// 网格以包围盒最小角为原点。传入退化包围盒（min > max）时不改动任何参数。
    void FitGridToBounds(const float3& mn, const float3& mx);

private:
    // 每探针存储的 float4 数量（9 SH + 7 保留）
    static constexpr u32 kFloats4PerProbe = 16;

    // 探针网格参数 uniform 结构（与 shader 中 ProbeGridParams 保持一致）
    // 需满足 std140 对齐：float4=16B, float4x4=64B
    struct ProbeGridUniform {
        float4   gridOrigin;    // xyz=世界空间原点, w=未使用
        float4   gridSize;      // xyz=探针数量(gridX,gridY,gridZ), w=cellSize
        float4   cameraPos;     // xyz=相机世界位置, w=未使用
        float4   params;        // x=intensity, y=numSamples, z=blendAlpha, w=historyValid
        float4x4 viewProj;      // 相机 View→Proj（世界→裁剪，用于探针→屏幕投影）
        float4x4 rsmLightViewProj;  // RSM 光源 VP（世界→RSM 光源空间投影）
        // x=useRSM（1=RSM 世界辐射度，0=屏幕 HDR 回退）
        // y=updateStride（时间维分摊：每 N 帧更新一轮探针，1=每帧全量）
        // z=updatePhase（本帧轮到的相位，shader 判 probeIndex % stride == phase）
        float4   flags;
    };

    rhi::IRHIDevice* m_Device = nullptr;
    u32 m_Width = 0, m_Height = 0;
    bool m_Ready = false;

    // Compute Pipeline
    std::unique_ptr<rhi::IRHIPipelineState> m_PSO;
    rhi::DescriptorSetLayoutHandle m_Layout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_Set    = rhi::kInvalidSet;

    // 探针数据（SSBO：每探针 16×float4，当前帧 blend 结果）
    std::unique_ptr<rhi::IRHIBuffer> m_ProbeBuffer;
    // 上一帧探针历史（SSBO，时间混合源）
    std::unique_ptr<rhi::IRHIBuffer> m_ProbeHistory;
    /// 当前探针缓冲的探针数（网格拟合改变探针数时据此重建）
    u32 m_ProbeBufferCount = 0;
    /// 历史探针缓冲中是否有**已写入**的数据（着色器 `historyValid`）。
    /// 初次运行与「探针数变化导致重建」之后都必须为 false：新缓冲是未初始化显存，
    /// 若当作有效历史参与 `blendAlpha` 混合，等于把垃圾按 0.85 的权重逐帧喂进 GI
    /// （静默偏色，且读数随显存布局变化 —— 与 §9.2-T 同类）。
    bool m_HistoryValid = false;

    // 光追 march 的探针射线辐射度（任务 17）：非拥有，由帧图每帧注入
    rhi::IRHIBuffer* m_TracedRadiance = nullptr;
    u32 m_TracedSamples = 0;

    // 探针网格参数 Uniform Buffer
    std::unique_ptr<rhi::IRHIBuffer> m_GridUniform;

    // 采样器
    std::unique_ptr<rhi::IRHISampler> m_PointSampler;    // 点采样（GBuffer 读取）
    std::unique_ptr<rhi::IRHISampler> m_LinearSampler;   // 线性采样（RSM 等读取）

    // 前帧 HDR 辐射度：改为消费共享组件（GIRadianceHistory），不再自有一份纹理与下采样
    // pass——多个 GI 源各拷一份等于每帧多付几次全屏下采样，正是 §3.5 反对的
    // 「白付一份全量成本」。非拥有，由管线注入（见 SetRadianceHistory）。
    GIRadianceHistory* m_Radiance = nullptr;
    u32 m_RadianceGeneration = 0;   // 已绑定的纹理代次；与组件比对以决定是否重绑描述符

    // GBuffer 输入（不持有所有权）
    rhi::IRHITexture* m_Depth  = nullptr;
    rhi::IRHITexture* m_Normal = nullptr;
    rhi::IRHITexture* m_Albedo = nullptr;

    // RSM 世界辐射度输入（B 路径，不持有所有权）
    rhi::IRHITexture* m_RSMPositionMap = nullptr;
    rhi::IRHITexture* m_RSMRadianceMap = nullptr;   // VPL 出射辐射度（任务 30 前是"法线+通量"打包图）
    float4x4 m_RSMLightViewProj = float4x4(1.0f);

    // IBL 辐照度（回退来源，不持有所有权）
    rhi::IRHITexture* m_IBLIrradiance = nullptr;

    // 相机
    bool m_CameraReady = false;
    float3 m_CameraPos;
    float4x4 m_ViewProj = float4x4(1.0f);  // 每帧从 CameraData 更新
};

} // namespace he::render
