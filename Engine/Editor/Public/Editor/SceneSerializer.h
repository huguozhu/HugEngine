#pragma once

#include "Core/Types.h"

namespace he {
    class World;
    class SceneGraph;
}

namespace he::editor {

// ============================================================
// SceneSerializer — 场景二进制序列化
//
// .hescene 格式: Header "HESC" + version + entities[] + hierarchy[]
// 每个 entity 包含 component 列表，通过反射 SerializeObject 序列化
// ============================================================
class SceneSerializer {
public:
    /// 保存场景到文件
    static bool Save(StringView filePath, World& world, SceneGraph& sg);

    /// 从文件加载场景（会清空现有 World/SceneGraph 内容）
    static bool Load(StringView filePath, World& world, SceneGraph& sg);

private:
    static constexpr u32 kMagic   = 0x43534548; // "HESC" (little-endian)
    static constexpr u32 kVersion = 1;
};

} // namespace he::editor
