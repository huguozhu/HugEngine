#include "AI/AIGC/AICommand.h"

#include "Scene/World.h"
#include "Scene/SceneGraph.h"

namespace he::ai::aigc {

GenerateSceneCommand::GenerateSceneCommand(World& world, SceneGraph& sg,
                                           String prompt, Generator generator)
    : m_World(world), m_SG(sg),
      m_Prompt(std::move(prompt)), m_Generator(std::move(generator)) {}

void GenerateSceneCommand::Execute() {
    if (!m_Generator) return;
    // 调生成器装配场景；失败时不产生任何实体（无副作用）
    SceneBuildResult r = m_Generator(m_World, m_SG, m_Prompt);
    if (!r.success) return;
    // 记录本次创建的全部实体，供 Undo 回滚
    m_Created = std::move(r.entities);
}

void GenerateSceneCommand::Undo() {
    // 逆序销毁全部已创建实体，World 回到生成前状态
    for (auto it = m_Created.rbegin(); it != m_Created.rend(); ++it)
        m_World.DestroyEntity(*it);
    m_Created.clear();
}

} // namespace he::ai::aigc
