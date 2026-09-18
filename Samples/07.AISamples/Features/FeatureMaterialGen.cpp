#include "Features/FeatureMaterialGen.h"

#include "AI/Runtime/AIDevice.h"
#include "AI/Runtime/Backend/GPUBackend.h"
#include "Scene/Transform.h"
#include "Scene/CubeComponent.h"
#include "Scene/LightComponent.h"
#include "Scene/PhysicalSkyComponent.h"
#include "Core/Log.h"
#include "imgui.h"

using namespace he;

bool FeatureMaterialGen::Initialize(rhi::IRHIDevice* device, rhi::IRHISwapChain* sc,
                                    he::ai::IAIDevice* ai) {
    m_Device = device;
    m_AI = ai;
    (void)sc;
    if (!m_Device || !m_AI) return false;

    // 场景：左方块（默认材质对照）+ 右方块（应用生成材质）+ 地面/光源/天空
    MeshComponent* generatedMesh = nullptr;
    {
        Entity e = m_World.CreateEntity("Cube_Default");
        auto* xform = m_World.AddComponent<TransformComponent>(e);
        xform->position = float3(-1.5f, 1.0f, 0.0f);
        auto* mesh = m_World.AddComponent<CubeComponent>(e);
        mesh->baseColorFactor = float4(1.0f, 1.0f, 1.0f, 1.0f);
        m_SG.SetParent(e, Entity{kInvalidEntity});
    }
    {
        Entity e = m_World.CreateEntity("Cube_AIGen");
        auto* xform = m_World.AddComponent<TransformComponent>(e);
        xform->position = float3(1.5f, 1.0f, 0.0f);
        generatedMesh = m_World.AddComponent<CubeComponent>(e);
        m_SG.SetParent(e, Entity{kInvalidEntity});
    }
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

    // 文生材质（LLM / 降级内置金色金属）→ 应用到右方块
    const String kPrompt = "磨砂金色金属材质，中等粗糙度";
    if (m_AI->GetCaps().supportsRemoteLLM) {
        he::ai::aigc::GenerativeAssetFactory factory;
        m_Material = factory.TextToMaterial(*m_AI, kPrompt, "");
    } else {
        HE_CORE_WARN("[MaterialGen] 未设置 DEEPSEEK_API_KEY，用内置规格演示");
        m_Material.success   = true;
        m_Material.baseColor = float4(1.0f, 0.72f, 0.0f, 1.0f);
        m_Material.metallic  = 1.0f;
        m_Material.roughness = 0.25f;
        m_Material.error     = "降级模式";
    }
    if (m_Material.success && generatedMesh) {
        generatedMesh->baseColorFactor = m_Material.baseColor;
        generatedMesh->metallicFactor  = m_Material.metallic;
        generatedMesh->roughnessFactor = m_Material.roughness;
        generatedMesh->emissiveFactor  = m_Material.emissive;
        m_Applied = true;
        HE_CORE_INFO("[MaterialGen] 已应用生成材质");
    }

    // GPU 纹理生成核（texture_gen）：棋盘 → 读回平均色
    if (m_AI->GetCaps().supportsGPU) {
        constexpr u32 kGenSize = 64;
        he::ai::AITensorDesc desc;
        desc.elementCount = kGenSize * kGenSize * 4;
        desc.dtype        = he::ai::AIDataType::FP32;
        auto out = m_AI->CreateTensor(desc);
        if (out) {
            he::ai::InferenceRequest req;
            req.kernel = he::ai::kKernelTextureGen;
            req.params = {{"width", "64"}, {"height", "64"}, {"pattern", "2"},
                          {"colorA", "0.1,0.5,0.9"}, {"colorB", "0.9,0.6,0.1"}};
            req.outputs.push_back(out.get());
            auto inf = m_AI->Submit(std::move(req));
            if (inf) {
                std::vector<float> px(kGenSize * kGenSize * 4);
                if (m_AI->ReadTensor(out.get(), Span<float>(px))) {
                    double sr = 0, sg = 0, sb = 0;
                    for (u32 i = 0; i < kGenSize * kGenSize; ++i) {
                        sr += px[i * 4];
                        sg += px[i * 4 + 1];
                        sb += px[i * 4 + 2];
                    }
                    u32 n = kGenSize * kGenSize;
                    m_GenAvgR = (float)(sr / n);
                    m_GenAvgG = (float)(sg / n);
                    m_GenAvgB = (float)(sb / n);
                    m_GenOk = true;
                }
            }
        }
    }
    return true;
}

void FeatureMaterialGen::Shutdown() {}

void FeatureMaterialGen::Update(float) {}

void FeatureMaterialGen::RenderUI() {
    ImGui::Begin("文生材质");
    ImGui::Text("材质生成: %s", m_Material.success ? "成功" : "失败");
    if (!m_Material.error.empty())
        ImGui::TextWrapped("说明: %s", m_Material.error.c_str());
    if (m_Material.success) {
        ImGui::Text("基色: (%.2f, %.2f, %.2f)", m_Material.baseColor.x,
                    m_Material.baseColor.y, m_Material.baseColor.z);
        ImGui::Text("金属度: %.2f | 粗糙度: %.2f", m_Material.metallic, m_Material.roughness);
        ImGui::Text("应用: %s", m_Applied ? "右方块（左方块为对照）" : "失败");
    }
    ImGui::Separator();
    ImGui::Text("GPU 纹理生成(texture_gen): %s", m_GenOk ? "成功" : "失败");
    if (m_GenOk)
        ImGui::Text("平均色: (%.2f, %.2f, %.2f) 64x64", m_GenAvgR, m_GenAvgG, m_GenAvgB);
    ImGui::End();
}
