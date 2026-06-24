#pragma once

#include "Core/Types.h"
#include "Scene/Entity.h"
#include "Containers/Array.h"

// ============================================================
// glTF 2.0 GLB 加载器 (Phase 1: mesh + vertex/index 数据)
// ============================================================

namespace he {

class World; // 前向声明

namespace asset {

/// glTF 加载结果
struct glTFResult {
    TArray<Entity> entities;   // 创建的实体列表
    usize          meshCount = 0;
    bool           success   = false;
    String         error;
};

/// 从 GLB 文件加载模型
///
/// 解析二进制 glTF (GLB) 格式：
///   12-byte header + JSON chunk + BIN chunk
///
/// Phase 1 支持:
///   - 网格顶点位置 (POSITION) + 法线 (NORMAL) + UV (TEXCOORD_0)
///   - 索引缓冲
///   - 基础 PBR 材质参数
///
glTFResult LoadGLB(World& world, const String& filePath);

} // namespace asset
} // namespace he
