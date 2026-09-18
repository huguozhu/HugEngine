// ============================================================
// PT_Common.slang — 全路径追踪共享函数库
//
// 提供：
//   1. PathPayload（RayGen/ClosestHit/Miss 共享的 48B 载荷）
//   2. 随机数工具（STBN 时空蓝噪声采样，确定性随机流）
//   3. BSDF 采样（GGX 镜面 + 余弦漫反射混合，返回混合 pdf）
//   4. MIS 幂次启发、光源采样（GPULight）、天空采样
//   5. 深度重建世界坐标 / 世界坐标 → UV 投影（velocity 用）
//
// 被 PT_Full.rgen / PT_Full.rchit / PT_Full.rmiss 与
// ReSTIR_Init / ReSTIR_Temporal / ReSTIR_Spatial 共享。
// ============================================================
#ifndef HUGENGINE_PT_COMMON_SLANG
#define HUGENGINE_PT_COMMON_SLANG

#include "ShaderTypes.slang"   // PTPushConstant / PTReservoir / GPULight
#include "pbr_common.slang"    // D_GGX / F_Schlick / PBR_BRDF（含 common.slang）

// ============================================================
// PathPayload — 路径追踪载荷（rchit/rmiss 必须逐字节一致）
// 112B = 7 × float4：基础命中信息 3 个 + Disney 材质参数 3 个 + 介质参数 1 个
//
// 字段顺序/偏移与 C++ 侧镜像结构 he::render::PathPayload
// （Engine/Render/RT/PathPayload.h）逐一对应：C++ 侧用
// static_assert(sizeof == 112) + 逐字段 offsetof 钉住；Slang 编译器不支持
// `sizeof` 形式的静态断言（实测 static_assert(sizeof(X) == N) 报 E20001），
// 故两端一致性由 Tools/check_path_payload.py 解析两份源码交叉校验
// （字段名、顺序、逐字段偏移），改动任一侧而不同步会被该脚本拦下。
// ============================================================
struct PathPayload {
    float4 albedoMetallic;  // 0  rgb=albedo, a=metallic
    float4 normalRough;     // 16 xyz=normal(世界空间), w=roughness
    float4 emissiveT;       // 32 rgb=emissive, a=hitT（miss: a=-1, rgb=sky）
    // ── Disney 参数（与 Material.h / RTPass 材质纹理 row4~row6 同源）──
    float4 disneyA;         // 48 x=anisotropic, y=subsurface, z=specular, w=sheen
    float4 disneyB;         // 64 x=clearcoat, y=clearcoatGloss, z=specularTint.r, w=specularTint.g
    float4 surfaceParams;   // 80 x=disneyC(specularTint.b), y=dielectricF0, z=ior, w=transmission
    float4 volumeParams;    // 96 x/y/z=σ_t（Beer-Lambert 吸收系数）, w 预留（材质纹理 row7）
};


// ============================================================
// 随机数工具（确定性，像素索引 + 帧号 + 样本号）
// 随机流由 STBN 时空蓝噪声 3D 纹理提供（g_BlueNoise 由各使用方 shader 声明），
// 相同 idx/frame/s → 同值（保持确定性），且空间/时间上更均匀。
// ============================================================

// STBN 采样：空间 (x,y) 取模 128，时间切层 (frame + offset) % 64。
// 用 Load() 整数取像素（Texture3D 需 int4：xyz + mip=0）——无采样器，
// 规避 RT 管线组合采样器坑。
float SampleBlueNoise1(uint2 idx, uint frame, uint offset) {
    uint z = (frame + offset * 23u) % 64u;
    return g_BlueNoise.Load(int4(idx.x & 127u, idx.y & 127u, z, 0)).r;
}

float Rand(uint2 idx, uint frame, uint s) {
    return SampleBlueNoise1(idx, frame, s);
}

uint RandInt(uint2 idx, uint frame, uint s, uint n) {
    // 用 g 通道 + 额外空间偏移，避免与 Rand 同层同坐标（降低相关性）
    uint z = (frame + s * 29u) % 64u;
    float v = g_BlueNoise.Load(int4((idx.x + 37u) & 127u, (idx.y + 61u) & 127u, z, 0)).g;
    return uint(v * float(max(n, 1u))) % max(n, 1u);
}

