// ============================================================
// Tests/TestJoltSmoke.cpp — Jolt Physics 集成冒烟（T1）
//
// 目的：验证 Jolt 引入后最小物理链路可用：
//   Factory 注册 → PhysicsSystem 初始化 → 静态地面 + 动态球
//   → 固定步长 Step → 自由落体位移 ≈ ½gt²（坐标系/单位/重力正确）
// 不依赖引擎任何模块（仅链接 Jolt），先于 Engine/Physics 落地。
// ============================================================

#include "doctest.h"

// Jolt 头文件约定：必须先包含 Jolt.h
#include <Jolt/Jolt.h>

// 核心与物理系统头
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/Memory.h>                 // RegisterDefaultAllocator（自定义分配器路径必需）
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>

#include <cmath>

using namespace JPH;

namespace {

// --- 对象层：0=静态(非移动) 1=动态(移动) ---
namespace Layers {
    constexpr ObjectLayer NON_MOVING = 0;
    constexpr ObjectLayer MOVING     = 1;
}

namespace BroadPhaseLayers {
    constexpr BroadPhaseLayer NON_MOVING(0);
    constexpr BroadPhaseLayer MOVING(1);
}

// 宽相层映射：对象层 → 宽相层
class BPLayerInterfaceImpl final : public BroadPhaseLayerInterface {
public:
    BPLayerInterfaceImpl() {
        mObjectToBroadPhase[Layers::NON_MOVING] = BroadPhaseLayers::NON_MOVING;
        mObjectToBroadPhase[Layers::MOVING]     = BroadPhaseLayers::MOVING;
    }
    uint GetNumBroadPhaseLayers() const override { return 2; }
    BroadPhaseLayer GetBroadPhaseLayer(ObjectLayer inLayer) const override { return mObjectToBroadPhase[inLayer]; }
    // Profiler 启用时 Jolt 要求提供层名（供调试输出）
    const char* GetBroadPhaseLayerName(BroadPhaseLayer inLayer) const override {
        return inLayer == BroadPhaseLayers::NON_MOVING ? "NON_MOVING" : "MOVING";
    }
private:
    BroadPhaseLayer mObjectToBroadPhase[2];
};

// 对象层 × 宽相层过滤：静态只与动态碰撞；动态与两者都碰撞
class ObjectVsBroadPhaseLayerFilterImpl final : public ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(ObjectLayer inLayer1, BroadPhaseLayer inLayer2) const override {
        if (inLayer1 == Layers::NON_MOVING) return inLayer2 == BroadPhaseLayers::MOVING;
        return true; // MOVING 与静态/动态都碰撞
    }
};

// 对象层 × 对象层过滤
class ObjectLayerPairFilterImpl final : public ObjectLayerPairFilter {
public:
    bool ShouldCollide(ObjectLayer inLayer1, ObjectLayer inLayer2) const override {
        if (inLayer1 == Layers::NON_MOVING && inLayer2 == Layers::NON_MOVING) return false; // 静态之间不碰撞
        return true;
    }
};

// Jolt 单例（Factory）进程内只注册一次；doctest 多个 TEST_CASE 共享
void EnsureJoltRegistered() {
    static bool s_Registered = [] {
        RegisterDefaultAllocator();             // 必须先注册默认分配器（自定义分配器路径）
        Factory::sInstance = new Factory();
        RegisterTypes();
        return true;
    }();
    (void)s_Registered;
}

} // namespace

