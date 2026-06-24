#pragma once

#include "Core/Types.h"

#include <functional>

// ============================================================
// Entity — 基于 UUID 的实体标识符
// ============================================================

namespace he {

/// 实体唯一标识符（64 位 UUID）
using EntityID = u64;

/// 无效实体常量
constexpr EntityID kInvalidEntity = 0;

/// 实体：轻量级句柄，仅包含 ID
struct Entity {
    EntityID id = kInvalidEntity;

    bool IsValid() const { return id != kInvalidEntity; }

    bool operator==(const Entity& other) const { return id == other.id; }
    bool operator!=(const Entity& other) const { return id != other.id; }
    bool operator<(const Entity& other)  const { return id < other.id; }
};

} // namespace he

// std::hash 支持
template<>
struct std::hash<he::Entity> {
    size_t operator()(const he::Entity& e) const noexcept {
        return std::hash<he::u64>{}(e.id);
    }
};
