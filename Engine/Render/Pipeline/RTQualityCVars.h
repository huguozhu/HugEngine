#pragma once

#include "Core/CVar.h"

namespace he::render {

// ============================================================
// RTQualityCVars — HybridRT 质量参数 CVar 声明（定义见 .cpp）
// 所有 r.RT.* CVar 集中于此，RT Passes 与 HybridRTPipeline 统一 include 访问
// ============================================================
// 开关
extern CVar<bool>  cvRTShadow;          // r.RT.Shadow            RT 阴影开关
extern CVar<bool>  cvRTAO;              // r.RT.AO                RT AO 开关
extern CVar<bool>  cvRTReflection;      // r.RT.Reflection        RT 反射开关
extern CVar<bool>  cvRTGI;              // r.RT.GI                RT GI 开关
// 分辨率
extern CVar<bool>  cvRTShadowHalfRes;   // r.RT.Shadow.HalfRes    阴影半分辨率
extern CVar<bool>  cvRTAOHalfRes;       // r.RT.AO.HalfRes        AO 半分辨率
extern CVar<bool>  cvRTReflectionHalfRes;// r.RT.Reflection.HalfRes 反射半分辨率
extern CVar<bool>  cvRTGIQuarterRes;    // r.RT.GI.QuarterRes     GI 四分之一分辨率
// 采样/追踪
extern CVar<i32>   cvRTAOSPP;           // r.RT.AO.SPP            AO 每像素射线数
extern CVar<float> cvRTAOMaxDist;       // r.RT.AO.MaxDistance    AO 遮蔽半径
extern CVar<i32>   cvRTReflectionSPP;   // r.RT.Reflection.SPP    反射每像素采样数
extern CVar<float> cvRTReflectionMaxDist;// r.RT.Reflection.MaxDistance 反射最大距离
extern CVar<i32>   cvRTGISPP;           // r.RT.GI.SPP            GI 每像素采样数
extern CVar<float> cvRTGIMaxDist;       // r.RT.GI.MaxDistance    GI 追踪范围
extern CVar<float> cvRTShadowMaxDist;   // r.RT.Shadow.MaxDistance 阴影最大距离
// 降噪
extern CVar<bool>  cvRTDenoiseTemporal; // r.RT.Denoise.Temporal  时域降噪开关
extern CVar<bool>  cvRTDenoiseSpatial;  // r.RT.Denoise.Spatial   空间滤波开关
extern CVar<float> cvRTDenoiseShadowBlend;   // r.RT.Denoise.Shadow.Blend
extern CVar<float> cvRTDenoiseAOBlend;       // r.RT.Denoise.AO.Blend
extern CVar<float> cvRTDenoiseReflectionBlend;// r.RT.Denoise.Reflection.Blend
extern CVar<float> cvRTDenoiseGIBlend;       // r.RT.Denoise.GI.Blend

} // namespace he::render
