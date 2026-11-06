#ifndef HUGENGINE_SHADER_TYPES
#define HUGENGINE_SHADER_TYPES
// ============================================================
// ShaderTypes.slang — C++ 与 Slang 共享 GPU 契约的**总入口**（兼容层）
//
// 【用法】
//   · Slang：`#include "ShaderTypes.slang"`（-I 指向 Shaders 目录）—— 与拆分前完全一致。
//   · C++：必须在 `namespace he::render` **之内** include（这些结构用 he::float4，且
//     C++ 侧把它们注册在 he::render）。**优先按需包含下面的切片**，不要图省事包含本文件：
//     本文件会把 9 个切片全部展开，等于回到拆分前。
//
//        ShaderTypes/GPUBindings.slang      descriptor set / binding 常量
//        ShaderTypes/GPUConstants.slang     引擎上限 / 默认值 / 算法常量 / 材质纹理槽
//        ShaderTypes/GITypes.slang          GI 源槽 + 通道合成参数（**无**向量类型）
//        ShaderTypes/GIBlendParams.slang    分层合成 UBO（含 float4x4，需先 include Math.h）
//        ShaderTypes/GPUSceneTypes.slang    光源 / 阴影 / 物体 / 材质 / 场景小结构
//        ShaderTypes/PushConstants/Forward.slang    PushConstantData / Shadow / GBuffer
//        ShaderTypes/PushConstants/Deferred.slang   DeferredLightingPushConstant
//        ShaderTypes/PushConstants/RT.slang         RTShadow / RTRayEffect
//        ShaderTypes/PushConstants/PT.slang         PT / PTReservoir / ReSTIR
//
// 【纪律】
//   1. 任何 C++/Slang 共享的结构或常量只能定义在 ShaderTypes/ 下；C++ 侧不得写逐字段
//      镜像（`using` 别名可以，逐字段抄一份不行 —— 尺寸断言挡不住"同尺寸、字段换了位置"）。
//   2. 切片必须能**单独**包含：各自带 include guard、各自 include GPUDefines.slang、
//      各自在 C++ 分支末尾 #undef。新增切片要同步 CMakeLists.txt 的 SLANG_INCLUDES，
//      否则增量构建会复用过期 SPV。
//   3. 改完跑一遍 C++ 构建：布局由各消费处的 static_assert 钉住。
// ============================================================

#include "ShaderTypes/GPUBindings.slang"
#include "ShaderTypes/GPUConstants.slang"
#include "ShaderTypes/GITypes.slang"
#include "ShaderTypes/GIBlendParams.slang"
#include "ShaderTypes/GPUSceneTypes.slang"
#include "ShaderTypes/PushConstants/Forward.slang"
#include "ShaderTypes/PushConstants/Deferred.slang"
#include "ShaderTypes/PushConstants/RT.slang"
#include "ShaderTypes/PushConstants/PT.slang"

// ---- 清理宏 ----
// 各切片的 C++ 分支已各自 #undef；这里为 **Slang 分支**收尾（与拆分前一致）。
#undef GPU_UINT
#undef GPU_INT
#undef GPU_STRUCT
#undef GPU_CONST
#undef GPU_CONST_BEGIN
#undef GPU_CONST_END

#endif // HUGENGINE_SHADER_TYPES
