// ============================================================
// Nanite_ClusterBVH.comp.slang — per-instance cluster BVH 的显式栈深度优先遍历
//   （§14.8 任务 14；设计 §5.1 Phase 2 的 "BVH traverse (cluster tree, depth-first)"）
//
// 【目标】逐实例遍历同一棵簇 BVH（由 CPU 构建并上传），用世界空间视锥对**节点球**与**簇球**
//   做保守剔除，把被接受的簇压缩写入可见簇列表（元素是 (instance, cluster) 二元组），同时
//   累加两个计数：可见簇数、**已访问节点数**。验收 = "BVH 节点数与遍历访问数可复现"。
//
// 【与 C++ 侧的布局契约】本文件的每个结构体都必须与
//   `Engine/Render/Nanite/NaniteTypes.h` 的 "§14.8 任务 14" 小节逐字段一致（那边用
//   sizeof + offsetof 的 static_assert 钉住）：
//     · `BVHNode`        —— 32B：float4 centerRadius + uint4 link(left,right,count,flags)
//     · `ClusterSphere`  —— 16B：float4(xyz = 网格空间球心，w = 半径)
//     · `ClusterRef`     —— 8B：(instance, cluster)
//     · `InstanceObject` —— GPUSceneObject（128B）契约的镜像；真身在
//       `Engine/Render/Pipeline/GPUScene.h:26-40`，`NaniteCull.cpp` 另有逐字段交叉 static_assert。
//       本文件**只用**它的平移列（localToWorld 的第 4 列）与 `indexCount`。
//     · push constant    —— 6 个世界空间视锥平面 + 4 个 u32（112B）。
//
// 【为什么把 localToWorld 拆成 4 个 float4 而不是写 `float4x4`】Slang 的矩阵布局约定
//   （行/列主序）与"内存里的 16 个 float"之间的对应关系依赖编译选项，读 `m[3]` 时容易
//   在两种约定下取到不同的槽位。拆成 `ltw0..ltw3` 后 `ltw3.xyz` **无条件**是内存里的
//   第 12/13/14 个 float（= 列主序矩阵的平移列，与 C++ 侧 `localToWorld[12..14]` 同一份比特），
//   风险归零。CPU 与 GPU 读的是同一份比特，因此不存在"两处各推一次"的末位差异。
//
// 【判据（必须与 CPU 参考逐字一致）】平面 `planes[i] = (n.xyz, d)`，`dot(n, p) + d >= 0` 在内侧；
//   任一平面满足 `dot(n, c) + d < -radius` ⇒ 不可见（**不加 epsilon**）。顺序
//   `[左, 右, 下, 上, 近, 远]`，法线已归一化（CPU 侧提取函数负责）。
//
// 【深度优先 + 显式栈】shader 没有递归，故用**固定大小的本地数组**当栈：先压右孩子、再压左
//   孩子 ⇒ 下一次弹出的是左孩子（左子树先访问），与 `NaniteTraverseClusterBVHCPU` 的顺序一致。
//   栈容量 32 取自 C++ 侧 `kNaniteBVHMaxStackDepth`（= 深度上限 24 + 8 余量）；构建器保证树高
//   ≤ 24 ⇒ 栈里同时存在的条目数（每层至多一个"待访问的右兄弟"）不会超过 24。真溢出不静默：
//   构建端把"深度上限"当硬不变量，单测直接断言。
//
// 【可见列表的顺序】用"原子取槽位再写"压缩 ⇒ 槽位分配顺序**不定**（GPU 线程调度相关）。
//   CPU 参考按实例升序 + DFS 顺序紧凑写入；验收读回会把 GPU 列表排序后再逐项比较
//   （集合等价，见 NaniteRenderer.cpp），与任务 13 同一口径。
//
// 【§14.3 依赖禁令】只依赖本模块自己的 POD，不引用 GI/Lumen/GPUCulling。
// ============================================================

