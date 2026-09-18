#pragma once

#include "GI/GlobalIllumination.h"
#include "RHI/RHI.h"
#include "RHI/Buffer.h"
#include "Math/Math.h"

namespace he::render {

// RSM（Reflective Shadow Map）默认分辨率
constexpr u32 kDefaultRSMResolution = 512;

// ============================================================================
// GI_RSM — Reflective Shadow Maps 全局光照
//
// 在方向光 Shadow Pass 后用相同视角渲染 RSM 数据（位置/法线/通量），
// PBR Shader 中在 light space 采样 RSM 获取单次反弹间接漫反射。
//
// 纹理（任务 30 起：一个附件一个量，约定见 ShaderTypes.slang 的「RSM 贴图通道约定」）：
//   - RSM_Position: RGBA16_FLOAT（worldPos.xyz）
//   - RSM_Normal:   RGBA16_FLOAT（worldNormal 编码到 [0,1]）
//   - RSM_Radiance: RGBA16_FLOAT（该 VPL 的出射辐射度 L_v = albedo·lightColor·intensity·NdotL/π）
// ============================================================================
class GI_RSM : public IGlobalIllumination {
public:
    GI_RSM()  = default;
    ~GI_RSM() override = default;

    // IRenderSubsystem
    bool Initialize(rhi::IRHIDevice* device, u32 width, u32 height) override;
    void Shutdown() override;
    void Update(const SubsystemContext& ctx) override;
    void Render(rhi::IRHICommandList* cmdList) override;
    void Bind(rhi::IRHICommandList* cmdList) const override;
    void OnResize(u32 width, u32 height) override;

    const char* GetName()  const override { return "GI_RSM"; }
    bool        IsReady()  const override { return m_Ready; }
    bool        IsEnabled() const override { return m_Enabled; }
    void SetEnabled(bool e) override { m_Enabled = e; }

    // IGlobalIllumination
    GIMode GetMode() const override { return GIMode::RSM; }

    // ---- 设置 RSM 参数 ----
    /// 【不再接收管线侧的物体缓冲】本 pass 自己持有 GPUObjectData[]：
    /// 管线那份是**相机可见性列表**（SceneRenderer 只写可见物体，索引是相机列表下标），
    /// 而本 pass 遍历全部网格、索引是自己的计数器 —— 借用它有两个后果：
    ///   ① 读到的材质字段（albedo）大多来自没填过的槽（实测只有 3% 的 texel 有非零辐射度）；
    ///   ② 本 pass 往里写 worldMatrix 会让"谁的数据在缓冲里"变得不可推理。
    /// 现在本 pass 只写自己的缓冲（只写 worldMatrix 这一个字段，其余字段不需要）。
    void SetLightViewProj(const float4x4& vp, u32 resolution,
                          rhi::IRHISampler* shadowSampler,
                          rhi::DescriptorSetHandle descSet);

    // 设置 Shadow Map 深度附件（渲染 RSM 时复用）
    // DEPRECATED: RSM 现在使用独立深度缓冲，不再复用 CSM ShadowMap
    void SetShadowDepthView(void* depthView) { m_ExternalDepthView = depthView; }

    /// 设置本帧的 GPULight SSBO（RSM_Generate.frag 的 u_Lights@binding 1）。
    /// 【为什么必须单独给】`RenderRSMPass` 此前把**对象缓冲**同时绑到了 binding 1 与 2，
    /// 于是着色器里的 `u_Lights[0]` 实际读到的是 `GPUObjectData[0]`（世界矩阵被当成光源颜色
    /// 与强度解释）⇒ 通量是人造值/garbage，RSM 间接光因此常年恒为 0（实测 S_rsm = 0 到 1e-7，
    /// 而 Lighting 里那段 VPL 求和的成本照样在付）。见文档 §9.2-AA。
    void SetLightBuffer(rhi::IRHIBuffer* lightBuffer) { m_ExternalLightBuf = lightBuffer; }

    // 从光源 POV 渲染几何体到 RSM 纹理（使用独立深度缓冲）
    void RenderRSMPass(rhi::IRHICommandList* cmd, he::World& world, he::SceneGraph& sg);

    // 纹理访问
    rhi::IRHITexture* GetRSMPositionMap() const { return m_RSMPos.get(); }
    /// 法线图（历史名 `GetRSMFluxMap`：任务 30 之前这张图的 .a 存通量，现在只存编码法线）
    rhi::IRHITexture* GetRSMFluxMap()     const { return m_RSMFlux.get(); }
    /// VPL 出射辐射度图（任务 30 新增；消费端不再需要"解包"法线与通量）
    rhi::IRHITexture* GetRSMRadianceMap() const { return m_RSMRadiance.get(); }
    rhi::IRHISampler* GetRSMSampler()     const { return m_RSMSampler.get(); }

private:
    std::unique_ptr<rhi::IRHITexture> m_RSMPos;        // RGBA16_FLOAT worldPos
    std::unique_ptr<rhi::IRHITexture> m_RSMFlux;       // RGBA16_FLOAT 编码法线
    std::unique_ptr<rhi::IRHITexture> m_RSMRadiance;   // RGBA16_FLOAT VPL 出射辐射度
    std::unique_ptr<rhi::IRHITexture> m_RSMDepth;  // D32_FLOAT 独立深度缓冲（不依赖 CSM ShadowMap）
    /// 本 pass 自己的 GPUObjectData[]（每帧重写 worldMatrix；见 SetLightViewProj 的说明）
    std::unique_ptr<rhi::IRHIBuffer>  m_ObjectBuf;
    std::unique_ptr<rhi::IRHISampler> m_RSMSampler;

    std::unique_ptr<rhi::IRHIPipelineState> m_RSMPSO;
    rhi::DescriptorSetLayoutHandle m_RSMLayout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_RSMSet    = rhi::kInvalidSet;
    void* m_ExternalDepthView = nullptr;  // 外部深度视图（保留兼容但不再用于 RSM）

    float4x4 m_LightVP;
    u32      m_RSMResolution = kDefaultRSMResolution;
    rhi::IRHIBuffer*        m_ExternalLightBuf = nullptr;   // GPULight SSBO（见 SetLightBuffer）
    rhi::DescriptorSetHandle m_ExternalDescSet = rhi::kInvalidSet;

    bool m_Ready = false;
};

} // namespace he::render
