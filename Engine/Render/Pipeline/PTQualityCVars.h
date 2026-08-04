#pragma once

#include "Core/CVar.h"

namespace he::render {

// ============================================================
// PTQualityCVars — 全路径追踪质量参数 CVar 声明（定义见 .cpp）
// 所有 r.PT.* CVar 集中于此，PTPass / PathTracingPipeline /
// ReSTIRPass / 02.Cube ImGui 统一 include 访问
// ============================================================
// 采样/追踪
extern CVar<i32>   cvPTSampleCount;    // r.PT.SPP           每像素样本数（1-8）
extern CVar<i32>   cvPTMaxBounces;     // r.PT.Bounces       最大弹射次数（1-8）
extern CVar<float> cvPTSkyIntensity;   // r.PT.SkyIntensity  天空强度
// 算法开关
extern CVar<bool>  cvPTDenoise;        // r.PT.Denoise       PT 时域降噪开关
extern CVar<bool>  cvPTReSTIR;         // r.PT.ReSTIR        ReSTIR DI 开关
extern CVar<bool>  cvPTMIS;            // r.PT.MIS            NEE MIS 开关
extern CVar<bool>  cvPTRoulette;       // r.PT.Roulette      俄罗斯轮盘赌开关
// 降噪参数
extern CVar<float> cvPTDenoiseBlend;   // r.PT.Denoise.Blend 时域混合因子
// ReSTIR 参数
extern CVar<i32>   cvPTRestirCandidates; // r.PT.ReSTIR.Candidates  初始采样候选数 M
extern CVar<i32>   cvPTRestirRadius;     // r.PT.ReSTIR.Radius      空间复用采样半径
extern CVar<i32>   cvPTRestirSamples;    // r.PT.ReSTIR.Samples     空间复用采样数

} // namespace he::render
