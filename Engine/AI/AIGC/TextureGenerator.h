#pragma once

#include "Core/Types.h"
#include "Math/Math.h"

#include <vector>

// ============================================================
// TextureGenerator — 程序化纹理生成器（G2.1）
//
// 生成即标准资产：LLM 输出纹理规格 JSON → 引擎按规格程序化
// 生成 RGBA 像素 → 写盘 PNG（Content/Generated/）→ 与 glTF
// 导入的纹理走同一条加载管线（stb_image），渲染零感知。
//
// 支持的 pattern（MVP）：solid / gradient / checker / noise。
// ============================================================

namespace he::ai::aigc {

/// 纹理规格（LLM 输出 / 程序化生成输入）
struct TextureSpec {
    String pattern = "gradient";   // solid / gradient / checker / noise
    u32    size    = 64;           // 宽 = 高（MVP 正方形）
    float3 colorA  = {0.2f, 0.5f, 0.9f};   // 主色 / 渐变起点 / 棋盘色1
    float3 colorB  = {0.9f, 0.2f, 0.2f};   // 副色 / 渐变终点 / 棋盘色2
};

/// 纹理生成结果（= 标准资产的最小形态）
struct TextureGenResult {
    bool success = false;
    String error;
    String path;                 // 生成的 PNG 资产文件路径
    u32    width  = 0;           // 纹理宽
    u32    height = 0;           // 纹理高
    std::vector<u8> pixels;      // RGBA8 像素（供直接创建 GPU 纹理）
};

class TextureGenerator {
public:
    /// 按规格生成 RGBA8 像素（CPU）
    static bool Generate(const TextureSpec& spec, std::vector<u8>& outRGBA);

    /// 写盘 PNG（stb_image_write）
    static bool WritePNG(const String& path, u32 w, u32 h, const u8* rgba);

    /// 解析 LLM 输出的纹理规格 JSON 文本（非法返回 false + 默认规格）
    static bool ParseSpec(const String& json, TextureSpec& out);

    /// 拼接纹理规格词表（注入 LLM，约束输出字段）
    static String BuildSpecPrompt();
};

} // namespace he::ai::aigc
