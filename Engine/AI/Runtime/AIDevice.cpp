#include "AI/Runtime/AIDevice.h"
#include "AI/Runtime/InferenceScheduler.h"
#include "AI/Runtime/Backend/RemoteBackend.h"
#include "Core/Log.h"

#include <cstdlib>
#include <utility>

namespace he::ai {

// ============================================================
// AIDevice — IAIDevice 门面实现（内部类，不暴露头文件）
// ============================================================

namespace {

class AIDeviceImpl : public IAIDevice {
public:
    AIDeviceImpl() = default;

    void RegisterBackend(std::unique_ptr<IAIBackend> backend) {
        m_Backends.push_back(std::move(backend));
        // 记录 LLM 后端快捷指针（RemoteBackend 提供 Chat/ChatStream）
        if (!m_LLM)
            m_LLM = dynamic_cast<RemoteBackend*>(m_Backends.back().get());
    }

    void SetScheduler(InferenceScheduler* scheduler) { m_Scheduler = scheduler; }

    // --- IAIDevice ---
    AIDeviceCaps GetCaps() const override {
        AIDeviceCaps caps;
        caps.supportsRemoteLLM = (m_LLM != nullptr);   // 当前仅远程 LLM 可用
        return caps;
    }

    Ref<IAIModel> LoadModel(AIModelFormat fmt, Span<const u8> weights,
                            const String& path) override {
        // 按格式找第一个支持的后端
        for (auto& b : m_Backends) {
            if (b->Supports(fmt))
                return b->Load(fmt, weights, path);
        }
        HE_CORE_WARN("[AIDevice] 没有支持该模型格式的后端，降级返回 nullptr");
        return nullptr;
    }

    Ref<IAITensor> CreateTensor(const AITensorDesc& desc) override {
        (void)desc;
        HE_CORE_WARN("[AIDevice] GPU/CPU 张量后端未就绪（A3 落地），返回 nullptr");
        return nullptr;
    }

    Ref<IAIInference> Submit(InferenceRequest&& req) override {
        (void)req;
        HE_CORE_WARN("[AIDevice] 通用推理后端未就绪（A3 落地），返回 nullptr");
        return nullptr;
    }

    // 与渲染零拷贝互操作：A3 的 GPUBackend 落地前不支持，返回 nullptr
    Ref<IAITensor> WrapRHITexture(rhi::IRHITexture* tex) override { (void)tex; return nullptr; }
    Ref<IAITensor> WrapRHIBuffer(rhi::IRHIBuffer* buf) override { (void)buf; return nullptr; }
    rhi::IRHIBuffer* ExportBuffer(IAITensor* t) override { (void)t; return nullptr; }

    String Chat(const String& systemPrompt, const String& userPrompt) override {
        if (!m_LLM) {
            HE_CORE_ERROR("[AIDevice] 远程 LLM 后端不可用");
            return {};
        }
        return m_LLM->Chat(systemPrompt, userPrompt);
    }

    void ChatStream(const String& systemPrompt, const String& userPrompt,
                    std::function<void(const String&)> onToken) override {
        if (!m_LLM) {
            HE_CORE_ERROR("[AIDevice] 远程 LLM 后端不可用");
            return;
        }
        // 有调度器：token 回调按序投递到主线程；无调度器：直接同步回调
        if (m_Scheduler) {
            m_LLM->ChatStream(systemPrompt, userPrompt,
                [this, onToken](const String& chunk) {
                    m_Scheduler->PostToMain([onToken, chunk] { onToken(chunk); });
                });
        } else {
            m_LLM->ChatStream(systemPrompt, userPrompt, onToken);
        }
    }

private:
    std::vector<std::unique_ptr<IAIBackend>> m_Backends;  // 已注册后端
    RemoteBackend* m_LLM = nullptr;                       // LLM 后端快捷指针
    InferenceScheduler* m_Scheduler = nullptr;            // 流式回调投递目标
};

} // namespace

// ============================================================
// 工厂：创建默认 AI 设备
// ============================================================

std::unique_ptr<IAIDevice> CreateAIDevice(InferenceScheduler* scheduler) {
    auto device = std::make_unique<AIDeviceImpl>();
    device->SetScheduler(scheduler);

    // 注册远程 LLM 后端（key 从环境变量读取，不硬编码）
    const char* key = std::getenv("DEEPSEEK_API_KEY");
    if (key && *key) {
        device->RegisterBackend(std::make_unique<RemoteBackend>(String(key)));
        HE_CORE_INFO("[AIDevice] RemoteBackend 已注册（DeepSeek）");
    } else {
        HE_CORE_WARN("[AIDevice] 未设置 DEEPSEEK_API_KEY，远程 LLM 不可用");
    }
    return device;
}

} // namespace he::ai
