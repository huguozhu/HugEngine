// ReSTIR_Common.slang — ReSTIR DI 三个 pass（Init / Spatial / Temporal）的共用件
//
// 提供：EmptyReservoir（空蓄水池）、EvaluateTarget（按光源索引算目标函数 pHat）
//
// 【为什么单独一个文件，而不是放进 PT_Common.slang】
// `EvaluateTarget` 依赖**包含者声明**的 `g_PC`（ReSTIRPushConstant）与 `g_Lights`。
// PT_Common 被 PT_Full.rgen/rchit/rmiss、PT_Atrous 等**不声明 g_Lights** 的文件 include，
// 一旦把本函数放进去，就会给它们强加一个用不到的全局依赖（实测：slangc 报
// `PT_Common.slang: undefined identifier 'g_Lights'`）。因此本模块只由三个 ReSTIR pass include。
//
// 【包含者必须声明】`g_Lights`（StructuredBuffer<GPULight>）与 `g_PC`（含 `lightCount`），
// 且 **include 本文件要放在这两个声明之后** —— 与 RT_Common / RT_DDGI 同一约定。
//
// 【收敛来源】这三个 pass 此前各有一份**逐字相同**的 EmptyReservoir 与 EvaluateTarget
//（仅注释差异：Spatial/Temporal 的越界防护注释）。
// ============================================================
#ifndef HUGENGINE_RESTIR_COMMON_SLANG
#define HUGENGINE_RESTIR_COMMON_SLANG

// ============================================================
// ReSTIR DI 共用：空蓄水池与目标函数（此前 Init/Spatial/Temporal 各一份逐字相同）
// 依赖包含者声明 `g_PC`（ReSTIRPushConstant）与 `g_Lights` —— 与上面的 g_BlueNoise 同一约定。
// ============================================================
// 空蓄水池（miss 像素 / 无有效候选）
PTReservoir EmptyReservoir() {
    PTReservoir r;
    r.lightIndex = 0xFFFFFFFFu;
    r.weightSum  = 0.0;
    r.M          = 0u;
    r.W          = 0.0;
    r.lightPos   = float4(0, 0, 0, 0);
    return r;
}

// 目标函数（与 Init/Temporal 一致）：p̂ = luminance(brdf·Li) / p_src（含真实 albedo/metallic）
float EvaluateTarget(float3 P, float3 N, float3 V, float roughness,
                     float3 albedo, float metallic,
                     uint lightIndex, out float3 lightPos) {
    uint li = min(lightIndex, g_PC.lightCount > 0 ? g_PC.lightCount - 1u : 0u);  // 越界防护
    float3 L; float3 Li; float lightPdf;
    if (!SampleLight(g_Lights[li], P, lightPos, L, Li, lightPdf)) return 0.0;
    if (dot(N, L) <= 0.0) return 0.0;
    float3 f = PBR_BRDF(albedo, metallic, roughness, N, V, L) * Li;
    return Luminance(f) / max(lightPdf, 1e-6);
}

#endif // HUGENGINE_RESTIR_COMMON_SLANG
