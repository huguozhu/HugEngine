// GPUCull.comp.slang — GPU Frustum + Hi-Z Culling → IndirectDrawCommand

#include "ShaderTypes.slang"   // IndirectCmd / GPUSceneObject / LightUB 等共享契约结构体

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
    // 保守余量：避免浮点精度在视锥边界处误剔除。平面已归一化，0.01≈1cm 世界空间
    return dot(n, p) + plane.w < -0.01;
}

bool IsOccluded(float3 mn, float3 mx) {
    float2 rMin=float2(1e10,1e10), rMax=float2(-1e10,-1e10);
    // 【T7 保守判定】投影是标准 Z（近=0、远=1），所以包围盒**最近**深度（min）才是遮挡判定的正确口径：
    // 只有"连最近点都在采样区域最远表面之后"才算整体被遮挡。此前取 max(ndc.z)（最远点）会让判定
    // 更容易成立 —— 单级 Hi-Z（hizMipCount=1，全分辨率深度）下会误剔除可见物体。
    float nearestZ = 1e30;
    for(int c=0;c<8;c++){float3 corner=float3((c&1)?mx.x:mn.x,(c&2)?mx.y:mn.y,(c&4)?mx.z:mn.z);
        float4 clip=mul(viewProjMatrix,float4(corner,1));
        if(clip.w<0)return false;   // 有角点在相机后：投影无效，保守判为未遮挡
        float3 ndc=clip.xyz/max(clip.w,0.0001);
        // 【T7 NDC→UV 的 Y 方向】两个后端都按"NDC +Y = 屏幕上方 = 纹理 v=0"渲染：
        // Vulkan 用负视口高度翻转 Y，D3D12 的视口变换同样把 +Y 映射到顶部。
        // 因此 v = 0.5 - ndc.y*0.5。此前写成 0.5 + ndc.y*0.5 ⇒ 采样到上下镜像的位置，
        // 拿别处像素的深度做比较，会大面积误剔除（实测 04：视锥可见 41 个 → 只剩 19 个，
        // 且画面逐渐缺失墙体）。这一条与"最近深度"合起来才构成保守判定。
        float2 s=float2(ndc.x*0.5+0.5,0.5-ndc.y*0.5);rMin=min(rMin,s);rMax=max(rMax,s);nearestZ=min(nearestZ,ndc.z);}
    rMin=clamp(rMin,0,1);rMax=clamp(rMax,0,1);
    float2 sz=(rMax-rMin)*screenSize;float mip=clamp(ceil(log2(max(1,max(sz.x,sz.y)))),0,float(hizMipCount-1));
    // 【C3 保守遮挡判定】原实现只采屏幕包围盒的 **4 个角**：min 深度金字塔的每个样本是
    // "该 mip texel 覆盖盒内**最近**表面"，4 个角只约束了角上 texel，对**区域内部没有任何约束**
    // —— 遮挡物只盖住包围盒中间时，4 角样本全是远处深度 ⇒ max(4 角) 偏小 ⇒ 判定更容易成立 ⇒
    // 误剔除（与上面"取 max(ndc.z) 更容易成立"是同一类错误，出在**采样覆盖**而不是角点选择上）。
    // 改成 3×3 覆盖采样（四个角 + 四个边中点 + 中心）：样本集合是原 4 角的超集，max 只可能变大
    // ⇒ 判定单调更保守（宁多画不可少画）。mip 由包围盒尺寸选定（盒 ≈ 1 个 texel），故 3×3 恰好
    // 覆盖盒跨 2×2 个 texel 的可能性。
    float maxDepth = 0.0;
    [unroll] for (int iy = 0; iy < 3; ++iy) {
        [unroll] for (int ix = 0; ix < 3; ++ix) {
            float2 uv = rMin + (rMax - rMin) * float2(float(ix) * 0.5, float(iy) * 0.5);
            maxDepth = max(maxDepth, u_HiZDepth.SampleLevel(u_HiZSampler, uv, mip));
        }
    }
    // 与采样区域的**最远**深度（3×3 覆盖采样的 max）比较：最近点仍比它远才判被遮挡。
    // 末尾 1e-6 是浮点余量（只往"多画"方向偏，不改变遮挡语义）。
    return nearestZ > maxDepth + 1e-6;
}

[numthreads(64,1,1)]
void main(uint3 id:SV_DispatchThreadID){
    if(id.x>=objectCount)return;
    GPUSceneObject o=u_SceneObjects[id.x];float3 mn=o.boundsMin.xyz,mx=o.boundsMax.xyz;
    // 【T7】visibilityFlags == 0 表示"不参与 GPU 驱动的间接绘制"：
    // 实例化网格（N 个实例共用 1 个对象条目）由 InstanceCuller 的逐实例路径单独绘制，
    // 若这里也发出命令，只会按对象矩阵画 1 个实例。
    if(o.visibilityFlags==0u)return;
    if(AABBOutside(mn,mx,frustumPlanes[0]))return;if(AABBOutside(mn,mx,frustumPlanes[1]))return;
    if(AABBOutside(mn,mx,frustumPlanes[2]))return;if(AABBOutside(mn,mx,frustumPlanes[3]))return;
    if(AABBOutside(mn,mx,frustumPlanes[4]))return;if(AABBOutside(mn,mx,frustumPlanes[5]))return;
    if(hizMipCount>0&&IsOccluded(mn,mx))return;
    uint idx;InterlockedAdd(u_DrawCount[0],1,idx);
    IndirectCmd cmd;cmd.indexCount=o.indexCount;cmd.instanceCount=1;cmd.firstIndex=o.firstIndex;cmd.vertexOffset=o.vertexOffset;cmd.firstInstance=o.objectID;
    u_IndirectCommands[idx]=cmd;
}
