#include "Pipeline/PTQualityCVars.h"

namespace he::render {

// 采样/追踪
CVar<i32>   cvPTSampleCount("r.PT.SPP", 1, "PT 每像素样本数（1-8）");
CVar<i32>   cvPTMaxBounces("r.PT.Bounces", 4, "PT 最大弹射次数（1-8）");
CVar<float> cvPTSkyIntensity("r.PT.SkyIntensity", 1.0f, "PT 天空强度");
// 算法开关（ReSTIR 默认关闭——阶段 A 先落稳朴素 NEE，阶段 B 再开启）
CVar<bool>  cvPTDenoise("r.PT.Denoise", false, "PT 时域降噪开关");
CVar<bool>  cvPTReSTIR("r.PT.ReSTIR", false, "ReSTIR DI 开关");
CVar<bool>  cvPTMIS("r.PT.MIS", true, "NEE MIS 开关");
CVar<bool>  cvPTRoulette("r.PT.Roulette", true, "俄罗斯轮盘赌开关");
// 降噪参数
CVar<float> cvPTDenoiseBlend("r.PT.Denoise.Blend", 0.30f, "PT 时域混合因子（历史损坏时仍有 30% 当前帧保底，避免全黑）");
// A-Trous 空间滤波
CVar<bool>  cvPTAtrous("r.PT.Atrous", true, "PT A-Trous 空间滤波开关");
CVar<i32>   cvPTAtrousIterations("r.PT.Atrous.Iterations", 4, "A-Trous 迭代数（1-5）");
CVar<float> cvPTAtrousSigmaDepth("r.PT.Atrous.SigmaDepth", 0.05f, "A-Trous 深度边权重 σ（米）");
CVar<float> cvPTAtrousSigmaNormal("r.PT.Atrous.SigmaNormal", 128.0f, "A-Trous 法线边权重指数");
CVar<float> cvPTAtrousSigmaColor("r.PT.Atrous.SigmaColor", 0.5f, "A-Trous 颜色边 σ 系数");
CVar<float> cvPTAtrousClamp("r.PT.Atrous.Clamp", 0.0f, "A-Trous 火萤钳制阈值（0=关）");
// ReSTIR 参数
CVar<i32>   cvPTRestirCandidates("r.PT.ReSTIR.Candidates", 16, "ReSTIR 初始采样候选数 M");
CVar<i32>   cvPTRestirRadius("r.PT.ReSTIR.Radius", 3, "ReSTIR 空间复用采样半径(像素)");
CVar<i32>   cvPTRestirSamples("r.PT.ReSTIR.Samples", 5, "ReSTIR 空间复用采样数");

} // namespace he::render
