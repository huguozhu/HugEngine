// ============================================================
// SceneReflect.cpp — Scene 组件反射注册
//
// 为 HE_COMPONENT() 标记的类提供 StaticClass() 定义
// ============================================================

#include "Scene/Component.h"
#include "Scene/Transform.h"
#include "Scene/MeshComponent.h"
#include "Scene/CubeComponent.h"
#include "Scene/SphereComponent.h"
#include "Scene/LightComponent.h"
#include "Scene/SkyboxComponent.h"
#include "Scene/PhysicalSkyComponent.h"
#include "Scene/CameraComponent.h"
#include "Scene/AnimationComponent.h"
#include "Scene/LevelComponent.h"
#include "Scene/ParticleComponent.h"
#include "Scene/ProjectileMovementComponent.h"
#include "Scene/HealthComponent.h"
#include "Scene/SpringArmComponent.h"
#include "Scene/BillboardComponent.h"
#include "Scene/TextRenderComponent.h"
#include "Scene/DecalComponent.h"
#include "Scene/CollisionComponent.h"
#include "Scene/CharacterMovementComponent.h"
#include "Scene/AbilityComponent.h"
#include "Scene/SplineComponent.h"
#include "Scene/InstancedMeshComponent.h"
#include "Scene/SkeletalMeshComponent.h"

namespace he {

// --- Component 基类注册 ---
HE_BEGIN_REGISTER(he::Component)
HE_END_REGISTER()

// --- TransformComponent 注册 ---
// 示范 AI 注解：AI_VISIBLE(进世界模型快照) + AI_WRITABLE(允许 AI 写) + AI_DESCRIPTION(LLM 说明)
HE_BEGIN_REGISTER(he::TransformComponent)
    HE_REGISTER_PROPERTY(he::TransformComponent, float3, position)
        HE_ATTR_CATEGORY("Transform") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("世界空间位置，单位米")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::TransformComponent, quat, rotation)
        HE_ATTR_CATEGORY("Transform") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("旋转四元数")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::TransformComponent, float3, scale)
        HE_ATTR_CATEGORY("Transform") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("三轴缩放")
    HE_END_PROPERTY()
HE_END_REGISTER()

// --- MeshComponent 注册 ---
HE_BEGIN_REGISTER(he::MeshComponent)
HE_END_REGISTER()

// --- CubeComponent 注册 ---
HE_BEGIN_REGISTER(he::CubeComponent)
HE_END_REGISTER()

// --- SphereComponent 注册 ---
HE_BEGIN_REGISTER(he::SphereComponent)
HE_END_REGISTER()

// --- LightComponent + 子类注册 ---
HE_BEGIN_REGISTER(he::LightComponent)
HE_END_REGISTER()

HE_BEGIN_REGISTER(he::DirectionalLight)
    HE_REGISTER_PROPERTY(he::DirectionalLight, float3, direction)
        HE_ATTR_CATEGORY("Light") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("光线方向（世界空间）")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::DirectionalLight, float3, color)
        HE_ATTR_CATEGORY("Light") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("光照颜色 [r,g,b] 0~1")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::DirectionalLight, float, intensity)
        HE_ATTR_CATEGORY("Light") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("光照强度（非物理模式乘数）")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::DirectionalLight, bool, castShadow)
        HE_ATTR_CATEGORY("Light") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("是否投射阴影")
    HE_END_PROPERTY()
HE_END_REGISTER()

HE_BEGIN_REGISTER(he::PointLight)
    HE_REGISTER_PROPERTY(he::PointLight, float3, color)
        HE_ATTR_CATEGORY("Light") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("光照颜色 [r,g,b] 0~1")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::PointLight, float, intensity)
        HE_ATTR_CATEGORY("Light") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("光照强度")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::PointLight, float, range)
        HE_ATTR_CATEGORY("Light") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("影响范围，单位米")
    HE_END_PROPERTY()
HE_END_REGISTER()

// --- SpotLight 注册（S0.1：补齐反射 + AI 注解，编辑器 Details 与 LLM 世界模型立即可用）---
// color/intensity/castShadow 为 LightComponent 基类成员，offsetof 对继承公有成员同样有效
HE_BEGIN_REGISTER(he::SpotLight)
    HE_REGISTER_PROPERTY(he::SpotLight, float3, direction)
        HE_ATTR_CATEGORY("Light") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("聚光锥轴方向（世界空间）")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::SpotLight, float3, color)
        HE_ATTR_CATEGORY("Light") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("光照颜色 [r,g,b] 0~1")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::SpotLight, float, intensity)
        HE_ATTR_CATEGORY("Light") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("光照强度")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::SpotLight, float, range)
        HE_ATTR_CATEGORY("Light") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("影响范围（米）")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::SpotLight, float, innerConeAngle)
        HE_ATTR_CATEGORY("Light") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("内锥角（弧度，全亮区）")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::SpotLight, float, outerConeAngle)
        HE_ATTR_CATEGORY("Light") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("外锥角（弧度，衰减边缘）")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::SpotLight, bool, castShadow)
        HE_ATTR_CATEGORY("Light") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("是否投射阴影")
    HE_END_PROPERTY()
