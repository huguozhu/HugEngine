#pragma once

// ============================================================
// PixelProbe —— 像素数值探针（验证/诊断基础设施）
//
// 【它解决什么问题】有些判据只需要**少数几个像素的线性值**，却必须来自 GPU 的真实输出：
// 白炉（全白环境 + albedo=1 + 关直接光）要求"任意两点亮度相等、且都等于 1.0"，
// 于是一次读回 2 个像素就能给出 0.98~1.02 的数值判据 —— 比截图看肉眼可靠得多。
//
// 【与 FrameDumpRecorder 的分工】
//   · FrameDumpRecorder：整幅纹理/缓冲 → **文件**（供离线逐像素/逐字节比对）；
//   · PixelProbe：少量像素 → **数值**（供进程内即时判据与 UI 显示）。
// 两者共用同一套同步思路：录制侧挂 timeline 信号，读取侧只等那一次提交
// （**不做设备级 WaitIdle** —— 那会连带等掉别的在飞工作，并掩盖"提交未落地"的真问题）。
//
// 【线程模型】Initialize/RecordReadback/FinishRecording 在录制该帧命令列表的线程上调用；
// TryReadBack 可在游戏线程（WaitForFence 没有归属断言），句柄与值用原子量跨线程传递。
//
// 【调用顺序（每帧）】
//   ① WantsFrame(...)                 —— 是否本帧采样（调用方还需自己判断开关）
//   ② RecordReadback(cmd, tex, ...)   —— render pass 之外，逐点 1×1 拷贝
//   ③ FinishRecording(device, cmd)    —— 录制结束处：挂 timeline 信号
//   ④ TryReadBack(device)             —— 提交之后：等 timeline → 映射 → 解码 → 算亮度
// ============================================================

#include "Core/Types.h"

#include "RHI/RHI.h"

#include <atomic>
#include <memory>
#include <vector>

namespace he::render {

class PixelProbe {
public:
    /// 一个采样点（绝对像素坐标；由调用方按窗口尺寸算）
    struct Sample {
        u32 x = 0;
        u32 y = 0;
    };

    /// 一个采样点的读数（线性空间 RGBA16_FLOAT 的 RGB + Rec.709 亮度）
    struct Value {
        float rgb[3]      = {0.0f, 0.0f, 0.0f};
        float luminance   = 0.0f;
    };

    /// 建 host 可见读回缓冲（maxSamples 个采样点；每点 8 字节 RGBA16_FLOAT）。
    /// 返回 false 表示缓冲创建失败（之后所有接口为空操作）。
    bool Initialize(rhi::IRHIDevice* device, u32 maxSamples);

    /// 本帧是否该采样（intervalFrames 为 0 时按 1 处理）
    [[nodiscard]] bool WantsFrame(u64 frameIndex, u32 intervalFrames) const {
        const u32 n = intervalFrames ? intervalFrames : 1u;
        return frameIndex % (u64)n == 0ull;
    }

    /// 录制读回：每个采样点一个 1×1 的 CopyTextureToBuffer（必须在 render pass 之外）
    void RecordReadback(rhi::IRHICommandList* cmd, rhi::IRHITexture* tex,
                        const Sample* samples, u32 count);

    /// 采样帧录制结束：给本帧挂 timeline 信号（读取侧只等这一次提交）
    void FinishRecording(rhi::IRHIDevice* device, rhi::IRHICommandList* cmd);

    /// 提交之后调用：等本帧 timeline → 映射 → half 解码 → 算亮度。返回是否读到了新值。
    bool TryReadBack(rhi::IRHIDevice* device);

    [[nodiscard]] const std::vector<Value>& Values() const { return m_Values; }
    /// 已采样点数（Initialize 时给定）
    [[nodiscard]] u32 Count() const { return m_Count; }

    /// 两点亮度比（b 的亮度接近 0 时返回 0，避免除零）
    [[nodiscard]] float LuminanceRatio(u32 a, u32 b) const;

private:
    /// 跨线程传递的 timeline 信号（原子量不可拷贝/移动 ⇒ 单独存放，惰性创建）
    struct CrossThreadFence {
        std::atomic<rhi::RHIFenceHandle> fence{rhi::kInvalidFence};
        std::atomic<u64>                 value{0};
    };

    bool                                m_Ready  = false;   // 缓冲已建好
    bool                                m_Recording = false;
    u32                                 m_Count  = 0;
    std::unique_ptr<rhi::IRHIBuffer>    m_Readback;          // host 可见读回缓冲
    std::unique_ptr<CrossThreadFence>   m_Fence;
    std::vector<Value>                  m_Values;
};

} // namespace he::render
