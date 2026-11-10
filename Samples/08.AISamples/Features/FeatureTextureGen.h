#pragma once

#include "Features/IFeature.h"

#include "AI/AIGC/TextureGenerator.h"

// ============================================================
// FeatureTextureGen — 功能4：文生纹理（原 08.TextureGen）
//   LLM 纹理规格（无 key 降级）→ 生成像素 → PNG 资产 → stbi 读回
//   → GPU 纹理 → AI 推理消费（平均色）。纯面板功能。
// ============================================================

class FeatureTextureGen : public IFeature {
public:
    const char* GetName() const override { return "文生纹理"; }
    bool NeedsRender3D() const override { return false; }

    bool Initialize(he::rhi::IRHIDevice* device, he::rhi::IRHISwapChain* sc,
                    he::ai::IAIDevice* ai) override;
    void Shutdown() override;
    void Update(float dt) override;
    void RenderUI() override;

private:
    he::rhi::IRHIDevice* m_Device = nullptr;
    he::ai::IAIDevice* m_AI = nullptr;

    he::ai::aigc::TextureGenResult m_Result;
    bool  m_LoadedOk = false;    // PNG 经 stbi 标准加载
    bool  m_InferOk  = false;    // 生成纹理经 AI 推理消费
    float m_AvgR = 0, m_AvgG = 0, m_AvgB = 0;
};
