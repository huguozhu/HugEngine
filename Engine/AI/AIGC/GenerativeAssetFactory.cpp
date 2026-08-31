#include "AI/AIGC/GenerativeAssetFactory.h"

#include "AI/Runtime/AIDevice.h"
#include "AI/PromptToScene.h"
#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Core/Log.h"

#include "nlohmann/json.hpp"

namespace he::ai::aigc {

namespace {

// 拼接材质规格词表（注入 LLM，约束输出字段）
String BuildMaterialSpecPrompt() {
    return R"(
生成一个材质规格 JSON，用于 PBR 材质。格式：
{"baseColor":[r,g,b],"metallic":0.0,"roughness":0.5,"emissive":[r,g,b],
 "texture":{"pattern":"solid|gradient|checker|noise","size":64,"colorA":[r,g,b],"colorB":[r,g,b]}}
规则：
- baseColor: [r,g,b] 0~1（基础色）
- metallic/roughness: 0~1
- emissive: [r,g,b] 0~1（无自发光填 [0,0,0]）
- texture 可选（无纹理需求可省略）；其 pattern/size/colorA/colorB 规则同纹理规格
- 只输出 JSON，不要输出解释文字。
)";
}

// 从材质规格 JSON 解析 → MaterialGenResult（含可选纹理规格）
MaterialGenResult ParseMaterialSpec(const String& content) {
    MaterialGenResult result;
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(content);
    } catch (const std::exception&) {
        result.error = "材质规格 JSON 解析失败";
        return result;
    }
    auto readVec3 = [&](const char* key, float3& dst) {
        if (j.contains(key) && j[key].is_array() && j[key].size() >= 3)
            dst = float3(j[key][0].get<float>(), j[key][1].get<float>(), j[key][2].get<float>());
    };
    float3 bc = {1, 1, 1}, em = {0, 0, 0};
    readVec3("baseColor", bc);
    readVec3("emissive", em);
    result.baseColor = float4(bc, 1.0f);
    result.emissive  = em;
    if (j.contains("metallic") && j["metallic"].is_number())
        result.metallic = j["metallic"].get<float>();
    if (j.contains("roughness") && j["roughness"].is_number())
        result.roughness = j["roughness"].get<float>();

    // 可选纹理规格
    if (j.contains("texture") && j["texture"].is_object()) {
        TextureSpec spec;
        TextureGenerator::ParseSpec(j["texture"].dump(), spec);
        TextureGenerator::Generate(spec, result.texture.pixels);
        result.texture.width = result.texture.height = spec.size;
        result.texture.success = !result.texture.pixels.empty();
    }
    result.success = true;
    return result;
}

} // namespace

TextureGenResult GenerativeAssetFactory::TextToTexture(he::ai::IAIDevice& device,
                                                      const String& prompt,
                                                      const String& outPath) {
    TextureGenResult result;

    // 1. LLM 输出纹理规格 JSON（注入规格词表约束字段）
    HE_CORE_INFO("[AIGC] 请求生成纹理: {}", prompt);
    String response = device.Chat(TextureGenerator::BuildSpecPrompt(), prompt);
    String content  = ExtractSceneJsonFromResponse(response);
    if (content.empty()) {
        result.error = "LLM 无有效响应（设备/网络不可用）";
        return result;
    }
    HE_CORE_INFO("[AIGC] 纹理规格: {}", content);

    // 2. 解析规格 → 程序化生成像素
    TextureSpec spec;
    if (!TextureGenerator::ParseSpec(content, spec)) {
        result.error = "纹理规格解析失败";
        return result;
    }
    if (!TextureGenerator::Generate(spec, result.pixels)) {
        result.error = "像素生成失败";
        return result;
    }
    result.width  = spec.size;
    result.height = spec.size;

    // 3. 写盘为标准资产文件（PNG，与 glTF 导入的纹理同源）
    if (!outPath.empty()) {
        if (!TextureGenerator::WritePNG(outPath, result.width, result.height,
                                        result.pixels.data())) {
            result.error = "PNG 写盘失败: " + outPath;
            return result;
        }
        result.path = outPath;
    }

    result.success = true;
    return result;
}

String GenerativeAssetFactory::GenerateSceneJson(he::ai::IAIDevice& device,
                                                 const String& prompt) {
    // 1. LLM 生成场景 JSON（system prompt 注入组件词表）
    HE_CORE_INFO("[AIGC] 请求生成场景: {}", prompt);
    String response = device.Chat(BuildSceneSystemPrompt(), prompt);
    if (response.empty()) return {};

    // 2. 只提取场景规格文本（不装配实体；装配由接受命令负责）
    return ExtractSceneJsonFromResponse(response);
}

GenerationResult GenerativeAssetFactory::GenerateScene(World& world, SceneGraph& sg,
                                                       he::ai::IAIDevice& device,
                                                       const String& prompt) {
    GenerationResult result;

    // 1. 生成场景规格
    String sceneJson = GenerateSceneJson(device, prompt);
    if (sceneJson.empty()) {
        result.error = "LLM 无响应或响应无效（设备/网络不可用）";
        return result;
    }

    // 2. 立即装配实体树（同步场景生成路径）
    he::ai::SceneBuildResult build = BuildScene(world, sg, sceneJson);
    result.success = build.success;
    result.error   = build.error;
    result.entities = std::move(build.entities);
    return result;
}