HE_END_REGISTER()

// --- RectLight 注册（S0.3 词表对齐：补 color/intensity/castShadow/softness，全部 AI 一等公民）---
HE_BEGIN_REGISTER(he::RectLight)
    HE_REGISTER_PROPERTY(he::RectLight, float, width)
        HE_ATTR_CATEGORY("Light") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("矩形宽度（米）")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::RectLight, float, height)
        HE_ATTR_CATEGORY("Light") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("矩形高度（米）")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::RectLight, float3, normal)
        HE_ATTR_CATEGORY("Light") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("发光面朝向法线")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::RectLight, float3, color)
        HE_ATTR_CATEGORY("Light") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("光照颜色 [r,g,b] 0~1")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::RectLight, float, intensity)
        HE_ATTR_CATEGORY("Light") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("光照强度")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::RectLight, float, range)
        HE_ATTR_CATEGORY("Light") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("影响范围（米）")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::RectLight, float, softness)
        HE_ATTR_CATEGORY("Light") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("软阴影系数 [0,1]（PCF 半径缩放）")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::RectLight, bool, castShadow)
        HE_ATTR_CATEGORY("Light") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("是否投射阴影")
    HE_END_PROPERTY()
HE_END_REGISTER()

HE_BEGIN_REGISTER(he::SkyboxComponent)
HE_END_REGISTER()

HE_BEGIN_REGISTER(he::PhysicalSkyComponent)
HE_END_REGISTER()

// --- CameraComponent 注册（S0.2：补齐反射 + AI 注解）---
// isMain 仅 AI_VISIBLE：AI 可读主相机标记但不可改（主相机归属场景作者决定）
HE_BEGIN_REGISTER(he::CameraComponent)
    HE_REGISTER_PROPERTY(he::CameraComponent, float, fov)
        HE_ATTR_CATEGORY("Camera") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("垂直视场角（度，默认 60）")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::CameraComponent, float, nearPlane)
        HE_ATTR_CATEGORY("Camera") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("近裁剪面距离")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::CameraComponent, float, farPlane)
        HE_ATTR_CATEGORY("Camera") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("远裁剪面距离")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::CameraComponent, bool, isMain)
        HE_ATTR_CATEGORY("Camera") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_DESCRIPTION("是否为主相机（场景中仅一个为 true）")
    HE_END_PROPERTY()
HE_END_REGISTER()

HE_BEGIN_REGISTER(he::AnimationComponent)
HE_END_REGISTER()

HE_BEGIN_REGISTER(he::LevelComponent)
HE_END_REGISTER()

HE_BEGIN_REGISTER(he::ParticleComponent)
HE_END_REGISTER()

// --- ProjectileMovementComponent 注册（Phase A7，玩法底座）---
// 实体引用（homingTarget）不注册：反射序列化不支持 u64 实体 ID，追踪目标由代码设置
HE_BEGIN_REGISTER(he::ProjectileMovementComponent)
    HE_REGISTER_PROPERTY(he::ProjectileMovementComponent, float, initialSpeed)
        HE_ATTR_CATEGORY("Projectile") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("初速（米/秒，方向 = 实体前向）")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::ProjectileMovementComponent, float, maxSpeed)
        HE_ATTR_CATEGORY("Projectile") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("最大速度（米/秒，0 = 不限速）")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::ProjectileMovementComponent, float, gravityScale)
        HE_ATTR_CATEGORY("Projectile") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("重力倍率（0 = 无重力直线运动）")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::ProjectileMovementComponent, bool, bHoming)
        HE_ATTR_CATEGORY("Projectile") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("是否追踪目标")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::ProjectileMovementComponent, float, lifetime)
        HE_ATTR_CATEGORY("Projectile") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("生命周期（秒，超时自动销毁；0 = 无限）")
    HE_END_PROPERTY()
HE_END_REGISTER()