// BVH 节点（32B）：与 NaniteBVHNode 逐字段一致
//   link.x = 内部节点左孩子下标 / 叶子簇表首下标
//   link.y = 内部节点右孩子下标 / 叶子为 kNaniteBVHNoChild
//   link.z = 内部节点 2 / 叶子簇数（≥1）
//   link.w = 位标志：bit0 = 1 表示叶子
struct BVHNode {
    float4 centerRadius;   // 偏移 0：xyz = 网格空间包围球心；w = 半径
    uint4  link;           // 偏移 16：(left, right, count, flags)
};

// 每簇包围球（网格空间；与 NaniteClusterSphere 一致：3×float + radius = 16B）
struct ClusterSphere {
    float4 centerRadius;   // xyz = 网格空间球心；w = 半径（≥ 0）
};

// 一条可见簇引用（8B；与 NaniteVisibleClusterRef 一致）
struct ClusterRef {
    uint instance;
    uint cluster;
};

// GPUSceneObject（128B）的 Slang 镜像：字段顺序/类型必须与 NaniteInstanceGpuObject 一致。
// 【本文件只用两个字段】`ltw3.xyz`（实例平移）与 `indexCount`（空实例跳过）；
//   其余字段原样保留，仅为把 128B 的步长钉对。
struct InstanceObject {
    float4 ltw0;              // 偏移 0  ：localToWorld 第 1 列
    float4 ltw1;              // 偏移 16 ：localToWorld 第 2 列
    float4 ltw2;              // 偏移 32 ：localToWorld 第 3 列
    float4 ltw3;              // 偏移 48 ：localToWorld 第 4 列（xyz = 平移，列主序）
    float4 boundsMin;         // 偏移 64 ：世界空间 AABB 下界（w 不用）
    float4 boundsMax;         // 偏移 80 ：世界空间 AABB 上界（w 不用）
    uint   meshIndex;         // 偏移 96
    uint   materialIndex;     // 偏移 100
    uint   objectID;          // 偏移 104
    uint   visibilityFlags;   // 偏移 108
    uint   indexCount;        // 偏移 112：0 ⇒ 空实例，跳过（与 CPU 参考同规则）
    uint   firstIndex;        // 偏移 116
    int    vertexOffset;      // 偏移 120
    uint   _pad;              // 偏移 124
};

// 显式栈容量：必须与 C++ 侧 kNaniteBVHMaxStackDepth（= 32）一致。
static const uint kBVHMaxStackDepth = 32u;

// 显式描述符绑定（set=0）：
//   0 = BVH 节点表（只读）
//   1 = 叶子簇表（只读；叶子用 [link.x, link.x + link.z)）
//   2 = 簇包围球表（只读，网格空间）
//   3 = 实例表（128B 契约，只读；本任务读平移列与 indexCount）
//   4 = 可见簇列表（读写；原子取槽位压缩写入）
//   5 = 可见簇计数（读写；单个 uint，每帧由命令缓冲内的拷贝清 0）
//   6 = 已访问节点计数（读写；单个 uint，同上）
[[vk::binding(0, 0)]] StructuredBuffer<BVHNode>       u_Nodes;
[[vk::binding(1, 0)]] StructuredBuffer<uint>          u_LeafClusters;
[[vk::binding(2, 0)]] StructuredBuffer<ClusterSphere> u_ClusterSpheres;
[[vk::binding(3, 0)]] StructuredBuffer<InstanceObject> u_Instances;
[[vk::binding(4, 0)]] RWStructuredBuffer<ClusterRef>  u_VisibleClusters;
[[vk::binding(5, 0)]] RWStructuredBuffer<uint>        u_VisibleClusterCount;
[[vk::binding(6, 0)]] RWStructuredBuffer<uint>        u_VisitedNodeCount;

