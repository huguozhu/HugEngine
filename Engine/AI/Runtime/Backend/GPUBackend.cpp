#include "AI/Runtime/Backend/GPUBackend.h"

#include "AITensorScale.comp.spv.h"   // k_AITensorScale_comp_spv（由 CompileShaders 生成）
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
    // 注册内置核：tensor_scale（AITensorScale.comp.slang 的嵌入 SPIR-V）
    RegisterBuiltinKernel(kKernelTensorScale, Span<const u32>(k_AITensorScale_comp_spv));
    HE_CORE_INFO("[GPUBackend] 已注册内置核: {}（共 {} 字 SPIR-V）",
                 kKernelTensorScale, std::size(k_AITensorScale_comp_spv));
}

GPUBackend::~GPUBackend() = default;

bool GPUBackend::Supports(AIModelFormat) const {
    // A3.1 无模型格式支持（Load 均返回 nullptr）；真实模型格式后续接入
    return false;
}

Ref<IAIModel> GPUBackend::Load(AIModelFormat, Span<const u8>, const String&) {
    return nullptr;
}

Ref<IAITensor> GPUBackend::Allocate(const AITensorDesc& desc) {
    if (!m_Device) return nullptr;
    if (desc.dtype != AIDataType::FP32) {
        HE_CORE_WARN("[GPUBackend] A3.1 仅支持 FP32 张量，拒绝分配");
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
    if (req.inputs.empty() || req.outputs.empty()) {
        HE_CORE_WARN("[GPUBackend] Run 需要至少 1 输入 + 1 输出张量");
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
            e->bindCount = (u32)(req.inputs.size() + req.outputs.size());
            it = m_External.emplace(hash, std::move(e)).first;
        }
        entry = it->second.get();
    } else {
        // ── 内置核：按名字查注册表 ──
        auto it = m_Builtin.find(req.kernel);
        if (it == m_Builtin.end()) {
            HE_CORE_WARN("[GPUBackend] 未知内置核: '{}'（可用: {}）",
                         req.kernel, kKernelTensorScale);
            return nullptr;
        }
        entry = it->second.get();
        name  = req.kernel;
        pcSize = entry->pcSize;
    }

    // 首次使用该核：构建描述符布局 + 计算管线（PSO 缓存）
    const u32 bindCount = (u32)(req.inputs.size() + req.outputs.size());
    if (!entry->pso) {
        if (entry->bindCount == 0) entry->bindCount = bindCount;  // 内置核首次记录绑定数
        if (!AcquireKernel(name, spirv, pcSize, entry->bindCount)) return nullptr;
    }

    // ============================================================
    // 推参：外部核直接用请求字节；内置核由 params 映射为字节
    // ============================================================
    std::vector<u8> pcBytes;
    if (spirv.empty()) {
        // 内置 tensor_scale：{u32 count, float scale}
        const u32 count = static_cast<GPUTensor*>(req.inputs[0])->GetDesc().elementCount;
        const float scale = GetParamFloat(req.params, "scale", 1.0f);
        struct ScalePC { u32 count; float scale; } pc = { count, scale };
        pcBytes.assign(reinterpret_cast<u8*>(&pc),
                       reinterpret_cast<u8*>(&pc) + sizeof(pc));
    } else {
        pcBytes = req.pushConstants;
    }

    // ============================================================
    // 执行（同步：dispatch → submit → wait）
    // ============================================================
    auto result = DispatchKernel(m_Device, entry->pso.get(), entry->set, pcBytes,
                                 req.inputs, req.outputs);
    if (result)
        HE_CORE_INFO("[GPUBackend] 核 '{}' 执行完成（{} 输入 / {} 输出）",
                     name, req.inputs.size(), req.outputs.size());
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

// ============================================================
// 内部实现
// ============================================================

void GPUBackend::RegisterBuiltinKernel(const char* name, Span<const u32> spirv) {
    auto e = std::make_unique<KernelEntry>();
    e->cs.spirv.assign(spirv.begin(), spirv.end());
    e->cs.stage      = rhi::ShaderStage::Compute;
    e->cs.entryPoint = "main";
    e->pcSize    = sizeof(u32) + sizeof(float);   // tensor_scale 推参：count + scale
    e->bindCount = 2;                             // 1 输入 + 1 输出
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

    // 1. 描述符布局：binding 0..n-1 全为 StorageBuffer（Compute 阶段）
    rhi::DescriptorSetLayoutDesc layoutDesc;
    for (u32 b = 0; b < bindingCount; ++b)
        layoutDesc.bindings.push_back({b, rhi::DescriptorType::StorageBuffer,
                                       1, rhi::kStageMaskCompute});
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
    HE_CORE_INFO("[GPUBackend] 核 '{}' PSO 已构建（{} 个 StorageBuffer 绑定，推参 {}B）",
                 name, bindingCount, pcSize);
    return true;
}

Ref<IAIInference> GPUBackend::DispatchKernel(rhi::IRHIDevice* device,
                                             const rhi::IRHIPipelineState* pso,
                                             rhi::DescriptorSetHandle set,
                                             Span<const u8> pushConstants,
                                             const std::vector<IAITensor*>& inputs,
                                             const std::vector<IAITensor*>& outputs) {
    // 1. 绑定描述符：inputs 依次绑定 0..n-1，outputs 依次绑定 n..
    for (u32 i = 0; i < inputs.size(); ++i) {
        auto* gpu = dynamic_cast<GPUTensor*>(inputs[i]);
        if (!gpu) return nullptr;
        device->UpdateDescriptorSet(set, i, rhi::DescriptorType::StorageBuffer,
                                    gpu->GetBuffer());
    }
    for (u32 i = 0; i < outputs.size(); ++i) {
        auto* gpu = dynamic_cast<GPUTensor*>(outputs[i]);
        if (!gpu) return nullptr;
        device->UpdateDescriptorSet(set, (u32)(inputs.size() + i),
                                    rhi::DescriptorType::StorageBuffer,
                                    gpu->GetBuffer());
    }

    // 2. 元素数 = 输入张量元素数（dispatch 组数 = ceil(count / 64)）
    auto* in = dynamic_cast<GPUTensor*>(inputs[0]);
    const u32 count = in ? in->GetDesc().elementCount : 0;
    const u32 groups = (count + 63) / 64;

    // 3. 录制 dispatch
    auto cmdList = device->CreateCommandList();
    cmdList->Begin();
    cmdList->SetPipeline(const_cast<rhi::IRHIPipelineState*>(pso));
    cmdList->BindDescriptorSet(rhi::kDescSetPerFrame, set);
    cmdList->SetPushConstants(0, (u32)pushConstants.size(), pushConstants.data());
    cmdList->SetDrawDebugLabel("AI Kernel");
    cmdList->Dispatch(groups, 1, 1);
    cmdList->End();

    // 4. 提交并等待（A3.1 同步执行；后续接入异步调度器）
    device->Submit(cmdList.get());
    device->WaitIdle();
    return std::make_shared<GPUInference>();
}

} // namespace he::ai
