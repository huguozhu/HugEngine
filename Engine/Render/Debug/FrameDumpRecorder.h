#pragma once

// ============================================================
// FrameDumpRecorder —— 帧中间结果转储器（验证/诊断基础设施）
//
// 【它解决什么问题】GI 相关的"对不对"很难靠肉眼看：某源为 0、频率分离是否必要、
// 线程化是否改变了渲染结果……都需要**逐像素/逐字节**的证据。这类证据的做法各处相同：
//   · 采样帧把若干纹理/缓冲整幅拷进 host 可见缓冲（必须在 render pass 之外录制）；
//   · 只等这一次提交（timeline 信号量）而不做设备级 WaitIdle；
//   · 落盘成无文件头的原始像素（.f16 / .bin）＋ _meta.txt（名称 宽 高 格式）＋ _camera.txt。
// 此前 06.GILab 与 07.Nanite 各有一份几乎相同的实现（同格式、同日志、同 _meta 写法），
// 已有两份"真值"⇒ 迟早漂移。故把**机制**收在这里，**策略**（哪些目标值得看）仍留在使用方：
// 使用方只写 AddTexture/AddBuffer 的清单，其余由本类负责。
//
// 【线程模型】Add*/FinishRecording 必须在**录制该帧命令列表的线程**上调用（渲染线程）；
// TryWriteBack 可以在游戏线程调用（WaitForFence 无归属断言），因此句柄与值用原子量跨线程传递。
//
// 【开关】环境变量：HE_DUMP_GI=<tag>、HE_DUMP_GI_FRAME=<帧号，默认 60>、
// HE_DUMP_LUMEN_PROBES=1（额外允许缓冲目标）。未开启 ⇒ IsActive()==false，所有接口为空操作，
// 调用方可以无脑调用（不需要到处写 if）。
// ============================================================

#include "Core/Types.h"

#include "RHI/RHI.h"

#include <atomic>
#include <memory>
#include <vector>

namespace he::render {

class FrameDumpRecorder {
public:
    /// 相机参数快照：落 _camera.txt 用（解析对照需要"渲染这一帧时"的相机参数）。
    /// 刻意不引用 render::CameraData —— 让本模块只依赖 Core + RHI，避免把管线头文件拉进来。
    struct CameraSnapshot {
        float pos[3]     = {0.0f, 0.0f, 0.0f};
        float forward[3] = {0.0f, 0.0f, 1.0f};
        float up[3]      = {0.0f, 1.0f, 0.0f};
        float fov        = 60.0f;
        float nearPlane  = 0.1f;
        float farPlane   = 1000.0f;
        float aspect     = 1.0f;
    };

    /// 读取环境变量（默认 HE_DUMP_GI / HE_DUMP_GI_FRAME / HE_DUMP_LUMEN_PROBES，输出前缀 "gi_"）
    static FrameDumpRecorder FromEnv();

    /// 同上，但可换开关名与输出前缀（例如 05.PathTracing 用 HE_DUMP_PT + "pt_"）。
    /// tagEnv 为空 ⇒ 恒不启用。
    static FrameDumpRecorder FromEnv(const char* prefix, const char* tagEnv, const char* frameEnv);

    /// 是否开启（未设置 HE_DUMP_GI ⇒ false）
    [[nodiscard]] bool IsActive() const { return m_Active; }
    /// 是否允许缓冲目标（HE_DUMP_LUMEN_PROBES=1）
    [[nodiscard]] bool IncludeBuffers() const { return m_IncludeBuffers; }
    [[nodiscard]] const String& Tag() const { return m_Tag; }
    [[nodiscard]] u64 FrameIndex() const { return m_FrameIndex; }

    /// 本帧是否该采样（内部保证"只采一次"：录制过一次后恒为 false）
    [[nodiscard]] bool WantsFrame(u64 renderFrameIndex) const {
        return m_Active && !m_Done && renderFrameIndex >= m_FrameIndex;
    }

    /// 是否"已录制、待落盘"（使用方可据此把落盘夹在自己那段读回之后；不适用时恒 false）
    [[nodiscard]] bool PendingWriteBack() const { return m_Active && m_Done && !m_Written; }

    /// 登记一个**纹理**目标：整幅拷进 host 可见缓冲（RGBA16F 等按实际格式算每像素字节）。
    /// 必须在 render pass 之外、与录制同一线程上调用。
    void AddTexture(rhi::IRHIDevice* device, rhi::IRHICommandList* cmd,
                    const char* name, rhi::IRHITexture* tex);

    /// 同上，但由调用方显式给出每像素字节与 _meta.txt 里的格式字样
    /// （05.PathTracing 的对照脚本用 "RGBA16F"/"R32F"/"BGRA8"，必须原样保留）。
    void AddTexture(rhi::IRHIDevice* device, rhi::IRHICommandList* cmd,
                    const char* name, rhi::IRHITexture* tex,
                    u32 bytesPerPixel, const char* formatTag);

    /// 登记一个**缓冲**目标：原始字节落盘（.bin），用于探针/单元图这类非纹理中间量。
    void AddBuffer(rhi::IRHIDevice* device, rhi::IRHICommandList* cmd,
                   const char* name, rhi::IRHIBuffer* src, usize bytes);

    /// 采样帧录制结束：给本帧挂 timeline 信号（读取侧只等这一次提交）。
    /// 若一个目标都没有 ⇒ 自动关闭并告警（避免每帧重试）。
    void FinishRecording(rhi::IRHIDevice* device, rhi::IRHICommandList* cmd);

    /// 提交之后调用：等 timeline（必要时先让提交路由排空）→ 映射 → 落盘。
    /// 返回 true 表示"本次真的落盘了"，调用方可据此请求退出窗口。
    bool TryWriteBack(rhi::IRHIDevice* device, const CameraSnapshot& camera);

private:
    /// 一个采样目标：纹理走 .f16，缓冲走 .bin
    struct Target {
        String                           name;
        rhi::IRHITexture*                tex = nullptr;
        std::unique_ptr<rhi::IRHIBuffer> buf;
        u32                              w = 0, h = 0;
        bool                             isBuffer = false;
        usize                            bytes    = 0;
        u32                              bytesPerPixel = 8;   // 按纹理实际格式算
        String                           formatTag;           // _meta.txt 里的格式字样（空 = 用 "<n>bit"）
    };

    /// 按纹理格式算每像素字节（写死 8 B 会把 R16F/R32F 的末尾未初始化内存算成"差异"）
    static u32 BytesPerPixelOf(rhi::Format f);

    bool                m_Active         = false;
    bool                m_IncludeBuffers = false;
    bool                m_Done           = false;   // 已录制拷贝（防止重复录制）
    bool                m_Written        = false;   // 已落盘（防止重复写文件）
    String              m_Tag;
    String              m_Prefix = "gi_";   // 输出文件名前缀（05.PathTracing 用 "pt_"）
    u64                 m_FrameIndex = 60ull;
    std::vector<Target> m_Targets;

    /// 跨线程传递的 timeline 信号（原子量不可拷贝/移动 ⇒ 单独存放，惰性创建）。
    /// 【A-5 收尾】诊断读回不用设备级 WaitIdle：录制侧挂 timeline，读取侧只等那一次提交。
    struct CrossThreadFence {
        std::atomic<rhi::RHIFenceHandle> fence{rhi::kInvalidFence};
        std::atomic<u64>                 value{0};
    };
    std::unique_ptr<CrossThreadFence> m_Fence;   // 只有真正录制时才创建
};

} // namespace he::render
