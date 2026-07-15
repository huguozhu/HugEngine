// GPUCull.comp.slang — GPU Frustum + Hi-Z Culling → IndirectDrawCommand

struct GPUSceneObject {
    float4x4 localToWorld; float4 boundsMin, boundsMax;
    uint meshIndex, materialIndex, objectID, visibilityFlags;
    uint indexCount, firstIndex; int vertexOffset; uint _pad1;
};

struct IndirectCmd { uint indexCount; uint instanceCount; uint firstIndex; int vertexOffset; uint firstInstance; };

[[vk::binding(0, 0)]] StructuredBuffer<GPUSceneObject> u_SceneObjects;
[[vk::binding(1, 0)]] RWStructuredBuffer<IndirectCmd> u_IndirectCommands;
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> u_DrawCount;
[[vk::binding(3, 0)]] Texture2D<float> u_HiZDepth;
[[vk::binding(3, 0)]] SamplerState     u_HiZSampler;

[[vk::push_constant]] cbuffer CullParams {
    float4 frustumPlanes[6]; float4x4 viewProjMatrix;
    float2 screenSize; uint hizMipCount; uint objectCount;
};

bool AABBOutside(float3 aabbMin, float3 aabbMax, float4 plane) {
    float3 n = plane.xyz;
    float3 p; p.x=(n.x>=0)?aabbMax.x:aabbMin.x; p.y=(n.y>=0)?aabbMax.y:aabbMin.y; p.z=(n.z>=0)?aabbMax.z:aabbMin.z;
    return dot(n,p)+plane.w<0;
}

bool IsOccluded(float3 mn, float3 mx) {
    float2 rMin=float2(1e10,1e10), rMax=float2(-1e10,-1e10); float mZ=0;
    for(int c=0;c<8;c++){float3 corner=float3((c&1)?mx.x:mn.x,(c&2)?mx.y:mn.y,(c&4)?mx.z:mn.z);
        float4 clip=mul(viewProjMatrix,float4(corner,1));float3 ndc=clip.xyz/max(clip.w,0.0001);
        if(clip.w<0)return false;float2 s=ndc.xy*0.5+0.5;rMin=min(rMin,s);rMax=max(rMax,s);mZ=max(mZ,ndc.z);}
    rMin=clamp(rMin,0,1);rMax=clamp(rMax,0,1);
    float2 sz=(rMax-rMin)*screenSize;float mip=clamp(ceil(log2(max(1,max(sz.x,sz.y)))),0,float(hizMipCount-1));
    float4 d;d.x=u_HiZDepth.SampleLevel(u_HiZSampler,rMin,mip);d.y=u_HiZDepth.SampleLevel(u_HiZSampler,float2(rMax.x,rMin.y),mip);
    d.z=u_HiZDepth.SampleLevel(u_HiZSampler,float2(rMin.x,rMax.y),mip);d.w=u_HiZDepth.SampleLevel(u_HiZSampler,rMax,mip);
    // 使用 max（最远深度）做保守遮挡测试：
    // 对象只需比采样区域内最深的位置更近即可通过，避免 min 在边界处误剔除
    return mZ > max(max(d.x, d.y), max(d.z, d.w));
}

[numthreads(64,1,1)]
void main(uint3 id:SV_DispatchThreadID){
    if(id.x>=objectCount)return;
    GPUSceneObject o=u_SceneObjects[id.x];float3 mn=o.boundsMin.xyz,mx=o.boundsMax.xyz;
    if(AABBOutside(mn,mx,frustumPlanes[0]))return;if(AABBOutside(mn,mx,frustumPlanes[1]))return;
    if(AABBOutside(mn,mx,frustumPlanes[2]))return;if(AABBOutside(mn,mx,frustumPlanes[3]))return;
    if(AABBOutside(mn,mx,frustumPlanes[4]))return;if(AABBOutside(mn,mx,frustumPlanes[5]))return;
    if(hizMipCount>0&&IsOccluded(mn,mx))return;
    uint idx;InterlockedAdd(u_DrawCount[0],1,idx);
    IndirectCmd cmd;cmd.indexCount=o.indexCount;cmd.instanceCount=1;cmd.firstIndex=o.firstIndex;cmd.vertexOffset=o.vertexOffset;cmd.firstInstance=o.objectID;
    u_IndirectCommands[idx]=cmd;
}
