#pragma once

#include "Core/Types.h"

// ============================================================
// SubmitRouter —— 提交路由（渲染线程化方案 §12 附录 C 的 **A-2** 最小落地）
//
// 【要解决什么】模式 2（游戏线程 + 渲染线程 + RHI 线程）要求 **RHI 线程是唯一提交者**：渲染线程
// 只负责录制（`Begin/End/BeginRenderPass/Draw…`），`Submit` / `Present` / fence 等待全部交给
// RHI 线程。但 `Submit`/`Present` 的调用点散在 7 个样例的主循环里 —— 让每个样例自己判断模式
// 会把策略复制 7 份，且新增样例容易漏。
//
// 【做法】让 RHI 后端在 `Submit` / `Present` 处询问一个**注入的策略对象**：
//   · 未注入（模式 0/1）：就地同步提交，行为与改造前逐位一致 ✓
//   · 注入（模式 2）：把"要提交什么"打包交给策略，由 RHI 线程稍后真正提交 ✓
// 于是样例代码一行不改，`UsesRHIThread()` 也终于有了消费者（此前只有定义）。
//
// 【为什么"就地路径"要单独一对 `ExecuteSubmit/ExecutePresent`】RHI 线程最终还是要真的提交，
// 而它调用的正是同一套 `Submit/Present` —— 不分开就会自我递归。所以设备上另开 `Execute*`：
// 只有已登记的提交者线程可以调用（`HE_ASSERT_SUBMIT_THREAD()`）。
//
// 【本阶段不做】A-1（命令流：渲染线程只 append 纯数据载荷）与 A-3（资源创建句柄化）。
// 本阶段交接的是**已录制完成的命令列表**：渲染线程仍然"碰 Vulkan"（录制），但**不再提交**，
// 这已经满足"唯一提交者"这一核心语义；A-1/A-3 见方案 §12.3 的后续批次。
// ============================================================

namespace he::rhi {

class IRHICommandList;
class IRHISwapChain;
class IRHIDevice;   // 供 RHI 线程持有设备指针（不需要完整定义）

class ISubmitRouter {
public:
    virtual ~ISubmitRouter() = default;

    /// 接管一次提交。返回 true 表示"已接管，调用方不要就地执行"。
    /// 【为什么返回 bool 而不是 void】模式切换的瞬间（渲染线程已发路由、RHI 线程尚未就绪）
    /// 需要能明确地退回"就地执行"，而不是把命令丢掉或死等。
    virtual bool RouteSubmit(IRHICommandList* cmdList) = 0;

    /// 接管一次呈现（必须与同一帧的 `RouteSubmit` 保持先后顺序）。
    /// 【为什么 imageIndex / waitSemaphore 是**参数**】它们必须在**渲染线程**上捕获：RHI 线程执行
    /// 呈现时，交换链可能已被下一帧的 acquire 改过（`m_CurrentImage` / acquire 槽位 / 信号量），
    /// 让 RHI 线程回头读交换链状态就是数据竞争 + 用例错位 —— 实测会命中
    /// `VUID-VkPresentInfoKHR-pImageIndices-01430` 与 `VUID-vkQueuePresentKHR-pWaitSemaphores-03268`。
    virtual bool RoutePresent(IRHISwapChain* swapChain, u32 imageIndex, void* waitSemaphore,
                              bool vsync) = 0;

    /// 排空并等待：停线程 / 销毁设备之前调用，保证已路由的提交全部落地
    virtual void FlushAndWait() = 0;
};

} // namespace he::rhi
