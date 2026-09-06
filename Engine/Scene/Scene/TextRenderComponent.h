#pragma once

#include "Scene/BillboardComponent.h"
#include "RHI/RHI.h"

#include <memory>
#include <vector>

// ============================================================
// TextRenderComponent — 3D 世界文字（对应 UE5 UTextRenderComponent）
//
// 继承 BillboardComponent（始终面向相机），由 TextRenderSystem 每帧：
//   1. 检测 text/fontPath/fontSize 脏标记
//   2. CPU 栅格化文字 → RGBA 位图（stb_truetype；系统字体兜底）
//   3. 重建 RHI 纹理 → 注册 bindless → 赋 materialID
//   4. 按位图宽高比更新广告牌 size（每 100 像素 = 1 米）
//
// 用法：
//   auto* tr = world.AddComponent<TextRenderComponent>(e);
//   tr->text = "实体名称"; tr->textColor = float4(1,1,0,1);
//   主循环每帧：TextRenderSystem::Update(world, device);
// ============================================================

namespace he {

class TextRenderComponent : public BillboardComponent {
    HE_COMPONENT()
public:
    // --- 参数 ---
    String text     = "Text";                // 文字内容（UTF-8）
    String fontPath;                         // 字体文件路径（空/不存在 → 依次尝试系统常见字体 → 内置 ASCII 位图字体兜底）
    float  fontSize = 64.0f;                 // 字号（像素高；世界高度 = 位图高/100 米）
    float4 textColor = float4(1.0f);         // 文字颜色（乘到白色字形纹理）

    // --- 运行时（TextRenderSystem 管理，勿手动改）---
    bool IsDirty() const {
        return text != m_LastText || fontPath != m_LastFontPath || fontSize != m_LastFontSize;
    }
    void ClearDirty() { m_LastText = text; m_LastFontPath = fontPath; m_LastFontSize = fontSize; }

    /// 追加持有运行时文字纹理与采样器。
    /// 注意：bindless 堆为 append-only（当前 Heap 设计），内容变化时追加新槽
    /// （上限 4096），旧纹理随组件存活直至析构 —— 高频动态更新需 Heap 环形化改造
    /// （见开发计划 Phase B 前的数据驱动化重构建议）。
    void AddRuntimeTexture(std::unique_ptr<rhi::IRHITexture> tex,
                           std::unique_ptr<rhi::IRHISampler> samp) {
        m_Textures.push_back(std::move(tex));
        m_Samplers.push_back(std::move(samp));
    }

private:
    // 脏标记缓存（与 IsDirty/ClearDirty 配合）
    String m_LastText;
    String m_LastFontPath;
    float  m_LastFontSize = 0.0f;

    // 历史纹理/采样器（保持存活，避免 bindless 堆持有悬垂指针）
    std::vector<std::unique_ptr<rhi::IRHITexture>> m_Textures;
    std::vector<std::unique_ptr<rhi::IRHISampler>> m_Samplers;
};

} // namespace he
