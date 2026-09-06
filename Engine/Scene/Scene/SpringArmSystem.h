#pragma once

#include "Core/Types.h"

// ============================================================
// SpringArmSystem — 弹簧臂驱动（静态系统，仿 AgentSystem）
//
// 每帧由调用方驱动一次：遍历所有 SpringArmComponent，
// 合成相机 Transform 写入关联相机实体（依赖 S0.4 主相机）。
// 目标/相机实体缺失时安全跳过（不崩溃、不告警刷屏）。
// ============================================================

namespace he {
class World;

class SpringArmSystem {
public:
    /// 驱动所有 SpringArmComponent 一帧
    static void Update(World& world, f32 dt);
};

} // namespace he
