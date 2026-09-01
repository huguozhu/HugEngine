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
constexpr const char* kKernelTensorScale = "tensor_scale";     // 逐元素缩放：out = in × scale
constexpr const char* kKernelTextureSample = "texture_sample"; // 纹理采样：out = tex(uv) × brightness
constexpr const char* kKernelTextureGen  = "texture_gen";      // GPU 生成纹理：solid/gradient/checker/noise

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

/// 纹理张量：包装渲染纹理（零拷贝视图，不拥有资源）。
/// 由 WrapRHITexture 创建，作为 compute 核的 CombinedImageSampler 输入。
class GPUTextureTensor : public IAITensor {
public:
    GPUTextureTensor(rhi::IRHITexture* tex, rhi::IRHISampler* sampler,
                     u32 width, u32 height)
        : m_Texture(tex), m_Sampler(sampler), m_Width(width), m_Height(height) {}

    rhi::IRHITexture* GetTexture() const { return m_Texture; }  // 渲染纹理（不拥有）
    rhi::IRHISampler* GetSampler() const { return m_Sampler; }  // 采样器（不拥有）
    u32 GetWidth()  const { return m_Width; }
    u32 GetHeight() const { return m_Height; }

private:
    rhi::IRHITexture* m_Texture;
    rhi::IRHISampler* m_Sampler;
    u32 m_Width  = 0;
    u32 m_Height = 0;
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

    // --- 与渲染零拷贝互操作（A3.2a）---
    /// 把渲染纹理包装为纹理张量（不拷贝；采样器由本后端默认提供，线性 + Clamp）
    Ref<IAITensor> WrapTexture(rhi::IRHITexture* tex);
    /// 导出推理输出缓冲的 GPU 句柄（供渲染管线直接绑定；非 GPUTensor 返回 nullptr）
    rhi::IRHIBuffer* ExportBuffer(IAITensor* t);

private:
    // 注册内置核（SPIR-V 来自 CompileShaders 生成的嵌入式头；bindings 描述描述符布局）
    void RegisterBuiltinKernel(const char* name, Span<const u32> spirv,
                               std::vector<rhi::DescriptorType> bindings,
                               u32 pcSize, u32 bindingCount);

    // 获取（或按需构建并缓存）核的 PSO；返回 false 表示构建失败
    bool AcquireKernel(const String& name, Span<const u32> spirv,
                       u32 pcSize, u32 bindingCount);

    // 执行已缓存的核：绑定张量/纹理 → 推参 → dispatch → 提交等待
    // @param kernelName 核名（用于 debug label，如 "tensor_scale"）
    // @param dispatchCount 线程数（0 = 默认按输出张量元素数推导）
    Ref<IAIInference> DispatchKernel(rhi::IRHIDevice* device,
                                     const rhi::IRHIPipelineState* pso,
                                     rhi::DescriptorSetHandle set,
                                     Span<const u8> pushConstants,
                                     const std::vector<IAITensor*>& inputs,
                                     const std::vector<IAITensor*>& textureInputs,
                                     const std::vector<IAITensor*>& outputs,
                                     u32 dispatchCount = 0,
                                     const String& kernelName = "kernel");

    // 单个核的缓存项（PSO + 描述符资源，按需构建）
    struct KernelEntry {
        rhi::ShaderBytecode cs;                          // shader 字节码
        std::unique_ptr<rhi::IRHIPipelineState> pso;     // 计算管线
        rhi::DescriptorSetLayoutHandle layout = rhi::kInvalidLayout;
        rhi::DescriptorSetHandle       set    = rhi::kInvalidSet;
        u32 pcSize     = 0;                              // push constant 大小
        u32 bindCount  = 0;                              // 描述符绑定总数
        std::vector<rhi::DescriptorType> bindings;       // 各 binding 的类型（布局描述）
    };

    rhi::IRHIDevice* m_Device = nullptr;
    std::unique_ptr<rhi::IRHISampler> m_DefaultSampler;  // WrapRHITexture 用（线性 + Clamp）

    // 内置核表（名 → 核条目；SPIR-V 编译期嵌入）
    std::unordered_map<String, std::unique_ptr<KernelEntry>> m_Builtin;
    // 外部核缓存（SPIR-V 哈希 → 核条目；运行时按需构建）
    std::unordered_map<u64, std::unique_ptr<KernelEntry>> m_External;
};

} // namespace he::ai
