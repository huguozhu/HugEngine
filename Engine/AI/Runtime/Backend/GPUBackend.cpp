#include "AI/Runtime/Backend/GPUBackend.h"

#include "AITensorScale.comp.spv.h"    // k_AITensorScale_comp_spv（内置：逐元素缩放）
#include "AITextureSample.comp.spv.h"  // k_AITextureSample_comp_spv（内置：纹理采样）
#include "Core/Log.h"

#include <cstring>
#include <utility>

namespace he::ai {

namespace {

// 从内置核参数表读取浮点参数（缺省/非法返回 def）
float GetParamFloat(const std::vector<std::pair<String, String>>& params,
                    const char* key, float def) {
    for (auto& [k, v] : params) {
        if (k == key) {
            try { return std::stof(v); }
            catch (const std::exception&) { return def; }
        }
    }
    return def;
}

// SPIR-V 二进制哈希（外部核缓存键）
u64 HashSpirv(Span<const u32> spirv) {
    u64 h = 14695981039346656037ull;
    for (u32 w : spirv) {
        for (int i = 0; i < 4; ++i) {
            h ^= (w >> (i * 8)) & 0xFF;
            h *= 1099511628211ull;
        }
    }
    return h;
}

// 已完成推理句柄：Run 采用同步执行（dispatch → submit → wait），句柄恒为完成态
class GPUInference : public IAIInference {
public:
    bool IsDone() const override { return true; }
    void Wait() override {}
    void Cancel() override {}
    void SetStreamCallback(std::function<void(const String&)>) override {}
};

} // namespace

GPUBackend::GPUBackend(rhi::IRHIDevice* device) : m_Device(device) {
    if (!m_Device) return;

    // 默认采样器（WrapRHITexture 用：线性过滤 + 边缘 Clamp）
    rhi::SamplerDesc sDesc;
    sDesc.minFilter = sDesc.magFilter = rhi::FilterMode::Linear;
    sDesc.addressU = sDesc.addressV = sDesc.addressW = rhi::AddressMode::ClampToEdge;
    m_DefaultSampler = m_Device->CreateSampler(sDesc);

    // 注册内置核
    RegisterBuiltinKernel(kKernelTensorScale,
                          Span<const u32>(k_AITensorScale_comp_spv),
                          {rhi::DescriptorType::StorageBuffer,     // binding 0: 输入
                           rhi::DescriptorType::StorageBuffer},    // binding 1: 输出
                          sizeof(u32) + sizeof(float), 2);         // 推参 {count, scale}
    RegisterBuiltinKernel(kKernelTextureSample,
                          Span<const u32>(k_AITextureSample_comp_spv),
                          {rhi::DescriptorType::CombinedImageSampler,  // binding 0: 纹理
                           rhi::DescriptorType::StorageBuffer},       // binding 1: 输出
                          sizeof(u32) * 2 + sizeof(float) * 2, 2);    // 推参 {w,h,brightness,pad}
    HE_CORE_INFO("[GPUBackend] 已注册内置核: {}, {}",
                 kKernelTensorScale, kKernelTextureSample);
}

GPUBackend::~GPUBackend() = default;

bool GPUBackend::Supports(AIModelFormat) const {
    // A3.2 阶段无模型格式支持（Load 均返回 nullptr）；真实模型格式后续接入
    return false;
}

Ref<IAIModel> GPUBackend::Load(AIModelFormat, Span<const u8>, const String&) {
    return nullptr;
}

Ref<IAITensor> GPUBackend::Allocate(const AITensorDesc& desc) {
    if (!m_Device) return nullptr;
    if (desc.dtype != AIDataType::FP32) {
        HE_CORE_WARN("[GPUBackend] A3.x 仅支持 FP32 张量，拒绝分配");
        return nullptr;
    }

    // 张量 = GPU Storage 缓冲（CPU 可访问，供上传/读回验证）
    rhi::BufferDesc bd;
    bd.size      = desc.elementCount * sizeof(float);
    bd.usage     = rhi::BufferUsage::Storage;
    bd.cpuAccess = true;
    auto buf = m_Device->CreateBuffer(bd);
    if (!buf) {
        HE_CORE_ERROR("[GPUBackend] 张量缓冲创建失败");
        return nullptr;
    }
    return std::make_shared<GPUTensor>(desc, std::move(buf));
}

Ref<IAIInference> GPUBackend::Run(InferenceRequest&& req) {
    if (!m_Device) return nullptr;
    if (req.inputs.empty() && req.textureInputs.empty()) {
        HE_CORE_WARN("[GPUBackend] Run 需要至少 1 个输入（缓冲或纹理）");
        return nullptr;
    }
    if (req.outputs.empty()) {
        HE_CORE_WARN("[GPUBackend] Run 需要至少 1 个输出张量");
        return nullptr;
    }

    // ============================================================
    // 计算核解析：外部 SPIR-V 优先，否则查内置核名
    // ============================================================
    String     name;
    Span<const u32> spirv = req.customSpirv;
    u32        pcSize = req.pushConstantSize;
    KernelEntry* entry = nullptr;

    if (!spirv.empty()) {
        // ── 外部核：按 SPIR-V 哈希查缓存 / 按需构建 ──
        const u64 hash = HashSpirv(spirv);
        name = "external#" + std::to_string(hash);
        auto it = m_External.find(hash);
        if (it == m_External.end()) {
            auto e = std::make_unique<KernelEntry>();
            e->cs.spirv.assign(spirv.begin(), spirv.end());
            e->cs.stage = rhi::ShaderStage::Compute;
            e->cs.entryPoint = "main";
            e->pcSize    = pcSize;
            e->bindCount = (u32)(req.inputs.size() + req.textureInputs.size() + req.outputs.size());
            // 外部核 MVP：全部 StorageBuffer 绑定
            for (u32 b = 0; b < e->bindCount; ++b)
                e->bindings.push_back(rhi::DescriptorType::StorageBuffer);
            it = m_External.emplace(hash, std::move(e)).first;
        }
        entry = it->second.get();
    } else {
        // ── 内置核：按名字查注册表 ──
        auto it = m_Builtin.find(req.kernel);
        if (it == m_Builtin.end()) {
            HE_CORE_WARN("[GPUBackend] 未知内置核: '{}'（可用: {}, {}）",
                         req.kernel, kKernelTensorScale, kKernelTextureSample);
            return nullptr;
        }
        entry = it->second.get();
        name  = req.kernel;
        pcSize = entry->pcSize;
    }

    // 校验请求的绑定组合与核的布局一致
    const u32 bindCount = (u32)(req.inputs.size() + req.textureInputs.size() + req.outputs.size());
    if (bindCount != entry->bindCount) {
        HE_CORE_WARN("[GPUBackend] 核 '{}' 期望 {} 个绑定，请求给了 {} 个",
                     name, entry->bindCount, bindCount);
        return nullptr;
    }

    // 首次使用该核：构建描述符布局 + 计算管线（PSO 缓存）
    if (!entry->pso) {
        if (!AcquireKernel(name, spirv, pcSize, entry->bindCount)) return nullptr;
    }

    // ============================================================
    // 推参：外部核直接用请求字节；内置核由 params + 张量元数据映射
    // ============================================================
    std::vector<u8> pcBytes;
    if (spirv.empty()) {
        if (name == kKernelTensorScale) {
            // {u32 count, float scale}
            const u32 count = static_cast<GPUTensor*>(req.inputs[0])->GetDesc().elementCount;
            const float scale = GetParamFloat(req.params, "scale", 1.0f);
            struct ScalePC { u32 count; float scale; } pc = { count, scale };
            pcBytes.assign(reinterpret_cast<u8*>(&pc),
                           reinterpret_cast<u8*>(&pc) + sizeof(pc));
        } else if (name == kKernelTextureSample) {
            // {u32 width, u32 height, float brightness, float pad}
            auto* tex = static_cast<GPUTextureTensor*>(req.textureInputs[0]);
            struct SamplePC { u32 w, h; float brightness, pad; } pc = {
                tex->GetWidth(), tex->GetHeight(),
                GetParamFloat(req.params, "brightness", 1.0f), 0.0f };
            pcBytes.assign(reinterpret_cast<u8*>(&pc),
                           reinterpret_cast<u8*>(&pc) + sizeof(pc));
        } else {
            HE_CORE_WARN("[GPUBackend] 内置核 '{}' 无推参映射", name);
            return nullptr;
        }
    } else {
        pcBytes = req.pushConstants;
    }

    // ============================================================
    // 执行（同步：dispatch → submit → wait）
    // ============================================================
    auto result = DispatchKernel(m_Device, entry->pso.get(), entry->set, pcBytes,
                                 req.inputs, req.textureInputs, req.outputs);
    if (result)
        HE_CORE_INFO("[GPUBackend] 核 '{}' 执行完成（{} 缓冲输入 / {} 纹理输入 / {} 输出）",
                     name, req.inputs.size(), req.textureInputs.size(), req.outputs.size());
    return result;
}

bool GPUBackend::WriteTensor(IAITensor* t, Span<const float> data, u32 offsetElems) {
    auto* gpu = dynamic_cast<GPUTensor*>(t);
    if (!gpu) return false;
    auto* buf = gpu->GetBuffer();
    float* p = static_cast<float*>(buf->Map());
    if (!p) return false;
    std::memcpy(p + offsetElems, data.data(), data.size() * sizeof(float));
    buf->Unmap();
    return true;
}

bool GPUBackend::ReadTensor(IAITensor* t, Span<float> out, u32 offsetElems) {
    auto* gpu = dynamic_cast<GPUTensor*>(t);
    if (!gpu) return false;
    auto* buf = gpu->GetBuffer();
    const float* p = static_cast<const float*>(buf->Map());
    if (!p) return false;
    std::memcpy(out.data(), p + offsetElems, out.size() * sizeof(float));
    buf->Unmap();
    return true;
}

Ref<IAITensor> GPUBackend::WrapTexture(rhi::IRHITexture* tex) {
    if (!tex || !m_DefaultSampler) return nullptr;
    // 零拷贝：纹理本身作输入，仅记录尺寸与采样器
    return std::make_shared<GPUTextureTensor>(tex, m_DefaultSampler.get(),
                                              tex->GetWidth(), tex->GetHeight());
}

rhi::IRHIBuffer* GPUBackend::ExportBuffer(IAITensor* t) {
    auto* gpu = dynamic_cast<GPUTensor*>(t);
    return gpu ? gpu->GetBuffer() : nullptr;   // 非 GPU 张量返回 nullptr
}

// ============================================================
// 内部实现
// ============================================================

void GPUBackend::RegisterBuiltinKernel(const char* name, Span<const u32> spirv,
                                       std::vector<rhi::DescriptorType> bindings,
                                       u32 pcSize, u32 bindingCount) {
    auto e = std::make_unique<KernelEntry>();
    e->cs.spirv.assign(spirv.begin(), spirv.end());
    e->cs.stage      = rhi::ShaderStage::Compute;
    e->cs.entryPoint = "main";
    e->pcSize    = pcSize;
    e->bindCount = bindingCount;
    e->bindings  = std::move(bindings);
    m_Builtin.emplace(name, std::move(e));
}

bool GPUBackend::AcquireKernel(const String& name, Span<const u32> spirv,
                               u32 pcSize, u32 bindingCount) {
    // 按名字/哈希找回条目（调用方已保证存在）
    KernelEntry* entry = nullptr;
    if (!spirv.empty()) {
        auto it = m_External.find(HashSpirv(spirv));
        if (it != m_External.end()) entry = it->second.get();
    } else {
        auto it = m_Builtin.find(name);
        if (it != m_Builtin.end()) entry = it->second.get();
    }
    if (!entry || !m_Device) return false;

    // 1. 描述符布局：按 entry->bindings 的类型列表构建（Compute 阶段）
    rhi::DescriptorSetLayoutDesc layoutDesc;
    for (u32 b = 0; b < bindingCount; ++b) {
        const rhi::DescriptorType type = (b < entry->bindings.size())
            ? entry->bindings[b] : rhi::DescriptorType::StorageBuffer;
        layoutDesc.bindings.push_back({b, type, 1, rhi::kStageMaskCompute});
    }
    entry->layout = m_Device->CreateDescriptorSetLayout(layoutDesc);
    entry->set    = m_Device->AllocateDescriptorSet(entry->layout);

    // 2. push constant 范围
    rhi::PushConstantRange pcRange;
    pcRange.stageMask = rhi::kStageMaskCompute;
    pcRange.offset    = 0;
    pcRange.size      = pcSize;

    // 3. 计算管线
    rhi::PipelineStateDesc psoDesc;
    psoDesc.bindPoint            = rhi::PipelineBindPoint::Compute;
    psoDesc.computeShader        = &entry->cs;
    psoDesc.pushConstantRanges   = {pcRange};
    psoDesc.descriptorSetLayouts = {entry->layout};
    psoDesc.debugName            = name.c_str();
    entry->pso = m_Device->CreatePipelineState(psoDesc);
    if (!entry->pso) {
        HE_CORE_ERROR("[GPUBackend] 核 '{}' 计算管线创建失败", name);
        return false;
    }
    HE_CORE_INFO("[GPUBackend] 核 '{}' PSO 已构建（{} 绑定，推参 {}B）",
                 name, bindingCount, pcSize);
    return true;
}

Ref<IAIInference> GPUBackend::DispatchKernel(rhi::IRHIDevice* device,
                                             const rhi::IRHIPipelineState* pso,
                                             rhi::DescriptorSetHandle set,
                                             Span<const u8> pushConstants,
                                             const std::vector<IAITensor*>& inputs,
                                             const std::vector<IAITensor*>& textureInputs,
                                             const std::vector<IAITensor*>& outputs) {
    u32 binding = 0;

    // 1a. 绑定缓冲输入（StorageBuffer）
    for (auto* t : inputs) {
        auto* gpu = dynamic_cast<GPUTensor*>(t);
        if (!gpu) return nullptr;
        device->UpdateDescriptorSet(set, binding++, rhi::DescriptorType::StorageBuffer,
                                    gpu->GetBuffer());
    }
    // 1b. 绑定纹理输入（CombinedImageSampler）
    for (auto* t : textureInputs) {
        auto* tex = dynamic_cast<GPUTextureTensor*>(t);
        if (!tex) return nullptr;
        device->UpdateDescriptorSet(set, binding++, rhi::DescriptorType::CombinedImageSampler,
                                    tex->GetTexture(), tex->GetSampler());
    }
    // 1c. 绑定输出缓冲（StorageBuffer）
    for (auto* t : outputs) {
        auto* gpu = dynamic_cast<GPUTensor*>(t);
        if (!gpu) return nullptr;
        device->UpdateDescriptorSet(set, binding++, rhi::DescriptorType::StorageBuffer,
                                    gpu->GetBuffer());
    }

    // 2. dispatch 组数：按输出张量元素数（MVP 约定输出与输入同规模）
    u32 count = 0;
    if (!outputs.empty()) {
        if (auto* gpu = dynamic_cast<GPUTensor*>(outputs[0]))
            count = gpu->GetDesc().elementCount;
    }
    // 纹理采样核：按纹理宽高分行列 dispatch（8×8 线程组）
    u32 groupsX = 1, groupsY = 1, groupsZ = 1;
    if (!textureInputs.empty()) {
        if (auto* tex = dynamic_cast<GPUTextureTensor*>(textureInputs[0])) {
            groupsX = (tex->GetWidth()  + 7) / 8;
            groupsY = (tex->GetHeight() + 7) / 8;
        }
    } else {
        groupsX = (count + 63) / 64;
    }

    // 3. 录制 dispatch
    auto cmdList = device->CreateCommandList();
    cmdList->Begin();
    cmdList->SetPipeline(const_cast<rhi::IRHIPipelineState*>(pso));
    cmdList->BindDescriptorSet(rhi::kDescSetPerFrame, set);
    cmdList->SetPushConstants(0, (u32)pushConstants.size(), pushConstants.data());
    cmdList->SetDrawDebugLabel("AI Kernel");
    cmdList->Dispatch(groupsX, groupsY, groupsZ);
    cmdList->End();

    // 4. 提交并等待（A3.x 同步执行；后续接入异步调度器）
    device->Submit(cmdList.get());
    device->WaitIdle();
    return std::make_shared<GPUInference>();
}

} // namespace he::ai
