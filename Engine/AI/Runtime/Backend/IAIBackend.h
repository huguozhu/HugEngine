#pragma once

#include "Core/Types.h"

#include <memory>
#include <vector>
#include <functional>

// ============================================================
// IAIBackend.h — AI 推理后端接口（GPU / CPU / Remote）
//
// 同时承载后端共享的基础类型：
//   IAIModel / IAITensor（句柄）、AIModelFormat、AIDataType、
//   AITensorDesc、InferenceRequest
// 避免 AIDevice.h 与 IAIBackend.h 循环包含。
// ============================================================

namespace he::ai {

// 引用类型别名（文档用 Ref<T>；库内统一为 shared_ptr）
template<typename T>
using Ref = std::shared_ptr<T>;

// 模型句柄 —— 编译后的模型资源（具体实现由后端提供）
class IAIModel {
public:
    virtual ~IAIModel() = default;
};

// 张量句柄 —— 张量资源（bindless 可见，可与渲染共享）
class IAITensor {
public:
    virtual ~IAITensor() = default;
};

// 模型格式
enum class AIModelFormat { ONNX, GGUF, SafeTensors, RTXNS_Slang };

// 张量数据类型
enum class AIDataType { FP32, FP16, BF16, FP8, INT8, INT4 };

// 张量描述 —— 镜像 rhi::BufferDesc/TextureDesc
struct AITensorDesc {
    u32        elementCount = 0;
    AIDataType dtype        = AIDataType::FP32;
    bool       bindlessVisible = true;   // 与渲染共享（零拷贝）
};

// 推理请求
struct InferenceRequest {
    IAIModel*               model = nullptr;      // 编译后的模型句柄（真实模型推理用；预置核可空）

    // --- 计算核选择（二选一）---
    String                  kernel;               // 内置核名（如 "tensor_scale"；customSpirv 非空时忽略）
    Span<const u32>         customSpirv = {};     // 外部核 SPIR-V 二进制（运行时提供的自定义着色器，优先于 kernel）

    // --- 外部核描述（customSpirv 非空时使用）---
    u32                     pushConstantSize = 0;   // push constant 布局大小（字节）
    std::vector<u8>         pushConstants;          // push constant 数据（原始字节，与 shader 布局对齐）

    // --- 内置核便利参数（kernel 非空时使用）---
    std::vector<std::pair<String, String>> params;  // KV 文本，如 {"scale","2.5"}；后端按核名解释

    // --- 张量与调度 ---
    // 描述符绑定约定（MVP）：binding 0..n-1 = inputs（只读），binding n.. = outputs（可写），全部 StorageBuffer
    std::vector<IAITensor*> inputs;               // 输入张量（按绑定顺序）
    std::vector<IAITensor*> outputs;              // 输出张量（按绑定顺序）
    u32                     batchSize = 1;
};

// 推理句柄 —— 支持阻塞等待 + 流式回调（LLM 必需）
class IAIInference {
public:
    virtual ~IAIInference() = default;
    virtual bool IsDone() const = 0;
    virtual void Wait() = 0;
    virtual void Cancel() = 0;
    // 流式：每个 token / 中间结果回调（LLM 必需）
    virtual void SetStreamCallback(std::function<void(const String&)> cb) = 0;
};

// ============================================================
// 后端接口 —— IAIDevice 按模型格式 + 能力选择后端
// ============================================================

class IAIBackend {
public:
    virtual ~IAIBackend() = default;
    // 该后端是否支持指定模型格式
    virtual bool Supports(AIModelFormat fmt) const = 0;
    // 加载模型（编译权重）
    virtual Ref<IAIModel> Load(AIModelFormat fmt, Span<const u8> weights, const String& path) = 0;
    // 分配张量
    virtual Ref<IAITensor> Allocate(const AITensorDesc& desc) = 0;
    // 执行推理（异步，返回句柄）
    virtual Ref<IAIInference> Run(InferenceRequest&& req) = 0;
};

} // namespace he::ai