// --- HealthComponent 注册（Phase A8，玩法底座：LLM 可"给敌人 100 点血"）---
HE_BEGIN_REGISTER(he::HealthComponent)
    HE_REGISTER_PROPERTY(he::HealthComponent, float, maxHealth)
        HE_ATTR_CATEGORY("Health") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("最大生命值")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::HealthComponent, float, currentHealth)
        HE_ATTR_CATEGORY("Health") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("当前生命值（0 = 死亡）")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::HealthComponent, bool, bInvincible)
        HE_ATTR_CATEGORY("Health") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("是否无敌（免疫伤害）")
    HE_END_PROPERTY()
HE_END_REGISTER()

// --- SpringArmComponent 注册（Phase A6，第三人称相机）---
// 实体引用（targetEntity/cameraEntity）不注册：由代码/场景装配时设置
HE_BEGIN_REGISTER(he::SpringArmComponent)
    HE_REGISTER_PROPERTY(he::SpringArmComponent, float3, targetOffset)
        HE_ATTR_CATEGORY("SpringArm") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("锚点偏移（目标局部空间，米）")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::SpringArmComponent, float, armLength)
        HE_ATTR_CATEGORY("SpringArm") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("弹簧臂长度（米）")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::SpringArmComponent, float, rotationLagSpeed)
        HE_ATTR_CATEGORY("SpringArm") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("旋转滞后速度（越大跟随越快，0 = 硬跟随）")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::SpringArmComponent, bool, bUsePawnControlRotation)
        HE_ATTR_CATEGORY("SpringArm") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("相机朝向是否跟随目标朝向")
    HE_END_PROPERTY()
HE_END_REGISTER()

// --- BillboardComponent 注册（Phase A4）---
// baseColorFactor/baseColorTexture 为 MeshComponent 基类成员（offsetof 对继承公有成员有效）
HE_BEGIN_REGISTER(he::BillboardComponent)
    HE_REGISTER_PROPERTY(he::BillboardComponent, float2, size)
        HE_ATTR_CATEGORY("Billboard") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("广告牌尺寸（世界单位，X=右，Y=上）")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::BillboardComponent, float4, baseColorFactor)
        HE_ATTR_CATEGORY("Billboard") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("颜色 [r,g,b,a] 0~1（半透明混合，a 为不透明度）")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::BillboardComponent, String, baseColorTexture)
        HE_ATTR_CATEGORY("Billboard") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("纹理路径（空 = 纯色）")
    HE_END_PROPERTY()
HE_END_REGISTER()

// --- TextRenderComponent 注册（Phase A5）---
HE_BEGIN_REGISTER(he::TextRenderComponent)
    HE_REGISTER_PROPERTY(he::TextRenderComponent, String, text)
        HE_ATTR_CATEGORY("TextRender") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("显示的文字内容（UTF-8）")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::TextRenderComponent, float4, textColor)
        HE_ATTR_CATEGORY("TextRender") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("文字颜色 [r,g,b,a] 0~1")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::TextRenderComponent, float, fontSize)
        HE_ATTR_CATEGORY("TextRender") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("字号（像素高，世界高度 = 位图高/100 米）")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::TextRenderComponent, String, fontPath)
        HE_ATTR_CATEGORY("TextRender") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_DESCRIPTION("字体文件路径（空 = 系统字体兜底）")
    HE_END_PROPERTY()
HE_END_REGISTER()

// --- DecalComponent 注册（Phase A3）---
HE_BEGIN_REGISTER(he::DecalComponent)
    HE_REGISTER_PROPERTY(he::DecalComponent, float2, size)
        HE_ATTR_CATEGORY("Decal") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("贴花尺寸（世界单位，X=宽，Y=高）")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::DecalComponent, float, rotation)
        HE_ATTR_CATEGORY("Decal") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("绕贴花法线旋转（弧度）")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::DecalComponent, float, opacity)
        HE_ATTR_CATEGORY("Decal") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("不透明度 [0,1]")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::DecalComponent, u8, blendMode)
        HE_ATTR_CATEGORY("Decal") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("混合模式：0=不透明 1=Alpha 截断 2=半透明混合")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::DecalComponent, String, decalTexture)
        HE_ATTR_CATEGORY("Decal") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("贴花纹理路径（空 = 纯色片）")
    HE_END_PROPERTY()
HE_END_REGISTER()

