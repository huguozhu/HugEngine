// ============================================================
// RSM_Generate.frag.slang — RSM 数据生成（**三** MRT，一个附件一个量）
//
// MRT 0: RGBA16_FLOAT — worldPos.xyz + 1
// MRT 1: RGBA16_FLOAT — worldNormal.xyz 编码到 [0,1] + 1
// MRT 2: RGBA16_FLOAT — 该 VPL 的**出射辐射度** L_v（Lambertian 一次反射）
//        L_v = albedo * lightColor * intensity * max(NdotL, 0) / π
//
// 【为什么是三个附件】（任务 30 / §9.2-AA ②③）
//   此前是两个附件塞三个量（MRT1 = 编码法线.rgb + 通量.a），于是"同一张图"在不同消费者
//   眼里含义不同：消费端要额外记住"rgb 是法线、a 才是通量"，DDGI 就曾把编码法线当辐射度读。
//   现在每个附件只表达一件事，消费者不需要"解包知识"——约定集中写在 ShaderTypes.slang 的
//   「RSM 贴图通道约定」一节。
//   同时通量从"只有标量强度"变成**带 albedo 与光源颜色的辐射度**：此前注释写着 albedo 而
//   代码只写了 intensity * NdotL，两者不符，且灰度通量让彩色物体的一次反弹失去颜色。
//
// 【为什么除以 π】Lambertian 面的出射辐射度 = albedo/π · E_direct。接收端要的是
//   E/π = Σ L_v · A · cosθ_s · cosθ_r / (π d²)（见着色器约定），因此把 1/π 放在生成端，
//   消费端只乘"每个采样点代表的世界面积 / π"这一个由光锥尺度推出的常量。
// ============================================================
#include "common.slang"  // HE_PI + ShaderTypes.slang（GPULight 共享定义）

// 【为什么 albedo 走 push constant 而不是 u_Objects[objectIndex].baseColorFactor】
//   对象缓冲是**相机可见性**筛选出来的（SceneRenderer 只写可见物体，索引是相机列表的下标），
//   而本 pass 遍历的是**全部网格**、索引是自己的计数器 —— 两者不是同一个索引空间。
//   从对象缓冲读材质字段会读到没被填过的槽（baseColorFactor = 0），实测只有 3% 的 texel
//   有非零辐射度（其余是被清成 0 的 albedo），而 pass 成本照付：§9.2-AA 同一族的第五例。
//   albedo 是**逐物体常量**，本来就该随 push constant 走（VS/FS 都能读）。
struct VSOutput {
    float4 position              : SV_Position;
    [[vk::location(0)]] float3 worldPos;
    [[vk::location(1)]] float3 worldNormal;
};

struct FSOutput {
    [[vk::location(0)]] float4 posAndUnused    : SV_Target0;
    [[vk::location(1)]] float4 normalAndUnused : SV_Target1;
    [[vk::location(2)]] float4 vplRadiance     : SV_Target2;
};

// GPULight 定义来自 ShaderTypes.slang（C++/Slang 共享）

[[vk::binding(1, 0)]] StructuredBuffer<GPULight> u_Lights;

[[vk::push_constant]] cbuffer RSMConstants {
    float4x4 lightViewProj;
    uint     objectIndex;
    uint     lightIndex;   // RSM 对应的光源在 u_Lights 中的索引
    uint     _pad[2];
    float4   vplAlbedo;    // 该 draw 的 VPL 漫反射率（见文件头：不能从对象缓冲读）
};

FSOutput fragmentMain(VSOutput input) {
    FSOutput output;

    // MRT 0: 世界空间位置
    output.posAndUnused = float4(input.worldPos, 1.0);

    // MRT 1: 世界空间法线（编码到 [0,1]）
    float3 N = normalize(input.worldNormal);
    output.normalAndUnused = float4(N * 0.5 + 0.5, 1.0);

    // MRT 2: VPL 出射辐射度 = albedo * 光源颜色 * 强度 * cos / π
    GPULight light = u_Lights[lightIndex];
    float3 L = normalize(-light.directionType.xyz);  // 方向光
    float NdotL = max(dot(N, L), 0.0);
    float3 radiance = vplAlbedo.rgb * light.colorIntensity.xyz
                    * light.colorIntensity.w * NdotL * (1.0 / HE_PI);
    output.vplRadiance = float4(radiance, 1.0);

    return output;
}
