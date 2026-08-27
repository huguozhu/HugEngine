#pragma once

#include "Core/Types.h"
#include "AI/WorldModel/Observation.h"

// ============================================================
// WorldModel — 反射驱动的世界模型
//
// 不新增任何场景数据结构：World + TypeRegistry 就是世界模型。
// - Snapshot:   把 World 序列化成 LLM 可读的语义快照（JSON 文本）
//   —— 只导出带 HE_ATTR_AI_VISIBLE 注解的属性
// - TypeSchema: 遍历 TypeRegistry，输出所有组件类型的可读写字段清单
//   —— 注入 LLM system prompt 作为「可用词汇表」
// ============================================================

namespace he {
class World;
} // namespace he

namespace he::ai {

class WorldModel {
public:
    /// 生成语义快照（LLM 可读 JSON 文本）。
    /// 只导出带 HE_ATTR_AI_VISIBLE 标记的属性；附带 HE_ATTR_AI_DESCRIPTION 作为字段说明。
    String Snapshot(World& world, const ObservationFilter& filter) const;

    /// 生成类型 schema（供 LLM 了解"世界长什么样、能做什么"）。
    /// 遍历 TypeRegistry，输出所有含 AI_VISIBLE 属性的组件类型及其字段清单。
    String TypeSchema() const;
};

} // namespace he::ai
