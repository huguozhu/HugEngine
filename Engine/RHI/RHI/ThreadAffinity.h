#pragma once

#include "Core/Assert.h"

#include <atomic>
#include <thread>

// ============================================================
// ThreadAffinity —— RHI 的线程归属（渲染线程化方案 B §4.5 / 阶段 0 任务 T0.1）
//
// 【为什么要它】RHI **不做线程安全**，而是规定"只有渲染线程能调 RHI"（方案 §3 铁律 1）。
// 没有归属机制时，游戏线程误调 RHI 只能靠"偶发崩溃 / 花屏 / validation 报错"发现，不可调试。
// 本类记录"设备拥有线程"（今天 = 创建设备的那个线程 = 主线程；阶段 2 起 = 渲染线程），
// 并提供 `HE_ASSERT_RENDER_THREAD()` 加在创建 / 销毁 / 录制 / 提交入口。
//
// 【为什么是原子成员】断言恰恰要在**非拥有线程**上求值（那正是要检测的情形），
// 普通成员会被判定为数据竞争（TSan / 校验层），故拥有者与标志位都用 `std::atomic`。
//
// 【阶段 0 行为不变】未 Claim（设备尚未创建或已销毁）时 `IsOwner()` 恒为 true ⇒ 断言不触发；
// Claim 记录的是创建设备的线程 ⇒ 阶段 0 期间拥有线程就是主线程，样例逐像素行为与现在一致。
// ============================================================

namespace he::rhi {

class ThreadAffinity {
public:
    /// 认领：把当前线程记为设备拥有线程（在创建设备处调用一次）
    void Claim() {
        m_Owner.store(std::this_thread::get_id(), std::memory_order_release);
        m_Claimed.store(true, std::memory_order_release);
    }

    /// 释放：注销拥有线程（设备销毁处调用）。释放后断言重新回到"永不触发"。
    void Release() { m_Claimed.store(false, std::memory_order_release); }

    [[nodiscard]] bool IsClaimed() const { return m_Claimed.load(std::memory_order_acquire); }

    /// 当前线程是否有权调用 RHI：未 Claim 时恒为 true（加载前 / 销毁后不设限）
    [[nodiscard]] bool IsOwner() const {
        if (!m_Claimed.load(std::memory_order_acquire)) return true;
        return m_Owner.load(std::memory_order_acquire) == std::this_thread::get_id();
    }

private:
    std::atomic<std::thread::id> m_Owner{};
    std::atomic<bool>            m_Claimed{false};
};

/// 进程级唯一实例（函数内静态：C++11 起初始化线程安全，无需在初始化顺序上操心）
[[nodiscard]] inline ThreadAffinity& GetThreadAffinity() {
    static ThreadAffinity s_Affinity;
    return s_Affinity;
}

} // namespace he::rhi

// --- 线程归属断言 ---
// 默认在**所有构建配置**下生效（比"仅 Debug"更早暴露违规调用）；定义 HE_DISABLE_THREAD_ASSERT
// 可整体关闭（用于兼容尚未迁移的旧调用点，或做性能对照）。
#ifdef HE_DISABLE_THREAD_ASSERT
    #define HE_ASSERT_RENDER_THREAD() ((void)0)
#else
    #define HE_ASSERT_RENDER_THREAD()                                                      \
        do {                                                                               \
            if (!::he::rhi::GetThreadAffinity().IsOwner()) {                               \
                ::he::AssertFailure("HE_ASSERT_RENDER_THREAD", __FILE__, __LINE__,         \
                                    "RHI 调用必须在渲染线程（方案 B §3 铁律 1）");         \
            }                                                                              \
        } while (0)
#endif
