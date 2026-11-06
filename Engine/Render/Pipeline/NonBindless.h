#pragma once

// ============================================================================
//  NonBindless.h —— 非 bindless 后端（D3D11）的逐绘制句柄交接
//
//  Vulkan / D3D12 的着色器用推送常量里的句柄直接索引 bindless 数组；D3D11（SM 5.0）不能动态
//  索引资源数组，着色器改用单个替代资源（ShaderTypes.slang 的 kGPUBinding_NB_*），句柄要由
//  渲染层经 IRHICommandList::SetNonBindlessHandle 交给后端（D3D11 后端实现规划 3.2）。
//  在把句柄写进推送常量的同一处调用下面的函数；其它后端里 SetNonBindlessHandle 是空操作。
// ============================================================================

#include "Pipeline/Material.h"   // ShaderTypes.slang 的 kGPUBinding_NB_* 常量
#include "RHI/CommandList.h"

namespace he::render {

/// 一次网格绘制用到的 bindless 句柄：
///   materialTexBase      —— 材质纹理基址（GPUObjectData::materialID，4 张纹理依次取）
///   instanceSSBOHandle   —— PBR / GBuffer 顶点着色器的 instanceSSBOHandle（实例变换；蒙皮时是骨骼矩阵）
///   instanceVisibleHandle —— 逐实例剔除后的可见下标列表
/// 实例 / 骨骼 / 可见列表句柄传 0 表示「本次绘制不用」（渲染层对这三类缓冲一直以 0 表示无效），
/// 交给后端时换成 kNonBindlessNone —— bindless 句柄 0 本身是合法句柄（如材质缓冲），不能照传
inline void SetDrawNonBindlessHandles(rhi::IRHICommandList* cmd, u32 materialTexBase,
                                      u32 instanceSSBOHandle = 0, u32 instanceVisibleHandle = 0) {
    const auto orNone = [](u32 h) { return h != 0u ? h : rhi::kNonBindlessNone; };
    cmd->SetNonBindlessHandle(kGPUBinding_NB_MaterialTextures, materialTexBase);
    // 顶点着色器按模式二选一读取：模式 2 读实例变换，模式 3 读骨骼矩阵，两者都来自 instanceSSBOHandle
    cmd->SetNonBindlessHandle(kGPUBinding_NB_InstanceTransforms, orNone(instanceSSBOHandle));
    cmd->SetNonBindlessHandle(kGPUBinding_NB_BoneMatrices, orNone(instanceSSBOHandle));
    cmd->SetNonBindlessHandle(kGPUBinding_NB_VisibleInstances, orNone(instanceVisibleHandle));
}

} // namespace he::render
