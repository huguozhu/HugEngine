// ============================================================
// Tests/TestAIGCProvider.cpp — 生成后端与资产工厂测试
//
// FakeAIDevice 返回固定 OpenAI 兼容响应，
// 验证 CloudAIGCProvider → GenerativeAssetFactory → SceneBuilder 链路。
// ============================================================

#include "doctest.h"

#include "AI/AIGC/AIGCProvider.h"
#include "AI/AIGC/GenerativeAssetFactory.h"
#include "AI/Runtime/AIDevice.h"
#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Scene/Transform.h"
#include "Scene/SphereComponent.h"

#include "nlohmann/json.hpp"

using namespace he;
using namespace he::ai;
using namespace he::ai::aigc;

namespace {

// 假 AI 设备：Chat 返回固定 OpenAI 兼容响应（含一个球场景）
struct FakeAIDevice : IAIDevice {
    AIDeviceCaps GetCaps() const override { return {}; }
    Ref<IAIModel> LoadModel(AIModelFormat, Span<const u8>, const String&) override { return nullptr; }
    Ref<IAITensor> CreateTensor(const AITensorDesc&) override { return nullptr; }
    Ref<IAIInference> Submit(InferenceRequest&&) override { return nullptr; }
    Ref<IAITensor> WrapRHITexture(rhi::IRHITexture*) override { return nullptr; }
    Ref<IAITensor> WrapRHIBuffer(rhi::IRHIBuffer*) override { return nullptr; }
    rhi::IRHIBuffer* ExportBuffer(IAITensor*) override { return nullptr; }

    String Chat(const String&, const String&) override {
        nlohmann::json scene = {
            {"entities", {{
                {"name", "Ball"},
                {"transform", {{"position", {0, 1, 0}}}},
                {"components", { {{"type", "Sphere"}, {"radius", 0.8f}} }}
            }}}
        };
        nlohmann::json resp = { {"choices", { {{"message", {{"content", scene.dump()}}}} }} };
        return resp.dump();
    }
    void ChatStream(const String&, const String&, std::function<void(const String&)>) override {}
};

} // namespace

TEST_CASE("GenerativeAssetFactory::GenerateScene 经假设备生成实体") {
    World world;
    SceneGraph sg(world);
    FakeAIDevice dev;

    GenerativeAssetFactory factory;
    GenerationResult r = factory.GenerateScene(world, sg, dev, "一个球");

    REQUIRE(r.success == true);
    REQUIRE(r.entities.size() == 1);
    auto* sphere = world.GetComponent<SphereComponent>(r.entities[0]);
    REQUIRE(sphere != nullptr);
    CHECK(sphere->radius == doctest::Approx(0.8f));
    auto* xform = world.GetComponent<TransformComponent>(r.entities[0]);
    REQUIRE(xform != nullptr);
    CHECK(xform->position.y == doctest::Approx(1.0f));
}

TEST_CASE("CloudAIGCProvider 生成场景规格 JSON（不装配实体）") {
    World world;
    SceneGraph sg(world);
    FakeAIDevice dev;
    CloudAIGCProvider provider(&dev);

    CHECK(provider.Supports(GenKind::Scene));
    CHECK(!provider.Supports(GenKind::Texture));   // 其余类型 G2 覆盖

    bool done = false;
    provider.Generate({GenKind::Scene, "一个球"}, [&](GenResult&& r) {
        done = true;
        CHECK(r.success);
        // 生成阶段只产出规格：World 不应有实体（装配推迟到接受命令）
        CHECK(world.GetEntityCount() == 0);
        // 规格应包含场景内容
        CHECK(r.sceneJson.find("Sphere") != String::npos);
    });
    CHECK(done);
}

TEST_CASE("CloudAIGCProvider 无设备时返回失败") {
    World world;
    SceneGraph sg(world);
    CloudAIGCProvider provider(nullptr);   // 设备为空

    bool done = false;
    provider.Generate({GenKind::Scene, "一个球"}, [&](GenResult&& r) {
        done = true;
        CHECK(r.success == false);
        CHECK(!r.error.empty());
    });
    CHECK(done);
}