// 分层抖动：将像素划分为 √N × √N 网格，每个采样取一个子格中心（STBN 偏移）
float2 StratifiedJitter(uint sampleIdx, uint sampleCount, uint frameIndex) {
    uint gridSize = uint(ceil(sqrt(float(sampleCount))));
    uint sx = sampleIdx % gridSize;
    uint sy = sampleIdx / gridSize;
    float2 bn = g_BlueNoise.Load(int4(sx & 127u, sy & 127u, (frameIndex + sampleIdx * 11u) % 64u, 0)).xy;
    return float2((float(sx) + bn.x) / float(gridSize),
                  (float(sy) + bn.y) / float(gridSize));
}

// ============================================================
// 方向采样
// ============================================================

// 余弦权重半球采样（绕法线 N），pdf = NdotL / PI
float3 CosineSampleHemisphere(float3 N, float2 u) {
    float phi = 2.0 * 3.14159265 * u.x;
    float r = sqrt(u.y);
    float x = r * cos(phi);
    float y = r * sin(phi);
    float z = sqrt(max(0.0, 1.0 - u.y));
    float3 Up = abs(N.z) < 0.999 ? float3(0, 0, 1) : float3(1, 0, 0);
    float3 T = normalize(cross(Up, N));
    float3 B = cross(N, T);
    return normalize(x * T + y * B + z * N);
}

// GGX 半向量重要性采样（绕法线 N），pdf_H = D_GGX(NdotH) * NdotH
float3 SampleGGXHalf(float3 N, float roughness, float2 u, out float NdotH) {
    float alpha = max(roughness * roughness, 0.001);
    float cosTheta = sqrt((1.0 - u.y) / (1.0 + (alpha * alpha - 1.0) * u.y));
    float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
    float phi = 6.2831853 * u.x;
    float3 H = float3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);
    float3 Up = abs(N.z) < 0.999 ? float3(0, 0, 1) : float3(1, 0, 0);
    float3 T = normalize(cross(Up, N));
    float3 B = cross(N, T);
    NdotH = cosTheta;
    return normalize(H.x * T + H.y * B + H.z * N);
}

// ============================================================
// BSDF 参数 — 与 pbr_common.PBR_BRDF 的参数一一对应
// （打包成结构体是为了让「求值 / 采样 / pdf」三处共用同一组参数，
//  避免长参数列表在调用点写错顺序导致求值与采样不一致）
// ============================================================
struct PTBSDFParams {
    float3 albedo;
    float  metallic;
    float  roughness;
    float4 disneyA;   // anisotropic, subsurface, specular, sheen
    float4 disneyB;   // clearcoat, clearcoatGloss, specularTint.rg
    float4 surface;   // disneyC(specularTint.b), dielectricF0, ior, transmission
};

PTBSDFParams MakeBSDFParams(float3 albedo, float metallic, float roughness,
                            float4 disneyA, float4 disneyB, float4 surface) {
    PTBSDFParams m;
    m.albedo    = albedo;
    m.metallic  = metallic;
    m.roughness = roughness;
    m.disneyA   = disneyA;
    m.disneyB   = disneyB;
    m.surface   = surface;
    return m;
}

// 各向异性切线基：与 pbr_common.PBR_BRDF 内的构造逐行一致。
// 采样端与求值端必须用同一组 T/B，否则各向异性方向对不上。
void BSDFTangentBasis(float3 N, out float3 T, out float3 B) {
    if (abs(N.y) < 0.999)
        T = normalize(cross(float3(0, 1, 0), N));
    else
        T = normalize(cross(float3(1, 0, 0), N));
    B = normalize(cross(N, T));
}

// ============================================================
// 各瓣采样概率 — 与 PBR_BRDF 的三个 lobe 对应
//   漫反射瓣（含 subsurface 与 sheen 的边缘项，sheen 叠加在漫反射上）
//   基础镜面瓣（GGX，各向异性时按 aniso 拉伸）
//   清漆瓣（KHR_materials_clearcoat，GGX 第二层）
//
// 概率只影响方差、不影响无偏性；关键是 pdf 求值（BSDFPdf）必须用**同一组
// 概率**，这样 MIS 权重引用的密度就是实际使用的采样策略密度。
// 默认材质（clearcoat=0）时 pClearcoat=0、pSpec/pDiff 与旧两瓣实现完全相同。
// ============================================================
void BSDFLobeProbabilities(PTBSDFParams m, out float pDiff, out float pSpec, out float pClearcoat) {
    float cc   = saturate(m.disneyB.x);                    // 清漆层强度
    float base = 1.0 - cc;                                 // 基础层（漫反射 + 镜面）
    float ps   = lerp(0.35, 0.95, saturate(m.metallic));   // 基础层内镜面占比（沿用旧实现）
    pSpec      = base * ps;
    pDiff      = base * (1.0 - ps);
    pClearcoat = cc;
}

