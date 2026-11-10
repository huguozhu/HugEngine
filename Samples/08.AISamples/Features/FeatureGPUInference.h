#pragma once

#include "Features/IFeature.h"

#include "AI/Runtime/Backend/GPUBackend.h"
#include "AI/Neural/NeuralUpscaler.h"

#include <memory>

// ============================================================
// FeatureGPUInference — 功能3：GPU 推理后端验证（原 07.GPUInference）
//   内置核 / 外部 SPIR-V / 零拷贝纹理采样 三条路径 + 神经子系统
//   纯面板功能（无 3D 场景）
// ============================================================

class FeatureGPUInference : public IFeature {
public:
    const char* GetName() const override { return "GPU 推理"; }
    bool NeedsRender3D() const override { return false; }

    bool Initialize(he::rhi::IRHIDevice* device, he::rhi::IRHISwapChain* sc,
                    he::ai::IAIDevice* ai) override;
    void Shutdown() override;
    void Update(float dt) override;
    void RenderUI() override;

private:
    // 三条推理路径（一次性执行，结果供面板展示）
    void RunInference();

    he::rhi::IRHIDevice* m_Device = nullptr;
    he::ai::IAIDevice* m_AI = nullptr;
    std::unique_ptr<he::render::NeuralUpscaler> m_Neural;

    // 内置核结果
    float  m_ScaleOut[4] = {};
    bool   m_ScaleOk = false;
    double m_ScaleMs = 0.0;
    // 外部核结果
    float  m_AddOut[4] = {};
    bool   m_AddOk = false;
    double m_AddMs = 0.0;
    // 零拷贝纹理结果
    bool   m_TexOk = false;
    double m_TexMs = 0.0;
};
