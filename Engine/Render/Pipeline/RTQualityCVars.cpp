#include "Pipeline/RTQualityCVars.h"

namespace he::render {

// 开关
CVar<bool>  cvRTShadow("r.RT.Shadow", true, "RT 阴影开关");
CVar<bool>  cvRTAO("r.RT.AO", true, "RT AO 开关");
CVar<bool>  cvRTReflection("r.RT.Reflection", true, "RT 反射开关");
CVar<bool>  cvRTGI("r.RT.GI", true, "RT GI 开关");
// 分辨率
CVar<bool>  cvRTShadowHalfRes("r.RT.Shadow.HalfRes", true, "RT 阴影半分辨率");
CVar<bool>  cvRTAOHalfRes("r.RT.AO.HalfRes", true, "RT AO 半分辨率");
CVar<bool>  cvRTReflectionHalfRes("r.RT.Reflection.HalfRes", true, "RT 反射半分辨率");
CVar<bool>  cvRTGIQuarterRes("r.RT.GI.QuarterRes", true, "RT GI 四分之一分辨率");
// 采样/追踪
CVar<i32>   cvRTAOSPP("r.RT.AO.SPP", 2, "RT AO 每像素射线数");
CVar<float> cvRTAOMaxDist("r.RT.AO.MaxDistance", 2.0f, "RT AO 遮蔽半径(m)");
CVar<i32>   cvRTReflectionSPP("r.RT.Reflection.SPP", 1, "RT 反射每像素采样数");
CVar<float> cvRTReflectionMaxDist("r.RT.Reflection.MaxDistance", 500.0f, "RT 反射最大追踪距离(m)");
CVar<i32>   cvRTGISPP("r.RT.GI.SPP", 1, "RT GI 每像素采样数");
CVar<float> cvRTGIMaxDist("r.RT.GI.MaxDistance", 30.0f, "RT GI 追踪范围(m)");
CVar<float> cvRTShadowMaxDist("r.RT.Shadow.MaxDistance", 200.0f, "RT 阴影最大追踪距离");
// 软阴影 / 反射分级
CVar<bool>  cvRTShadowSoft("r.RT.Shadow.Soft", false, "RT 软阴影开关");
CVar<i32>   cvRTShadowSPP("r.RT.Shadow.SPP", 4, "RT 软阴影每光源采样数（1=硬阴影）");
CVar<float> cvRTReflectionMaxRoughness("r.RT.Reflection.MaxRoughness", 0.6f, "RT 反射最大粗糙度，超过用 IBL prefilter");
// 降噪
CVar<bool>  cvRTDenoiseTemporal("r.RT.Denoise.Temporal", true, "RT 时域降噪开关");
CVar<bool>  cvRTDenoiseSpatial("r.RT.Denoise.Spatial", true, "RT 空间滤波开关");
CVar<float> cvRTDenoiseShadowBlend("r.RT.Denoise.Shadow.Blend", 0.05f, "RT 阴影时域混合因子");
CVar<float> cvRTDenoiseAOBlend("r.RT.Denoise.AO.Blend", 0.05f, "RT AO 时域混合因子");
CVar<float> cvRTDenoiseReflectionBlend("r.RT.Denoise.Reflection.Blend", 0.10f, "RT 反射时域混合因子");
CVar<float> cvRTDenoiseGIBlend("r.RT.Denoise.GI.Blend", 0.15f, "RT GI 时域混合因子");

} // namespace he::render
