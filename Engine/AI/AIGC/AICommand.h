#pragma once

#include "Core/Types.h"
#include "Containers/Array.h"
#include "Scene/Entity.h"
#include "Editor/Command.h"
#include "AI/SceneBuilder.h"

#include <functional>
#include <utility>

// ============================================================
// AICommand — 生成即命令（可撤销）
//
// 「生成场景」命令：Execute 调用生成器装配实体树并记录列表，
// Undo 遍历列表销毁全部实体 —— 一次 AI 生成可完整回滚。
// 复用 Editor 模块的 he::Command / he::CommandHistory。
// ============================================================

namespace he {
class World;
class SceneGraph;
} // namespace he

namespace he::ai::aigc {

/// 「生成场景」命令：Execute 创建实体，Undo 销毁它们
class GenerateSceneCommand : public he::Command {
public:
    // 生成器：prompt → 场景构建结果（默认走 GenerativeAssetFactory）
    using Generator = std::function<SceneBuildResult(World&, SceneGraph&, const String&)>;

    GenerateSceneCommand(World& world, SceneGraph& sg, String prompt, Generator generator);

    // 执行生成：调生成器 → 记录创建的实体列表（供 Undo 回滚）
    void Execute() override;
    // 撤销：逆序销毁 Execute 创建的全部实体
    void Undo() override;
    String GetDescription() const override { return "AI 生成场景: " + m_Prompt; }

private:
    World&        m_World;      // 场景世界（不拥有）
    SceneGraph&   m_SG;         // 场景图（不拥有）
    String        m_Prompt;     // 生成提示词
    Generator     m_Generator;  // 生成器（生成逻辑可注入，便于测试）
    TArray<Entity> m_Created;   // Execute 创建的实体（Undo 用）
};

} // namespace he::ai::aigc
