// ParticleComponent.cpp — GPU 粒子系统 CPU 端实现

#include "Scene/ParticleComponent.h"
#include "Scene/Transform.h"
#include "Scene/World.h"
#include "Core/Log.h"
#include <cmath>

namespace he {

// ============================================================
// 辅助函数: 向上取整到 2 的幂
// ============================================================
static u32 To2Power(u32 i) {
    i--;
    i |= i >> 1;
    i |= i >> 2;
    i |= i >> 4;
    i |= i >> 8;
    i |= i >> 16;
    i++;
    return i;
}

// ============================================================
// 生命周期控制
// ============================================================

void ParticleComponent::Play() {
    if (m_ParticleState != ParticleState::Stopped) {
        HE_CORE_WARN("ParticleComponent::Play — 非 Stopped 状态下调用");
        return;
    }
    m_NeedInit      = true;
    m_Elapsed       = 0.0f;
    m_LastEmitTime  = 0.0f;
    m_ParticleState         = ParticleState::Playing;

    // 计算最大粒子数: particlesPerSec * maxLifeTime * 1.5 (安全余量)
    m_MaxParticles = u32(m_Param.particlesPerSec * m_Param.maxLifeTime * 1.5f + 15) / 16 * 16;
    m_EmitCount    = 0;

    if (m_Callback)
        m_Callback("Particle", ParticleMsgType::Start);

    HE_CORE_INFO("ParticleComponent::Play — maxParticles={}", m_MaxParticles);
}

void ParticleComponent::Pause() {
    if (m_ParticleState != ParticleState::Playing) return;
    m_ParticleState = ParticleState::Pause;
    if (m_Callback)
        m_Callback("Particle", ParticleMsgType::Pause);
}

void ParticleComponent::Resume() {
    if (m_ParticleState != ParticleState::Pause) return;
    m_ParticleState = ParticleState::Playing;
    if (m_Callback)
        m_Callback("Particle", ParticleMsgType::Resume);
}

void ParticleComponent::Stop() {
    if (m_ParticleState != ParticleState::Playing) return;
    m_Elapsed      = 0.0f;
    m_LastEmitTime = 0.0f;
    m_ParticleState        = ParticleState::Stopped;
    if (m_Callback)
        m_Callback("Particle", ParticleMsgType::Stop);
}

// ============================================================
// Tick — CPU 端累积时间
// ============================================================

void ParticleComponent::Tick(float deltaTime) {
    if (m_ParticleState != ParticleState::Playing)
        m_TickDeltaTime = 0.0f;
    else
        m_TickDeltaTime = deltaTime;
}

// ============================================================
// GetWorldEmitPosition
// ============================================================

float3 ParticleComponent::GetWorldEmitPosition() const {
    // 通过 World→Entity→TransformComponent 获取位置
    // 后续完善：从本地 Transform 计算世界空间位置
    Entity e = GetEntity();
    World* w = GetWorld();
    if (w) {
        auto* transform = w->GetComponent<TransformComponent>(e);
        if (transform) {
            float4x4 localMat = transform->GetLocalMatrix();
            return float3(localMat[3].x, localMat[3].y, localMat[3].z);
        }
    }
    return float3(0, 0, 0);
}

} // namespace he
