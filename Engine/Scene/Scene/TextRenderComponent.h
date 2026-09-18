#pragma once

#include "Scene/BillboardComponent.h"
#include "RHI/RHI.h"
#include "RHI/FrameRetireQueue.h"

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
// 任务 23：bindless 槽位环形化 —— 文字变化时**复用同一个槽位**（ReleaseTexture →
// 过保护期后复用），旧纹理进 N 帧延迟释放队列（有界，不再无界保活）。
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

    /// 已退役的运行时纹理/采样器（N 帧延迟释放：等引用它的帧完成才真正销毁）
    struct RetiredTexture {
        std::unique_ptr<rhi::IRHITexture> texture;
        std::unique_ptr<rhi::IRHISampler> sampler;
    };

    /// 当前正在使用的运行时纹理/采样器（版本变化时被替换）
    std::unique_ptr<rhi::IRHITexture> runtimeTexture;
    std::unique_ptr<rhi::IRHISampler> runtimeSampler;

    /// 退役队列（任务 23：**有界**的 N 帧延迟释放，替代原来的无界 vector 保活）
    rhi::FrameRetireQueue<RetiredTexture> retiredTextures;

    /// 帧边界推进：释放保护期已过的旧纹理（TextRenderSystem::Update 每帧调用一次）
    void AdvanceRetireQueue() { retiredTextures.Advance(); }

    /// 退役当前运行时纹理（新纹理就位时调用；旧资源在 N 帧后释放）
    void RetireRuntimeTexture() {
        if (!runtimeTexture && !runtimeSampler) return;
        retiredTextures.Retire(RetiredTexture{ std::move(runtimeTexture),
                                               std::move(runtimeSampler) });
    }

    /// 待释放纹理数（调试/判据：验证"有界"，不会随重建次数增长）
    u32 GetRetiredTextureCount() const { return retiredTextures.GetPendingCount(); }

    /// 运行时纹理重建次数（调试/判据：用来对比"重建次数 ↑ 而 bindless 槽位不增"）
    u32 GetRebuildCount() const { return m_RebuildCount; }
    void MarkRebuilt() { ++m_RebuildCount; }

private:
    // 脏标记缓存（与 IsDirty/ClearDirty 配合）
    String m_LastText;
    String m_LastFontPath;
    float  m_LastFontSize = 0.0f;
    u32    m_RebuildCount = 0;
};

} // namespace he
