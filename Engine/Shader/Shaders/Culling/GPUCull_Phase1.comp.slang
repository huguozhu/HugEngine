// GPUCull_Phase1.comp.slang — 两阶段遮挡剔除 Phase 1
// 功能：视锥剔除 + 上帧 Hi-Z 粗筛 → 输出候选物体索引
//
// 与 GPUCull.comp.slang 的区别：
//   - 输出为紧凑的候选索引数组（u32[]），而非 IndirectDrawCommand
//   - 由两阶段模式调用，Phase 2 对候选做精筛

#include "ShaderTypes.slang"   // IndirectCmd / GPUSceneObject / LightUB 等共享契约结构体
[[vk::binding(0, 0)]] StructuredBuffer<GPUSceneObject> u_SceneObjects;
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> u_CandidateIndices;  // 候选索引数组
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> u_CandidateCount;    // 候选数量
[[vk::binding(3, 0)]] Texture2D<float> u_HiZDepth;                  // 上帧 Hi-Z
[[vk::binding(3, 0)]] SamplerState     u_HiZSampler;

[[vk::push_constant]] cbuffer CullParams {
    float4 frustumPlanes[6]; float4x4 viewProjMatrix;
    float2 screenSize; uint hizMipCount; uint objectCount;
};

bool AABBOutside(float3 aabbMin, float3 aabbMax, float4 plane) {
    float3 n = plane.xyz;
    float3 p; p.x=(n.x>=0)?aabbMax.x:aabbMin.x; p.y=(n.y>=0)?aabbMax.y:aabbMin.y; p.z=(n.z>=0)?aabbMax.z:aabbMin.z;
    // 保守余量：避免浮点精度在视锥边界处误剔除。平面已归一化，0.01≈1cm 世界空间
    return dot(n, p) + plane.w < -0.01;
}

bool IsOccluded(float3 mn, float3 mx) {
    float2 rMin=float2(1e10,1e10), rMax=float2(-1e10,-1e10);
    // 【T7 保守判定】标准 Z（近=0、远=1）：取包围盒**最近**深度（min）而不是最远深度（max）。
    // 只有连最近点都在采样区域最远表面之后，才能认定整体被遮挡；用最远点会误剔除。
    float nearestZ = 1e30;
    for(int c=0;c<8;c++){float3 corner=float3((c&1)?mx.x:mn.x,(c&2)?mx.y:mn.y,(c&4)?mx.z:mn.z);
        float4 clip=mul(viewProjMatrix,float4(corner,1));
        if(clip.w<0)return false;   // 角点在相机后：保守判为未遮挡
        float3 ndc=clip.xyz/max(clip.w,0.0001);
        // 【T7】NDC→UV 的 Y 方向：两后端都是"NDC +Y = 屏幕上方 = 纹理 v=0"（见 GPUCull.comp.slang 说明）
        float2 s=float2(ndc.x*0.5+0.5,0.5-ndc.y*0.5);rMin=min(rMin,s);rMax=max(rMax,s);nearestZ=min(nearestZ,ndc.z);}
    rMin=clamp(rMin,0,1);rMax=clamp(rMax,0,1);
    float2 sz=(rMax-rMin)*screenSize;float mip=clamp(ceil(log2(max(1,max(sz.x,sz.y)))),0,float(hizMipCount-1));
    float4 d;d.x=u_HiZDepth.SampleLevel(u_HiZSampler,rMin,mip);d.y=u_HiZDepth.SampleLevel(u_HiZSampler,float2(rMax.x,rMin.y),mip);
    d.z=u_HiZDepth.SampleLevel(u_HiZSampler,float2(rMin.x,rMax.y),mip);d.w=u_HiZDepth.SampleLevel(u_HiZSampler,rMax,mip);
    // 采样区域取最远深度（max 四角）：最近点仍更远 ⇒ 保守地判为被遮挡
    return nearestZ > max(max(d.x, d.y), max(d.z, d.w));
}

[numthreads(64,1,1)]
void main(uint3 id:SV_DispatchThreadID){
    if(id.x>=objectCount)return;
    GPUSceneObject o=u_SceneObjects[id.x];float3 mn=o.boundsMin.xyz,mx=o.boundsMax.xyz;
    // 【T7】visibilityFlags == 0 = 不参与 GPU 驱动的间接绘制（实例化网格走逐实例路径）
    if(o.visibilityFlags==0u)return;
    if(AABBOutside(mn,mx,frustumPlanes[0]))return;if(AABBOutside(mn,mx,frustumPlanes[1]))return;
    if(AABBOutside(mn,mx,frustumPlanes[2]))return;if(AABBOutside(mn,mx,frustumPlanes[3]))return;
    if(AABBOutside(mn,mx,frustumPlanes[4]))return;if(AABBOutside(mn,mx,frustumPlanes[5]))return;
    if(hizMipCount>0&&IsOccluded(mn,mx))return;
    uint idx;InterlockedAdd(u_CandidateCount[0],1,idx);
    u_CandidateIndices[idx]=id.x;  // 写入物体在场景 SSBO 中的索引
}
