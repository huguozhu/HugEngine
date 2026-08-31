#pragma once

#include "Render/Subsystem/RenderSubsystem.h"
#include "AI/Runtime/Backend/IAIBackend.h"   // Ref<IAITensor>（共享指针句柄）

#include <memory>

// ============================================================
// NeuralUpscaler — 首个神经渲染子系统（A3.2b 收编验证）
//
// 以 IRenderSubsystem 形态实现，内部经 IAIDevice 推理：
//   每帧 Update：WrapRHITexture(输入纹理) → Submit(texture_sample 核)
//              → 输出缓冲读回统计（平均亮度）供面板展示
// 证明「神经特性 = 渲染子系统 + 统一推理底座」，不再直调 SDK。
//
// 说明：本实现以「验证架构形态」为目标（静态渐变输入纹理 +
// 每帧推理统计），真实超分（低分辨率渲染 + 上采样）在 A3.2c 接入 SDK 时落地。
// ============================================================

namespace he {
class CommandHistory;
} // namespace he

namespace he::ai {
class IAIDevice;
} // namespace he::ai

namespace he::render {

class NeuralUpscaler : public IRenderSubsystem {
public:
    explicit NeuralUpscaler(he::ai::IAIDevice* aiDevice);
    ~NeuralUpscaler() override;

    // ---- IRenderSubsystem ----
    bool Initialize(rhi::IRHIDevice* device, u32 width, u32 height) override;
    void Shutdown() override;
    void Update(const SubsystemContext& ctx) override;
    void Render(rhi::IRHICommandList* cmdList) override;
    void Bind(rhi::IRHICommandList* cmdList) const override;
    void OnResize(u32 width, u32 height) override;

    const char* GetName()  const override { return "NeuralUpscaler"; }
    bool IsReady()  const override { return m_Initialized; }
    bool IsEnabled() const override { return m_Enabled; }
    void SetEnabled(bool enabled) override { m_Enabled = enabled; }

    // ---- 状态查询（面板展示用）----
    u32  GetInferenceCount() const { return m_InferenceCount; }   // 累计推理次数
    double GetLastInferenceMs() const { return m_LastInferenceMs; } // 上次推理用时
    float GetLastAvgBrightness() const { return m_LastAvgBrightness; } // 上次读回平均亮度

private:
    // 每帧执行一次纹理采样推理（Wrap → Submit → 读回统计）
    void RunInference();

    he::ai::IAIDevice* m_AI = nullptr;            // 推理底座（不拥有）

    std::unique_ptr<rhi::IRHITexture> m_InputTex; // 输入纹理（程序化渐变，模拟渲染输出）
    he::ai::Ref<he::ai::IAITensor>    m_OutputTensor; // 推理输出缓冲张量

    u32    m_TexSize   = 32;                      // 输入纹理尺寸
    bool   m_Initialized = false;

    // 推理统计
    u32    m_InferenceCount = 0;
    double m_LastInferenceMs = 0.0;
    float  m_LastAvgBrightness = 0.0f;
};

} // namespace he::render
