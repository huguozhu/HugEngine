#pragma once

#include "Core/Types.h"
#include "RHI/SwapChain.h"

// ============================================================
// FramePresenter —— 帧外壳的 acquire/present 收口（L4-1）
//
// 【为什么要有它】`AcquireNextImage` 与 `Present` 是整帧里**最后两个散落在样例里**的帧序 API 调用
// （每个样例各一对）。L4-3 要把帧外壳变成命令流（RHI 线程自己 Begin→翻译→End→Submit→Present），
// 那时只需要改这一个对象，而不是再改 8 个样例。
//
// 【为什么 BeginFrame / EndFrame 不合成一个调用】两者在**不同线程**上发生：
//   · `BeginFrame`（acquire）在游戏线程 —— 渲染命令录制前就要知道图像下标；
//   · `EndFrame`（present）在渲染命令里 —— 模式 1/2 下由渲染线程执行，模式 0 就在游戏线程。
// 合成一个调用会把线程归属搅乱，而这正是 L4-2/L4-3 之前必须保持不变的东西。
//
// 【vsync】必须与交换链创建时一致：只改描述而 Present 仍传旧值会被锁在刷新率上
// （06.GILab 历史上踩过，见该文件对应注释）。
//
// 【它只是个轻量包装】不持有资源、不分配、不做同步；可以按帧构造，也可以放进 lambda 按引用捕获。
// ============================================================

namespace he::rhi {

class FramePresenter {
public:
    FramePresenter() = default;
    explicit FramePresenter(IRHISwapChain* swapChain) : m_SwapChain(swapChain) {}

    void SetSwapChain(IRHISwapChain* swapChain) { m_SwapChain = swapChain; }

    /// 取下一张交换链图像。失败（窗口最小化 / 需要重建）返回 false ⇒ 调用方跳过本帧。
    [[nodiscard]] bool BeginFrame() {
        return m_SwapChain != nullptr && m_SwapChain->AcquireNextImage();
    }

    /// 呈现本帧（`vsync` 要与交换链创建时一致）。
    void EndFrame(bool vsync) {
        if (m_SwapChain) m_SwapChain->Present(vsync);
    }

    /// 本帧的交换链图像下标（录制期需要）。
    [[nodiscard]] u32 ImageIndex() const {
        return m_SwapChain ? m_SwapChain->GetCurrentBackBufferIndex() : 0u;
    }

    /// L4-2：开始录制本帧的命令缓冲（不取得所有权，只借用）。
    void BeginRecording(IRHIDevice* device, IRHICommandList* cmdList) {
        m_Device      = device;
        m_CommandList = cmdList;
        if (m_CommandList) m_CommandList->Begin();
    }

    /// L4-2：结束录制并提交。**present 不在这里**（EndFrame 负责），两者之间可能还夹着
    /// FlushComputeWork() 之类的逻辑，收口时不能把它挤掉。
    void EndRecording() {
        if (!m_CommandList) return;
        m_CommandList->End();
        if (m_Device) m_Device->Submit(m_CommandList);
        m_CommandList = nullptr;
    }

    [[nodiscard]] bool IsValid() const { return m_SwapChain != nullptr; }

private:
    IRHISwapChain* m_SwapChain = nullptr;
    IRHIDevice*       m_Device      = nullptr;   // L4-2：仅借用
    IRHICommandList*  m_CommandList = nullptr;   // L4-2：仅借用（EndRecording 后清空）
};

} // namespace he::rhi
