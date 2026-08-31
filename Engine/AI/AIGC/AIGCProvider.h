#pragma once

#include "Core/Types.h"
#include "Containers/Array.h"
#include "Scene/Entity.h"

#include <functional>
#include <utility>

// ============================================================
// AIGCProvider — 生成后端抽象
//
// 并行底座层的 IAIBackend，但语义是「生成」而非「通用推理」。
// 本地后端（LocalAIGCProvider，经 GPUBackend 跑扩散/文生3D）
// 在 G2 里程碑接入；当前实现云端后端（CloudAIGCProvider，
// LLM 编排 → GenerativeAssetFactory）。
// ============================================================

namespace he {
class World;
class SceneGraph;
} // namespace he

namespace he::ai {
class IAIDevice;
} // namespace he::ai

namespace he::ai::aigc {

// 生成类型
enum class GenKind { Scene, Texture, Mesh, Material, Animation };

// 生成请求
struct GenRequest {
    GenKind kind   = GenKind::Scene;
    String  prompt;
};

// 生成结果（场景类请求携带场景规格 JSON，装配推迟到接受命令）
struct GenResult {
    bool success = false;
    String error;
    String sceneJson;   // LLM 输出的场景规格（Scene 类请求）
};

// 生成后端接口
class IAIGCProvider {
public:
    virtual ~IAIGCProvider() = default;
    // 是否支持该生成类型
    virtual bool Supports(GenKind kind) const = 0;
    // 执行生成，完成后回调 onDone（实现方负责同步/异步语义；异步投递由 AIPipeline 负责）
    virtual void Generate(const GenRequest& req, std::function<void(GenResult&&)> onDone) = 0;
};

// 云端生成后端：LLM 编排 → 场景规格 JSON
// （不装配实体 —— 装配由「接受」时的 GenerateSceneCommand 负责，保证可撤销与线程安全）
class CloudAIGCProvider : public IAIGCProvider {
public:
    explicit CloudAIGCProvider(IAIDevice* device) : m_Device(device) {}

    bool Supports(GenKind kind) const override { return kind == GenKind::Scene; }

    // 同步生成后回调（AIPipeline 负责放到后台线程执行）
    void Generate(const GenRequest& req, std::function<void(GenResult&&)> onDone) override;

private:
    IAIDevice*  m_Device = nullptr;   // 推理设备（不拥有）
};

} // namespace he::ai::aigc
