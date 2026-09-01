#include "Features/FeatureTextureGen.h"

#include "AI/Runtime/AIDevice.h"
#include "AI/Runtime/Backend/GPUBackend.h"
#include "AI/AIGC/GenerativeAssetFactory.h"
#include "Core/Log.h"
#include "imgui.h"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <filesystem>
#include <vector>

using namespace he;

bool FeatureTextureGen::Initialize(rhi::IRHIDevice* device, rhi::IRHISwapChain* sc,
                                   he::ai::IAIDevice* ai) {
    m_Device = device;
    m_AI = ai;
    (void)sc;
    if (!m_Device || !m_AI) return false;

    // 输出路径：Content/Generated/tex_gen_checker.png（与 glTF 纹理同目录）
    String outPath = String(HUGE_CONTENT_DIR) + "Generated/tex_gen_checker.png";
    std::filesystem::path outP(outPath);
    std::error_code ec;
    std::filesystem::create_directories(outP.parent_path(), ec);

    const String kPrompt = "棋盘纹理：蓝色与红色相间，64 像素";
    if (m_AI->GetCaps().supportsRemoteLLM) {
        he::ai::aigc::GenerativeAssetFactory factory;
        m_Result = factory.TextToTexture(*m_AI, kPrompt, outPath);
    } else {
        HE_CORE_WARN("[TextureGen] 未设置 DEEPSEEK_API_KEY，用内置规格演示");
        he::ai::aigc::TextureSpec spec;
        spec.pattern = "checker";
        spec.size    = 64;
        spec.colorA  = float3(0.2f, 0.4f, 0.9f);
        spec.colorB  = float3(0.9f, 0.2f, 0.2f);
        m_Result.success = true;
        m_Result.width = m_Result.height = spec.size;
        m_Result.error = "降级模式";
        he::ai::aigc::TextureGenerator::Generate(spec, m_Result.pixels);
        if (he::ai::aigc::TextureGenerator::WritePNG(outPath, spec.size, spec.size,
                                                     m_Result.pixels.data()))
            m_Result.path = outPath;
    }

    // 标准加载管线读回 PNG → GPU 纹理
    if (m_Result.success && !m_Result.path.empty()) {
        int w = 0, h = 0, ch = 0;
        u8* pixels = stbi_load(m_Result.path.c_str(), &w, &h, &ch, 4);
        if (pixels) {
            rhi::TextureDesc tDesc;
            tDesc.format = rhi::Format::RGBA8_UNORM;
            tDesc.width  = (u32)w;
            tDesc.height = (u32)h;
            tDesc.mipLevels = 1;
            tDesc.usage = rhi::TextureUsage::ShaderResource | rhi::TextureUsage::TransferDst;
            tDesc.initialData = pixels;
            auto tex = m_Device->CreateTexture(tDesc);
            stbi_image_free(pixels);
            m_LoadedOk = tex != nullptr;

            // 生成纹理 → AI 推理消费（texture_sample）→ 平均色
            if (m_LoadedOk && m_AI->GetCaps().supportsGPU) {
                auto texTensor = m_AI->WrapRHITexture(tex.get());
                he::ai::AITensorDesc desc;
                desc.elementCount = m_Result.width * m_Result.height * 4;
                desc.dtype        = he::ai::AIDataType::FP32;
                auto out = m_AI->CreateTensor(desc);
                if (texTensor && out) {
                    he::ai::InferenceRequest req;
                    req.kernel = he::ai::kKernelTextureSample;
                    req.params = {{"brightness", "1.0"}};
                    req.textureInputs.push_back(texTensor.get());
                    req.outputs.push_back(out.get());
                    auto inf = m_AI->Submit(std::move(req));
                    if (inf) {
                        std::vector<float> data(m_Result.width * m_Result.height * 4);
                        if (m_AI->ReadTensor(out.get(), Span<float>(data))) {
                            double sr = 0, sg = 0, sb = 0;
                            for (u32 i = 0; i < m_Result.width * m_Result.height; ++i) {
                                sr += data[i * 4];
                                sg += data[i * 4 + 1];
                                sb += data[i * 4 + 2];
                            }
                            u32 n = m_Result.width * m_Result.height;
                            m_AvgR = (float)(sr / n);
                            m_AvgG = (float)(sg / n);
                            m_AvgB = (float)(sb / n);
                            m_InferOk = true;
                        }
                    }
                }
            }
        }
    }
    HE_CORE_INFO("[TextureGen] 生成: {} 加载: {} 推理: {}",
                 m_Result.success, m_LoadedOk, m_InferOk);
    return true;
}

void FeatureTextureGen::Shutdown() {}

void FeatureTextureGen::Update(float) {}

void FeatureTextureGen::RenderUI() {
    ImGui::Begin("文生纹理");
    ImGui::Text("生成: %s", m_Result.success ? "成功" : "失败");
    if (!m_Result.error.empty())
        ImGui::TextWrapped("说明: %s", m_Result.error.c_str());
    ImGui::Text("资产文件: %s", m_Result.path.empty() ? "(未写盘)" : m_Result.path.c_str());
    ImGui::Text("尺寸: %u x %u", m_Result.width, m_Result.height);
    ImGui::Text("标准加载(stbi): %s", m_LoadedOk ? "通过" : "失败");
    ImGui::Text("AI 推理消费: %s", m_InferOk ? "通过" : "失败");
    if (m_InferOk)
        ImGui::Text("平均色: (%.2f, %.2f, %.2f)", m_AvgR, m_AvgG, m_AvgB);
    ImGui::End();
}
