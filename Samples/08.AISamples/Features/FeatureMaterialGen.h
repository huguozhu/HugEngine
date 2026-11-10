#pragma once

#include "Features/IFeature.h"

#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "AI/AIGC/GenerativeAssetFactory.h"

#include <vector>

// ============================================================
// FeatureMaterialGen — 功能5：文生材质（原 09.MaterialGen）
//   LLM 材质规格（无 key 降级）→ 应用到右方块（MeshComponent PBR）
//   + GPU 纹理生成核 texture_gen（本地 GPU 后端）
// ============================================================

class FeatureMaterialGen : public IFeature {
public:
    const char* GetName() const override { return "文生材质"; }

    bool Initialize(he::rhi::IRHIDevice* device, he::rhi::IRHISwapChain* sc,
                    he::ai::IAIDevice* ai) override;
    void Shutdown() override;
    void Update(float dt) override;
    void RenderUI() override;

    he::World* GetWorld() override { return &m_World; }
    he::SceneGraph* GetSceneGraph() override { return &m_SG; }

private:
    he::World m_World;
    he::SceneGraph m_SG{m_World};
    he::ai::IAIDevice* m_AI = nullptr;
    he::rhi::IRHIDevice* m_Device = nullptr;

    he::ai::aigc::MaterialGenResult m_Material;
    bool m_Applied = false;
    bool m_GenOk = false;      // GPU 纹理生成核结果
    float m_GenAvgR = 0, m_GenAvgG = 0, m_GenAvgB = 0;
};
