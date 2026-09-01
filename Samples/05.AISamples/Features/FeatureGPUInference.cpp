#include "Features/FeatureGPUInference.h"

#include "AI/Runtime/AIDevice.h"
#include "AITensorAdd.comp.spv.h"   // k_AITensorAdd_comp_spv（外部核 SPIR-V）
#include "Core/Log.h"
#include "imgui.h"

#include <chrono>
#include <vector>

using namespace he;

bool FeatureGPUInference::Initialize(rhi::IRHIDevice* device, rhi::IRHISwapChain* sc,
                                     he::ai::IAIDevice* ai) {
    m_Device = device;
    m_AI = ai;
    (void)sc;
    if (!m_Device || !m_AI) return false;

    // 神经子系统（IRenderSubsystem 形态，每帧经 IAIDevice 推理）
    m_Neural = std::make_unique<he::render::NeuralUpscaler>(m_AI);
    m_Neural->Initialize(m_Device, 1280, 720);

    RunInference();   // 启动即执行三条推理路径
    return true;
}

void FeatureGPUInference::Shutdown() {
    if (m_Neural) m_Neural->Shutdown();
    m_Neural.reset();
}

void FeatureGPUInference::Update(float) {
    if (m_Neural) {
        render::SubsystemContext ctx;
        m_Neural->Update(ctx);   // 每帧神经推理（统计展示）
    }
}

void FeatureGPUInference::RunInference() {
    constexpr u32 kCount = 4;

    // ── 路径1：内置核 tensor_scale（out = in × 2.5）──
    {
        const float scale = 2.5f;
        float inData[kCount] = {1, 2, 3, 4};
        he::ai::AITensorDesc desc;
        desc.elementCount = kCount;
        desc.dtype        = he::ai::AIDataType::FP32;
        auto in  = m_AI->CreateTensor(desc);
        auto out = m_AI->CreateTensor(desc);
        if (in && out && m_AI->WriteTensor(in.get(), Span<const float>(inData, kCount))) {
            he::ai::InferenceRequest req;
            req.kernel = he::ai::kKernelTensorScale;
            req.params = {{"scale", std::to_string(scale)}};
            req.inputs.push_back(in.get());
            req.outputs.push_back(out.get());
            auto t0 = std::chrono::high_resolution_clock::now();
            auto inf = m_AI->Submit(std::move(req));
            auto t1 = std::chrono::high_resolution_clock::now();
            m_ScaleMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
            m_ScaleOk = inf && m_AI->ReadTensor(out.get(), Span<float>(m_ScaleOut, kCount));
            for (u32 i = 0; i < kCount; ++i)
                if (m_ScaleOut[i] != inData[i] * scale) { m_ScaleOk = false; break; }
        }
        HE_CORE_INFO("[GPUInference] 内置核: {}", m_ScaleOk ? "通过" : "失败");
    }

    // ── 路径2：外部核 customSpirv（AITensorAdd，out = in + 10）──
    {
        const float add = 10.0f;
        float inData[kCount] = {1, 2, 3, 4};
        he::ai::AITensorDesc desc;
        desc.elementCount = kCount;
        desc.dtype        = he::ai::AIDataType::FP32;
        auto in  = m_AI->CreateTensor(desc);
        auto out = m_AI->CreateTensor(desc);
        if (in && out && m_AI->WriteTensor(in.get(), Span<const float>(inData, kCount))) {
            struct AddPC { u32 count; float add; } pc = { kCount, add };
            he::ai::InferenceRequest req;
            req.customSpirv      = Span<const u32>(k_AITensorAdd_comp_spv);
            req.pushConstantSize = sizeof(pc);
            req.pushConstants.assign(reinterpret_cast<u8*>(&pc),
                                     reinterpret_cast<u8*>(&pc) + sizeof(pc));
            req.inputs.push_back(in.get());
            req.outputs.push_back(out.get());
            auto t0 = std::chrono::high_resolution_clock::now();
            auto inf = m_AI->Submit(std::move(req));
            auto t1 = std::chrono::high_resolution_clock::now();
            m_AddMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
            m_AddOk = inf && m_AI->ReadTensor(out.get(), Span<float>(m_AddOut, kCount));
            for (u32 i = 0; i < kCount; ++i)
                if (m_AddOut[i] != inData[i] + add) { m_AddOk = false; break; }
        }
        HE_CORE_INFO("[GPUInference] 外部核: {}", m_AddOk ? "通过" : "失败");
    }

    // ── 路径3：零拷贝纹理采样（texture_sample）──
    {
        constexpr u32 kTexSize = 64;
        const float brightness = 2.0f;
        std::vector<u8> texData(kTexSize * kTexSize * 4);
        for (u32 y = 0; y < kTexSize; ++y)
            for (u32 x = 0; x < kTexSize; ++x) {
                u8* p = &texData[(y * kTexSize + x) * 4];
                p[0] = (u8)(x * 255 / (kTexSize - 1));
                p[1] = (u8)(y * 255 / (kTexSize - 1));
                p[2] = 128;
                p[3] = 255;
            }
        rhi::TextureDesc tDesc;
        tDesc.format = rhi::Format::RGBA8_UNORM;
        tDesc.width  = kTexSize;
        tDesc.height = kTexSize;
        tDesc.mipLevels = 1;
        tDesc.usage = rhi::TextureUsage::ShaderResource | rhi::TextureUsage::TransferDst;
        tDesc.initialData = texData.data();
        auto tex = m_Device->CreateTexture(tDesc);

        he::ai::AITensorDesc desc;
        desc.elementCount = kTexSize * kTexSize * 4;
        desc.dtype        = he::ai::AIDataType::FP32;
        auto out = m_AI->CreateTensor(desc);
        auto texTensor = m_AI->WrapRHITexture(tex.get());
        if (texTensor && out) {
            std::vector<float> readback(kTexSize * kTexSize * 4);
            he::ai::InferenceRequest req;
            req.kernel = he::ai::kKernelTextureSample;
            req.params = {{"brightness", std::to_string(brightness)}};
            req.textureInputs.push_back(texTensor.get());
            req.outputs.push_back(out.get());
            auto t0 = std::chrono::high_resolution_clock::now();
            auto inf = m_AI->Submit(std::move(req));
            auto t1 = std::chrono::high_resolution_clock::now();
            m_TexMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
            m_TexOk = inf && m_AI->ReadTensor(out.get(), Span<float>(readback));
            if (m_TexOk) {
                m_TexOk = (std::abs(readback[2] - 0.5f * brightness) < 0.05f) &&
                          (std::abs(readback[3] - 1.0f * brightness) < 0.05f);
            }
        }
        HE_CORE_INFO("[GPUInference] 零拷贝纹理核: {}", m_TexOk ? "通过" : "失败");
    }
}