// ============================================================
// 冒烟：球从 10m 高处自由下落 0.5s，y ≈ 10 - ½·9.81·0.5²
// ============================================================
TEST_CASE("Jolt 冒烟：自由落体符合 ½gt²") {
    EnsureJoltRegistered();

    // 简化碰撞层实现（见文件顶部匿名命名空间）
    BPLayerInterfaceImpl broadPhaseLayerInterface;
    ObjectVsBroadPhaseLayerFilterImpl objectVsBroadPhaseFilter;
    ObjectLayerPairFilterImpl objectLayerPairFilter;

    // 线程池作业系统（2 工作线程；注：JobSystemSingleThreaded 在本环境会导致 Update 挂起）
    JobSystemThreadPool jobSystem(1024, 8, 2);
    // 每步窄相检测用的临时分配器（10MB）
    TempAllocatorImpl tempAllocator(10 * 1024 * 1024);

    // 物理世界：上限保守，测试够用即可
    PhysicsSystem physics;
    physics.Init(1024, 0, 65536, 10240, broadPhaseLayerInterface,
                 objectVsBroadPhaseFilter, objectLayerPairFilter);
    physics.SetGravity(Vec3(0.0f, -9.81f, 0.0f));

    BodyInterface& bodyInterface = physics.GetBodyInterface();

    // --- 静态地面：Box(half=100,0.5,100)，中心 y=-1 → 顶面 y=-0.5 ---
    BoxShapeSettings groundSettings(Vec3(100.0f, 0.5f, 100.0f));
    groundSettings.SetEmbedded();   // 形状内嵌在创建结构里，免手动释放
    ShapeSettings::ShapeResult groundResult = groundSettings.Create();
    REQUIRE(groundResult.IsValid());
    BodyCreationSettings groundCreation(groundResult.Get(), RVec3(0.0f, -1.0f, 0.0f), Quat::sIdentity(),
                                        EMotionType::Static, Layers::NON_MOVING);
    Body* ground = bodyInterface.CreateBody(groundCreation);
    REQUIRE(ground != nullptr);
    bodyInterface.AddBody(ground->GetID(), EActivation::DontActivate);

    // --- 动态球：半径 0.5，质量 1kg，初始 y=10（自由下落 0.5s 不会触地）---
    SphereShapeSettings ballSettings(0.5f);
    ballSettings.SetEmbedded();
    ShapeSettings::ShapeResult ballResult = ballSettings.Create();
    REQUIRE(ballResult.IsValid());
    BodyCreationSettings ballCreation(ballResult.Get(), RVec3(0.0f, 10.0f, 0.0f), Quat::sIdentity(),
                                      EMotionType::Dynamic, Layers::MOVING);
    ballCreation.mOverrideMassProperties = EOverrideMassProperties::CalculateInertia;
    ballCreation.mMassPropertiesOverride.mMass = 1.0f;
    Body* ball = bodyInterface.CreateBody(ballCreation);
    REQUIRE(ball != nullptr);
    bodyInterface.AddBody(ball->GetID(), EActivation::Activate);

    // --- 固定步长 1/60s，模拟 0.5s（30 步）---
    // Jolt v5.6 签名：Update(dt, collisionSteps, tempAllocator, jobSystem)
    const float fixedDt = 1.0f / 60.0f;
    for (int i = 0; i < 30; ++i)
        physics.Update(fixedDt, 1, &tempAllocator, &jobSystem);

    // 解析解：y = 10 - ½·9.81·0.5² ≈ 8.774；容差 ±0.2（数值积分误差）
    RVec3 pos = bodyInterface.GetCenterOfMassPosition(ball->GetID());
    float expectedY = 10.0f - 0.5f * 9.81f * 0.5f * 0.5f;
    CHECK(std::fabs((float)pos.GetY() - expectedY) < 0.2f);
    CHECK((float)pos.GetX() < 0.01f);   // 水平方向不应漂移
    CHECK((float)pos.GetZ() < 0.01f);

    // 继续模拟至触地：球中心落至 y≈0（地面顶面 -0.5 + 球半径 0.5）静止
    for (int i = 0; i < 600; ++i)
        physics.Update(fixedDt, 1, &tempAllocator, &jobSystem);
    pos = bodyInterface.GetCenterOfMassPosition(ball->GetID());
    CHECK(std::fabs((float)pos.GetY() - 0.0f) < 0.1f);          // 落地静止
    CHECK(bodyInterface.IsActive(ball->GetID()) == false);      // 休眠（不再移动）

    // --- 清理：删体（物理系统析构前）---
    bodyInterface.RemoveBody(ball->GetID());
    bodyInterface.DestroyBody(ball->GetID());
    bodyInterface.RemoveBody(ground->GetID());
    bodyInterface.DestroyBody(ground->GetID());
}
