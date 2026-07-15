// GPUCull_TwoPhase.comp.slang — 两阶段遮挡剔除 Phase 2
// 功能：读取 Phase 1 候选物体索引，使用当前帧 Hi-Z 精筛 → 输出 IndirectDraw 命令
//
// Dispatch: (candidateCount + 63) / 64, 1, 1

struct GPUSceneObject {
    float4x4 localToWorld; float4 boundsMin, boundsMax;
    uint meshIndex, materialIndex, objectID, visibilityFlags;
    uint indexCount, firstIndex; int vertexOffset; uint _pad1;
};

struct IndirectCmd { uint indexCount; uint instanceCount; uint firstIndex; int vertexOffset; uint firstInstance; };

[[vk::binding(0, 0)]] StructuredBuffer<GPUSceneObject> u_SceneObjects;  // 场景物体 SSBO
[[vk::binding(1, 0)]] StructuredBuffer<uint> u_Candidates;              // Phase 1 候选索引
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> u_DrawCount;             // 最终可见物体计数
[[vk::binding(3, 0)]] Texture2D<float> u_HiZ;                           // 当前帧 Hi-Z 深度金字塔
[[vk::binding(3, 0)]] SamplerState     u_HiZSampler;
[[vk::binding(4, 0)]] RWStructuredBuffer<IndirectCmd> u_IndirectCmds;   // 最终 IndirectDraw 命令

[[vk::push_constant]] cbuffer CullParams {
    float4x4 viewProjMatrix;
    float2   screenSize;
    uint     hizMipCount;
    uint     objectCount;  // Phase 1 候选数
};

bool IsOccluded(float3 mn, float3 mx) {
    float2 rMin=float2(1e10,1e10), rMax=float2(-1e10,-1e10); float mZ=0;
    for(int c=0;c<8;c++){float3 corner=float3((c&1)?mx.x:mn.x,(c&2)?mx.y:mn.y,(c&4)?mx.z:mn.z);
        float4 clip=mul(viewProjMatrix,float4(corner,1));float3 ndc=clip.xyz/max(clip.w,0.0001);
        if(clip.w<0)return false;float2 s=ndc.xy*0.5+0.5;rMin=min(rMin,s);rMax=max(rMax,s);mZ=max(mZ,ndc.z);}
    rMin=clamp(rMin,0,1);rMax=clamp(rMax,0,1);
    float2 sz=(rMax-rMin)*screenSize;float mip=clamp(ceil(log2(max(1,max(sz.x,sz.y)))),0,float(hizMipCount-1));
    float4 d;d.x=u_HiZ.SampleLevel(u_HiZSampler,rMin,mip);d.y=u_HiZ.SampleLevel(u_HiZSampler,float2(rMax.x,rMin.y),mip);
    d.z=u_HiZ.SampleLevel(u_HiZSampler,float2(rMin.x,rMax.y),mip);d.w=u_HiZ.SampleLevel(u_HiZSampler,rMax,mip);
    return mZ>min(min(d.x,d.y),min(d.z,d.w));
}

[numthreads(64,1,1)]
void main(uint3 id:SV_DispatchThreadID){
    if(id.x>=objectCount)return;

    // 读取候选物体在场景 SSBO 中的索引
    uint objectIndex = u_Candidates[id.x];
    GPUSceneObject o = u_SceneObjects[objectIndex];
    float3 mn = o.boundsMin.xyz, mx = o.boundsMax.xyz;

    // 使用当前帧 Hi-Z 做精确遮挡测试
    if(hizMipCount>0&&IsOccluded(mn,mx))return;

    // 通过测试：写入 IndirectDraw 命令
    uint idx;InterlockedAdd(u_DrawCount[0],1,idx);
    IndirectCmd cmd;
    cmd.indexCount=o.indexCount;cmd.instanceCount=1;cmd.firstIndex=o.firstIndex;
    cmd.vertexOffset=o.vertexOffset;cmd.firstInstance=o.objectID;
    u_IndirectCmds[idx]=cmd;
}
