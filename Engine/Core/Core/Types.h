#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>
#include <span>
#include <memory>
#include <functional>
#include <optional>

// ============================================================
// HugEngine fundamental types
// ============================================================

namespace he {

// --- Fixed-width integers ---
using i8  = int8_t;
using i16 = int16_t;
using i32 = int32_t;
using i64 = int64_t;

using u8  = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;

using f32 = float;
using f64 = double;

using usize = size_t;

// --- String types ---
using String      = std::string;
using StringView  = std::string_view;

// --- Smart pointers ---
template<typename T>
using UniquePtr = std::unique_ptr<T>;

template<typename T>
using SharedPtr = std::shared_ptr<T>;

template<typename T>
using WeakPtr = std::weak_ptr<T>;

template<typename T, typename... Args>
UniquePtr<T> MakeUnique(Args&&... args) {
    return std::make_unique<T>(std::forward<Args>(args)...);
}

template<typename T, typename... Args>
SharedPtr<T> MakeShared(Args&&... args) {
    return std::make_shared<T>(std::forward<Args>(args)...);
}

// --- Non-copyable / Non-movable ---
#define HE_DECLARE_NON_COPYABLE(ClassName)          \
    ClassName(const ClassName&) = delete;           \
    ClassName& operator=(const ClassName&) = delete

#define HE_DECLARE_NON_MOVABLE(ClassName)           \
    ClassName(ClassName&&) = delete;                \
    ClassName& operator=(ClassName&&) = delete

// --- Utility ---
template<typename T>
using Optional = std::optional<T>;

template<typename T>
using Span = std::span<T>;

constexpr u64 INVALID_U64 = ~0ull;
constexpr u32 INVALID_U32 = ~0u;

// 默认窗口分辨率（1080p），初始化时被实际窗口尺寸覆盖
constexpr u32 kDefaultWindowWidth  = 1920;
constexpr u32 kDefaultWindowHeight = 1080;

// 默认阴影贴图分辨率
constexpr u32 kDefaultShadowMapSize = 2048;

// 默认相机投影参数
constexpr float kDefaultFOV       = 60.0f;     // 垂直视场角（度）
constexpr float kDefaultNearPlane = 0.1f;      // 近裁剪面
constexpr float kDefaultFarPlane  = 2000.0f;   // 远裁剪面

} // namespace he
