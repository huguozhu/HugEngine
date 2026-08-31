#pragma once

#include "Core/Types.h"
#include "AI/Runtime/Backend/IAIBackend.h"

#include <memory>
#include <vector>
#include <functional>

// ============================================================
// AIDevice.h — 推理硬件接口（IHI）门面
//
// 与 rhi::IRHIDevice 同构，抽象「模型执行」而非「图元绘制」。
// IAIDevice 按模型格式 + 能力选择后端（GPU / CPU / Remote）。
//
// 本文件是 A1 基座的接口层；GPU/CPU 后端在 A3 落地，
// 当前仅 RemoteBackend（LLM）可用，其余能力返回空并告警降级。
// 基础类型（IAIModel/IAITensor/AIModelFormat 等）见 IAIBackend.h。
// ============================================================

// 前向声明 RHI 类型（RHI 的命名空间是 he::rhi，与 RHI/RHI.h 一致）
namespace he::rhi {
class IRHITexture;
class IRHIBuffer;
class IRHIDevice;
} // namespace he::rhi

namespace he::ai {
class InferenceScheduler;   // 前向声明（工厂参数用）
} // namespace he::ai

namespace he::ai {

// AI 设备能力 —— 在 rhi::DeviceCaps 之上扩展 AI 专属能力
struct AIDeviceCaps {
    bool supportsGPU       = false;   // GPUBackend 可用（张量 + compute 推理）
    bool supportsCPU       = false;   // ONNX Runtime / GGUF 可用
    bool supportsRemoteLLM = false;   // 远程大模型可用
    bool supportsNPU       = false;   // NPU 直通（预留）
    u32  maxModelSizeMB    = 0;       // 单模型内存上限
};

// ★ 推理设备 —— 与 rhi::IRHIDevice 同构
class IAIDevice {
public:
    virtual ~IAIDevice() = default;
    virtual AIDeviceCaps GetCaps() const = 0;

    // 模型生命周期（按格式分派后端；无可用后端返回 nullptr）
    virtual Ref<IAIModel> LoadModel(AIModelFormat fmt,
                                    Span<const u8> weights,
                                    const String& path) = 0;

    // 张量创建（GPU/CPU 后端未就绪时返回 nullptr）
    virtual Ref<IAITensor> CreateTensor(const AITensorDesc& desc) = 0;

    // 提交推理（异步，返回句柄）
    virtual Ref<IAIInference> Submit(InferenceRequest&& req) = 0;

    // ★ 与渲染零拷贝互操作（神经渲染的关键；后端不支持则返回 nullptr）
    virtual Ref<IAITensor> WrapRHITexture(rhi::IRHITexture* tex) = 0;   // 读渲染纹理（he::rhi）
    virtual Ref<IAITensor> WrapRHIBuffer(rhi::IRHIBuffer* buf)  = 0;    // 读 bindless 缓冲
    virtual rhi::IRHIBuffer* ExportBuffer(IAITensor* t)          = 0;   // 写回渲染缓冲

    // --- LLM 便捷方法（委托 RemoteBackend；无则返回空串）---
    virtual String Chat(const String& systemPrompt, const String& userPrompt) = 0;
    // 流式对话：token 经调度器按序投递到主线程回调
    virtual void ChatStream(const String& systemPrompt, const String& userPrompt,
                            std::function<void(const String&)> onToken) = 0;

    // --- 张量数据交换（CPU ↔ GPU，A3.1 验证用；神经渲染走 Wrap/Export 零拷贝）---
    /// 上传数据到张量（FP32 元素）。失败返回 false。
    virtual bool WriteTensor(IAITensor* t, Span<const float> data, u32 offsetElems = 0) = 0;
    /// 从张量读取数据（FP32 元素）。失败返回 false。
    virtual bool ReadTensor(IAITensor* t, Span<float> out, u32 offsetElems = 0) = 0;
};

// 工厂：创建默认 AI 设备（注册 RemoteBackend + GPUBackend）。
// API key 从环境变量 DEEPSEEK_API_KEY 读取（远程 LLM）；
// rhiDevice 非空时注册 GPU 后端（张量分配 + compute 推理）。
// scheduler 用于流式回调投递到主线程（可为空 = 直接同步回调）。
std::unique_ptr<IAIDevice> CreateAIDevice(InferenceScheduler* scheduler = nullptr,
                                          rhi::IRHIDevice* rhiDevice = nullptr);

} // namespace he::ai
