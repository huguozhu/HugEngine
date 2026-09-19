#pragma once

// ============================================================
// PostProcess/DenoiseSignal.h — 统一降噪框架（《Lumen设计与实现》§10 的 **11.3**）
//
// 【为什么需要它】11.1 把逐行同构的附属 pass 合并了、11.2 把 RT 的降噪链数据化了，但两件事
// 仍然没有共同的**信号层**：
//   ① 多信号共存时没有统一的分类与历史分配 —— 每个 `RTDenoiser` 自己 `CreateTexture` 一张历史，
//      `SpatialDenoiseAux` 又有自己的一套；谁有多少张、多大、什么格式，只有各自的构造点知道。
//   ② 有效性（`alpha < 0`）协议在**四处断裂**：合成端着色器里是逐源硬编码的 `if (id == ...)`，
//      新增一个源就得记得再加一处分支 —— 漏掉的那处会表现为"画面莫名变暗"，而且无法归因。
//
// 本文件把这两件事收敛成三个东西：
//   · `DenoiseSignal`  ——  一个"待降噪信号"的完整描述（含 `needsUpscale`）；
//   · `DenoiseHistoryPool`  ——  **统一的历史纹理分配**（同名同尺寸同格式只建一次）；
//   · 有效性契约 `denoise::`  ——  **唯一真值**：`alpha < 0` = 本条无数据。
//
// 【验收口径（§10 的 11.3，逐条对应）】
//   · 抽象必须在**真实的多信号共存**下验收：Lumen 的 Screen Probe / Radiance Cache 与既有
//     GI / 反射 / 阴影同帧 —— `DenoiseSignalRegistry::LogSummary()` 一次打印出当帧所有信号；
//   · 框架级有效性契约：合成端只读 `alpha < 0` 这一个约定（着色器侧的 `SourceIsValid()`）；
//   · `needsUpscale`：半分辨率输出**也要降噪**，降噪后再重建升采样。
// ============================================================

#include "RHI/RHI.h"
#include "Core/Types.h"

#include <memory>
#include <string>
#include <vector>

namespace he::render {

// ============================================================
// 框架级有效性契约（唯一真值）
//
// 【约定】任何 GI 信号的输出纹理，其 **alpha 通道**表达"本条有没有数据"：
//     alpha >= 0  ⇒ 有效（rgb 是本次估计）
//     alpha <  0  ⇒ **本条无数据**，合成端必须**跳过该源**（不计入归一化的分母）
//
// 【为什么是"分母也要跳过"】通道合成是加权归一化（`num/den`）。把无数据的源按 0 计入分母，
// 同通道其它源会被无谓稀释 —— 画面变暗，而且从画面上看不出原因（GI 计划 §9.2-C 就是这么
// 断出来的）。所以契约的两半同等重要：值无效 **且** 权重不计。
// ============================================================
namespace denoise {

/// 无效标记的取值（供 C++ 侧写清屏/填充色时使用；着色器侧只判 `< 0`）
inline constexpr float kInvalidAlpha = -1.0f;

/// 唯一判定入口（C++ 侧）。着色器侧对应 `SourceIsValid()`。
[[nodiscard]] inline bool IsValid(float alpha) { return alpha >= 0.0f; }

/// 中文说明（日志/断言信息用），让"约定写在哪"这件事在运行期也可见。
/// 内联在头里是**故意的**：单元测试要能在不链接渲染模块的前提下把这句契约本身钉住。
[[nodiscard]] inline const char* ValidityContractName() {
    return "有效性契约：alpha >= 0 表示本条有数据；alpha < 0 表示本条无数据，"
           "合成端必须跳过该源（值不计入分子，权重也不计入分母）";
}

} // namespace denoise

// ============================================================
// DenoiseSignal —— 一个待降噪信号的完整描述
// ============================================================
struct DenoiseSignal {
    std::string       name;                 // 唯一名字（RG pass 名 / 日志都用它）
    rhi::IRHITexture* input   = nullptr;    // 主 pass 输出（带噪声）
    rhi::IRHITexture* output  = nullptr;    // 降噪结果（合成端采样它）
    rhi::IRHITexture* depth   = nullptr;    // 引导（可空 ⇒ 该信号不做屏幕空间降噪）
    rhi::IRHITexture* normal  = nullptr;
    rhi::IRHITexture* velocity = nullptr;   // 时域重投影用（可空 ⇒ 无时域）
    /// 【11.3 新属性】信号分辨率低于消费端时置真：降噪之后必须**重建升采样**再交给合成端。
    /// 半分辨率直接采样（旧行为）会把降噪整个跳过 —— 半分辨率恰恰最需要降噪。
    bool  needsUpscale = false;
    u32   width = 0, height = 0;            // 信号自身分辨率
    u32   targetWidth = 0, targetHeight = 0;  // 消费端（全分辨率）
    /// 参数按信号赋值（11.1 的集中点：不再散落在各 Provider 里）
    float depthSigma = 10.0f;
    float normalSigma = 8.0f;