// 各向异性 GGX 半向量采样（NDF 采样，参数为各向异性 alpha）。
// 用「方向相关 alpha」把各向同性公式推广到椭圆分布：
//   a²(φ) = ax²cos²φ + ay²sin²φ
// 与之配对的 pdf 由 BSDFPdf 用同一组 (ax, ay) 经 D_GGX_aniso 计算。
// aniso=0（默认材质）时走 SampleGGXHalf，采样、pdf 与旧实现逐位一致。
float3 SampleGGXHalfAniso(float3 N, float3 T, float3 B,
                          float ax, float ay, float2 u, out float NdotH) {
    float phi    = 6.2831853 * u.x;
    float cosPhi = cos(phi), sinPhi = sin(phi);
    float a2     = ax * ax * cosPhi * cosPhi + ay * ay * sinPhi * sinPhi;
    float cosTheta = sqrt((1.0 - u.y) / max(1.0 + (a2 - 1.0) * u.y, 1e-6));
    float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
    NdotH = cosTheta;
    return normalize(sinTheta * cosPhi * T + sinTheta * sinPhi * B + cosTheta * N);
}

// 混合 pdf（三瓣之和）— 采样与 MIS 共用同一函数，保证权重与采样策略一致
float BSDFPdf(PTBSDFParams m, float3 N, float3 V, float3 L) {
    float pDiff, pSpec, pCc;
    BSDFLobeProbabilities(m, pDiff, pSpec, pCc);

    float NdotL = dot(N, L);
    if (NdotL <= 0.0) return 0.0;
    float NdotV = max(dot(N, V), 1e-4);
    float3 H = normalize(V + L);
    float NdotH = max(dot(N, H), 0.0);

    float alpha  = max(m.roughness * m.roughness, 1e-4);
    float aspect = sqrt(1.0 - 0.9 * saturate(m.disneyA.x));
    float ax = alpha / aspect, ay = alpha * aspect;
    float3 T, B;
    BSDFTangentBasis(N, T, B);

    float Ds = (m.disneyA.x > 0.0)
             ? D_GGX_aniso(NdotH, dot(H, T), dot(H, B), ax, ay)
             : D_GGX(NdotH, m.roughness);
    float pdfSpec = Ds * NdotH / (4.0 * NdotV);
    float pdfDiff = NdotL / 3.14159265;
    float Dc      = D_GGX(NdotH, m.disneyB.y);
    float pdfCc   = Dc * NdotH / (4.0 * NdotV);

    return pDiff * pdfDiff + pSpec * pdfSpec + pCc * pdfCc;
}

// ============================================================
// 电介质界面 / 参与介质（PT 任务 4：glTF transmission + volume）
// ============================================================

// 电介质菲涅尔反射率（Schlick 近似，f0 为垂直入射反射率）
float FresnelDielectric(float cosTheta, float f0) {
    float c = saturate(cosTheta);
    return f0 + (1.0 - f0) * pow(1.0 - c, 5.0);
}

// Snell 折射：I 为入射方向（指向表面，已归一化），N 为朝向入射侧的法线，
// eta = n1/n2（空气→介质时 = 1/ior）。返回 false = 全内反射（TIR）。
bool RefractDirection(float3 I, float3 N, float eta, out float3 T) {
    float cosI = dot(N, I);
    float sin2T = eta * eta * (1.0 - cosI * cosI);
    if (sin2T > 1.0) { T = float3(0, 0, 0); return false; }   // 全内反射
    float cosT = sqrt(max(0.0, 1.0 - sin2T));
    T = normalize(eta * I - (eta * cosI + cosT) * N);
    return true;
}

