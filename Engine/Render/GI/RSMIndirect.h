#pragma once

#include "RHI/RHI.h"
#include "RHI/Buffer.h"
#include "Math/Math.h"

#include <memory>

namespace he::render {

// ============================================================================
// RSMIndirect — RSM 间接光（16 点 Poisson 盘 VPL 求和）的半分辨率求值 pass
// （任务 16 / B3）
//
// 【为什么要有这个 pass】这段求和在搬出来之前是 DeferredLighting.frag.slang 里的逐像素
// 函数：16 个 VPL × 2 张 RSM 贴图 = 32 次采样。实测 Lighting 在漫反射层栈含 RSM 时
// 0.882 ms、不含时 0.433 ms —— 这一项独占约 0.45 ms，比 SSGI 还大。搬到半分辨率后
// 成本降到 1/4 量级，Lighting 侧只剩一次纹理采样。
//
// 【它不是 GI 源】RSM 的 GI 源身份由 GI_RSM（光源视锥光栅化）承担；本 pass 只是
// Lighting 采样 GISOURCE_RSM 的**求值前置**，因此由管线持有（与 SSAO 同类），
// 不注册进 IGIProvider 表：它没有自己的通道输出，也不参与归一化合成。
//
// 【门控】只在「本帧 RSM pass 真的注册了」时渲染（帧图传 rsmPassRegistered）：
// 没有产出就必须让 Lighting 回绑黑色占位，而不是采到未初始化显存（§9.2-T）。
// ============================================================================
class RSMIndirect {
public:
    RSMIndirect() = default;
    ~RSMIndirect() = default;

    bool Initialize(rhi::IRHIDevice* device, u32 width, u32 height);
    void Shutdown();
    void OnResize(u32 width, u32 height);

    /// 本帧输入：GBuffer 深度 / 世界坐标 / 法线（与 Lighting 同源，保证口径一致）
    void SetInputs(rhi::IRHITexture* depth, rhi::IRHITexture* worldPos, rhi::IRHITexture* normal);

    /// 本帧 RSM 光源参数。lightViewProj 必须与 RSM pass 用**同一个**矩阵，
    /// 否则 VPL 查表位置与写入位置不一致（帧图把同一个值同时给两边）。
    void SetRSM(rhi::IRHITexture* positionMap, rhi::IRHITexture* fluxMap,
                const float4x4& lightViewProj, float shadowType, float shadowStrength, u32 lightCount);

    /// **必须在 BeginOffscreenPass 之前调用**：RHI 用「当前已绑定的 PSO」推导 RenderPass
    /// 来建 Framebuffer，先开 pass 再绑管线会建出附件数不匹配的 Framebuffer
    /// （`VUID-VkFramebufferCreateInfo-attachmentCount-00876`，设备会直接挂住——§9.2-V 的
    /// 同一个坑，SSGI/SSR 的 Provider 也是靠 PreBind 规避）。
    void PreBind(rhi::IRHICommandList* cmd);
    /// 绘制半分辨率的 RSM 间接光辐照度（调用方负责 Begin/EndOffscreenPass）
    void Render(rhi::IRHICommandList* cmd);

    rhi::IRHITexture* GetOutput()  const { return m_Output.get(); }
    rhi::IRHISampler* GetSampler() const { return m_LinearSampler.get(); }   // 升采样用线性 clamp
    u32 GetOutputWidth()  const { return m_Output ? m_Output->GetWidth()  : 0u; }
    u32 GetOutputHeight() const { return m_Output ? m_Output->GetHeight() : 0u; }
    bool IsReady() const { return m_Ready; }

    /// 半分辨率系数（固定 1/2）：这一项是低频量，见文件头与文档 §10.2
    static constexpr u32 kDownscale = 2;

private:
    void CreateOutput(u32 width, u32 height);

    // 描述符集绑定号（与 RSM_Indirect.frag.slang 的 vk::binding 一致）
    static constexpr u32 kBindDepth    = 0;
    static constexpr u32 kBindWorldPos = 1;
    static constexpr u32 kBindNormal   = 2;
    static constexpr u32 kBindRSMPos   = 3;
    static constexpr u32 kBindRSMFlux  = 4;
    static constexpr u32 kBindParams   = 5;

    rhi::IRHIDevice* m_Device = nullptr;
    u32 m_Width = 0, m_Height = 0;

    std::unique_ptr<rhi::IRHIPipelineState> m_PSO;
    rhi::DescriptorSetLayoutHandle m_Layout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_Set    = rhi::kInvalidSet;

    std::unique_ptr<rhi::IRHIBuffer>  m_ParamsUBO;
    std::unique_ptr<rhi::IRHITexture> m_Output;
    std::unique_ptr<rhi::IRHISampler> m_PointSampler;    // GBuffer 读取
    std::unique_ptr<rhi::IRHISampler> m_LinearSampler;   // RSM 贴图读取 + 升采样

    rhi::IRHITexture* m_Depth    = nullptr;
    rhi::IRHITexture* m_WorldPos = nullptr;
    rhi::IRHITexture* m_Normal   = nullptr;
    rhi::IRHITexture* m_RSMPos   = nullptr;
    rhi::IRHITexture* m_RSMFlux  = nullptr;

    bool m_Ready = false;
};

} // namespace he::render
