// GBuffer.frag — 延迟渲染 GBuffer 片段着色器（8 MRT + velocity）
// MRT 0: RGBA16  albedo.rgb + metallic
// MRT 1: RGBA16  worldNormal.xyz * 0.5 + 0.5 + roughness
// MRT 2: RGBA16  emissive.rgb + ao
// MRT 3: RG16    velocity.xy = currUV - prevUV（UV 空间运动矢量）
// MRT 4: RGBA16  worldPos.xyz + dielectricF0.a
// MRT 5: RGBA16  disneyA（anisotropic/subsurface/specular/sheen）
// MRT 6: RGBA16  disneyB（clearcoat/clearcoatGloss/specularTint.rg）
// MRT 7: RGBA16  **光照图键**（任务 31）：uv0.xy + objectIndex + 0
//
// 【为什么光照图键必须有独立通道】烘焙光照图是**逐纹素**的数据（每个 (物体, UV0) 对应
// 一个 texel），光照 pass 必须能从每个像素反查"我该读哪一页的哪一个 texel"。GBuffer 里
// 已有的 worldPos 做不到这件事（同一个世界位置可能对应多个 UV），而 UV0 与 objectIndex
// 合起来就是键：页号 = objectIndex，页内坐标 = uv0。这就是任务 18 记下的前置条件 ①。

#include "common.slang"

struct VSOutput {
    float4 position              : SV_Position;
    [[vk::location(0)]] float3 worldPos;
    [[vk::location(1)]] float3 worldNormal;
    [[vk::location(2)]] float2 uv;
    [[vk::location(3)]] float4 prevClipPos;
};

// GPUObjectData 定义来自 ShaderTypes.slang（C++/Slang 共享）

// set=0: per-frame data (u_Objects + bindless textures)
[[vk::binding(kGPUBinding_ObjectData, kGPUDescSet_PerFrame)]] StructuredBuffer<GPUObjectData> u_Objects;

// Bindless 纹理通过 common.slang 中的 u_Textures[] / u_Samplers[] 访问
// 偏移: materialID + 0 = BaseColor, +1 = Normal, +2 = MetallicRoughness, +3 = Occlusion

[[vk::push_constant]] cbuffer FrameConstants {
    float4x4 viewProjMatrix;
    float4x4 prevViewProjMatrix;
    uint     objectIndex;
    uint     _pad[15];
};

struct FSOutput {
    [[vk::location(0)]] float4 albedoMetallic  : SV_Target0;
    [[vk::location(1)]] float4 normalRoughness : SV_Target1;
    [[vk::location(2)]] float4 emissiveAO      : SV_Target2;
    [[vk::location(3)]] float2 velocity         : SV_Target3;  // UV 空间运动矢量
    [[vk::location(4)]] float4 worldPos         : SV_Target4;  // 世界坐标（Lighting pass 直接读取）
    [[vk::location(5)]] float4 disneyA          : SV_Target5;  // 迪士尼参数A：anisotropic, subsurface, specular, sheen
    [[vk::location(6)]] float4 disneyB          : SV_Target6;  // 迪士尼参数B：clearcoat, clearcoatGloss, specularTint.r, specularTint.g
    // 光照图键（任务 31）：xy=UV0，z=objectIndex（页号）。光照图按物体分页，页内坐标就是 UV0
    [[vk::location(7)]] float4 lightmapKey      : SV_Target7;
};

