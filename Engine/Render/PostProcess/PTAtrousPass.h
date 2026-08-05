#pragma once
#include "RHI/RHI.h"
#include "Math/Math.h"
#include <memory>

namespace he::render {

// ============================================================
// PTAtrousPass — SVGF 风格 A-Trous 空间滤波（compute）
// 对时域降噪后的 PT 输出做多迭代边缘感知空间滤波：
//   输入：denoised HDR + depth + normal
//   输出：空间滤波结果（ToneMap 输入）
// 仿 RTDenoiser 模式：输出纹理 + PSO + set0 描述符集。
// ============================================================
class PTAtrousPass {
    HE_DECLARE_NON_COPYABLE(PTAtrousPass);

public:
    struct Config {
        u32   width  = 0;
        u32   height = 0;
        u32   iterations      = 4;
        float sigmaDepth      = 0.05f;   // 深度边权重 σ（米）
        float normalPower     = 128.0f;  // 法线边权重指数
        float sigmaColor      = 0.5f;    // 颜色边 σ 系数
        float clampThreshold  = 0.0f;    // 火萤钳制阈值（0=关）
    };

    PTAtrousPass()  = default;
    ~PTAtrousPass() = default;

    bool Initialize(rhi::IRHIDevice* device, const Config& cfg);
    void Shutdown();
    void OnResize(u32 w, u32 h);

    // 每帧绑定输入（Render 前调用，外部不持有所有权）
    void SetInputs(rhi::IRHITexture* color, rhi::IRHITexture* depth, rhi::IRHITexture* normal);

    // 运行时参数更新（CVar 热更新；Render 时写入 push constant）
    void SetParams(u32 iterations, float sigmaDepth, float normalPower,
                   float sigmaColor, float clampThreshold);

    void Render(rhi::IRHICommandList* cmd);

    rhi::IRHITexture* GetOutput() const { return m_Output.get(); }
    bool IsReady() const { return m_Ready; }

private:
    void CreateTextures(u32 w, u32 h);

    Config m_Cfg;
    rhi::IRHIDevice* m_Device = nullptr;
    u32 m_Width = 0, m_Height = 0;
    bool m_Ready = false;

    std::unique_ptr<rhi::IRHITexture> m_Output;   // 空间滤波结果
    std::unique_ptr<rhi::IRHIPipelineState> m_PSO;
    rhi::DescriptorSetLayoutHandle m_Layout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_Set    = rhi::kInvalidSet;

    rhi::IRHITexture* m_Color  = nullptr;
    rhi::IRHITexture* m_Depth  = nullptr;
    rhi::IRHITexture* m_Normal = nullptr;
};

} // namespace he::render
