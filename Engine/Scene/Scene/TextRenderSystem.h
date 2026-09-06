#pragma once

#include "Core/Types.h"

#include <vector>

// ============================================================
// TextRenderSystem — 3D 文字栅格化与纹理管理（静态系统）
//
// 纯 CPU 部分（RasterizeText）不依赖 RHI，可单元测试；
// Update 在有 RHI 设备时负责纹理/bindless 资源生命周期。
// ============================================================

namespace he {
class World;

namespace rhi { class IRHIDevice; }

class TextRenderSystem {
public:
    /// 栅格化 UTF-8 文字 → RGBA8 位图（白字 + alpha 覆盖；行序自下而上，
    /// 适配 Billboard 四边形 UV 原点在左下）。
    /// 字体查找顺序：fontPath → C:/Windows/Fonts/msyh.ttc（微软雅黑，CJK）
    /// → simhei.ttf → simsun.ttc → arial.ttf → 内置 ASCII 位图字体（stb_easy_font 兜底）。
    /// @return 是否成功生成（兜底字体保证 ASCII 恒成功）
    static bool RasterizeText(const String& text, const String& fontPath, float fontSize,
                              u32& outW, u32& outH, std::vector<u8>& outPixels);

    /// 每帧驱动所有 TextRenderComponent：
    /// 脏（text/fontPath/fontSize 变化）→ 重栅格化 → 更新广告牌尺寸；
    /// 有设备时重建纹理并注册 bindless（materialID 指向 baseColor 槽）。
    /// @param device 可空（无设备时仅更新尺寸/材质色，不建纹理）
    static void Update(World& world, rhi::IRHIDevice* device);
};

} // namespace he
