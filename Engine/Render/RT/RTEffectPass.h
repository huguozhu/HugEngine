#pragma once

#include "RHI/RHI.h"
#include "Pipeline/RTPass.h"
#include "Pipeline/Camera.h"
#include "Math/Math.h"
#include <memory>
#include <vector>

namespace he::render {

// ============================================================
// RTExecuteContext — RT 效果 Pass 每帧执行上下文
// 由 HybridRTPipeline 在 BuildFrameGraph 的 RT Pass lambda 中填充
// ============================================================
struct RTExecuteContext {
    float4x4 invViewProj = float4x4(1.0f);   // 逆 ViewProj（深度 → 世界坐标）
    float3   cameraPos   = float3(0.0f);     // 相机世界坐标
    u32      frameIndex  = 0;                // 帧索引（时域抖动用）
    rhi::IRHITexture* gbDepth   = nullptr;   // GBuffer 深度
    rhi::IRHITexture* gbNormal  = nullptr;   // GBuffer 法线（world space）
    rhi::IRHITexture* gbWorldPos = nullptr;  // GBuffer 世界坐标（MRT4）
    rhi::IRHITexture* gbAlbedo  = nullptr;   // GBuffer 反照率（MRT0）
    rhi::IRHIBuffer*  lightBuffer = nullptr; // GPULight[] SSBO（当前帧槽）
    u32               lightCount  = 0;       // 有效光源数
    rhi::IRHITexture* sceneMaterialTex = nullptr; // 场景材质纹理（3×N RGBA32F，ClosestHit 查询用）
    rhi::IRHITexture* sceneTriangleNormals = nullptr; // 三角形顶点法线纹理（ClosestHit 平滑法线用）
    rhi::IRHIBuffer*  ddgiProbeBuffer = nullptr;  // DDGI 探针 SSBO（GI miss 回退用）
};

// ============================================================
// RTEffectPass — RT 效果 Pass 基类
//
// 为 HybridRT 的每个 RT 效果（阴影/反射/AO/GI）提供公共能力：
//   1. set0 RayGen 描述符集布局创建 + 描述符集分配
//   2. 独立效果 RT 管线 + SBT（RTPass::CreateEffectPipeline）
//   3. 输出 UAV 纹理创建
//   4. BindRTPipeline + 绑描述符集 + TraceRays 调度
//
// 子类职责：在 Execute() 中更新 set0 描述符，然后按顺序
// BindRT() → SetPushConstants() → TraceRays() 发射光线。
//（必须先绑 RT 管线再推常量：vkCmdPushConstants 使用当前绑定布局，
//  若先推常量会应用到上一 Pass 的布局，范围不匹配导致写入失败）
// ============================================================
// 反射/GI ClosestHit 光源（48B/个，从 GPULight[] 显式抽取 → UniformBuffer）
// 与 RT_HitCommon.slang 的 RTHitLight 布局一致（避免 ClosestHit 访问 SSBO）
struct RTHitLightGPU {
    float4 colorIntensity;   // rgb=颜色, w=强度
    float4 positionRange;    // xyz=位置, w=范围
    float4 directionType;    // xyz=方向, w=类型 (0=Dir,1=Point,2=Spot)
};
static_assert(sizeof(RTHitLightGPU) == 48, "RTHitLightGPU must be 48 bytes");

class RTEffectPass {
    HE_DECLARE_NON_COPYABLE(RTEffectPass);

public:
    RTEffectPass()  = default;
    virtual ~RTEffectPass() = default;

    // 初始化
    // @param width/height      输出纹理尺寸（效果实际分辨率）
    // @param rayGenBindings    set0 的 RayGen 资源绑定描述
    // @param rtShaders/shaderGroups 效果管线着色器组合
    // @param pcRange            push constant 范围（RayGen 阶段）
    // @param outFormat/outUsage 输出 UAV 纹理格式与用途
    // @param maxPayloadSize     Reflection/GI 需要 32 字节（Shadow/AO 16 字节）
    bool Initialize(rhi::IRHIDevice* device,
                    u32 width, u32 height,
                    std::vector<rhi::DescriptorSetLayoutBinding> rayGenBindings,
                    std::vector<rhi::ShaderBytecode> rtShaders,
                    std::vector<rhi::RTShaderGroup> shaderGroups,
                    rhi::PushConstantRange pcRange,
                    rhi::Format outFormat,
                    rhi::TextureUsage outUsage,
                    u32 maxPayloadSize = rhi::kRTMaxPayloadSize,
                    u32 maxRecursionDepth = rhi::kRTMaxRecursionDepth,
                    StringView debugName = "RTEffect");

    void Shutdown();

    // 子类实现：更新 set0 描述符 + push constants → 调 BindAndTrace
    virtual void Execute(rhi::IRHICommandList* cmd,
                         rhi::IRHIAccelerationStructure* tlas,
                         const RTExecuteContext& ctx) = 0;

    // ── 访问器 ──
    rhi::IRHITexture* GetOutput() const { return m_Output.get(); }
    u32 GetWidth()  const { return m_Width; }
    u32 GetHeight() const { return m_Height; }
    bool IsValid() const { return m_Pipeline && m_Pipeline->pipeline != nullptr; }

protected:
    // 绑定效果管线 + set0（+ 可选 set1/set2）——必须在 SetPushConstants 之前调用，
    // 否则 push constant 会应用到上一 Pass 的管线布局（降噪等图形 Pass 布局范围不匹配 → 写入失败）
    void BindRT(rhi::IRHICommandList* cmd,
                rhi::DescriptorSetHandle set1 = rhi::kInvalidSet,
                rhi::DescriptorSetHandle set2 = rhi::kInvalidSet);

    // 发射光线（SetPushConstants 之后调用）
    void TraceRays(rhi::IRHICommandList* cmd, u32 w, u32 h);

    // 输出纹理 → UnorderedAccess（RT 写）。首帧从 Undefined 过渡，
    // 后续帧从 ShaderResource 过渡（上一帧被 Lighting 以 SRV 采样结束）。
    void PrepareOutputUAV(rhi::IRHICommandList* cmd);

    // 创建 ClosestHit 光源 UBO（48B × 16，反射/GI 用）
    bool CreateHitLightUB(rhi::IRHIDevice* device);
    // 从 GPULight[] SSBO 显式抽取阴影光源数据填充 UBO（每帧）
    void FillHitLightUB(const RTExecuteContext& ctx);

    rhi::IRHIDevice* m_Device = nullptr;
    std::unique_ptr<RTPass::RTEffectPipeline> m_Pipeline;
    rhi::DescriptorSetLayoutHandle m_RayGenLayout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_RayGenSet    = rhi::kInvalidSet;
    std::unique_ptr<rhi::IRHITexture> m_Output;
    std::unique_ptr<rhi::IRHIBuffer>  m_LightUB;   // RTHitLightGPU[16]（反射/GI ClosestHit）
    rhi::PushConstantRange m_PCRange;
    u32 m_Width = 0, m_Height = 0;
    bool m_OutputWritten = false;   // 输出纹理是否已被写入（首帧屏障用 Undefined）
    String m_DebugName;
};

} // namespace he::render
