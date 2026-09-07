// ============================================================
// Physics/PhysicsSystem.cpp — 刚体物理系统实现（C2 T4，★关键路径）
//
// Jolt 单例（Factory 等）进程内已由 PhysicsWorld::Initialize 注册。
// 全局物理世界 + Entity→BodyID 映射（MVP 单 world）。
// ============================================================

#include "Physics/PhysicsSystem.h"

#include "Physics/PhysicsWorld.h"
#include "Physics/JoltConversions.h"
#include "Physics/RigidBodyComponent.h"

#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Scene/Transform.h"
#include "Scene/CollisionComponent.h"

#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>

#include <unordered_map>

namespace he::physics {

namespace {
    // 全局物理世界（MVP 单 world；多 world 需 per-world 容器，留后续）
    PhysicsWorld s_World;
    std::unordered_map<he::Entity, JPH::BodyID> s_Bodies;
    float s_Accumulator = 0.0f;

    constexpr f32 kFixedDt   = 1.0f / 120.0f;   // 固定步长
    constexpr int kMaxSteps  = 8;               // 单帧最大步数（防 spiral of death）
    constexpr f32 kMaxAccum  = kFixedDt * kMaxSteps;

    // 按 RigidBodyComponent.shape 创建 Jolt 形状（0=球 1=盒 2=胶囊）
    JPH::Ref<JPH::Shape> CreateShape(const RigidBodyComponent& rb) {
        switch (rb.shape) {
        case 1: return new JPH::BoxShape(JPH::Vec3(rb.halfExtent, rb.halfExtent, rb.halfExtent));
        case 2: return new JPH::CapsuleShape(rb.height * 0.5f, rb.radius);
        default: return new JPH::SphereShape(rb.radius);
        }
    }
} // namespace

void PhysicsSystem::Update(he::World& world, he::SceneGraph&, f32 dt) {
    if (dt <= 0.0f) return;
    if (!s_World.IsReady()) s_World.Initialize();
    auto& bi = s_World.GetBodyInterface();

    // ---- 1. 新增动态 body（实体有 RigidBodyComponent 但未入物理世界）----
    world.ForEach<RigidBodyComponent>([&](he::Entity e, RigidBodyComponent& rb) {
        if (s_Bodies.count(e)) return;   // 已在物理世界
        auto* xf = world.GetComponent<TransformComponent>(e);
        if (!xf) return;

        auto shape = CreateShape(rb);
        JPH::BodyCreationSettings bcs(shape, ToJoltPos(xf->position), ToJolt(xf->rotation),
            rb.isDynamic ? JPH::EMotionType::Dynamic : JPH::EMotionType::Static,
            rb.isDynamic ? Layers::MOVING : Layers::NON_MOVING);
        if (rb.isDynamic) {
            bcs.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
            bcs.mMassPropertiesOverride.mMass = rb.mass;
        }
        bcs.mFriction        = rb.friction;
        bcs.mRestitution     = rb.restitution;
        bcs.mLinearDamping   = rb.linearDamping;
        bcs.mAngularDamping  = rb.angularDamping;

        JPH::Body* body = bi.CreateBody(bcs);
        if (!body) return;
        body->SetUserData(e.id);   // 记录 Entity 用于回写/回收
        bi.AddBody(body->GetID(), rb.isDynamic ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
        s_Bodies[e] = body->GetID();
    });

    // ---- 1b. 新增静态碰撞体（T5：带 CollisionComponent 且无 RigidBodyComponent → Jolt static body）----
    world.ForEach<CollisionComponent>([&](he::Entity e, CollisionComponent& cc) {
        if (!cc.bEnabled) return;                                        // 禁用
        if (s_Bodies.count(e)) return;                                   // 已在物理世界
        if (world.GetComponent<RigidBodyComponent>(e)) return;           // 动态刚体走 RigidBody 路径
        auto* xf = world.GetComponent<TransformComponent>(e);
        if (!xf) return;

        JPH::Ref<JPH::Shape> shape;
        switch (cc.shape) {
        case CollisionShape::Sphere:
            shape = new JPH::SphereShape(cc.radius);
            break;
        case CollisionShape::Capsule: {
            float halfH = std::max(cc.height * 0.5f - cc.radius, 0.01f);   // 段半长（Jolt 需 >0）
            shape = new JPH::CapsuleShape(halfH, cc.radius);
            break;
        }
        default:   // AABB
            shape = new JPH::BoxShape(ToJolt(cc.halfExtents));
            break;
        }

        JPH::BodyCreationSettings bcs(shape, ToJoltPos(xf->position), ToJolt(xf->rotation),
            JPH::EMotionType::Static, Layers::NON_MOVING);
        JPH::Body* body = bi.CreateBody(bcs);
        if (!body) return;
        body->SetUserData(e.id);
        bi.AddBody(body->GetID(), JPH::EActivation::DontActivate);
        s_Bodies[e] = body->GetID();
    });

    // ---- 2. 固定步长 Step（accumulator）----
    s_Accumulator += dt;
    f32 clampedAccum = s_Accumulator > kMaxAccum ? kMaxAccum : s_Accumulator;   // 防 spiral
    int steps = 0;
    while (clampedAccum >= kFixedDt && steps < kMaxSteps) {
        s_World.Step(kFixedDt);
        clampedAccum -= kFixedDt;
        ++steps;
    }
    s_Accumulator = clampedAccum;

    // ---- 3. 回写 Transform（激活动态 body 才回写；静态在创建时已就位）----
    for (auto it = s_Bodies.begin(); it != s_Bodies.end(); ) {
        const he::Entity e = it->first;
        const JPH::BodyID bid = it->second;
        auto* xf = world.GetComponent<TransformComponent>(e);
        if (!xf) {
            // 实体已销毁 → 回收 body 并清映射
            bi.RemoveBody(bid); bi.DestroyBody(bid);
            it = s_Bodies.erase(it);
            continue;
        }
        // 通过 BodyInterface 查询当前 world 变换（动态激活才回写）
        if (bi.IsActive(bid)) {   // 动态激活才回写；静态（DontActivate）保持创建时位置
            xf->position = ToGlmPos(bi.GetCenterOfMassPosition(bid));
            xf->rotation = ToGlm(bi.GetRotation(bid));
        }
        ++it;
    }

    // ---- 4. 清理已禁用（enabled=false）的 body ----（MVP：冻结——置休眠/移除由调用方处理）
}

bool PhysicsSystem::HasBody(he::World&, he::Entity e) {
    return s_Bodies.count(e) != 0;
}

} // namespace he::physics
