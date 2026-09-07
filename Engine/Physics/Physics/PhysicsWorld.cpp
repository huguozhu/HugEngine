// ============================================================
// Physics/PhysicsWorld.cpp — Jolt PhysicsSystem 封装实现（C2 T2）
// ============================================================

#include "Physics/PhysicsWorld.h"

#include "Core/Log.h"

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/Memory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/RegisterTypes.h>

#include <memory>

namespace he::physics {

// Jolt 单例（Factory）进程内只注册一次
static void EnsureJoltRegistered() {
    static bool s_Registered = [] {
        JPH::RegisterDefaultAllocator();
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
        return true;
    }();
    (void)s_Registered;
}

bool PhysicsWorld::Initialize(const PhysicsInitDesc& desc) {
    if (m_Ready) return true;
    EnsureJoltRegistered();

    m_JobSystem     = std::make_unique<JPH::JobSystemThreadPool>(1024, 8, 2);
    m_TempAllocator = std::make_unique<JPH::TempAllocatorImpl>(10 * 1024 * 1024);

    m_System.Init(desc.maxBodies, 0, desc.maxBodyPairs, desc.maxContactConstraints,
                  m_BPLayerInterface, m_ObjectVsBroadPhaseFilter, m_ObjectLayerPairFilter);
    m_System.SetGravity(JPH::Vec3(0.0f, desc.gravity, 0.0f));

    m_Ready = true;
    HE_CORE_INFO("PhysicsWorld initialized (bodies={}, pairs={})", desc.maxBodies, desc.maxBodyPairs);
    return true;
}

void PhysicsWorld::Step(f32 fixedDt) {
    if (!m_Ready || fixedDt <= 0.0f) return;
    m_System.Update(fixedDt, 1, m_TempAllocator.get(), m_JobSystem.get());
}

void PhysicsWorld::Shutdown() {
    if (!m_Ready) return;
    // body 由 PhysicsSystem（T4）负责销毁；Jolt PhysicsSystem 析构自动清理内部资源，
    // 此处仅释放 JobSystem / TempAllocator（它们独立于 PhysicsSystem 生命期）。
    m_JobSystem.reset();
    m_TempAllocator.reset();
    m_Ready = false;
    HE_CORE_INFO("PhysicsWorld shutdown");
}

} // namespace he::physics
