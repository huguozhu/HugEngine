// ============================================================
// Atmosphere.slang — 空中透视（大气消光 + 入射散射）
// 与 PhysicalSky.frag.slang 的 Preetham 模型共用浑浊度参数，
// 为场景对象提供基于距离 + 高度的瑞利/米氏单散射消光。
// ============================================================
#ifndef HUGENGINE_ATMOSPHERE_SLANG
#define HUGENGINE_ATMOSPHERE_SLANG

// 海平面散射系数（单位 1/m）
static const float3 kAerialBetaR = float3(5.802e-6, 13.558e-6, 33.100e-6); // 瑞利 λ⁻⁴
static const float  kAerialBetaM = 3.996e-6;                                // 米氏 λ⁰（×浑浊度）
static const float  kAerialScaleHeightR = 8000.0f;   // 瑞利标高（米）
static const float  kAerialScaleHeightM = 1200.0f;   // 米氏标高（米）

// 空中透视：对线性 HDR 颜色施加距离/高度相关大气消光 + 入射散射。
// camPos / worldPos 世界空间（单位米）；sunDir 指向太阳；turbidity 与 Preetham 一致。
// 高度 y 相对海平面（地面视为 y=0），低于海平面按 0 处理。
void ApplyAerialPerspective(inout float3 color,
                            float3 camPos, float3 worldPos,
                            float3 sunDir, float turbidity)
{
    if (turbidity <= 0.0) return;  // 浑浊度 <= 0 表示空中透视关闭（物理范围 1~10，0 为关闭哨兵）

    float dist = length(worldPos - camPos);
    if (dist < 1e-3) return;  // 距离过近无大气效应

    float camHeight = max(camPos.y, 0.0);
    float objHeight = max(worldPos.y, 0.0);

    // 平均密度 × 路径长度（单散射简化，忽略地球曲率）
    float optR = (exp(-camHeight / kAerialScaleHeightR) + exp(-objHeight / kAerialScaleHeightR)) * 0.5 * dist;
    float optM = (exp(-camHeight / kAerialScaleHeightM) + exp(-objHeight / kAerialScaleHeightM)) * 0.5 * dist;

    float3 betaR = kAerialBetaR;
    float3 betaM = float3(kAerialBetaM) * turbidity;
    float3 transmittance = exp(-(betaR * optR + betaM * optM));

    // 入射散射色：瑞利蓝天随太阳高度亮化、随浑浊度灰化
    float sunElevation = clamp(sunDir.y, 0.0, 1.0);
    float3 inscatter = float3(0.38, 0.52, 0.72);  // 地平线瑞利蓝
    inscatter = lerp(inscatter, float3(0.72, 0.72, 0.74), clamp(turbidity / 10.0, 0.0, 1.0));
    inscatter *= (0.6 + 1.4 * sunElevation);

    color = color * transmittance + inscatter * (1.0 - transmittance);
}

#endif // HUGENGINE_ATMOSPHERE_SLANG
