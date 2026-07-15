#pragma once

#include "Core/Types.h"
#include "Scene/Entity.h"
#include "Containers/Array.h"

// ============================================================
// glTF 2.0 加载器（cgltf 驱动）
//
// 支持 .glb（二进制）和 .gltf（JSON + 外部资源）两种格式。
// 使用 cgltf 库进行完整 spec-compliant 解析，包括：
//   - 全部 mesh / primitive 加载
//   - 完整节点层级 + TRS 变换
//   - PBR 材质参数（metallic/roughness、baseColor、emissive 等）
//   - 纹理路径提取
//   - 自动处理 byteStride、归一化、稀疏访问器
// ============================================================

namespace he {

class World;      // 前向声明
class SceneGraph; // 前向声明

namespace asset {

/// glTF 加载结果
struct glTFResult {
    TArray<Entity> entities;   // 所有创建的实体（遍历顺序）
    usize          meshCount = 0;
    bool           success   = false;
    String         error;
};

/// 加载 glTF 2.0 文件（.glb 或 .gltf）
///
/// @param world       实体将被创建到的 World
/// @param sceneGraph  用于建立节点父子层级的 SceneGraph
/// @param filePath    文件路径（.glb 或 .gltf）
/// @return            包含实体列表、网格计数、成功标志和错误信息的 glTFResult
///
/// 用法:
///   auto result = asset::LoadGLTF(*world, *sceneGraph, "Content/sponza.glb");
///   if (result.success) { ... }
glTFResult LoadGLTF(World& world, SceneGraph& sceneGraph, const String& filePath);

} // namespace asset
} // namespace he
