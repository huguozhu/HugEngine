// ============================================================
// PhysicalSkyComponent.cpp — 物理天空组件实现
// ============================================================

#include "Scene/PhysicalSkyComponent.h"
#include "Scene/World.h"
#include "Scene/LightComponent.h"
#include "Core/Log.h"

namespace he {

void PhysicalSkyComponent::OnCreate() {
    sunDirection = glm::normalize(sunDirection);
}

void SyncPhysicalSkyToSun(World& world) {
    // 取第一个启用中的物理天空
    const PhysicalSkyComponent* sky = nullptr;
    world.ForEach<PhysicalSkyComponent>([&](Entity, PhysicalSkyComponent& ps) {
        if (ps.enabled && !sky) sky = &ps;
    });
    if (!sky) return;

    // 方向光 direction 指向光线传播方向（shader 内 L = normalize(-direction)），
    // 太阳 sunDirection 指向太阳本身，故光线方向取反
    float3 lightDir = -glm::normalize(sky->sunDirection);
    float  illuminance = sky->sunIntensity * sky->sunIlluminance;

    bool synced = false;
    world.ForEach<DirectionalLight>([&](Entity, DirectionalLight& dl) {
        if (dl.syncWithPhysicalSky) {
            dl.direction   = lightDir;      // 光线方向同步（与太阳方向相反）
            dl.illuminance = illuminance;   // 照度同步（lux，>0 进入物理模式）
            synced = true;
        }
    });

    // 一次性验证日志：打印同步后的方向与照度，便于核对太阳同步是否生效
    static bool s_SunSyncLogged = false;
    if (synced && !s_SunSyncLogged) {
        s_SunSyncLogged = true;
        HE_CORE_INFO("[PhysicalSky] 太阳同步: direction=({:.3f},{:.3f},{:.3f}), illuminance={:.1f} lux",
                     lightDir.x, lightDir.y, lightDir.z, illuminance);
    }
}

bool GetPhysicalSkySun(World& world, float3& sunDirection, float& turbidity) {
    bool found = false;
    world.ForEach<PhysicalSkyComponent>([&](Entity, PhysicalSkyComponent& ps) {
        if (ps.enabled && !found) {
            sunDirection = ps.sunDirection;
            turbidity    = ps.turbidity;
            found        = true;
        }
    });
    return found;
}

} // namespace he
