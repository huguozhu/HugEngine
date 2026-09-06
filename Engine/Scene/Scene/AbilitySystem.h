#pragma once

#include "Core/Types.h"

// ============================================================
// AbilitySystem — 技能冷却计时（静态系统，简化 GAS 运行时）
//
// 每帧由调用方驱动一次：递减所有 AbilityComponent 的冷却剩余，
// 钳制到 0（技能转为可施放）。资源回复属游戏逻辑，由调用方处理。
// ============================================================

namespace he {
class World;

class AbilitySystem {
public:
    /// 驱动所有 AbilityComponent 一帧（冷却递减）
    static void Update(World& world, f32 dt);
};

} // namespace he
