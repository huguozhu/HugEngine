// ============================================================
// Tests/TestSkeletonLoader.cpp — Phase C C1a glTF 骨骼加载器单元测试
//
// 覆盖：SimpleSkin（2 关节 + 动画 + 蒙皮顶点）、Fox（多关节 + 3 剪辑）、
//       静态模型不产生骨架（容错）。
// ============================================================

#include "doctest.h"

#include "Asset/glTFLoader.h"
#include "Scene/SkeletonAsset.h"
#include "Scene/World.h"
#include "Scene/SceneGraph.h"

using namespace he;
using namespace he::asset;

namespace {
String AssetPath(const char* rel) {
    return String(HUGE_CONTENT_DIR) + rel;
}
} // namespace

TEST_CASE("glTFLoader 解析 SimpleSkin 骨架资产") {
    World world;
    SceneGraph sg(world);
    glTFResult r = LoadGLTF(world, sg, AssetPath("Models/Skeletal/SimpleSkin/SimpleSkin.gltf"));
    REQUIRE(r.success == true);
    REQUIRE(r.skeletons.size() == 1);

    auto& skel = *r.skeletons[0];
    // SimpleSkin：2 个关节（节点 1/2），1 个动画
    REQUIRE(skel.joints.size() == 2);
    CHECK(skel.clips.size() >= 1);

    // 关节层级：有根（parent=-1）与子关节
    bool hasRoot = false, hasChild = false;
    for (auto& j : skel.joints) {
        if (j.parent == -1) hasRoot = true;
        else hasChild = true;
    }
    CHECK(hasRoot);
    CHECK(hasChild);

    // 逆绑定矩阵非全零（SimpleSkin 关节 0 有真实数据）
    float4x4 ib = skel.joints[0].inverseBind;
    float ibLenSq = 0.0f;
    for (int c = 0; c < 4; ++c) ibLenSq += glm::dot(ib[c], ib[c]);
    CHECK(ibLenSq > 0.0001f);

    // 蒙皮顶点：非空 + 权重归一化 + 关节索引非全零（至少有一个非 0 关节影响）
    REQUIRE(skel.vertices.size() > 0);
    bool anyNonZeroJoint = false;
    for (auto& v : skel.vertices) {
        float wsum = v.weight[0] + v.weight[1] + v.weight[2] + v.weight[3];
        CHECK(wsum == doctest::Approx(1.0f).epsilon(0.01));
        for (int k = 0; k < 4; ++k)
            if (v.joint[k] != 0) anyNonZeroJoint = true;
    }
    CHECK(anyNonZeroJoint);
    CHECK(skel.indices.size() > 0);

    // 剪辑：至少一个关节通道 + 时长 > 0
    CHECK(skel.clips[0].channels.size() >= 1);
    CHECK(skel.clips[0].duration > 0.0f);
    CHECK(skel.clips[0].channels[0].times.size() >= 2);
}

TEST_CASE("glTFLoader 解析 Fox 骨架与三段动画") {
    World world;
    SceneGraph sg(world);
    glTFResult r = LoadGLTF(world, sg, AssetPath("Models/Skeletal/Fox.glb"));
    REQUIRE(r.success == true);
    REQUIRE(r.skeletons.size() >= 1);

    auto& skel = *r.skeletons[0];
    // Fox：约 52 个关节，3 段动画（Survey/Walk/Run）
    REQUIRE(skel.joints.size() >= 20);
    REQUIRE(skel.clips.size() == 3);
    CHECK(skel.vertices.size() > 0);

    // 三段剪辑都含关节通道且时长递增典型（Walk/Run > 0.5s）
    for (auto& clip : skel.clips) {
        CHECK(clip.channels.size() >= 1);
        CHECK(clip.duration > 0.1f);
    }

    // 剪辑内关节下标合法
    for (auto& clip : skel.clips) {
        for (auto& ch : clip.channels) {
            CHECK(ch.jointIndex >= 0);
            CHECK(ch.jointIndex < (i32)skel.joints.size());
        }
    }

    // 旋转关键帧为归一化四元数
    for (auto& clip : skel.clips) {
        for (auto& ch : clip.channels) {
            for (auto& q : ch.rotations) {
                CHECK(glm::length(q) == doctest::Approx(1.0f).epsilon(0.01));
            }
        }
    }
}

TEST_CASE("glTFLoader 静态模型无骨架与失败容错") {
    // Sponza（静态模型，无 skins）→ skeletons 为空
    World world;
    SceneGraph sg(world);
    glTFResult r = LoadGLTF(world, sg, AssetPath("gltf/Sponza/glTF/Sponza.gltf"));
    REQUIRE(r.success == true);
    CHECK(r.skeletons.empty());

    // 不存在的文件 → 失败 + skeletons 为空
    World world2;
    SceneGraph sg2(world2);
    glTFResult bad = LoadGLTF(world2, sg2, AssetPath("Models/Skeletal/NotExist.gltf"));
    CHECK(bad.success == false);
    CHECK(bad.skeletons.empty());
}