FSOutput fragmentMain(VSOutput input) {
    GPUObjectData obj = u_Objects[objectIndex];

    // ── MRT 0-2：PBR 材质（bindless 纹理数组采样，按 textureMask 判断槽有无纹理） ──
    uint texBase = obj.materialID;
    float4 baseColorSample = (obj.textureMask & kGPUMaterialTexMask_BaseColor) != 0u
        ? u_Textures[texBase + kGPUMaterialTexSlot_BaseColor].Sample(u_Samplers[texBase + kGPUMaterialTexSlot_BaseColor], input.uv)
        : float4(1.0);
    float3 albedo   = obj.baseColorFactor.rgb * baseColorSample.rgb;
    float  alpha    = obj.baseColorFactor.a * baseColorSample.a;
    float4 mrSample = (obj.textureMask & kGPUMaterialTexMask_MetallicRough) != 0u
        ? u_Textures[texBase + kGPUMaterialTexSlot_MetallicRough].Sample(u_Samplers[texBase + kGPUMaterialTexSlot_MetallicRough], input.uv)
        : float4(0.0, 1.0, 0.0, 1.0);   // 中性：g=1(rough 不动) b=0(metallic 不动)
    float  metallic = obj.metallicFactor * mrSample.b;

    if (alpha < obj.alphaCutoff) discard;

    // 法线：有真实法线贴图才混合；无法线贴图（占位白会错误偏转 N）直接用几何法线
    float3 N = normalize(input.worldNormal);
    if ((obj.textureMask & kGPUMaterialTexMask_Normal) != 0u) {
        float3 texNormal = u_Textures[texBase + kGPUMaterialTexSlot_Normal]
            .Sample(u_Samplers[texBase + kGPUMaterialTexSlot_Normal], input.uv).rgb * 2.0 - 1.0;
        N = normalize(N + texNormal * 0.5);
    }

    float roughness = clamp(obj.roughnessFactor * mrSample.g, 0.04, 1.0);
    float ao = obj.aoFactor * (((obj.textureMask & kGPUMaterialTexMask_Occlusion) != 0u)
        ? u_Textures[texBase + kGPUMaterialTexSlot_Occlusion].Sample(u_Samplers[texBase + kGPUMaterialTexSlot_Occlusion], input.uv).r
        : 1.0);

    // ── MRT 3：Velocity（UV 空间运动矢量 = currUV - prevUV） ──
    float3 currNDC = input.position.xyz / input.position.w;
    float2 currUV  = currNDC.xy * 0.5 + 0.5;

    float3 prevNDC = input.prevClipPos.xyz / input.prevClipPos.w;
    float2 prevUV  = prevNDC.xy * 0.5 + 0.5;

    float2 motion = currUV - prevUV;

    FSOutput output;
    output.albedoMetallic  = float4(albedo, metallic);
    output.normalRoughness = float4(N * 0.5 + 0.5, roughness);
    output.emissiveAO      = float4(obj.emissiveFactor.rgb, ao);
    output.velocity        = motion;
    output.worldPos        = float4(input.worldPos, obj.dielectricF0);  // 世界坐标存入 GBuffer（a 携带电介质 F0，延迟光照用）
    output.disneyA         = obj.disneyA;  // 迪士尼参数A透传到 Deferred 光照（MRT5）
    output.disneyB         = obj.disneyB;  // 迪士尼参数B透传到 Deferred 光照（MRT6）
    // ── MRT 7：光照图键（任务 31）────────────────────────────────────────────────
    // 页号 = objectIndex（每物体一页，GPUScene 按组件收集 ⇒ 一页只装一个物体）。
    // 页内坐标 = **按主导轴的箱式投影**：6 个面各占一个 tile（3 列 × 2 行），世界位置用
    // **本物体自己的 AABB**（obj.boundsMin/Max）归一化到 tile 内。
    //
    // 【为什么不用 uv0】uv0 是平铺纹理坐标：实测 128² 页下 70.60% 的 texel 会被 ≥2 个不同
    // 世界位置命中（uv 范围 [-1.42, 28.97]，只有 67% 的像素在 [0,1] 内）⇒ 用它当键会把无关
    // 表面点混进同一个 texel。箱式投影不需要给网格加 UV1 通道、也不需要离线展开工具，且
    // **烘焙端可以用同一个公式**（写页与读页同源）。
    // 【已知边界】箱式投影对"同一法线方向的多个平行面"不唯一（空心壳体的内外表面、两层
    // 平行墙会有部分重叠）—— 可见面上这种重叠很少（见 lightmap_key_check 的实测），真正
    // 的唯一展开留给后续（离线展开 / UV1）。
    {
        const float3 sz = max(obj.boundsMax.xyz - obj.boundsMin.xyz, float3(1e-4, 1e-4, 1e-4));
        float3 lp = (input.worldPos - obj.boundsMin.xyz) / sz;   // 归一化到 [0,1]
        lp = saturate(lp);
        const float3 an = abs(N);                                // 主导轴（法线越大越正对）
        uint face;
        float2 tuv;
        if (an.x >= an.y && an.x >= an.z) {
            face = (N.x >= 0.0) ? 0u : 1u;   // ±X：u 取 z、v 取 y
            tuv  = float2(lp.z, lp.y);
        } else if (an.y >= an.z) {
            face = (N.y >= 0.0) ? 2u : 3u;   // ±Y：u 取 x、v 取 z
            tuv  = float2(lp.x, lp.z);
        } else {
            face = (N.z >= 0.0) ? 4u : 5u;   // ±Z：u 取 x、v 取 y
            tuv  = float2(lp.x, lp.y);
        }
        // 负轴方向翻转 u，让"从物体外面看"的 tile 朝向一致
        if ((face & 1u) != 0u) tuv.x = 1.0 - tuv.x;
        tuv.y = 1.0 - tuv.y;                 // 与引擎的 uv 约定（v 向下）一致
        const float2 tile = float2(1.0 / 3.0, 0.5);              // 3 列 × 2 行
        const float2 origin = float2(float(face % 3u) * tile.x, float(face / 3u) * tile.y);
        output.lightmapKey = float4(origin + saturate(tuv) * tile, float(objectIndex), 0.0);
    }
    return output;
}
