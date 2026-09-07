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
// PhysicsWorld — Jolt 世界封装
// ============================================================
class PhysicsWorld {
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
