#pragma once

#include "Core/Types.h"
#include "Containers/Array.h"
#include "Scene/Entity.h"
#include "Scene/AnimationComponent.h"   // TranslationKey / ScaleKey（动画生成结果）
#include "AI/AIGC/TextureGenerator.h"   // TextureGenResult（纹理生成结果）
#include "AI/AIGC/MeshGenerator.h"      // MeshGenResult（网格生成结果）

// ============================================================
// GenerativeAssetFactory — 「AI 版 glTFLoader」
//
// prompt → 生成后端 → 标准资产。
// 核心洞察：生成内容 = 把"人工在编辑器里点出来的操作"换成
// "模型输出的操作"，落点仍是同一套 Entity/Component 树，
// 渲染/编辑/序列化对数据来源零感知。
//
// 本类只实现 GenerateScene（场景生成）；纹理/网格/材质/动画
// 生成（TextToTexture 等）在 G2 里程碑接入本地 GPU 后端。
// ============================================================

namespace he {
class World;
class SceneGraph;
} // namespace he

namespace he::ai {
class IAIDevice;
} // namespace he::ai

namespace he::ai::aigc {

// 生成结果 —— 与 asset::glTFResult 同构（对齐 Engine/Asset/Asset/glTFLoader.h）
struct GenerationResult {
    TArray<Entity> entities;    // 生成的实体树（与 glTF 导入一致）
    usize          assetCount = 0;
    bool           success    = false;
    String         error;
};

// 材质生成结果 —— 标准材质资产（MeshComponent PBR 字段 + 可选纹理）
struct MaterialGenResult {
    bool success = false;
    String error;
    float4 baseColor = {1.0f, 1.0f, 1.0f, 1.0f};   // 基础色 RGBA
    float3 emissive  = {0.0f, 0.0f, 0.0f};          // 自发光
    float  metallic  = 0.0f;                        // 金属度
    float  roughness = 0.8f;                        // 粗糙度
    TextureGenResult texture;                       // 可选生成纹理（可空）
};

// 动画生成结果 —— 标准动画资产（Transform 关键帧轨道）
struct AnimGenResult {
    bool success = false;
    String error;
    String name = "gen_anim";
    float duration = 1.0f;                          // 总时长（秒）
    bool   loop    = true;                          // 循环播放
    std::vector<TranslationKey> translations;       // 位置关键帧
    std::vector<ScaleKey>       scales;             // 缩放关键帧
};

class GenerativeAssetFactory {
public:
    /// 生成场景规格（LLM 输出的场景 JSON 文本），不装配实体。
    /// 供 AIGC 异步管线使用：生成结果先入「待接受队列」，接受时才装配（可撤销）。
    /// @return 场景 JSON 文本；失败返回空串
    String GenerateSceneJson(he::ai::IAIDevice& device, const String& prompt);

    /// 一句话生成一个场景，返回标准 Entity 树（立即装配）。
    /// 内部：GenerateSceneJson + SceneBuilder。
    GenerationResult GenerateScene(World& world, SceneGraph& sg,
                                   he::ai::IAIDevice& device, const String& prompt);

    /// 文生纹理（G2.1）：LLM 输出纹理规格 → 程序化生成像素 → 写盘 PNG。
    /// @param outPath 生成资产文件路径（空 = 只产出内存像素，不写盘）
    TextureGenResult TextToTexture(he::ai::IAIDevice& device,
                                   const String& prompt, const String& outPath);

    /// 文生材质（G2.2）：LLM 输出材质+可选纹理规格 → 材质参数与纹理。
    /// 产物为标准材质（MeshComponent PBR 字段），可直接应用。
    MaterialGenResult TextToMaterial(he::ai::IAIDevice& device,
                                     const String& prompt, const String& textureOutPath);

    /// 文生网格（G2.3）：LLM 输出形状规格 → 程序化生成顶点/索引。
    /// 产物为标准网格数据（与 glTF 导入同构），可 SetMeshData 渲染。
    MeshGenResult TextToMesh(he::ai::IAIDevice& device, const String& prompt);

    /// 文生动画（G2.3）：LLM 输出关键帧规格 → 生成 Transform 动画轨道。
    /// 产物为标准动画（AnimationComponent 可播放）。
    AnimGenResult TextToAnimation(he::ai::IAIDevice& device, const String& prompt);
};

} // namespace he::ai::aigc
