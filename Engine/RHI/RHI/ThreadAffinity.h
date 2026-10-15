#pragma once

#include "Core/Assert.h"
#include "Core/Types.h"   // u32（录制者进入深度）——不依赖调用方先包含 Types.h

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
//
// ============================================================
// 【两级归属：拥有线程 vs 录制者线程】（补齐 15.多线程架构 §19.4 的缺口）
//
// MTCR（多线程命令录制）是**唯一**被设计成"非拥有线程也能录"的部分：`ForwardPipeline` 把
// 8 条 secondary command list 分给 8 个 `JobSystem` worker，每条列表（连同它自己的
// `VkCommandPool` 与 3 个 sec CB）在同一时刻**只被一个 worker 独占**（见
// `docs/HugEngine引擎介绍/15.多线程架构与渲染实现分析.md` §5.3/§5.4）。
//
// 【为什么不能直接删掉断言】删掉就等于"任何线程都能录 secondary"，通道从此无法审计：哪天
// 游戏线程误碰了别人的 sec list，只能靠偶发花屏发现 —— 那正是 T0.1 加断言的动机。
// 所以这里把归属拆成两级：
//   · 拥有线程（`IsOwner`）—— 主列表录制 / Submit / Present / AcquireNextImage / 设备销毁，
//     仍由 `HE_ASSERT_RENDER_THREAD()` 独占，一条都不放宽；
//   · 录制者线程（`IsRecorder`）—— 只有**显式**进入 `SecondaryRecordScope` 的线程才算。
//     worker 在任务体开头声明、任务结束自动撤销（RAII），未声明的线程录 secondary 照样命中
//     断言。
//
// 【为什么用 thread_local 而不是"线程 id 集合 + 锁"】角色是**每线程私有**的：断言只会在
// 录制线程自己身上求值，别的线程既看不到也不需要看到。thread_local 计数因此无需任何同步，
// 也让"进入/退出"与 worker 生命周期天然对齐（无注册表残留）。
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

    // --- 录制者角色（次级命令缓冲，MTCR 专用）---

    /// 当前线程声明"我要录次级命令缓冲"。按深度计数，支持嵌套（成对调用）。
    void PushRecorder() { ++RecorderDepth(); }
    /// 撤销一次声明（与 `PushRecorder` 成对；深度已为 0 时忽略，避免下溢）
    void PopRecorder() {
        u32& depth = RecorderDepth();
        if (depth > 0u) --depth;
    }
    /// 当前线程是否已声明为录制者（拥有线程检查不经过这里）
    [[nodiscard]] bool IsRecorder() const { return RecorderDepth() > 0u; }

private:
    /// 每线程私有的录制者进入深度（见文件头"为什么用 thread_local"）
    static u32& RecorderDepth() {
        thread_local u32 s_Depth = 0u;
        return s_Depth;
    }

    std::atomic<std::thread::id> m_Owner{};
    std::atomic<bool>            m_Claimed{false};
};

/// 进程级唯一实例（函数内静态：C++11 起初始化线程安全，无需在初始化顺序上操心）
[[nodiscard]] inline ThreadAffinity& GetThreadAffinity() {
    static ThreadAffinity s_Affinity;
    return s_Affinity;
}

/// RAII 声明：把当前线程登记为"次级命令缓冲录制者"，析构时自动撤销。
/// 【给谁用】MTCR 的 worker 任务体开头构造一个即可（`ForwardPipeline::RenderScene` 的并行
/// 录制任务）；作用域一结束就恢复成"普通线程"，不会在 worker 被复用时留下残留角色。
/// 【为什么必须显式声明】这样"谁有权录 secondary"在代码里是可检索、可审计的：漏声明 → 断言
/// 直接命中，而不是悄悄录出一条竞争的命令缓冲。
class SecondaryRecordScope {
public:
    SecondaryRecordScope()  { GetThreadAffinity().PushRecorder(); }
    ~SecondaryRecordScope() { GetThreadAffinity().PopRecorder(); }

    // 不可拷贝/移动：角色与具体线程绑定，跟着对象搬家会变成"另一条线程撤不掉"
    SecondaryRecordScope(const SecondaryRecordScope&)            = delete;
    SecondaryRecordScope& operator=(const SecondaryRecordScope&) = delete;
    SecondaryRecordScope(SecondaryRecordScope&&)                 = delete;
    SecondaryRecordScope& operator=(SecondaryRecordScope&&)      = delete;
};

} // namespace he::rhi

// --- 线程归属断言 ---
// 默认在**所有构建配置**下生效（比"仅 Debug"更早暴露违规调用）；定义 HE_DISABLE_THREAD_ASSERT
// 可整体关闭（用于兼容尚未迁移的旧调用点，或做性能对照）。
#ifdef HE_DISABLE_THREAD_ASSERT
    #define HE_ASSERT_RENDER_THREAD() ((void)0)
    #define HE_ASSERT_RECORD_THREAD() ((void)0)
#else
    #define HE_ASSERT_RENDER_THREAD()                                                      \
        do {                                                                               \
            if (!::he::rhi::GetThreadAffinity().IsOwner()) {                               \
                ::he::AssertFailure("HE_ASSERT_RENDER_THREAD", __FILE__, __LINE__,         \
                                    "RHI 调用必须在渲染线程（方案 B §3 铁律 1）");         \
            }                                                                              \
        } while (0)

    // 次级命令缓冲录制（MTCR）专用：拥有线程，或**已显式声明**的录制者线程。
    // 其余录制入口（主列表 Begin/End、Submit、Present、AcquireNextImage、设备销毁）仍用
    // `HE_ASSERT_RENDER_THREAD()`，两级归属的边界见文件头说明。
    #define HE_ASSERT_RECORD_THREAD()                                                      \
        do {                                                                               \
            if (!::he::rhi::GetThreadAffinity().IsOwner() &&                               \
                !::he::rhi::GetThreadAffinity().IsRecorder()) {                            \
                ::he::AssertFailure("HE_ASSERT_RECORD_THREAD", __FILE__, __LINE__,         \
                                    "次级命令缓冲录制必须在拥有线程或已声明的录制者线程上发起"); \
            }                                                                              \
        } while (0)
#endif