void FeatureGPUInference::RenderUI() {
    ImGui::Begin("GPU 推理");
    ImGui::Text("内置核 tensor_scale: %s (%.2f ms)", m_ScaleOk ? "通过" : "失败", m_ScaleMs);
    if (m_ScaleOk) {
        String s;
        for (u32 i = 0; i < 4; ++i) s += (i ? ", " : "[") + std::to_string((int)m_ScaleOut[i]);
        ImGui::TextWrapped("  [1,2,3,4] x2.5 -> %s]", s.c_str());
    }
    ImGui::Text("外部核 customSpirv(add): %s (%.2f ms)", m_AddOk ? "通过" : "失败", m_AddMs);
    if (m_AddOk) {
        String s;
        for (u32 i = 0; i < 4; ++i) s += (i ? ", " : "[") + std::to_string((int)m_AddOut[i]);
        ImGui::TextWrapped("  [1,2,3,4] +10 -> %s]", s.c_str());
    }
    ImGui::Text("零拷贝纹理核: %s (%.2f ms)", m_TexOk ? "通过" : "失败", m_TexMs);
    ImGui::Separator();
    ImGui::Text("神经子系统 NeuralUpscaler: %s",
                (m_Neural && m_Neural->IsReady()) ? "就绪" : "不可用");
    if (m_Neural && m_Neural->IsReady()) {
        ImGui::Text("累计推理: %u 次 | 上次: %.2f ms | 平均亮度: %.3f",
                    m_Neural->GetInferenceCount(),
                    m_Neural->GetLastInferenceMs(),
                    m_Neural->GetLastAvgBrightness());
    }
    ImGui::End();
}