MaterialGenResult GenerativeAssetFactory::TextToMaterial(he::ai::IAIDevice& device,
                                                        const String& prompt,
                                                        const String& textureOutPath) {
    MaterialGenResult result;

    // 1. LLM 输出材质规格 JSON（注入规格词表约束字段）
    HE_CORE_INFO("[AIGC] 请求生成材质: {}", prompt);
    String response = device.Chat(BuildMaterialSpecPrompt(), prompt);
    String content  = ExtractSceneJsonFromResponse(response);
    if (content.empty()) {
        result.error = "LLM 无有效响应（设备/网络不可用）";
        return result;
    }
    HE_CORE_INFO("[AIGC] 材质规格: {}", content);

    // 2. 解析材质参数（含可选纹理规格）
    result = ParseMaterialSpec(content);
    if (!result.success) return result;

    // 3. 可选纹理：写盘为标准资产文件
    if (result.texture.success && !textureOutPath.empty() && !result.texture.pixels.empty()) {
        if (TextureGenerator::WritePNG(textureOutPath, result.texture.width,
                                       result.texture.height, result.texture.pixels.data()))
            result.texture.path = textureOutPath;
    }
    return result;
}

MeshGenResult GenerativeAssetFactory::TextToMesh(he::ai::IAIDevice& device,
                                                 const String& prompt) {
    MeshGenResult result;

    // 1. LLM 输出形状规格 JSON
    HE_CORE_INFO("[AIGC] 请求生成网格: {}", prompt);
    String response = device.Chat(MeshGenerator::BuildSpecPrompt(), prompt);
    String content  = ExtractSceneJsonFromResponse(response);
    if (content.empty()) {
        result.error = "LLM 无有效响应（设备/网络不可用）";
        return result;
    }
    HE_CORE_INFO("[AIGC] 网格规格: {}", content);

    // 2. 解析规格 → 程序化生成顶点/索引
    String shape = "cube";
    float  size  = 1.0f;
    u32    segments = 16;
    if (!MeshGenerator::ParseSpec(content, shape, size, segments)) {
        result.error = "网格规格解析失败";
        return result;
    }
    if (!MeshGenerator::Generate(shape, size, segments, result)) {
        result.error = "网格生成失败";
        return result;
    }
    return result;
}

AnimGenResult GenerativeAssetFactory::TextToAnimation(he::ai::IAIDevice& device,
                                                      const String& prompt) {
    AnimGenResult result;

    // 1. 拼接动画规格词表并请求 LLM
    String specPrompt = R"(
生成一个 Transform 动画规格 JSON。格式：
{"duration":2.0,"loop":true,
 "keyframes":[{"time":0.0,"position":[x,y,z],"scale":[sx,sy,sz]},
              {"time":1.0,"position":[x,y,z]}]}
规则：
- duration: 总时长（秒）
- loop: 是否循环
- keyframes: 按时间升序；每帧 time 必须、position 可选、scale 可选
- 只输出 JSON，不要输出解释文字。
)";
    HE_CORE_INFO("[AIGC] 请求生成动画: {}", prompt);
    String response = device.Chat(specPrompt, prompt);
    String content  = ExtractSceneJsonFromResponse(response);
    if (content.empty()) {
        result.error = "LLM 无有效响应（设备/网络不可用）";
        return result;
    }
    HE_CORE_INFO("[AIGC] 动画规格: {}", content);

    // 2. 解析关键帧
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(content);
    } catch (const std::exception&) {
        result.error = "动画规格解析失败";
        return result;
    }
    if (j.contains("duration") && j["duration"].is_number())
        result.duration = j["duration"].get<float>();
    if (j.contains("loop") && j["loop"].is_boolean())
        result.loop = j["loop"].get<bool>();
    if (j.contains("name") && j["name"].is_string())
        result.name = j["name"].get<String>();

    if (!j.contains("keyframes") || !j["keyframes"].is_array() || j["keyframes"].empty()) {
        result.error = "动画缺少 keyframes";
        return result;
    }
    for (auto& kf : j["keyframes"]) {
        if (!kf.contains("time") || !kf["time"].is_number()) continue;
        float t = kf["time"].get<float>();
        auto readVec3 = [&](const char* key, float3& dst) {
            if (kf.contains(key) && kf[key].is_array() && kf[key].size() >= 3)
                dst = float3(kf[key][0].get<float>(), kf[key][1].get<float>(), kf[key][2].get<float>());
        };
        float3 pos(0), scl(1);
        readVec3("position", pos);
        readVec3("scale", scl);
        result.translations.push_back({t, pos});
        result.scales.push_back({t, scl});
    }
    result.success = !result.translations.empty();
    if (!result.success)
        result.error = "动画无有效关键帧";
    return result;
}

} // namespace he::ai::aigc
