// ============================================================
// TestFrameSceneSnapshot.cpp — 阶段 1 T1.1：快照定义（布局一致性与值语义）
//
// 【为什么要有它】快照的全部价值有两条：**布局与着色器一致**（否则渲染线程 memcpy 出来的
// 光源是垃圾）与**只有值没有指针**（否则"不可变"是假的）。第一条已经在头文件里用逐字段
// `static_assert` 钉死，本文件把能在运行期验证的部分补齐：
//   ① `SnapshotLight` 与 `GPULight` 的大小/读回逐位一致（`ToGpu`/`FromGpu` 往返）；
//   ② 快照可以整份移动（交接语义），移动后内容不丢；
//   ③ `Clear()` 保留容量（每帧复用不重新分配）；
//   ④ `SnapshotDrawItem` 可平凡拷贝（渲染线程要整块上传）。
// ============================================================
#include "Threading/FrameSceneSnapshot.h"

#include <doctest/doctest.h>

#include <cstring>
#include <utility>

using namespace he;
using namespace he::render;

TEST_CASE("FrameSceneSnapshot：SnapshotLight 与 GPULight 布局一致且逐位往返") {
    CHECK(sizeof(SnapshotLight) == sizeof(GPULight));   // 与头文件 static_assert 互相印证

    GPULight gpu{};
    gpu.colorIntensity = float4(0.25f, 0.5f, 0.75f, 12.0f);
    gpu.directionType  = float4(-0.3f, -1.0f, 0.2f, 2.0f);   // Spot
    gpu.positionRange  = float4(3.0f, 4.0f, 5.0f, -8.5f);    // 负范围 = 物理模式标记
    gpu.coneAngles     = float2(0.2f, 0.45f);
    gpu.shadowIndex    = 3;
    gpu.shadowRadius   = 0.35f;

    const SnapshotLight snap = SnapshotLight::FromGpu(gpu);
    CHECK(snap.colorIntensity.x == 0.25f);
    CHECK(snap.directionType.w == 2.0f);
    CHECK(snap.positionRange.w == -8.5f);      // 负极值必须原样保留（物理模式靠它判断）
    CHECK(snap.coneAngles.y == 0.45f);
    CHECK(snap.shadowIndex == 3);
    CHECK(snap.shadowRadius == 0.35f);

    const GPULight back = snap.ToGpu();
    CHECK(std::memcmp(&gpu, &back, sizeof(GPULight)) == 0);   // 整块逐位一致
}

TEST_CASE("FrameSceneSnapshot：整份移动后内容不丢（交接语义）") {
    FrameSceneSnapshot a;
    a.frameIndex   = 42;
    a.frameSlot    = 1;
    a.deltaTime    = 0.016f;
    a.viewportWidth  = 1920;
    a.viewportHeight = 1080;
    a.sourceWorldVersion = 7;

    SnapshotDrawItem draw;
    draw.objectID  = 11;
    draw.meshIndex = 2;
    draw.object.worldMatrix = float4x4(2.0f);
    a.draws.push_back(draw);

    SnapshotLight light;
    light.shadowIndex = -1;
    a.lights.push_back(light);
    a.skinMatrices.push_back(float4x4(1.0f));

    CHECK_FALSE(a.IsEmpty());
    const auto* movedDraws = a.draws.data();          // 记下缓冲地址以核对"移动而非拷贝"

    FrameSceneSnapshot b = std::move(a);
    CHECK(b.frameIndex == 42u);
    CHECK(b.frameSlot == 1u);
    CHECK(b.viewportWidth == 1920u);
    CHECK(b.sourceWorldVersion == 7u);
    REQUIRE(b.draws.size() == 1u);
    CHECK(b.draws[0].objectID == 11u);
    CHECK(b.draws[0].meshIndex == 2u);
    CHECK(b.draws[0].object.worldMatrix[0][0] == 2.0f);
    CHECK(b.draws.data() == movedDraws);              // vector 缓冲被移走（没有整份深拷贝）
    CHECK(b.lights.size() == 1u);
    CHECK(b.skinMatrices.size() == 1u);
}

TEST_CASE("FrameSceneSnapshot：Clear 保留容量，可逐帧复用") {
    FrameSceneSnapshot snap;
    snap.Reserve(64u, 16u, 8u);

    for (int frame = 0; frame < 3; ++frame) {
        snap.Clear();                                  // 每帧开头复用
        CHECK(snap.IsEmpty());
        for (u32 i = 0; i < 64u; ++i) snap.draws.push_back(SnapshotDrawItem{});
        for (u32 i = 0; i < 16u; ++i) snap.lights.push_back(SnapshotLight{});
        for (u32 i = 0; i < 8u; ++i)  snap.skinMatrices.push_back(float4x4(1.0f));
        CHECK(snap.draws.size() == 64u);
    }
    CHECK(snap.draws.capacity() >= 64u);               // 容量保留（没有反复分配）
    snap.Clear();
    CHECK(snap.draws.empty());
    CHECK(snap.draws.capacity() >= 64u);
    CHECK(snap.IsEmpty());
}

TEST_CASE("FrameSceneSnapshot：元素可平凡拷贝（渲染线程整块上传的前提）") {
    SnapshotDrawItem src;
    src.objectID  = 99;
    src.meshIndex = 5;
    src.object.boundsMin = float4(-1.0f, -2.0f, -3.0f, 0.0f);
    src.prevWorldMatrix  = float4x4(3.0f);

    SnapshotDrawItem dst;
    std::memcpy(&dst, &src, sizeof(SnapshotDrawItem));   // 平凡 ⇒ memcpy 合法
    CHECK(dst.objectID == 99u);
    CHECK(dst.meshIndex == 5u);
    CHECK(dst.object.boundsMin.y == -2.0f);
    CHECK(dst.prevWorldMatrix[1][1] == 3.0f);
    CHECK(std::memcmp(&src, &dst, sizeof(SnapshotDrawItem)) == 0);
}