// 均匀介质的自由程采样（指数分布，§9.1「随机弹射距离采样」）
// 生存概率 P(d > D) = exp(-σ_t·D)，与 BeerTransmittance 的解析解一致
float SampleDistanceInMedium(float u, float sigmaT) {
    return -log(max(1.0 - u, 1e-6)) / max(sigmaT, 1e-6);
}

// Beer-Lambert 透射率（解析解）：T = exp(-σ_t · d)
float3 BeerTransmittance(float3 sigmaT, float d) {
    return exp(-sigmaT * max(d, 0.0));
}

// ============================================================
// 混合 BSDF 采样（漫反射余弦 + 各向异性 GGX + 清漆 GGX + 透射/界面）
//
// 分支概率按**能量份额**分配（这是本实现无偏的关键）：
//   pRefl = t·F      pRefr = t·(1-F)      pBsdf = 1-t      （t = transmission）
// 与之配对：delta 分支的 f 就是同一份额，故分支权重恒为 1（反射）或 albedo（折射）；
// 连续 BSDF 分支则照常用 f/pdf（总 pdf 里的 (1-t) 与 f 里的 (1-t) 相互抵消）。
//
// 出参：
//   pdf        —— 连续 BSDF 分支的条件 pdf（delta 分支不使用）
//   deltaWeight—— delta 分支（反射/折射）的权重乘子
//   isDelta    —— 本次采样是否为 delta 瓣（rgen 据此跳过 f/pdf 计算）
// ============================================================
float3 SampleBSDF(PTBSDFParams m, float3 N, float3 V,
                  float2 uLobe, float2 uDir, out float pdf,
                  out float3 deltaWeight, out bool isDelta) {
    pdf         = 0.0;
    deltaWeight = float3(0, 0, 0);
    isDelta     = false;

    // ── 透射界面（delta 瓣）──
    const float t = saturate(m.surface.w);
    if (t > 0.0) {
        float NdotV = max(dot(N, V), 1e-4);
        float F = FresnelDielectric(NdotV, m.surface.y);
        float pRefl = t * F;
        float pRefr = t * (1.0 - F);
        if (uLobe.x < pRefl) {
            // 菲涅尔反射：分支权重 = 1（份额 t·F 已作为概率）
            isDelta = true;
            deltaWeight = float3(1.0);
            pdf = pRefl;
            return normalize(reflect(-V, N));
        }
        if (uLobe.x < pRefl + pRefr) {
            // 折射（空气→介质，eta = 1/ior）；全内反射则退化为反射
            float3 T;
            float eta = 1.0 / max(m.surface.z, 1.0);
            if (RefractDirection(-V, N, eta, T)) {
                isDelta = true;
                deltaWeight = m.albedo;       // 透射色 = baseColor（glTF transmission 语义）
                pdf = pRefr;
                return T;
            }
            isDelta = true;
            deltaWeight = float3(1.0);        // TIR：全部反射
            pdf = pRefr;
            return normalize(reflect(-V, N));
        }
    }

    // ── 连续 BSDF 分支（漫反射 / 镜面 / 清漆）──
    float pDiff, pSpec, pCc;
    BSDFLobeProbabilities(m, pDiff, pSpec, pCc);

    float alpha  = max(m.roughness * m.roughness, 1e-4);
    float aspect = sqrt(1.0 - 0.9 * saturate(m.disneyA.x));
    float ax = alpha / aspect, ay = alpha * aspect;
    float3 T, B;
    BSDFTangentBasis(N, T, B);

    // 界面分支已占用 [0, t) 的 u 区间，这里把它映射回 [0,1)
    float u = (t > 0.0) ? saturate((uLobe.x - t) / max(1.0 - t, 1e-6)) : uLobe.x;

    float3 wi;
    if (u < pDiff) {
        // 漫反射：余弦半球（sheen 的边缘项叠加在漫反射上，同属该瓣）
        wi = CosineSampleHemisphere(N, uDir);
    } else if (u < pDiff + pSpec) {
        // 基础镜面：GGX 半向量 → 反射方向
        float NdotH;
        float3 H = (m.disneyA.x > 0.0)
                 ? SampleGGXHalfAniso(N, T, B, ax, ay, uDir, NdotH)
                 : SampleGGXHalf(N, m.roughness, uDir, NdotH);
        wi = normalize(reflect(-V, H));
    } else {
        // 清漆层：GGX 半向量（粗糙度由 clearcoatGloss 给出，与求值的 D_GGX 同参）
        float NdotH;
        float3 H = SampleGGXHalf(N, m.disneyB.y, uDir, NdotH);
        wi = normalize(reflect(-V, H));
    }

    if (dot(N, wi) <= 0.0) { pdf = 0.0; return float3(0, 0, 1); }
    pdf = BSDFPdf(m, N, V, wi);
    return wi;
}

