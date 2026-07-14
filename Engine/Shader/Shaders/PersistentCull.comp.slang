// PersistentCull.comp.slang — 持久化线程组视锥 + Hi-Z 遮挡剔除
// Dispatch 一次后，每帧通过 u_PTGParams.frameIndex 触发处理
// 减少 GPU 调度开销，适合物体数量波动不大的场景

static const uint PTG_TOTAL_THREADS = 64;
static const uint PTG_GROUP_COUNT   = 4;   // 4 组 × 64 线程 = 256 线程
static const uint MAX_ITERATIONS    = 1024; // 单帧最大迭代次数（TDR 保护）

// 每帧参数（CPU 每帧通过 Map/Unmap 更新）
struct PTGParams {
    float4x4 viewProj;         // [0..64)   投影矩阵（世界→裁剪空间）
    float4   frustumPlanes[6]; // [64..160) 6 个视锥平面（Gribb-Hartmann）
    float2   screenSize;       // [160..168) 屏幕像素宽高
    uint     objectCount;      // [168]     场景物体总数
    uint     hizMipCount;      // [172]     Hi-Z mip 级数（0=禁用遮挡剔除）
    uint     frameIndex;       // [176]     帧索引（CPU 写入以触发处理，0xFFFFFFFF=退出）
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

// ============================================================
// 描述符绑定
// ============================================================
[[vk::binding(0, 0)]] ConstantBuffer<PTGParams>         u_PTGParams;
[[vk::binding(1, 0)]] StructuredBuffer<GPUSceneObject>  u_SceneObjects;
[[vk::binding(2, 0)]] Texture2D<float>                  u_HiZDepth;
[[vk::binding(2, 0)]] SamplerState                      u_HiZSampler;
[[vk::binding(3, 0)]] RWStructuredBuffer<IndirectCmd>   u_IndirectCommands;
[[vk::binding(4, 0)]] RWStructuredBuffer<uint>          u_DrawCount;

// ============================================================
// 共享变量（线程组内可见）
// ============================================================
groupshared uint g_VisibleCount;
groupshared uint g_ProcessedFrameIndex;

// ============================================================
// 视锥剔除：AABB 与单平面的相交测试
// ============================================================
bool AABBOutside(float3 aabbMin, float3 aabbMax, float4 plane) {
    float3 n = plane.xyz;
    float3 p;
    p.x = (n.x >= 0) ? aabbMax.x : aabbMin.x;
    p.y = (n.y >= 0) ? aabbMax.y : aabbMin.y;
    p.z = (n.z >= 0) ? aabbMax.z : aabbMin.z;
    return dot(n, p) + plane.w < 0;
}

// ============================================================
// Hi-Z 遮挡剔除：将 AABB 8 个顶点投影到屏幕空间，
// 计算覆盖区域，采样对应 mip 级别的深度值进行比较
// ============================================================
bool IsOccluded(float3 mn, float3 mx) {
    float2 rMin = float2(1e10, 1e10), rMax = float2(-1e10, -1e10);
    float mZ = 0;
    for (int c = 0; c < 8; c++) {
        float3 corner = float3((c & 1) ? mx.x : mn.x,
                               (c & 2) ? mx.y : mn.y,
                               (c & 4) ? mx.z : mn.z);
        float4 clip = mul(u_PTGParams.viewProj, float4(corner, 1));
        float3 ndc = clip.xyz / max(clip.w, 0.0001);
        if (clip.w < 0) return false;  // 跨近平面不剔除
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
    return mZ > min(min(d.x, d.y), min(d.z, d.w));
}

// ============================================================
// 主入口：持久化线程组主循环
// ============================================================
[shader("compute")]
[numthreads(PTG_TOTAL_THREADS, 1, 1)]
void main(uint3 gtid : SV_GroupThreadID, uint3 gid : SV_GroupID) {
    // 初始化共享变量（仅主线程执行）
    if (gtid.x == 0) {
        g_VisibleCount = 0;
        g_ProcessedFrameIndex = 0;  // 初始为 0，等待帧索引 >= 1 才处理
    }
    GroupMemoryBarrierWithGroupSync();

    while (true) {
        // Spin-wait：等待 CPU 写入新的帧索引
        uint frameIdx;
        do {
            frameIdx = u_PTGParams.frameIndex;
        } while (frameIdx == g_ProcessedFrameIndex);

        // 检测退出信号（0xFFFFFFFF = 关闭 PTG）
        if (frameIdx == 0xFFFFFFFF) return;

        // 记录当前处理的帧索引
        g_ProcessedFrameIndex = frameIdx;

        // 重置本帧可见计数
        if (gtid.x == 0) g_VisibleCount = 0;
        GroupMemoryBarrierWithGroupSync();

        // 计算本线程在全局中的起始物体索引
        uint count = u_PTGParams.objectCount;
        uint globalIdx = gid.x * PTG_TOTAL_THREADS + gtid.x;
        uint totalThreads = PTG_GROUP_COUNT * PTG_TOTAL_THREADS;

        // TDR 保护：限制单帧最大迭代次数
        // 通过 stride = totalThreads 实现线程间负载均衡
        for (uint iter = 0; iter < MAX_ITERATIONS && globalIdx < count;
             iter++, globalIdx += totalThreads) {

            GPUSceneObject o = u_SceneObjects[globalIdx];
            float3 mn = o.boundsMin.xyz, mx = o.boundsMax.xyz;

            // 视锥剔除：6 个平面依次测试
            bool visible = true;
            [unroll] for (int i = 0; i < 6 && visible; i++) {
                float3 n = u_PTGParams.frustumPlanes[i].xyz;
                float3 p;
                p.x = (n.x >= 0) ? mx.x : mn.x;
                p.y = (n.y >= 0) ? mx.y : mn.y;
                p.z = (n.z >= 0) ? mx.z : mn.z;
                if (dot(n, p) + u_PTGParams.frustumPlanes[i].w < 0) visible = false;
            }
            if (!visible) continue;

            // Hi-Z 遮挡剔除（启用时）
            if (u_PTGParams.hizMipCount > 0 && IsOccluded(mn, mx)) continue;

            // 可见：原子写入 IndirectDrawCommand
            uint slot;
            InterlockedAdd(g_VisibleCount, 1, slot);
            IndirectCmd cmd;
            cmd.indexCount    = o.indexCount;
            cmd.instanceCount = 1;
            cmd.firstIndex    = o.firstIndex;
            cmd.vertexOffset  = o.vertexOffset;
            cmd.firstInstance = o.objectID;
            u_IndirectCommands[slot] = cmd;
        }

        // 确保所有写入对其他线程组可见
        DeviceMemoryBarrierWithGroupSync();

        // 主线程更新最终可见物体计数
        if (gtid.x == 0) {
            u_DrawCount[0] = g_VisibleCount;
        }
    }
}
