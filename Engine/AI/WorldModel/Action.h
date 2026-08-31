#pragma once

#include "Core/Types.h"

#include <memory>

// ============================================================
// Action — 智能体动作（AI 写世界的声明式原语）
//
// AI 不直接改内存：只输出结构化 Action，
// 由 CompileAction 编译成 he::Command（走 CommandHistory，可撤销）。
//
// 支持的 op（MVP）：
//   "SpawnEntity"   —— 新建实体（argsJson = 单个实体场景 JSON，复用 SceneBuilder）
//   "SetTransform"  —— 修改实体 Transform（position/scale）
//   "SetProperty"   —— 经反射修改指定组件属性
// ============================================================

namespace he {
class World;
class SceneGraph;
class Command;
} // namespace he

namespace he::ai {

/// 智能体动作
struct Action {
    u64    targetEntity = 0;   // 目标实体（SpawnEntity 忽略）
    String op;                 // 操作类型：SpawnEntity / SetTransform / SetProperty / CallTool
    String argsJson;           // 参数（JSON 对象文本）
};

/// 把一条 Action 编译成一条 he::Command。
/// 非法动作（实体不存在、属性不存在、op 不支持）返回 nullptr，不产生副作用。
std::unique_ptr<he::Command> CompileAction(he::World& world, he::SceneGraph& sg,
                                           const Action& a);

} // namespace he::ai
