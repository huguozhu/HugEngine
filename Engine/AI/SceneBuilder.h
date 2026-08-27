#pragma once

#include "Core/Types.h"
#include "Containers/Array.h"
#include "Scene/Entity.h"

// ============================================================
// SceneBuilder — 场景 JSON 解释器（JSON → World）
//
// 把 LLM 输出的场景 JSON 翻译成真实的 Entity/Component 树。
// MVP 用硬编码组件映射（不碰反射属性注册），
// 只支持 Cube/Sphere/DirectionalLight/PointLight/PhysicalSky 五种组件。
// 无 RHI 设备时几何缓冲创建被跳过，因此可无设备单测。
// ============================================================

namespace he {
class World;
class SceneGraph;
} // namespace he

namespace he::ai {

/// 场景构建结果
struct SceneBuildResult {
    bool success = false;
    String error;
    TArray<Entity> entities;   // 已创建的实体（按 JSON 顺序）
};

/// 把场景 JSON 解释成真实的 Entity/Component 树。
/// 硬编码支持 Cube/Sphere/DirectionalLight/PointLight/PhysicalSky。
SceneBuildResult BuildScene(World& world, SceneGraph& sg, const String& sceneJson);

} // namespace he::ai