// --- CollisionComponent 注册（Phase B5，物理与移动的前置）---
HE_BEGIN_REGISTER(he::CollisionComponent)
    HE_REGISTER_PROPERTY(he::CollisionComponent, u8, shape)
        HE_ATTR_CATEGORY("Collision") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("碰撞形状：0=AABB 盒 1=球体 2=胶囊")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::CollisionComponent, float3, halfExtents)
        HE_ATTR_CATEGORY("Collision") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("AABB 三轴半尺寸（米）")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::CollisionComponent, float, radius)
        HE_ATTR_CATEGORY("Collision") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("球体/胶囊半径（米）")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::CollisionComponent, float, height)
        HE_ATTR_CATEGORY("Collision") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("胶囊总高（米，含两端半球）")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::CollisionComponent, bool, bEnabled)
        HE_ATTR_CATEGORY("Collision") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("是否参与碰撞检测")
    HE_END_PROPERTY()
HE_END_REGISTER()

// --- CharacterMovementComponent 注册（Phase B3）---
// 输入与运行时字段（inputDirection/bWantsJump/bRunning/velocity/bOnGround）不注册：由系统/代码驱动
HE_BEGIN_REGISTER(he::CharacterMovementComponent)
    HE_REGISTER_PROPERTY(he::CharacterMovementComponent, float, walkSpeed)
        HE_ATTR_CATEGORY("Movement") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("步行速度（米/秒）")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::CharacterMovementComponent, float, runSpeed)
        HE_ATTR_CATEGORY("Movement") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("跑步速度（米/秒）")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::CharacterMovementComponent, float, jumpHeight)
        HE_ATTR_CATEGORY("Movement") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("跳跃高度（米）")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::CharacterMovementComponent, float, gravity)
        HE_ATTR_CATEGORY("Movement") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("重力加速度（米/秒²）")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::CharacterMovementComponent, float, maxSlopeAngle)
        HE_ATTR_CATEGORY("Movement") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("最大可站立坡度（度）")
    HE_END_PROPERTY()
HE_END_REGISTER()

// --- AbilityComponent 注册（Phase B4，简化 GAS）---
// 技能列表（skills/cooldownRemaining）为复合结构不注册，由代码注册技能
HE_BEGIN_REGISTER(he::AbilityComponent)
    HE_REGISTER_PROPERTY(he::AbilityComponent, float, resource)
        HE_ATTR_CATEGORY("Ability") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("当前资源（技能消耗用）")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::AbilityComponent, float, maxResource)
        HE_ATTR_CATEGORY("Ability") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("资源上限")
    HE_END_PROPERTY()
HE_END_REGISTER()

// --- SplineComponent 注册（Phase B2）---
// 控制点数组（points）为复合结构不注册，由代码 AddPoint 构建
HE_BEGIN_REGISTER(he::SplineComponent)
    HE_REGISTER_PROPERTY(he::SplineComponent, bool, bClosedLoop)
        HE_ATTR_CATEGORY("Spline") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("是否闭合路径（末点回连首点）")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::SplineComponent, bool, bShowPath)
        HE_ATTR_CATEGORY("Spline") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("是否显示调试路径（预留）")
    HE_END_PROPERTY()
HE_END_REGISTER()

// --- InstancedMeshComponent 注册（Phase B1）---
// 实例变换数组（instanceTransforms）为运行时数据不注册，由代码设置
HE_BEGIN_REGISTER(he::InstancedMeshComponent)
    HE_REGISTER_PROPERTY(he::InstancedMeshComponent, String, meshPath)
        HE_ATTR_CATEGORY("InstancedMesh") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_DESCRIPTION("网格资产路径（MVP 用内置立方体，预留 glTF 实例化）")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::InstancedMeshComponent, bool, enableFrustumCull)
        HE_ATTR_CATEGORY("InstancedMesh") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("逐实例视锥剔除（预留）")
    HE_END_PROPERTY()
HE_END_REGISTER()

// --- SkeletalMeshComponent 注册（Phase C C1b）---
HE_BEGIN_REGISTER(he::SkeletalMeshComponent)
    HE_REGISTER_PROPERTY(he::SkeletalMeshComponent, float, playSpeed)
        HE_ATTR_CATEGORY("SkeletalMesh") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("动画播放速度倍率")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::SkeletalMeshComponent, bool, playing)
        HE_ATTR_CATEGORY("SkeletalMesh") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("是否播放动画")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::SkeletalMeshComponent, bool, looping)
        HE_ATTR_CATEGORY("SkeletalMesh") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("动画是否循环")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::SkeletalMeshComponent, i32, currentClip)
        HE_ATTR_CATEGORY("SkeletalMesh") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("当前播放剪辑下标（-1 = 绑定姿势）")
    HE_END_PROPERTY()
HE_END_REGISTER()

} // namespace he
