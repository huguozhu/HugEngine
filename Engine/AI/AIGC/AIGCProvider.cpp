#include "AI/AIGC/AIGCProvider.h"

#include "AI/AIGC/GenerativeAssetFactory.h"
#include "AI/Runtime/AIDevice.h"
#include "Core/Log.h"

namespace he::ai::aigc {

void CloudAIGCProvider::Generate(const GenRequest& req,
                                 std::function<void(GenResult&&)> onDone) {
    GenResult result;

    // 仅支持场景生成（其他类型由 G2 的本地后端覆盖）
    if (!Supports(req.kind)) {
        result.error = "CloudAIGCProvider 暂不支持该生成类型";
        if (onDone) onDone(std::move(result));
        return;
    }
    if (!m_Device) {
        result.error = "推理设备不可用";
        if (onDone) onDone(std::move(result));
        return;
    }

    // LLM 编排：prompt → 场景规格 JSON（不装配实体，装配交给接受命令）
    GenerativeAssetFactory factory;
    String sceneJson = factory.GenerateSceneJson(*m_Device, req.prompt);

    result.success   = !sceneJson.empty();
    result.error     = sceneJson.empty() ? "LLM 生成失败（无有效场景 JSON）" : "";
    result.sceneJson = std::move(sceneJson);

    if (onDone) onDone(std::move(result));
}

} // namespace he::ai::aigc
