#pragma once

#include "Core/Types.h"
#include "Containers/Array.h"
#include "Scene/MeshComponent.h"   // StaticVertex

// ============================================================
// MeshGenerator — 程序化网格生成器（G2.3）
//
// 生成即标准资产：LLM 输出形状规格 → 程序化生成顶点/索引
// （与 glTF 导入器 / CubeComponent 产出的网格同构），
// 可直接 SetMeshData 到 MeshComponent 渲染。
//
// 支持形状（MVP）：cube / sphere / pyramid / torus。
// ============================================================

namespace he::ai::aigc {

/// 网格生成结果（= 标准网格数据，与 glTF 导入同构）
struct MeshGenResult {
    bool success = false;
    String error;
    String name;                    // 网格名（形状名）
    TArray<he::StaticVertex> vertices;   // 顶点（position/normal/uv）
    TArray<u32> indices;                  // 索引（三角形）
};

class MeshGenerator {
public:
    /// 按形状生成网格数据
    /// @param shape cube / sphere / pyramid / torus
    /// @param size 尺寸（cube 边长 / sphere 半径 / pyramid 底边长 / torus 大半径）
    /// @param segments 细分（sphere 经线 / torus 主段数）
    static bool Generate(const String& shape, float size, u32 segments, MeshGenResult& out);

    /// 解析 LLM 输出的形状规格 JSON（非法返回 false）
    static bool ParseSpec(const String& json, String& shape, float& size, u32& segments);

    /// 拼接形状规格词表（注入 LLM，约束输出字段）
    static String BuildSpecPrompt();
};

} // namespace he::ai::aigc
