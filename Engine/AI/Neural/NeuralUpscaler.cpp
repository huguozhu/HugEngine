#include "AI/Neural/NeuralUpscaler.h"

#include "AI/Runtime/AIDevice.h"
#include "AI/Runtime/Backend/GPUBackend.h"
#include "RHI/RHI.h"
#include "Core/Log.h"

#include <chrono>
#include <vector>

namespace he::render {

NeuralUpscaler::NeuralUpscaler(he::ai::IAIDevice* aiDevice) : m_AI(aiDevice) {}

NeuralUpscaler::~NeuralUpscaler() {
    Shutdown();
}

bool NeuralUpscaler::Initialize(rhi::IRHIDevice* device, u32 width, u32 height) {
    m_Device = device;
    m_Width  = width;
    m_Height = height;
    if (!m_Device || !m_AI) {
        HE_CORE_WARN("[NeuralUpscaler] 设备或 AI 底座不可用，初始化失败");
        return false;
    }
    if (!m_AI->GetCaps().supportsGPU) {
        HE_CORE_WARN("[NeuralUpscaler] GPU 推理后端不可用，子系统禁用");
        return false;
    }

    // 1. 输入纹理：程序化渐变（模拟"渲染输出"，真实场景接管线帧缓冲）
    m_TexSize = 32;
    std::vector<u8> texData(m_TexSize * m_TexSize * 4);
    for (u32 y = 0; y < m_TexSize; ++y)
        for (u32 x = 0; x < m_TexSize; ++x) {
            u8* p = &texData[(y * m_TexSize + x) * 4];
            p[0] = (u8)(x * 255 / (m_TexSize - 1));
            p[1] = (u8)(y * 255 / (m_TexSize - 1));
            p[2] = 96;
            p[3] = 255;
        }
    rhi::TextureDesc tDesc;
    tDesc.format = rhi::Format::RGBA8_UNORM;
    tDesc.width  = m_TexSize;
    tDesc.height = m_TexSize;
    tDesc.mipLevels = 1;
    tDesc.usage = rhi::TextureUsage::ShaderResource | rhi::TextureUsage::TransferDst;
    tDesc.initialData = texData.data();
    m_InputTex = m_Device->CreateTexture(tDesc);

    // 2. 输出缓冲张量：m_TexSize² 个 float4
    he::ai::AITensorDesc desc;
    desc.elementCount = m_TexSize * m_TexSize * 4;
    desc.dtype        = he::ai::AIDataType::FP32;
    m_OutputTensor = m_AI->CreateTensor(desc);
    if (!m_InputTex || !m_OutputTensor) {
        HE_CORE_ERROR("[NeuralUpscaler] 资源创建失败");
        return false;
    }

    m_Initialized = true;
    HE_CORE_INFO("[NeuralUpscaler] 初始化完成（{}×{} 输入，经 IAIDevice 推理）", m_TexSize, m_TexSize);
    return true;
}

void NeuralUpscaler::Shutdown() {
    m_OutputTensor.reset();
    m_InputTex.reset();
    m_Initialized = false;
}

void NeuralUpscaler::Update(const SubsystemContext& ctx) {
    (void)ctx;
    if (!m_Initialized || !m_Enabled) return;
    // 每帧执行一次神经推理（CPU 侧触发；内部同步等待完成）
    RunInference();
}

void NeuralUpscaler::Render(rhi::IRHICommandList* cmdList) {
    (void)cmdList;
    // A3.2b：推理在 Update 内同步完成，无额外 GPU 录制；
    // 真实超分在此处把推理结果写入渲染目标（A3.2c）
}

void NeuralUpscaler::Bind(rhi::IRHICommandList* cmdList) const {
    (void)cmdList;   // 本子系统输出仅用于统计展示，无需绑定
}

void NeuralUpscaler::OnResize(u32 width, u32 height) {
    m_Width  = width;
    m_Height = height;
    // 输入纹理为固定程序化内容，无需重建
}

// ============================================================
// 每帧推理：Wrap 纹理 → Submit(texture_sample) → 读回统计
// ============================================================

void NeuralUpscaler::RunInference() {
    // 零拷贝包装输入纹理为 AI 张量（不拷贝像素）
    auto texTensor = m_AI->WrapRHITexture(m_InputTex.get());
    if (!texTensor) return;

    he::ai::InferenceRequest req;
    req.kernel = he::ai::kKernelTextureSample;
    req.params = {{"brightness", "1.0"}};      // 采样系数 1.0（保持原亮度）
    req.textureInputs.push_back(texTensor.get());
    req.outputs.push_back(m_OutputTensor.get());

    auto t0 = std::chrono::high_resolution_clock::now();
    auto inf = m_AI->Submit(std::move(req));
    auto t1 = std::chrono::high_resolution_clock::now();
    m_LastInferenceMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    if (!inf) return;

    // 读回并计算平均亮度（验证推理输出可被 CPU 消费）
    std::vector<float> data(m_TexSize * m_TexSize * 4);
    if (!m_AI->ReadTensor(m_OutputTensor.get(), Span<float>(data))) return;

    double sum = 0.0;
    for (u32 i = 0; i < m_TexSize * m_TexSize; ++i)
        sum += data[i * 4];   // 只统计 r 通道
    m_LastAvgBrightness = (float)(sum / (m_TexSize * m_TexSize));
    ++m_InferenceCount;
}

} // namespace he::render
