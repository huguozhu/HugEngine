// PersistentCull.comp.slang — PTG 视锥 + Hi-Z 遮挡剔除
//
// 两种模式：
//   PTG_PERSISTENT=1: 持久化线程组，Dispatch 一次后 spin-wait 等待每帧信号
//   PTG_PERSISTENT=0: 每帧 Dispatch，处理完成后退出（与普通 GPUCull 相同）

#ifndef PTG_PERSISTENT
#define PTG_PERSISTENT 0
#endif

static const uint PTG_TOTAL_THREADS = 64;
static const uint MAX_ITERATIONS    = 1024; // 单帧最大迭代次数（TDR 保护）

// 每帧参数（CPU 每帧通过 Map/Unmap 更新）
struct PTGParams {
    float4x4 viewProj;         // [0..64)   投影矩阵（世界→裁剪空间）
    float4   frustumPlanes[6]; // [64..160) 6 个视锥平面（Gribb-Hartmann）
    float2   screenSize;       // [160..168) 屏幕像素宽高
    uint     objectCount;      // [168]     场景物体总数
    uint     hizMipCount;      // [172]     Hi-Z mip 级数（0=禁用遮挡剔除）
    uint     frameIndex;       // [176]     帧索引（0xFFFFFFFF=退出持久化模式）
};

// 场景物体数据（256 字节，与现有 GPUSceneObject 一致）
struct GPUSceneObject {
    float4x4 localToWorld; float4 boundsMin, boundsMax;
    uint meshIndex, materialIndex, objectID, visibilityFlags;
    uint indexCount, firstIndex; int vertexOffset; uint _pad1;
};

// 间接绘制命令（与现有 IndirectDrawCommand 一致）
struct IndirectCmd {
    uint indexCount; uint instanceCount; uint firstIndex;
    int vertexOffset; uint firstInstance;
};

[[vk::binding(0, 0)]] ConstantBuffer<PTGParams>         u_PTGParams;
[[vk::binding(1, 0)]] StructuredBuffer<GPUSceneObject>  u_SceneObjects;
[[vk::binding(2, 0)]] Texture2D<float>                  u_HiZDepth;
[[vk::binding(2, 0)]] SamplerState                      u_HiZSampler;
[[vk::binding(3, 0)]] RWStructuredBuffer<IndirectCmd>   u_IndirectCommands;
[[vk::binding(4, 0)]] RWStructuredBuffer<uint>          u_DrawCount;

bool AABBOutside(float3 aabbMin, float3 aabbMax, float4 plane) {
    float3 n = plane.xyz;
    float3 p;
    p.x = (n.x >= 0) ? aabbMax.x : aabbMin.x;
    p.y = (n.y >= 0) ? aabbMax.y : aabbMin.y;
    p.z = (n.z >= 0) ? aabbMax.z : aabbMin.z;
    return dot(n, p) + plane.w < 0;
}

bool IsOccluded(float3 mn, float3 mx) {
    float2 rMin = float2(1e10, 1e10), rMax = float2(-1e10, -1e10);
    float mZ = 0;
    for (int c = 0; c < 8; c++) {
        float3 corner = float3((c & 1) ? mx.x : mn.x,
                               (c & 2) ? mx.y : mn.y,
                               (c & 4) ? mx.z : mn.z);
        float4 clip = mul(u_PTGParams.viewProj, float4(corner, 1));
        float3 ndc = clip.xyz / max(clip.w, 0.0001);
        if (clip.w < 0) return false;
        float2 s = ndc.xy * 0.5 + 0.5;
        rMin = min(rMin, s);
        rMax = max(rMax, s);
        mZ = max(mZ, ndc.z);
    }
    rMin = clamp(rMin, 0, 1);
    rMax = clamp(rMax, 0, 1);
    float2 sz = (rMax - rMin) * u_PTGParams.screenSize;
    float mip = clamp(ceil(log2(max(1, max(sz.x, sz.y)))),
                      0, float(u_PTGParams.hizMipCount - 1));
    float4 d;
    d.x = u_HiZDepth.SampleLevel(u_HiZSampler, rMin, mip);
    d.y = u_HiZDepth.SampleLevel(u_HiZSampler, float2(rMax.x, rMin.y), mip);
    d.z = u_HiZDepth.SampleLevel(u_HiZSampler, float2(rMin.x, rMax.y), mip);
    d.w = u_HiZDepth.SampleLevel(u_HiZSampler, rMax, mip);
    return mZ > max(max(d.x, d.y), max(d.z, d.w));
}

groupshared uint g_DoneFrameIndex;

[shader("compute")]
[numthreads(PTG_TOTAL_THREADS, 1, 1)]
void main(uint3 gtid : SV_GroupThreadID) {

#if PTG_PERSISTENT
    // ── 持久化模式：Dispatch 一次，spin-wait 循环 ──
    if (gtid.x == 0) g_DoneFrameIndex = 0;
    GroupMemoryBarrierWithGroupSync();

    while (true) {
        uint frameIdx;
        do { frameIdx = u_PTGParams.frameIndex; }
        while (frameIdx == g_DoneFrameIndex);

        if (frameIdx == 0xFFFFFFFF) return;  // 退出信号
        g_DoneFrameIndex = frameIdx;

        if (gtid.x == 0) u_DrawCount[0] = 0;
        GroupMemoryBarrierWithGroupSync();
#else
    // ── 每帧 Dispatch 模式：处理一次即退出 ──
    if (gtid.x == 0) u_DrawCount[0] = 0;
    GroupMemoryBarrierWithGroupSync();
#endif

        // ── 并行剔除所有物体 ──
        uint count = u_PTGParams.objectCount;
        uint globalIdx = gtid.x;

        for (uint iter = 0; iter < MAX_ITERATIONS && globalIdx < count;
             iter++, globalIdx += PTG_TOTAL_THREADS) {

            GPUSceneObject o = u_SceneObjects[globalIdx];
            float3 mn = o.boundsMin.xyz, mx = o.boundsMax.xyz;

            bool visible = true;
            [unroll] for (int i = 0; i < 6 && visible; i++) {
                if (AABBOutside(mn, mx, u_PTGParams.frustumPlanes[i])) visible = false;
            }
            if (!visible) continue;

            if (u_PTGParams.hizMipCount > 0 && IsOccluded(mn, mx)) continue;

            uint slot;
            InterlockedAdd(u_DrawCount[0], 1, slot);
            IndirectCmd cmd;
            cmd.indexCount    = o.indexCount;
            cmd.instanceCount = 1;
            cmd.firstIndex    = o.firstIndex;
            cmd.vertexOffset  = o.vertexOffset;
            cmd.firstInstance = o.objectID;
            u_IndirectCommands[slot] = cmd;
        }

        DeviceMemoryBarrier();

#if PTG_PERSISTENT
    }  // end while(true)
#endif
}
