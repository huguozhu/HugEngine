#pragma once

#include "Core/Types.h"
#include "Containers/Array.h"
#include "Scene/Entity.h"

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
};

} // namespace he::ai::aigc
