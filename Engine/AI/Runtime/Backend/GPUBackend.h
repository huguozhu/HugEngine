#pragma once

#include "AI/Runtime/Backend/IAIBackend.h"
#include "RHI/RHI.h"

#include <memory>
#include <unordered_map>

// ============================================================
// GPUBackend — GPU 推理后端（基于 RHI compute）
//
// 计算核来源（二选一，见 InferenceRequest）：
//   1. 内置核：按 kernel 名查内部注册表（引擎编译的 shader，如 "tensor_scale"）
//   2. 外部核：customSpirv 携带 SPIR-V 二进制（运行时自定义着色器，即插即用）
// 两者都经「按需构建 + PSO 缓存」路径：首次遇到某 SPIR-V 才建描述符布局与
// 计算管线，之后命中缓存直接 dispatch —— 加算子无需改本文件。
//
// A3.1 约束：描述符绑定全部为 StorageBuffer（inputs 只读在前，outputs 可写在后）；
// push constant 为原始字节块（外部核直接传，内置核由 params 映射）。
// ============================================================

namespace he::ai {

// 内置计算核名（InferenceRequest::kernel 取值）
constexpr const char* kKernelTensorScale = "tensor_scale";   // 逐元素缩放：out = in × scale

/// GPU 张量实现：承载 RHI 缓冲（Storage，可 Map 读写）
class GPUTensor : public IAITensor {
public:
    GPUTensor(AITensorDesc desc, std::unique_ptr<rhi::IRHIBuffer> buffer)
        : m_Desc(desc), m_Buffer(std::move(buffer)) {}

    const AITensorDesc& GetDesc() const { return m_Desc; }
    rhi::IRHIBuffer* GetBuffer() { return m_Buffer.get(); }

private:
    AITensorDesc                     m_Desc;    // 张量描述
    std::unique_ptr<rhi::IRHIBuffer> m_Buffer;  // GPU 缓冲（CPU 可访问）
};

/// GPU 推理后端
class GPUBackend : public IAIBackend {
public:
    explicit GPUBackend(rhi::IRHIDevice* device);
    ~GPUBackend();

    // --- IAIBackend ---
    bool Supports(AIModelFormat fmt) const override;   // A3.1 无模型格式
    Ref<IAIModel> Load(AIModelFormat fmt, Span<const u8> weights,
                       const String& path) override;
    Ref<IAITensor> Allocate(const AITensorDesc& desc) override;
    Ref<IAIInference> Run(InferenceRequest&& req) override;

    // --- CPU ↔ GPU 张量数据交换（A3.1 验证用；神经渲染走 Wrap/Export 零拷贝）---
    bool WriteTensor(IAITensor* t, Span<const float> data, u32 offsetElems = 0);
    bool ReadTensor(IAITensor* t, Span<float> out, u32 offsetElems = 0);

private:
    // 注册内置核（SPIR-V 来自 CompileShaders 生成的嵌入式头）
    void RegisterBuiltinKernel(const char* name, Span<const u32> spirv);

    // 获取（或按需构建并缓存）核的 PSO；返回 false 表示构建失败
    bool AcquireKernel(const String& name, Span<const u32> spirv,
                       u32 pcSize, u32 bindingCount);

    // 执行已缓存的核：绑定张量 → 推参 → dispatch → 提交等待
    Ref<IAIInference> DispatchKernel(rhi::IRHIDevice* device,
                                     const rhi::IRHIPipelineState* pso,
                                     rhi::DescriptorSetHandle set,
                                     Span<const u8> pushConstants,
                                     const std::vector<IAITensor*>& inputs,
                                     const std::vector<IAITensor*>& outputs);

    // 单个核的缓存项（PSO + 描述符资源，按需构建）
    struct KernelEntry {
        rhi::ShaderBytecode cs;                          // shader 字节码
        std::unique_ptr<rhi::IRHIPipelineState> pso;     // 计算管线
        rhi::DescriptorSetLayoutHandle layout = rhi::kInvalidLayout;
        rhi::DescriptorSetHandle       set    = rhi::kInvalidSet;
        u32 pcSize     = 0;                              // push constant 大小
        u32 bindCount  = 0;                              // StorageBuffer 绑定数
    };

    rhi::IRHIDevice* m_Device = nullptr;

    // 内置核表（名 → 核条目；SPIR-V 编译期嵌入）
    std::unordered_map<String, std::unique_ptr<KernelEntry>> m_Builtin;
    // 外部核缓存（SPIR-V 哈希 → 核条目；运行时按需构建）
    std::unordered_map<u64, std::unique_ptr<KernelEntry>> m_External;
};

} // namespace he::ai
