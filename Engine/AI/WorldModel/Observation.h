#pragma once

#include "Core/Types.h"
#include "Math/Math.h"

#include <vector>

// ============================================================
// Observation — AI 观察世界的过滤器
//
// 控制 WorldModel::Snapshot 的输出范围，
// 避免把整帧世界（可能上千实体）塞进 LLM prompt。
// ============================================================

namespace he::ai {

/// 观察过滤器 —— 控制快照范围（避免把整帧世界塞进 prompt）
struct ObservationFilter {
    float  radius = -1.0f;                       // 以 center 为球心的空间范围（-1 = 全场景，暂未启用）
    float3 center = {0, 0, 0};                   // 空间过滤中心（配合 radius 使用）
    u64    targetEntity = 0;                     // 0 = 全部实体，否则仅该实体
    std::vector<StringView> componentTypes;      // 只导出这些组件类型（空 = 全部）
};

} // namespace he::ai