[[vk::push_constant]] cbuffer NaniteClusterBVHParams {
    float4 planes[6];         // 偏移 0 ：世界空间视锥六平面（[左,右,下,上,近,远]，n 已归一化）
    uint   instanceCount;     // 偏移 96：本帧实例数（= cfg 键 nanite_instance_test_count，已钳制）
    uint   clusterCount;      // 偏移 100：簇数（= 簇球表条数）
    uint   nodeCount;         // 偏移 104：BVH 节点数
    uint   visibleCapacity;   // 偏移 108：可见簇列表容量（超出容量的槽位不写；计数照常累加，
                              //             读回时按容量截断 —— 与 CPU 参考的写入口径一致）
};

// 球-视锥可见性：任一面外侧即不可见（与 NaniteSphereVisibleInFrustum 同判据、同比较符）
bool sphereVisibleInFrustum(float3 center, float radius) {
    // 与 CPU 侧一致：半径 < 0 按 0 处理（NaN 半径走"可见"分支，生成侧不产生 NaN）
    float r = (radius > 0.0f) ? radius : 0.0f;
    for (int i = 0; i < 6; ++i) {
        float distance = dot(planes[i].xyz, center) + planes[i].w;
        if (distance < -r) return false;
    }
    return true;
}

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= instanceCount) return;

    // ① 空实例（无索引可画）不参与 —— 与 CPU 参考的 indexCount == 0 跳过逐条对应
    InstanceObject instance = u_Instances[id.x];
    if (instance.indexCount == 0u) return;

    // ② 实例变换的平移列（列主序的第 4 列）；本任务的合成实例是纯平移，半径不变
    float3 origin = instance.ltw3.xyz;

    // ③ 每实例一次深度优先遍历（显式栈；先压右、再压左 ⇒ 左子树先访问）
    uint visited = 0u;
    uint stack[kBVHMaxStackDepth];
    uint stackSize = 0u;
    stack[stackSize++] = 0u;   // 根节点恒为 0（CPU 构建器保证）

    while (stackSize > 0u) {
        uint nodeIndex = stack[--stackSize];
        if (nodeIndex >= nodeCount) continue;   // 防御：非法孩子下标不越界
        ++visited;                              // 先计数、后判可见（与 CPU 参考同位置）

        BVHNode node = u_Nodes[nodeIndex];
        if (!sphereVisibleInFrustum(origin + node.centerRadius.xyz, node.centerRadius.w)) {
            continue;   // 节点球不可见 ⇒ 整棵子树跳过（节点球是后代簇球的保守并集）
        }

        if ((node.link.w & 1u) != 0u) {
            // 叶子：对 [link.x, link.x + link.z) 的每个簇做球测试
            uint first = node.link.x;
            uint count = node.link.z;
            for (uint k = 0u; k < count; ++k) {
                uint cluster = u_LeafClusters[first + k];
                if (cluster >= clusterCount) continue;   // 防御：越界簇下标不读球表
                ClusterSphere sphere = u_ClusterSpheres[cluster];
                if (!sphereVisibleInFrustum(origin + sphere.centerRadius.xyz, sphere.centerRadius.w)) {
                    continue;
                }
                // 原子取槽位：计数缓冲的最终值 = 被接受的簇引用总数（与 CPU 的 written 同义）
                uint slot;
                InterlockedAdd(u_VisibleClusterCount[0], 1u, slot);
                if (slot < visibleCapacity) {
                    ClusterRef ref;
                    ref.instance = id.x;
                    ref.cluster  = cluster;
                    u_VisibleClusters[slot] = ref;
                }
            }
        } else {
            // 内部：先压右、再压左 ⇒ 下一次弹出的是左孩子（深度优先、顺序与 CPU 一致）
            // 溢出不静默：构建端把树高硬限制在 kBVHMaxStackDepth 以内（单测断言）。
            if (stackSize + 2u <= kBVHMaxStackDepth) {
                stack[stackSize++] = node.link.y;
                stack[stackSize++] = node.link.x;
            }
        }
    }

    // ④ 把本线程（本实例）访问过的节点数一次性累加：整数加法可交换 ⇒ 与线程顺序无关，
    //    同输入两次运行的读数逐位相同（这正是"遍历访问数可复现"的实现前提）。
    InterlockedAdd(u_VisitedNodeCount[0], visited);
}