// ============================================================
// 共享 BRDF 求值（统一入口）
// 与光栅化 PBR.frag / DeferredLighting.frag 调用的是同一个 pbr_common.PBR_BRDF，
// 参数也来自同一份打包（RT/PTMaterialParams.h）→ 两侧对同一材质必然一致
// ============================================================
float3 PT_EvalBRDF(PTBSDFParams m, float3 N, float3 V, float3 L) {
    return PBR_BRDF(m.albedo, m.metallic, m.roughness, N, V, L,
                    m.surface.y, float2(0.0, 1.0),
                    m.disneyA, m.disneyB, m.surface.x);
}

// ============================================================
// MIS — 幂次启发（power heuristic）
// ============================================================
float PowerHeuristic(float a, float b) {
    return (a * a) / max(a * a + b * b, 1e-6);
}

// ============================================================
// 天空采样 — 渐变天空（与 RT_Sponza.rmiss 同款色板）
// ============================================================
float3 SampleSky(float3 direction, float skyIntensity) {
    float t = clamp(direction.y * 0.5 + 0.5, 0.0, 1.0);
    float3 skyBottom = float3(0.55, 0.45, 0.35);   // 暖色地面
    float3 skyTop    = float3(0.25, 0.45, 0.7);    // 蓝色天顶
    return lerp(skyBottom, skyTop, t) * skyIntensity;
}

// ============================================================
// 光源采样 — 从 GPULight 计算采样位置/方向/辐射度/概率密度
// 方向光/点光源均为确定性采样（pdf=1，单点光源为 delta 分布）
// ============================================================
bool SampleLight(GPULight light, float3 P,
                 out float3 lightPos, out float3 L, out float3 Li, out float lightPdf) {
    lightPdf = 1.0;
    if (light.colorIntensity.w <= 0.0) return false;

    if (light.directionType.w == 0.0) {
        // 方向光：光强与距离无关，光源位置取任意远处点。
        // L 指向光源，须取反——direction 字段为光传播方向（与光栅化 Deferred/PBR 管线一致）。
        L = normalize(-light.directionType.xyz);
        lightPos = P + L * 10000.0;
        Li = light.colorIntensity.rgb * light.colorIntensity.w;
        return true;
    }

    // 点/聚光灯：方向指向光源，距离衰减 saturate(1 - d/range)^2（与 RT_HitCommon 一致）
    float range = abs(light.positionRange.w);
    lightPos = light.positionRange.xyz;
    float3 toLight = lightPos - P;
    float dist = length(toLight);
    if (range > 0.0 && dist > range + 0.001) return false;
    L = toLight / max(dist, 1e-5);
    float d = saturate(1.0 - dist / range);
    Li = light.colorIntensity.rgb * light.colorIntensity.w * (d * d);
    return true;
}

// ============================================================
// 坐标变换
// ============================================================

// 世界坐标 → 屏幕 UV（GLM y-up 投影 → Vulkan UV，y 翻转）
float2 TransformWorldToUV(float3 worldPos, float4x4 viewProj) {
    float4 clip = mul(viewProj, float4(worldPos, 1.0));
    float3 ndc = clip.xyz / clip.w;
    return float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
}

// 线性视图深度 → 世界坐标（透视相机：cameraPos + 反投影方向 * depth）
float3 ReconstructWorldPos(float2 uv, float depth, float4x4 invViewProj, float3 cameraPos) {
    float2 ndc = uv * 2.0 - 1.0;
    ndc.y = -ndc.y;  // Vulkan NDC y 翻转
    float4 clipPos = float4(ndc, 1.0, 1.0);
    float4 worldPos4 = mul(invViewProj, clipPos);
    float3 dir = normalize(worldPos4.xyz / worldPos4.w - cameraPos);
    return cameraPos + dir * depth;
}

#endif // HUGENGINE_PT_COMMON_SLANG
