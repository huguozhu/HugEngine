#pragma once

#include "Threading/RenderThreadContext.h"   // T2.6：帧上下文（唯一 RHI 出口）

#include "Core/Types.h"

#include <functional>
#include <memory>
#include <vector>
#include <cstdlib>   // 【P7】HE_MTCR_MIN_DRAWS 读取（std::getenv / std::atoi）

// ============================================================
// ParallelRecordHelper —— 并行命令录制（MTCR）的公共骨架（T3.1）
//
// 【为什么要抽出来】这套骨架原先只写在 `ForwardPipeline::RenderScene` 里：
//   ① 一组 secondary command list 池（复用；每次 BeginSecondary 前清空）
//   ② 按 draw 数分块（每块一条列表）
//   ③ 每块在 `rhi::SecondaryRecordScope` 里录制（两级归属：worker 必须显式声明自己是录制者）
//   ④ `JobSystem::ParallelInvoke` 并行
//   ⑤ 主线程按块序 `ExecuteSecondary` 合并
// Deferred / PathTracing 接并行录制时要重复这五步 ⇒ 抽成一份，避免各管线各写一套
// （"归属声明"与"按块序合并"这两处写错就是跨线程竞争或顺序不确定）。
//
// 【调用方只负责什么】`recordChunk`：给定一条 secondary 列表与 [start,end) 的 draw 区间，
// 自己设 viewport/scissor、绑描述符集、推 push constant、发 Draw。管线特有的东西（PSO、bindless 集、
// 顶点索引缓冲来源）全留在回调里 ⇒ 本文件不依赖任何具体管线。
//
// 【顺序纪律】合并按**块序**（0..n-1），与录制完成顺序无关 ⇒ 绘制顺序确定、可逐位复现。
// ============================================================

namespace he::rhi {
class IRHIDevice;
class IRHICommandList;
class IRHIPipelineState;
} // namespace he::rhi

namespace he::render {

class ParallelRecordHelper {
public:
    /// 每块录制的回调：`secCmd` 是分给该块的次级列表；只录 [start,end) 这个区间。
    using RecordChunkFn = std::function<void(rhi::IRHICommandList& secCmd, u32 start, u32 end)>;

    ParallelRecordHelper() = default;
    ~ParallelRecordHelper() { Shutdown(); }

    /// 建池（管线初始化时调用一次）。`listCount` 会被裁到实际可用数。
    /// 【为什么没有 layout 参数】次级列表的来源是 `IRHIDevice::CreateSecondaryCommandList()`，
    /// 其描述符集布局随 `BeginSecondary(pso)` 的 PSO 走（与既有 Forward 用法一致）。
    bool Initialize(rhi::IRHIDevice* device, u32 listCount, const char* debugName);

    void Shutdown();

    [[nodiscard]] u32  ListCount() const { return static_cast<u32>(m_Lists.size()); }
    [[nodiscard]] bool IsReady()   const { return !m_Lists.empty(); }

    /// 【T3.4】并行录制的最小规模阈值：低于它的 draw 数走串行更划算。
    /// 依据：单条 draw 串行录制约 1–2 µs，而 `ParallelInvoke` 的唤醒 + 每块 `BeginSecondary/End`
    /// 的固定开销在数十 µs 量级 ⇒ 64 条是"并行收益转正"的经验下界（06 的 Forward / Deferred
    /// 都远超它，因此不影响现有判据场景）。
    static constexpr u32 kMinDrawsForParallel = 64;

    /// 【P7】并行门槛的运行期覆盖：`HE_MTCR_MIN_DRAWS=<n>`（默认仍是 64）。
    /// 存在的理由：样例的绘制数达不到 64 条门槛，bundle 路径从来没在真实场景里执行过
    /// （见文档 2.1「多线程录制」一项）。把门槛降到 8 / 16 就能让同一条代码路径真正跑起来，
    /// 便于验证「bundle 执行 + 与串行截图一致 + 0 报错」，不用改常量重编译。
    [[nodiscard]] static u32 MinDrawsForParallel() {
        static const u32 s_min = [] {
            const char* v = std::getenv("HE_MTCR_MIN_DRAWS");
            if (!v || !v[0]) return kMinDrawsForParallel;
            const int n = std::atoi(v);
            return (n > 0) ? static_cast<u32>(n) : kMinDrawsForParallel;
        }();
        return s_min;
    }

    /// 【T3.4】调用方据此决定走并行还是串行（false 时请回落到原有的主列表直接录制路径）。
    [[nodiscard]] static bool ShouldRecordParallel(u32 drawCount) {
        return drawCount >= MinDrawsForParallel();
    }

    /// 并行录制 + 合并。`mainCmd` 是主线程列表（在其上 ExecuteSecondary）。
    /// @param drawCount 本帧要录制的 draw 数（0 或池未就绪时直接返回，调用方自行回退）
    void Record(rhi::IRHICommandList* mainCmd, u32 drawCount, rhi::IRHIPipelineState* pso,
                const RecordChunkFn& recordChunk);

private:
    rhi::IRHIDevice*                                  m_Device = nullptr;

    /// 【T2.6】本帧的渲染线程上下文（可空；空时解析回退 `m_Device`）
    const RenderThreadContext* m_FrameCtx = nullptr;

public:
    /// 【T2.6】设置本帧的渲染线程上下文（搬迁过渡期用）
    void SetFrameContext(const RenderThreadContext* ctx) { m_FrameCtx = ctx; }

    // 【T2.6】统一解析入口。回退分支直接调 m_Device->Resolve*（不能被同一个扫描命中，
    // 否则会变成无限递归）；也不引入新的访问标签，避免改变后续成员的访问级别。
    [[nodiscard]] rhi::IRHIBuffer* ResB(rhi::RHIBufferHandle h) const {
        const RenderThreadContext* c = m_FrameCtx ? m_FrameCtx : GetFrameContext();
        if (c) return c->ResolveBuffer(h);
        return m_Device ? m_Device->ResolveBuffer(h) : nullptr;
    }
    [[nodiscard]] rhi::IRHITexture* ResT(rhi::RHITextureHandle h) const {
        const RenderThreadContext* c = m_FrameCtx ? m_FrameCtx : GetFrameContext();
        if (c) return c->ResolveTexture(h);
        return m_Device ? m_Device->ResolveTexture(h) : nullptr;
    }
    [[nodiscard]] rhi::IRHIPipelineState* ResP(rhi::RHIPipelineStateHandle h) const {
        const RenderThreadContext* c = m_FrameCtx ? m_FrameCtx : GetFrameContext();
        if (c) return c->ResolvePipelineState(h);
        return m_Device ? m_Device->ResolvePipelineState(h) : nullptr;
    }

    [[nodiscard]] rhi::IRHISampler* ResS(rhi::RHISamplerHandle h) const {
        const RenderThreadContext* c = m_FrameCtx ? m_FrameCtx : GetFrameContext();
        if (c) return c->ResolveSampler(h);
        return m_Device ? m_Device->ResolveSampler(h) : nullptr;
    }

    std::vector<std::unique_ptr<rhi::IRHICommandList>> m_Lists;
    const char*                                       m_DebugName = "SecRecord";
};

} // namespace he::render