    [[nodiscard]] bool HasSpatialGuide() const { return depth != nullptr && normal != nullptr; }
};

// ============================================================
// DenoiseHistoryPool —— 统一的历史纹理分配
//
// 【为什么集中分配】每个降噪器各建各的历史，会带来三件麻烦：显存账算不清（谁建的多大只有各自
// 知道）、resize 时容易漏掉某一个、以及"同名信号被建了两张"（例如 SSGI 与 SSR 都用默认名）。
// 集中到一处之后：同名同尺寸同格式**只建一次**，`LogSummary()` 能一次报出全部历史纹理。
// ============================================================
class DenoiseHistoryPool {
public:
    void Initialize(rhi::IRHIDevice* device) { m_Device = device; }
    void Shutdown();

    /// 取（或创建）某信号的历史纹理。name 相同、尺寸与格式一致时复用同一张。
    /// 返回的纹理**由池持有**，调用方只借指针（生命周期到 Shutdown/OnResize）。
    rhi::IRHITexture* Acquire(const char* name, u32 w, u32 h, rhi::Format fmt);

    /// 尺寸变化：释放全部历史（下一帧按新尺寸重建，`RTDenoiser` 会重置首帧标记）
    void OnResize(u32 /*w*/, u32 /*h*/);

    [[nodiscard]] u32 Count() const { return (u32)m_Entries.size(); }
    [[nodiscard]] u64 TotalBytes() const;
    /// 逐条打印（名字/尺寸/格式/占用），用于"统一分配"这件事的可核对性
    void LogSummary(const char* tag) const;

private:
    struct Entry {
        std::string name;
        u32 width = 0, height = 0;
        rhi::Format format = rhi::Format::Unknown;
        std::unique_ptr<rhi::IRHITexture> tex;
    };
    rhi::IRHIDevice* m_Device = nullptr;
    std::vector<Entry> m_Entries;
};

// ============================================================
// DenoiseSignalRegistry —— 当帧所有信号的登记处（"多信号共存"的唯一视角）
//
// 每帧由各 Provider 通过 `DescribeSignals()` 登记自己的信号；帧图在构图前后各看一次：
// 登记完打印一次摘要（名字/分辨率/是否要升采样/参数），于是"框架真的看见了所有信号"这件事
// 有运行期证据，而不是靠"我加了接口"来自证。
// ============================================================
class DenoiseSignalRegistry {
public:
    void Clear() { m_Signals.clear(); }
    void Register(const DenoiseSignal& s) { m_Signals.push_back(s); }

    [[nodiscard]] u32 Count() const { return (u32)m_Signals.size(); }
    [[nodiscard]] const DenoiseSignal& Get(u32 i) const { return m_Signals[i]; }
    /// 有屏幕空间引导（可以做降噪）的信号数
    [[nodiscard]] u32 DenoiseCount() const {
        u32 n = 0;
        for (const auto& s : m_Signals) if (s.HasSpatialGuide()) ++n;
        return n;
    }
    /// 需要重建升采样的信号数（11.3 的 `needsUpscale`）
    [[nodiscard]] u32 UpscaleCount() const {
        u32 n = 0;
        for (const auto& s : m_Signals) if (s.needsUpscale) ++n;
        return n;
    }
    /// 有速度缓冲（可以做时域重投影）的信号数
    [[nodiscard]] u32 TemporalCount() const {
        u32 n = 0;
        for (const auto& s : m_Signals) if (s.velocity != nullptr) ++n;
        return n;
    }

    /// 逐条打印。**只在信号集合发生变化时打印**（名字集合的哈希变化），避免每帧刷屏。
    void LogSummary(const char* tag);

private:
    std::vector<DenoiseSignal> m_Signals;
    size_t m_LastSignature = 0;   // 上次打印时的信号名集合哈希
};

} // namespace he::render
