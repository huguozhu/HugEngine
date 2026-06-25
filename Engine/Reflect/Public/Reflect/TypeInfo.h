#pragma once

// ============================================================
// TypeInfo.h — 反射系统兼容入口
//
// 内部包含稳定 API + 当前宏后端。
// 新代码推荐直接按需引入：
//   - 消费者（序列化/编辑器）: #include "Reflect/ReflectionAPI.h"
//   - 类声明（HE_CLASS 等）:  #include "Reflect/ReflectionMacros.h"
// ============================================================

#include "Reflect/ReflectionAPI.h"
#include "Reflect/ReflectionMacros.h"
