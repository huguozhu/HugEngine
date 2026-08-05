#pragma once

#include "RHI/RHI.h"
#include "Pipeline/RTPass.h"
#include "Pipeline/Camera.h"
#include "Math/Math.h"
#include <memory>

namespace he::render {

// ============================================================
// PTRenderContext — 全路径追踪 Pass 每帧执行上下文
// 由 PathTracingPipeline 在 BuildFrameGraph 的 PT_Render lambda 中填充
// ============================================================
struct PTRenderContext {
    float4x4 invViewProj  = float4x4(1.0f);   // 逆 ViewProj（相机光线）
    float4x4 prevViewProj = float4x4(1.0f);   // 上一帧 VP（velocity 重投影）
    float3   cameraPos    = float3(0.0f);     // 相机世界坐标
    u32      frameIndex   = 0;                // 帧索引（抖动/随机种子）
    u32      maxBounces   = 4;                // 最大弹射次数
    u32      sampleCount  = 1;                // 每像素 SPP
    float    skyIntensity = 1.0f;             // 天空强度
    u32      flags        = 0;                // bit0=useReSTIR, bit1=MIS, bit2=俄罗斯轮盘赌, bit3=NEE
    rhi::IRHIBuffer* lightBuffer   = nullptr; // GPULight[] SSBO（当前帧槽位）
    u32              lightCount    = 0;       // 有效光源数
    rhi::IRHIBuffer* finalReservoir = nullptr; // ReSTIR FinalReservoir SSBO（上帧数据，可为空）
    rhi::IRHITexture* sceneMaterialTex = nullptr;   // 场景材质纹理（4×N，ClosestHit 用）
    rhi::IRHITexture* sceneTriangleNormals = nullptr; // 三角形顶点法线纹理（ClosestHit 用）
    rhi::IRHITexture* blueNoise = nullptr;          // STBN 3D 纹理（RayGen Load 采样）
};

// ============================================================
// PTPass — 全路径追踪 Pass
//
// 迭代式路径追踪 RayGen 直接渲染整帧（NEE+MIS+俄罗斯轮盘赌+天空），
// 输出 5 个 UAV（HDR 颜色 + GBuffer：深度/法线/速度/albedoMetallic），供 RTDenoiser
// 时域累积与 ReSTIR_Init 重建世界坐标使用。
//
// 与 RTEffectPass 的区别：输出 5 张 UAV 纹理（而非 1 张），故不继承
// 基类，直接复用 RTPass::CreateEffectPipeline 创建独立管线 + SBT。
//
// set0 绑定：
//   b0=TLAS(RG), b1..b4=四输出 UAV(RG), b5=GPULight[] SSBO(RG),
//   b6=材质纹理(CH), b7=三角形法线(CH), b8=FinalReservoir SSBO(RG), b9=albedoMetallic UAV(RG),
//   b10=STBN 3D 蓝噪声(RG, 无采样器 Load 采样)
// ============================================================
class PTPass {
    HE_DECLARE_NON_COPYABLE(PTPass);

public:
    PTPass()  = default;
    ~PTPass() = default;

    // 初始化：创建 set0 布局/描述符集 + 独立 RT 管线 + 5 张输出纹理
    bool Initialize(rhi::IRHIDevice* device, u32 width, u32 height);
    void Shutdown();

    // 每帧执行：屏障 5 输出 → 更新 set0 描述符 → 推常量 → TraceRays
    void Execute(rhi::IRHICommandList* cmd, rhi::IRHIAccelerationStructure* tlas,
                 const PTRenderContext& ctx);

    // ── 访问器 ──
    rhi::IRHITexture* GetHDR()      const { return m_HDR.get(); }
    rhi::IRHITexture* GetDepth()    const { return m_Depth.get(); }
    rhi::IRHITexture* GetNormal()   const { return m_Normal.get(); }
    rhi::IRHITexture* GetVelocity() const { return m_Velocity.get(); }
    rhi::IRHITexture* GetAlbedoMetallic() const { return m_AlbedoMetallic.get(); }
    u32 GetWidth()  const { return m_Width; }
    u32 GetHeight() const { return m_Height; }
    bool IsValid()  const { return m_Pipeline && m_Pipeline->pipeline != nullptr; }

private:
    // 5 张输出纹理 → UnorderedAccess（RT 写）。首帧从 Undefined 过渡，
    // 后续帧从 ShaderResource 过渡（上帧被降噪器/ReSTIR 以 SRV 采样结束）。
    void PrepareOutputUAV(rhi::IRHICommandList* cmd);

    rhi::IRHIDevice* m_Device = nullptr;
    u32 m_Width = 0, m_Height = 0;

    std::unique_ptr<RTPass::RTEffectPipeline> m_Pipeline;
    rhi::DescriptorSetLayoutHandle m_Layout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_Set    = rhi::kInvalidSet;
    rhi::PushConstantRange         m_PCRange;

    // 5 张输出纹理（同生共死，单标志控制首帧屏障）
    std::unique_ptr<rhi::IRHITexture> m_HDR;      // RGBA16_FLOAT 噪声 HDR
    std::unique_ptr<rhi::IRHITexture> m_Depth;    // R32_FLOAT 线性视图深度
    std::unique_ptr<rhi::IRHITexture> m_Normal;   // RGBA16_FLOAT 世界法线(0~1)+roughness
    std::unique_ptr<rhi::IRHITexture> m_Velocity; // RG16_FLOAT 屏幕速度
    std::unique_ptr<rhi::IRHITexture> m_AlbedoMetallic; // RGBA16_FLOAT albedo(rgb)+metallic(a)
    bool m_OutputWritten = false;
};

} // namespace he::render
