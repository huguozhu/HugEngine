#pragma once

// ============================================================
// Physics/PhysicsWorld.h — Jolt PhysicsSystem 封装（C2 T2）
//
// 封装 JPH::PhysicsSystem 的初始化/步进/关闭 + 碰撞层接口。
// 引擎侧通过本类驱动物理世界（固定步长由 PhysicsSystem 处理）。
// ============================================================

#include "Core/Types.h"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>

#include <memory>

namespace he::physics {

/// 物理世界初始化描述（maxBodies / maxBodyPairs / maxContactConstraints）
struct PhysicsInitDesc {
    u32 maxBodies              = 1024;
    u32 maxBodyPairs           = 65536;
    u32 maxContactConstraints  = 10240;
    float gravity              = -9.81f;
};

// ============================================================
// 碰撞层：0=静态(非移动) 1=动态(移动)
// ============================================================
namespace Layers {
    constexpr JPH::ObjectLayer NON_MOVING = 0;
    constexpr JPH::ObjectLayer MOVING     = 1;
}
namespace BroadPhaseLayers {
    constexpr JPH::BroadPhaseLayer NON_MOVING(0);
    constexpr JPH::BroadPhaseLayer MOVING(1);
}

/// 宽相层映射：对象层 → 宽相层
class BPLayerInterfaceImpl final : public JPH::BroadPhaseLayerInterface {
public:
    BPLayerInterfaceImpl() {
        mObjectToBroadPhase[Layers::NON_MOVING] = BroadPhaseLayers::NON_MOVING;
        mObjectToBroadPhase[Layers::MOVING]     = BroadPhaseLayers::MOVING;
    }
    unsigned int GetNumBroadPhaseLayers() const override { return 2; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer inLayer) const override { return mObjectToBroadPhase[inLayer]; }
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer inLayer) const override {
        return inLayer == BroadPhaseLayers::NON_MOVING ? "NON_MOVING" : "MOVING";
    }
private:
    JPH::BroadPhaseLayer mObjectToBroadPhase[2];
};

/// 对象层 × 宽相层过滤：静态只与动态碰撞；动态与两者都碰撞
class ObjectVsBroadPhaseLayerFilterImpl final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer inLayer1, JPH::BroadPhaseLayer inLayer2) const override {
        if (inLayer1 == Layers::NON_MOVING) return inLayer2 == BroadPhaseLayers::MOVING;
        return true;
    }
};

/// 对象层 × 对象层过滤：静态之间不碰撞
class ObjectLayerPairFilterImpl final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer inLayer1, JPH::ObjectLayer inLayer2) const override {
        if (inLayer1 == Layers::NON_MOVING && inLayer2 == Layers::NON_MOVING) return false;
        return true;
    }
};

// ============================================================
// JoltRuntimeGuard — 保证 Jolt 的全局分配器 / Factory 在**任何 Jolt 对象构造之前**就绪
//
// 【为什么需要它，以及为什么必须是基类】（D1 / 任务 2 的根因）
//   Jolt 的 `JPH::Allocate` / `JPH::Free` / `Factory::sInstance` 都是**全局函数指针**，由
//   `RegisterDefaultAllocator()` + `new Factory` 建立。它们未建立时是 nullptr，而 Jolt 的
//   `JPH_OVERRIDE_NEW_DELETE` 宏让大量 Jolt 类型的 `new/delete` **直接调用这些指针**。
//   本工程的注册原先只发生在 `PhysicsWorld::Initialize()` 里（惰性），而
//   `he::physics` 的 `s_World` 是一个**进程级静态对象**（`Physics/PhysicsSystem.cpp`）：
//   它的析构在进程退出时必然执行，**与 Initialize 有没有被调用无关**。于是"从没初始化过
//   物理"的运行（例如只跑 GI 单测、`--no-run`）在退出时走到 `~JPH::PhysicsSystem` 里的
//   `delete[]`，经宏调用**空的 `JPH::Free`** ⇒ 跳到地址 0（0xC0000005、RIP=0、目标地址 0）。
//   实测指纹：单跑任意一个用例都崩（161/161）、整包跑全部用例不崩（因为其中有用例会调用
//   `PhysicsSystem::Update` 顺带完成注册）。
//   【为什么用基类】成员的构造发生在构造函数体之前，所以"在构造函数体里注册"来不及；
//   基类先于成员构造，这是唯一能保证顺序的写法。
// ============================================================
struct JoltRuntimeGuard {
    JoltRuntimeGuard();
};

// ============================================================
// PhysicsWorld — Jolt 世界封装
// ============================================================
class PhysicsWorld : private JoltRuntimeGuard {
public:
    bool Initialize(const PhysicsInitDesc& desc = {});
    void Step(f32 fixedDt);   // 内部 JPH::PhysicsSystem::Update
    void Shutdown();

    JPH::PhysicsSystem&       GetSystem()             { return m_System; }
    JPH::BodyInterface&       GetBodyInterface()      { return m_System.GetBodyInterface(); }
    JPH::TempAllocator*       GetTempAllocator()      { return m_TempAllocator.get(); }
    JPH::JobSystem*           GetJobSystem()          { return m_JobSystem.get(); }
    bool                      IsReady() const         { return m_Ready; }

private:
    JPH::PhysicsSystem        m_System;
    BPLayerInterfaceImpl      m_BPLayerInterface;
    ObjectVsBroadPhaseLayerFilterImpl m_ObjectVsBroadPhaseFilter;
    ObjectLayerPairFilterImpl m_ObjectLayerPairFilter;

    std::unique_ptr<JPH::JobSystemThreadPool> m_JobSystem;   // 2 工作线程
    std::unique_ptr<JPH::TempAllocatorImpl>   m_TempAllocator;   // 10MB
    bool m_Ready = false;
};

} // namespace he::physics
