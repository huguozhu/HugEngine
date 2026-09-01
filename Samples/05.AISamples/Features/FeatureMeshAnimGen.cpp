#include "Features/FeatureMeshAnimGen.h"

#include "AI/Runtime/AIDevice.h"
#include "AI/AIGC/MeshGenerator.h"
#include "Scene/Transform.h"
#include "Scene/CubeComponent.h"
#include "Scene/AnimationComponent.h"
#include "Scene/LightComponent.h"
#include "Scene/PhysicalSkyComponent.h"
#include "Core/Log.h"
#include "imgui.h"

using namespace he;

bool FeatureMeshAnimGen::Initialize(rhi::IRHIDevice* device, rhi::IRHISwapChain* sc,
                                    he::ai::IAIDevice* ai) {
    m_AI = ai;
    (void)device; (void)sc;
    if (!m_AI) return false;

    // 生成对象实体（先用默认几何，下面覆盖为生成网格）
    {
        Entity e = m_World.CreateEntity("GenObject");
        m_GenEntity = e;
        m_World.AddComponent<TransformComponent>(e);
        auto* mesh = m_World.AddComponent<CubeComponent>(e);
        mesh->baseColorFactor = float4(0.8f, 0.5f, 0.2f, 1.0f);
        m_SG.SetParent(e, Entity{kInvalidEntity});
    }
    // 地面 / 光源 / 天空
    {
        Entity e = m_World.CreateEntity("Ground");
        auto* xform = m_World.AddComponent<TransformComponent>(e);
        xform->position = float3(0, -1, 0);
        xform->scale    = float3(20, 0.2f, 20);
        auto* mesh = m_World.AddComponent<CubeComponent>(e);
        mesh->baseColorFactor = float4(0.3f, 0.3f, 0.35f, 1.0f);
        m_SG.SetParent(e, Entity{kInvalidEntity});
    }
    {
        Entity e = m_World.CreateEntity("Sun");
        m_World.AddComponent<TransformComponent>(e);
        auto* dl = m_World.AddComponent<DirectionalLight>(e);
        dl->direction = float3(0.5f, -1, 0.5f);
        dl->intensity = 5.0f;
        m_SG.SetParent(e, Entity{kInvalidEntity});
    }
    {
        Entity e = m_World.CreateEntity("Sky");
        m_World.AddComponent<TransformComponent>(e);
        m_World.AddComponent<PhysicalSkyComponent>(e);
        m_SG.SetParent(e, Entity{kInvalidEntity});
    }

    // 文生网格（LLM / 降级金字塔）→ SetMeshData
    const String kMeshPrompt = "一个金字塔，尺寸 1.2";
    if (m_AI->GetCaps().supportsRemoteLLM) {
        he::ai::aigc::GenerativeAssetFactory factory;
        m_Mesh = factory.TextToMesh(*m_AI, kMeshPrompt);
    } else {
        HE_CORE_WARN("[MeshAnimGen] 未设置 DEEPSEEK_API_KEY，用内置规格演示");
        m_Mesh.name = "pyramid";
        he::ai::aigc::MeshGenerator::Generate("pyramid", 1.2f, 16, m_Mesh);
        m_Mesh.error = "降级模式";
    }
    auto* genMesh = m_World.GetComponent<MeshComponent>(m_GenEntity);
    if (m_Mesh.success && genMesh) {
        genMesh->SetMeshData(m_Mesh.vertices, m_Mesh.indices);
        HE_CORE_INFO("[MeshAnimGen] 已应用生成网格: {} ({} 顶点)",
                     m_Mesh.name, m_Mesh.vertices.size());
    }

    // 文生动画（LLM / 降级左右摆动）→ AnimationComponent
    const String kAnimPrompt = "左右摆动动画：0 秒在原点，1 秒移到右侧，2 秒回到原点";
    if (m_AI->GetCaps().supportsRemoteLLM) {
        he::ai::aigc::GenerativeAssetFactory factory;
        m_Anim = factory.TextToAnimation(*m_AI, kAnimPrompt);
    } else {
        HE_CORE_WARN("[MeshAnimGen] 未设置 DEEPSEEK_API_KEY，用内置规格演示");
        m_Anim.success = true;
        m_Anim.name    = "swing";
        m_Anim.duration = 2.0f;
        m_Anim.loop    = true;
        m_Anim.translations = {{0.0f, float3(0, 0.5f, 0)},
                               {1.0f, float3(1.5f, 0.5f, 0)},
                               {2.0f, float3(0, 0.5f, 0)}};
        m_Anim.error = "降级模式";
    }
    if (m_Anim.success) {
        m_GenAnim = m_World.AddComponent<AnimationComponent>(m_GenEntity);
        m_GenAnim->clips.resize(1);
        auto& clip = m_GenAnim->clips[0];
        clip.name = m_Anim.name;
        clip.duration = m_Anim.duration;
        clip.looping  = m_Anim.loop;
        for (auto& k : m_Anim.translations) clip.translations.push_back(k);
        for (auto& k : m_Anim.scales)       clip.scales.push_back(k);
        m_GenAnim->currentClip = 0;
        m_GenAnim->playing = true;
        HE_CORE_INFO("[MeshAnimGen] 已应用生成动画: {}（{} 关键帧）",
                     m_Anim.name, m_Anim.translations.size());
    }
    return true;
}

void FeatureMeshAnimGen::Shutdown() {}

void FeatureMeshAnimGen::Update(float dt) {
    // 播放生成动画（驱动目标 Transform）
    if (m_GenAnim && m_GenAnim->playing) {
        m_GenAnim->Update(dt, m_World.GetComponent<TransformComponent>(m_GenEntity));
    }
}

void FeatureMeshAnimGen::RenderUI() {
    ImGui::Begin("文生网格与动画");
    ImGui::Text("网格生成: %s", m_Mesh.success ? "成功" : "失败");
    if (m_Mesh.success)
        ImGui::Text("形状: %s | 顶点: %zu | 索引: %zu",
                    m_Mesh.name.c_str(),
                    (size_t)m_Mesh.vertices.size(), (size_t)m_Mesh.indices.size());
    if (!m_Mesh.error.empty())
        ImGui::TextWrapped("说明: %s", m_Mesh.error.c_str());
    ImGui::Separator();
    ImGui::Text("动画生成: %s", m_Anim.success ? "成功" : "失败");
    if (m_Anim.success)
        ImGui::Text("名称: %s | 关键帧: %zu | 时长: %.1fs | 循环: %s",
                    m_Anim.name.c_str(), (size_t)m_Anim.translations.size(),
                    m_Anim.duration, m_Anim.loop ? "是" : "否");
    if (!m_Anim.error.empty())
        ImGui::TextWrapped("说明: %s", m_Anim.error.c_str());
    ImGui::End();
}
