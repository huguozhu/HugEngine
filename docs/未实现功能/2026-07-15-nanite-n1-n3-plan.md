# Nanite N1-N3 实现计划 — 预处理 + 剔除 + 软光栅 GBuffer

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [x]`) syntax for tracking.

**Goal:** 实现 Nanite 虚拟几何从预处理到 GBuffer 渲染的完整路径

**Architecture:** N1(Python)预处理 → 生成.nanite(Cluster/LOD/DAG/量化) → N2(C++)GPU上传+两阶段剔除(Instance→Cluster) → N3(Compute Shader)软光栅化写入GBuffer

**Tech Stack:** Python 3.11+ / meshoptimizer / C++20 / Slang Compute / VK 1.3 / DeferredPipeline

**Spec:** `docs/superpowers/specs/2026-07-15-lumen-nanite-design.md`

## Global Constraints

- 所有新加代码添加中文注释
- Commit log 使用中文，不含 AI 信息
- Python 预处理脚本独立于引擎运行时，依赖仅 meshoptimizer + numpy
- C++ 运行时集成到 HugEngineRender 模块
- Shader 统一使用 Slang .comp/.mesh 命名规范
- 头文件列入 CMakeLists target_sources，使用 huge_source_group()

---

## File Structure

```
新增:
  Tools/NanitePreprocess/
    NanitePreprocess.py           # 主入口：Mesh → .nanite
    NaniteCluster.py              # Cluster 划分 (meshoptimizer)
    NaniteLOD.py                  # 边折叠 LOD 生成
    NaniteDAG.py                  # DAG 去重
    NaniteQuantize.py             # 顶点量化 R10G10B10A2
    NanitePack.py                 # .nanite 二进制打包 + 头部写入

  Engine/Scene/Scene/NaniteComponent.h   # NaniteMesh 场景组件
  Engine/Scene/Scene/NaniteComponent.cpp # 上传.nanite→GPU Buffer

  Engine/Render/Pipeline/
    NaniteRenderer.h              # 渲染器：剔除 + LOD + 光栅化调度
    NaniteRenderer.cpp
    NaniteUpload.h                # GPU 资源创建 (Cluster/VB/IB/Indirection Buffer)
    NaniteUpload.cpp
    NaniteCulling.h               # 两阶段剔除 (Instance + Cluster)
    NaniteCulling.cpp
    NaniteLODSelect.h             # LOD 选择
    NaniteLODSelect.cpp
    NaniteSoftRaster.h            # Compute 软光栅化到 GBuffer
    NaniteSoftRaster.cpp

  Engine/Shader/Shaders/Nanite/
    Nanite_InstanceCull.comp      # 视锥 + Hi-Z 实例剔除
    Nanite_ClusterCull.comp       # BVH 遍历 + Cluster 剔除
    Nanite_LODSelect.comp         # LOD 选择 (projected error)
    Nanite_SoftRaster.comp        # 计算着色器软件光栅化
    NaniteTypes.slang             # GPU 端 NaniteCluster 等共享结构体
    NaniteShared.slang            # 光栅化公用函数

修改:
  Engine/Render/Pipeline/DeferredPipeline.h       # 新增 Nanite GBuffer 模式
  Engine/Render/Pipeline/DeferredPipeline.cpp      # Nanite 路径集成
  Engine/Render/CMakeLists.txt                     # 新增源文件
  Engine/Shader/CMakeLists.txt                     # 新增 Nanite Shader
```

---

### Task 1: Nanite 数据格式定义

**Files:**
- Create: `Engine/Shader/Shaders/Nanite/NaniteTypes.slang`
- Create: `Engine/Render/Pipeline/NaniteRenderer.h` (数据格式部分)

**Interfaces:**
- Produces: `NaniteCluster` GPU 结构体, `NaniteInstance` GPU 结构体, `.nanite` 文件头

```cpp
// Engine/Render/Pipeline/NaniteRenderer.h

#pragma once
#include "RHI/RHI.h"
#include <vector>
#include <memory>

namespace he::render {

// ── GPU 端结构体 (与 NaniteTypes.slang 保持一致) ──
struct alignas(16) NaniteCluster {
    float4 boundingSphere;       // xyz=center, w=radius
    float4 coneAxisAngle;        // xyz=coneAxis, w=coneAngle(cos)
    u32    triangleOffset;       // index buffer 偏移 (三角形数)
    u32    triangleCount;        // 三角形数 (≤64)
    u32    vertexOffset;         // vertex buffer 偏移
    u32    materialID;           // bindless 材质 ID
    float  maxParentLODError;    // 父级 LOD 误差阈值
    u32    childClusterOffset;   // BVH 子节点起始索引 (0 表示叶子)
    u32    childCount;
    u32    _pad;
};

// GPU 实例 (per-mesh-instance)
struct alignas(16) NaniteInstance {
    float4x4 worldMatrix;
    float4x4 normalMatrix;
    float4   boundsCenterRadius;  // xyz=center, w=radius (world space)
    u32      clusterBase;         // cluster buffer 中的起始索引
    u32      clusterCount;
    u32      vertexBase;
    u32      indexBase;
    u32      materialBase;        // bindless 材质起始索引
    u32      flags;               // bit0: visible
    float    lodScale;            // LOD 缩放因子
    float    _pad0;
};

// .nanite 文件头 (C++ / Python 共享)
struct NaniteFileHeader {
    char     magic[8];          // "NANITE01"
    u32      version;           // 1
    u32      clusterCount;
    u32      vertexCount;       // 量化后顶点数
    u32      indexCount;        // 三角形索引数
    u32      materialCount;
    u32      lodLevelCount;     // LOD 层数
    u32      flags;             // bit0: hasDAG
    float    bboxMin[3];
    float    bboxMax[3];
    float    maxLODError;       // 最大几何误差
    u32      _reserved[8];
};

} // namespace he::render
```

```hlsl
// Engine/Shader/Shaders/Nanite/NaniteTypes.slang

// GPU 端 NaniteCluster — 与 C++ NaniteRenderer.h 保持一致
struct NaniteCluster {
    float4 boundingSphere;       // xyz=center, w=radius
    float4 coneAxisAngle;
    uint   triangleOffset;
    uint   triangleCount;
    uint   vertexOffset;
    uint   materialID;
    float  maxParentLODError;
    uint   childClusterOffset;
    uint   childCount;
    uint   _pad;
};

struct NaniteInstance {
    float4x4 worldMatrix;
    float4x4 normalMatrix;
    float4   boundsCenterRadius;
    uint     clusterBase;
    uint     clusterCount;
    uint     vertexBase;
    uint     indexBase;
    uint     materialBase;
    uint     flags;
    float    lodScale;
    float    _pad0;
};

// 量化顶点 (R10G10B10A2 + 量化范围)
struct NaniteVertex {
    uint packedPosition;   // R10G10B10A2_SNORM (xyz) + w=1
    uint packedNormal;     // R10G10B10A2_SNORM (xyz)
    uint packedUV;         // R16G16_UNORM (uv)
    uint _pad;
};

// 三角形索引 (3×u16 打包到一个 u32[2])
// indices[0]: i0 | (i1 << 16)
// indices[1]: i2 | (padding << 16)
```

- [ ] **Step 1: 创建头文件 + Shader 结构体**

创建 `Engine/Render/Pipeline/NaniteRenderer.h` 和 `Engine/Shader/Shaders/Nanite/NaniteTypes.slang`，包含上述全部代码。

- [ ] **Step 2: 编译验证**

```bash
cmake -B build && cmake --build build --target HugEngineRender --config Debug
```

预期：编译通过 (仅结构体定义，无链接依赖)。

---

### Task 2: Python 预处理 — Cluster 划分

**Files:**
- Create: `Tools/NanitePreprocess/NanitePreprocess.py` (主入口骨架)
- Create: `Tools/NanitePreprocess/NaniteCluster.py`

**Interfaces:**
- Consumes: meshoptimizer `meshopt_buildMeshlets`
- Produces: `cluster_bounds: list[(center, radius, cone_axis, cone_angle)]`, `cluster_indices: list[list[u16]]`

```python
# Tools/NanitePreprocess/NaniteCluster.py

import meshoptimizer
import numpy as np

# 三角形索引 (glTF 兼容)
# indices: np.ndarray shape=(N,3) dtype=uint32
# vertices: np.ndarray shape=(V,3) dtype=float32

def build_clusters(indices: np.ndarray, vertices: np.ndarray,
                   max_vertices: int = 128, max_triangles: int = 64,
                   cone_weight: float = 0.5) -> list:
    """
    使用 meshoptimizer meshopt_buildMeshlets 将网格划分为 cluster。
    返回:
        clusters: list[dict] 每个 cluster 包含:
            - vertices: list[u32] 顶点索引列表
            - indices: list[u32] 三角形索引 (3×triCount)
            - bounds_center: float3
            - bounds_radius: float
            - cone_axis: float3 (法线锥轴)
            - cone_cutoff: float (法线锥角度 cos)
    """
    # 1. 调用 meshoptimizer 划分
    # meshopt_buildMeshlets 返回 meshlet 数组
    max_meshlets = 4096  # 初始容量
    meshlets = meshoptimizer.build_meshlets(
        indices, vertices, max_vertices, max_triangles, cone_weight
    )

    # 2. 计算每个 cluster 的包围球 + 法线锥
    clusters = []
    for m in meshlets:
        # 从 meshopt_Meshlet 提取数据
        vert_indices = list(m.vertices[:m.vertex_count])
        tri_indices = list(m.indices[:m.triangle_count * 3])

        # 计算包围球 (简单遍历)
        verts = vertices[vert_indices]
        center = verts.mean(axis=0)
        radius = np.max(np.linalg.norm(verts - center, axis=1))

        # 法线锥: meshopt 已计算 (m.cone_apex, m.cone_axis, m.cone_cutoff)
        cone_axis = np.array([m.cone_axis[0], m.cone_axis[1], m.cone_axis[2]])
        cone_cutoff = m.cone_cutoff

        clusters.append({
            'vertices': vert_indices,
            'indices': tri_indices,
            'bounds_center': center,
            'bounds_radius': float(radius),
            'cone_axis': cone_axis,
            'cone_cutoff': float(cone_cutoff),
            'triangle_count': m.triangle_count,
            'vertex_count': m.vertex_count,
        })

    return clusters
```

- [ ] **Step 1: 安装 meshoptimizer Python bindings**

```bash
pip install meshoptimizer numpy
```

- [ ] **Step 2: 编写 Cluster 划分代码**

写入 `Tools/NanitePreprocess/NaniteCluster.py` 和主入口 `NanitePreprocess.py`。

- [ ] **Step 3: 准备测试网格并验证**

```bash
python Tools/NanitePreprocess/NaniteCluster.py --input test_cube.obj
```

预期输出: cluster 数量 > 0，每个 cluster 顶点数 ≤ 128，三角形数 ≤ 64。

- [ ] **Step 4: 验证 Cluster 正确性**

在 Python 中重建 cluster 三角形，检查所有顶点索引合法。

---

### Task 3: Python 预处理 — LOD 生成 + DAG + 量化

**Files:**
- Create: `Tools/NanitePreprocess/NaniteLOD.py`
- Create: `Tools/NanitePreprocess/NaniteDAG.py`
- Create: `Tools/NanitePreprocess/NaniteQuantize.py`

**Interfaces:**
- Consumes: Task 2 clusters, meshoptimizer `meshopt_simplify`
- Produces: LOD 层级 clusters, DAG 去重表, 量化顶点/索引缓冲

```python
# Tools/NanitePreprocess/NaniteLOD.py

def generate_lods(indices: np.ndarray, vertices: np.ndarray,
                  base_clusters: list, max_levels: int = 6) -> list[list]:
    """
    使用边折叠生成 LOD 层级。
    返回: lods[level] = clusters_at_level
    LOD0 = base_clusters (原始)
    LOD1 = 50% triangles
    LOD2 = 25% triangles
    ...
    """
    lods = [base_clusters]

    for level in range(1, max_levels):
        # 简化：目标三角形数约为上一级的 50%
        target_triangles = len(indices) // (2 ** level)
        if target_triangles < 64:
            break  # 已经足够简化，不再生成更多层级

        # meshopt_simplify
        simplified_indices = meshoptimizer.simplify(
            indices, vertices, target_triangles
        )

        # 对简化后的网格重新划分 cluster
        simplified_clusters = build_clusters(
            np.array(simplified_indices).reshape(-1, 3), vertices
        )
        lods.append(simplified_clusters)

    return lods


# Tools/NanitePreprocess/NaniteDAG.py

def build_dag(lods: list[list]) -> tuple[list, list[tuple[int, int]]]:
    """
    跨 LOD 层级去重相同的 cluster，构建 DAG。
    返回:
        unique_clusters: 去重后的所有 cluster (按 LOD 排列)
        dedup_map: [(lod, cluster_idx) → unique_idx] 映射表
    """
    cluster_hashes = {}  # hash → unique_idx
    unique_clusters = []
    dedup_map = []

    for lod_idx, lod in enumerate(lods):
        for cluster_idx, c in enumerate(lod):
            # 哈希 cluster 内容 (顶点 + 索引 + bounds)
            content = (
                tuple(sorted(c['vertices'])),
                tuple(sorted(c['indices'])),
                tuple(c['bounds_center']),
                c['bounds_radius'],
            )
            h = hash(content)

            if h in cluster_hashes:
                dedup_map.append((lod_idx, cluster_idx, cluster_hashes[h]))
            else:
                unique_idx = len(unique_clusters)
                cluster_hashes[h] = unique_idx
                unique_clusters.append({**c, 'lod': lod_idx, 'unique_id': unique_idx})
                dedup_map.append((lod_idx, cluster_idx, unique_idx))

    return unique_clusters, dedup_map


# Tools/NanitePreprocess/NaniteQuantize.py

def quantize_vertices(vertices: np.ndarray, bbox_min: np.ndarray,
                      bbox_max: np.ndarray, bits: int = 10) -> np.ndarray:
    """
    将顶点量化到 R10G10B10A2_SNORM 空间。
    bits=10: 每轴 [-512, 511] 范围，精度 ~0.1%
    """
    extent = bbox_max - bbox_min
    scale = (2 ** (bits - 1) - 1) / np.max(extent)
    quantized = ((vertices - bbox_min) * scale).astype(np.int32)
    # 打包到 uint32: x[9:0] | y[19:10] | z[29:20] | w[31:30]
    packed = np.zeros(len(vertices), dtype=np.uint32)
    packed |= ((quantized[:, 0].astype(np.uint32) & 0x3FF))       # x bits 0-9
    packed |= ((quantized[:, 1].astype(np.uint32) & 0x3FF) << 10) # y bits 10-19
    packed |= ((quantized[:, 2].astype(np.uint32) & 0x3FF) << 20) # z bits 20-29
    # w=1 (implicit, decode 时补)
    return packed
```

- [ ] **Step 1: 编写 LOD 生成代码**

写入 `NaniteLOD.py`。在 `NanitePreprocess.py` 主流程中串联。

- [ ] **Step 2: 编写 DAG 去重**

写入 `NaniteDAG.py`。使用哈希表去重 cluster。

- [ ] **Step 3: 编写顶点量化**

写入 `NaniteQuantize.py`。量化精度 10 bits per axis。

- [ ] **Step 4: 集成测试**

```bash
python Tools/NanitePreprocess/NanitePreprocess.py --input Content/gltf/Sponza/glTF/Sponza.gltf --output Sponza.nanite
```

预期: 输出 .nanite 文件，cluster 数量 > 0，LOD 层级 > 0，DAG 去重率 > 10%。

---

### Task 4: Python 预处理 — .nanite 打包

**Files:**
- Create: `Tools/NanitePreprocess/NanitePack.py`

**Interfaces:**
- Consumes: Task 2 clusters + Task 3 LODs + DAG + quantized vertices
- Produces: `.nanite` 二进制文件

```python
# Tools/NanitePreprocess/NanitePack.py

import struct

def pack_nanite(output_path: str, header: dict, clusters: list,
                vertices: np.ndarray, indices: np.ndarray,
                materials: list, lod_offsets: list):
    """
    打包 .nanite 二进制文件:
        [NaniteFileHeader 128B]
        [NaniteCluster[]      (clusterCount × 64B)]
        [quantized vertices[] (vertexCount × 12B)]
        [indices[]            (indexCount × 4B)]
        [materials[]          (materialCount × 8B, bindless IDs)]
        [LOD offsets[]        (lodLevelCount × 4B)]
    """
    with open(output_path, 'wb') as f:
        # Header
        f.write(b'NANITE01')
        f.write(struct.pack('<I', header['version']))
        f.write(struct.pack('<I', header['cluster_count']))
        f.write(struct.pack('<I', header['vertex_count']))
        f.write(struct.pack('<I', header['index_count']))
        f.write(struct.pack('<I', header['material_count']))
        f.write(struct.pack('<I', header['lod_level_count']))
        f.write(struct.pack('<I', header['flags']))
        f.write(struct.pack('<3f', *header['bbox_min']))
        f.write(struct.pack('<3f', *header['bbox_max']))
        f.write(struct.pack('<f', header['max_lod_error']))
        f.write(struct.pack('<32x'))  # reserved[8]

        # Clusters (64B each)
        for c in clusters:
            f.write(struct.pack('<4f', *c['bounds_center'], c['bounds_radius']))
            f.write(struct.pack('<4f', *c['cone_axis'], c['cone_cutoff']))
            f.write(struct.pack('<4I', c['triangle_offset'], c['triangle_count'],
                                c['vertex_offset'], c['material_id']))
            f.write(struct.pack('<f', c['max_parent_lod_error']))
            f.write(struct.pack('<2I', c['child_cluster_offset'], c['child_count']))
            f.write(struct.pack('<I', 0))  # _pad

        # Quantized vertices (12B each: position(4B) + normal(4B) + uv(4B))
        for v in vertices:
            f.write(struct.pack('<I', v['packed_position']))
            f.write(struct.pack('<I', v['packed_normal']))
            f.write(struct.pack('<I', v['packed_uv']))

        # Indices (4B each)
        f.write(indices.astype('<u4').tobytes())

        # Materials (8B each: bindless texture ID)
        for m in materials:
            f.write(struct.pack('<2I', m.get('albedo_tex', 0), m.get('normal_tex', 0)))

        # LOD offsets (4B each)
        for off in lod_offsets:
            f.write(struct.pack('<I', off))

    print(f"[NanitePack] {output_path}: {len(clusters)} clusters, "
          f"{len(vertices)} vertices, {header['lod_level_count']} LOD levels")
```

- [ ] **Step 1: 编写二进制打包**

写入 `NanitePack.py`。

- [ ] **Step 2: 端到端验证**

```bash
python Tools/NanitePreprocess/NanitePreprocess.py --input Sponza.gltf --output Sponza.nanite
python -c "
import struct
with open('Sponza.nanite', 'rb') as f:
    magic = f.read(8)
    version = struct.unpack('<I', f.read(4))[0]
    assert magic == b'NANITE01'
    assert version == 1
    print('.nanite header OK')
"
```

预期: magic="NANITE01", version=1。

---

### Task 5: C++ 运行时 — NaniteComponent + GPU 上传

**Files:**
- Create: `Engine/Scene/Scene/NaniteComponent.h`
- Create: `Engine/Scene/Scene/NaniteComponent.cpp`
- Create: `Engine/Render/Pipeline/NaniteUpload.h`
- Create: `Engine/Render/Pipeline/NaniteUpload.cpp`

**Interfaces:**
- Consumes: Task 1 data structures, RHI buffer creation
- Produces: `NaniteComponent` (存放 GPU buffers + instance data)
- Produces: `NaniteUpload::UploadNaniteFile(path) → NaniteGPUData`

```cpp
// Engine/Scene/Scene/NaniteComponent.h
#pragma once
#include "Scene/Component.h"
#include "Scene/Transform.h"
#include "Render/Pipeline/NaniteRenderer.h"
#include <memory>
#include <string>

namespace he {

class NaniteComponent : public Component {
    HE_COMPONENT()
public:
    NaniteComponent() = default;

    // 关联的 .nanite 资源路径
    std::string naniteAsset;
    // GPU 数据句柄 (上传后的 cluster/vertex/index buffers)
    bool gpuReady = false;
    u32    instanceID = ~0u;  // GPU Instance 数组中的索引
};

} // namespace he


// Engine/Render/Pipeline/NaniteUpload.h
#pragma once
#include "Pipeline/NaniteRenderer.h"
#include "RHI/RHI.h"
#include <vector>

namespace he::render {

// 上传 .nanite 文件到 GPU 的返回数据
struct NaniteGPUData {
    std::unique_ptr<rhi::IRHIBuffer> clusterBuffer;   // NaniteCluster[]
    std::unique_ptr<rhi::IRHIBuffer> vertexBuffer;    // 量化顶点
    std::unique_ptr<rhi::IRHIBuffer> indexBuffer;     // 三角形索引
    std::unique_ptr<rhi::IRHIBuffer> materialBuffer;  // 材质 bindless ID
    u32 clusterCount;
    u32 vertexCount;
    u32 indexCount;
    u32 materialCount;
    u32 lodLevelCount;
    float maxLODError;
    float3 bboxMin, bboxMax;
};

// 从 .nanite 文件加载并上传到 GPU
class NaniteUpload {
public:
    static NaniteGPUData UploadNaniteFile(
        rhi::IRHIDevice* device,
        const std::string& filePath
    );
};

} // namespace he::render
```

```cpp
// Engine/Render/Pipeline/NaniteUpload.cpp

NaniteGPUData NaniteUpload::UploadNaniteFile(
    rhi::IRHIDevice* device, const std::string& filePath)
{
    // 1. 读取 .nanite 文件
    std::ifstream file(filePath, std::ios::binary);
    HE_ASSERT(file.is_open(), "NaniteUpload: 无法打开文件");

    NaniteFileHeader header;
    file.read(reinterpret_cast<char*>(&header), sizeof(header));
    HE_ASSERT(std::strncmp(header.magic, "NANITE01", 8) == 0,
              "NaniteUpload: 无效的 .nanite 文件头");

    // 2. 读取 Cluster 数据
    std::vector<NaniteCluster> clusters(header.clusterCount);
    file.read(reinterpret_cast<char*>(clusters.data()),
              header.clusterCount * sizeof(NaniteCluster));

    // 3. 读取顶点 + 索引
    std::vector<u32> vertices(header.vertexCount * 3);  // 3 u32 per vertex
    file.read(reinterpret_cast<char*>(vertices.data()),
              header.vertexCount * 3 * sizeof(u32));

    std::vector<u32> indices(header.indexCount);
    file.read(reinterpret_cast<char*>(indices.data()),
              header.indexCount * sizeof(u32));

    // 4. 创建 GPU Buffers
    NaniteGPUData data;
    data.clusterCount = header.clusterCount;
    data.vertexCount  = header.vertexCount;
    data.indexCount   = header.indexCount;
    data.lodLevelCount = header.lodLevelCount;
    data.maxLODError  = header.maxLODError;

    // Cluster Buffer
    rhi::BufferDesc clusterDesc;
    clusterDesc.size = sizeof(NaniteCluster) * header.clusterCount;
    clusterDesc.usage = rhi::BufferUsage::Storage;
    data.clusterBuffer = device->CreateBuffer(clusterDesc);
    std::memcpy(data.clusterBuffer->Map(), clusters.data(), clusterDesc.size);
    data.clusterBuffer->Unmap();

    // Vertex Buffer
    rhi::BufferDesc vtxDesc;
    vtxDesc.size = header.vertexCount * 3 * sizeof(u32);
    vtxDesc.usage = rhi::BufferUsage::Storage;
    data.vertexBuffer = device->CreateBuffer(vtxDesc);
    std::memcpy(data.vertexBuffer->Map(), vertices.data(), vtxDesc.size);
    data.vertexBuffer->Unmap();

    // Index Buffer
    rhi::BufferDesc idxDesc;
    idxDesc.size = header.indexCount * sizeof(u32);
    idxDesc.usage = rhi::BufferUsage::Storage;
    data.indexBuffer = device->CreateBuffer(idxDesc);
    std::memcpy(data.indexBuffer->Map(), indices.data(), idxDesc.size);
    data.indexBuffer->Unmap();

    HE_CORE_INFO("NaniteUpload: {} clusters, {} vertices, {} indices",
                 header.clusterCount, header.vertexCount, header.indexCount);
    return data;
}
```

- [ ] **Step 1: 创建 NaniteComponent**

写入 `NaniteComponent.h/cpp`。添加反射注册到 `SceneReflect.cpp`。

- [ ] **Step 2: 创建 NaniteUpload**

写入 `NaniteUpload.h/cpp`。实现二进制读取 + GPU buffer 创建。

- [ ] **Step 3: 编译验证**

```bash
cmake --build build --target HugEngineRender --config Debug
```

预期: 编译通过，NaniteUpload 可被调用。

---

### Task 6: GPU 剔除 — Instance Culling

**Files:**
- Create: `Engine/Render/Pipeline/NaniteCulling.h`
- Create: `Engine/Render/Pipeline/NaniteCulling.cpp`
- Create: `Engine/Shader/Shaders/Nanite/Nanite_InstanceCull.comp`

**Interfaces:**
- Consumes: NaniteInstance[] SSBO, camera VP matrix
- Produces: visible instance bitmask / compact list

```hlsl
// Engine/Shader/Shaders/Nanite/Nanite_InstanceCull.comp
#include "NaniteTypes.slang"

[[vk::binding(0, 0)]] StructuredBuffer<NaniteInstance> u_Instances;
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> u_VisibleInstances;  // compact output
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> u_VisibleCount;      // atomic counter

struct CullParams {
    float4x4 viewProj;
    float4   frustumPlanes[6];  // 视锥 6 平面 (normal.xyz, distance)
    uint     instanceCount;
    float    nearPlane;
    float    farPlane;
    uint     _pad;
};
[[vk::push_constant]] CullParams u_Params;

// 视锥 AABB 测试
bool frustumCull(float3 center, float radius, float4 planes[6]) {
    [unroll]
    for (int i = 0; i < 6; i++) {
        float dist = dot(planes[i].xyz, center) + planes[i].w;
        if (dist < -radius) return false;  // 所有 8 个角都在平面外
    }
    return true;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= u_Params.instanceCount) return;

    NaniteInstance inst = u_Instances[tid.x];
    float3 center = inst.boundsCenterRadius.xyz;
    float  radius = inst.boundsCenterRadius.w;

    // Phase 1: 视锥剔除
    if (!frustumCull(center, radius, u_Params.frustumPlanes)) return;

    // Phase 2: Hi-Z 遮挡剔除 (后续 Task 添加)
    // 对齐到下一阶段

    // Compact 写入
    uint slot;
    InterlockedAdd(u_VisibleCount[0], 1, slot);
    u_VisibleInstances[slot] = tid.x;  // 原始 instance 索引
}
```

```cpp
// Engine/Render/Pipeline/NaniteCulling.h
#pragma once
#include "RHI/RHI.h"
#include "Pipeline/NaniteRenderer.h"
#include <memory>

namespace he::render {

class NaniteCulling {
public:
    bool Initialize(rhi::IRHIDevice* device);
    void Shutdown();

    // 设置实例数据 (每帧调用)
    void SetInstances(rhi::IRHIDevice* device,
                      const std::vector<NaniteInstance>& instances);

    // Instance Culling (Compute dispatch)
    void DispatchInstanceCull(rhi::IRHICommandList* cmd,
                              const float4x4& viewProj,
                              u32 instanceCount);

    // Cluster Culling (phase 2, 后续 Task)
    void DispatchClusterCull(rhi::IRHICommandList* cmd);

    rhi::IRHIBuffer* GetInstanceBuffer()    const { return m_InstanceBuf.get(); }
    rhi::IRHIBuffer* GetVisibleInstances()  const { return m_VisibleInstBuf.get(); }
    rhi::IRHIBuffer* GetVisibleCount()      const { return m_VisibleCountBuf.get(); }

private:
    bool m_Initialized = false;

    std::unique_ptr<rhi::IRHIBuffer> m_InstanceBuf;
    std::unique_ptr<rhi::IRHIBuffer> m_VisibleInstBuf;
    std::unique_ptr<rhi::IRHIBuffer> m_VisibleCountBuf;

    std::unique_ptr<rhi::IRHIPipelineState> m_InstanceCullPSO;
    rhi::DescriptorSetLayoutHandle m_InstanceCullLayout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_InstanceCullSet    = rhi::kInvalidSet;

    rhi::ShaderBytecode m_InstanceCullCS;
};

} // namespace he::render
```

- [ ] **Step 1: 编写 Instance Cull Compute Shader**

写入 `Nanite_InstanceCull.comp`。

- [ ] **Step 2: 编写 C++ 端 NaniteCulling**

写入 `NaniteCulling.h/cpp`。创建 PSO + DescriptorSet + Buffer。

- [ ] **Step 3: 编译 Shader + 编译 C++**

```bash
slangc Engine/Shader/Shaders/Nanite/Nanite_InstanceCull.comp -target spirv -entry main -stage compute -I Engine/Shader/Shaders/ -o build/Engine/Shader/Shaders/Nanite_InstanceCull.comp.spv
cmake --build build --target HugEngineRender --config Debug
```

---

### Task 7: GPU 剔除 — Cluster Culling + BVH 遍历

**Files:**
- Create: `Engine/Shader/Shaders/Nanite/Nanite_ClusterCull.comp`

**Interfaces:**
- Consumes: visible instances (Task 6 output), NaniteCluster[] buffer
- Produces: compact cluster list + draw commands

```hlsl
// Engine/Shader/Shaders/Nanite/Nanite_ClusterCull.comp
#include "NaniteTypes.slang"

[[vk::binding(0, 0)]] StructuredBuffer<NaniteInstance> u_Instances;
[[vk::binding(1, 0)]] StructuredBuffer<NaniteCluster>  u_Clusters;
[[vk::binding(2, 0)]] StructuredBuffer<uint> u_VisibleInstances;
[[vk::binding(3, 0)]] RWStructuredBuffer<uint> u_VisibleClusters;  // compact
[[vk::binding(4, 0)]] RWStructuredBuffer<uint> u_VisibleClusterCount;

struct ClusterCullParams {
    float4x4 viewProj;
    float4   frustumPlanes[6];
    uint     instanceCount;
    uint     totalClusterCount;
    uint     _pad0;
    uint     _pad1;
};
[[vk::push_constant]] ClusterCullParams u_Params;

// BVH 深度优先遍历 + 视锥剔除
void traverseBVH(uint instanceIdx, uint clusterBase,
                 float4 frustumPlanes[6], float4x4 viewProj) {
    // 栈式 BVH 遍历 (最大深度 16)
    uint stack[16];
    uint stackPtr = 0;
    stack[stackPtr++] = clusterBase;  // 根 cluster

    while (stackPtr > 0) {
        uint clusterIdx = stack[--stackPtr];
        NaniteCluster c = u_Clusters[clusterIdx];

        // 视锥剔除 cluster bounds
        if (!frustumCull(c.boundingSphere.xyz, c.boundingSphere.w, frustumPlanes))
            continue;

        // 法线锥背面剔除
        float3 viewDir = normalize(c.boundingSphere.xyz - /*camPos*/ float3(0,0,0));
        if (dot(viewDir, c.coneAxisAngle.xyz) < c.coneAxisAngle.w)
            continue;

        // 叶子节点: 输出可见 cluster
        if (c.childCount == 0) {
            uint slot;
            InterlockedAdd(u_VisibleClusterCount[0], 1, slot);
            u_VisibleClusters[slot] = clusterIdx;
            continue;
        }

        // 内部节点: 压栈子节点
        for (uint i = 0; i < c.childCount && stackPtr < 16; i++) {
            stack[stackPtr++] = c.childClusterOffset + i;
        }
    }
}

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= u_Params.instanceCount) return;

    uint instanceIdx = u_VisibleInstances[tid.x];  // 从 Phase 1 输出读取
    NaniteInstance inst = u_Instances[instanceIdx];

    // 从根 cluster 开始 BVH 遍历
    traverseBVH(instanceIdx, inst.clusterBase,
                 u_Params.frustumPlanes, u_Params.viewProj);
}
```

- [ ] **Step 1: 编写 Cluster Cull Shader**

写入 `Nanite_ClusterCull.comp`。

- [ ] **Step 2: 扩展 NaniteCulling C++ 端**

添加 Cluster Cull PSO + Dispatch 方法。

- [ ] **Step 3: 编译验证**

---

### Task 8: Compute Shader 软光栅化 — GBuffer 写入

**Files:**
- Create: `Engine/Shader/Shaders/Nanite/Nanite_SoftRaster.comp`
- Create: `Engine/Shader/Shaders/Nanite/NaniteShared.slang`
- Create: `Engine/Render/Pipeline/NaniteSoftRaster.h`
- Create: `Engine/Render/Pipeline/NaniteSoftRaster.cpp`

**Interfaces:**
- Consumes: visible clusters, vertex/index buffers, GBuffer MRT
- Produces: GBuffer (Albedo/Normal/Emissive/Velocity/WorldPos/Depth)

```hlsl
// Engine/Shader/Shaders/Nanite/NaniteShared.slang

// 量化顶点解码
float3 decodeVertexPosition(uint packed, float3 bboxMin, float3 bboxMax) {
    float3 extent = bboxMax - bboxMin;
    float invScale = max(extent.x, max(extent.y, extent.z)) / 511.0;
    float3 pos;
    pos.x = float(int(packed & 0x3FF) - 512) * invScale + bboxMin.x;
    pos.y = float(int((packed >> 10) & 0x3FF) - 512) * invScale + bboxMin.y;
    pos.z = float(int((packed >> 20) & 0x3FF) - 512) * invScale + bboxMin.z;
    return pos;
}

float3 decodeVertexNormal(uint packed) {
    float3 n;
    n.x = float(int(packed & 0x3FF) - 512) / 511.0;
    n.y = float(int((packed >> 10) & 0x3FF) - 512) / 511.0;
    n.z = float(int((packed >> 20) & 0x3FF) - 512) / 511.0;
    return normalize(n);
}

float2 decodeVertexUV(uint packed) {
    return float2(
        float(packed & 0xFFFF) / 65535.0,
        float((packed >> 16) & 0xFFFF) / 65535.0
    );
}
```

```hlsl
// Engine/Shader/Shaders/Nanite/Nanite_SoftRaster.comp
#include "NaniteTypes.slang"
#include "NaniteShared.slang"

[[vk::binding(0, 0)]] StructuredBuffer<NaniteCluster> u_Clusters;
[[vk::binding(1, 0)]] StructuredBuffer<uint> u_Vertices;      // 3 u32 per vertex
[[vk::binding(2, 0)]] StructuredBuffer<uint> u_Indices;       // packed u16×3 per tri

[[vk::binding(3, 0)]] RWTexture2D<float4> u_GBufferA;  // Albedo+Metallic
[[vk::binding(4, 0)]] RWTexture2D<float4> u_GBufferB;  // Normal+Roughness
[[vk::binding(5, 0)]] RWTexture2D<float4> u_GBufferC;  // Emissive+AO
[[vk::binding(6, 0)]] RWTexture2D<float2> u_GBufferVel;
[[vk::binding(7, 0)]] RWTexture2D<float4> u_GBufferWorldPos;
[[vk::binding(8, 0)]] RWTexture2D<float>  u_GBufferDepth;

struct RasterParams {
    float4x4 viewProj;
    float4x4 prevViewProj;
    float3   bboxMin;
    float3   bboxMax;
    uint     clusterCount;
    float2   screenSize;
    uint     materialID;  // bindless base
    uint     _pad;
};
[[vk::push_constant]] RasterParams u_Params;

// 三角形光栅化 (每个 cluster 64 线程, 每线程处理 1 个三角形)
// 使用 atomicMin 写深度进行 interlock
[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= u_Params.clusterCount) return;

    NaniteCluster c = u_Clusters[tid.x];
    uint materialID = c.materialID + u_Params.materialBase;

    // 遍历 cluster 内三角形
    for (uint t = 0; t < c.triangleCount; t++) {
        uint idxBase = (c.triangleOffset + t) * 3;
        uint i0 = u_Indices[idxBase];
        uint i1 = u_Indices[idxBase + 1];
        uint i2 = u_Indices[idxBase + 2];

        // 解码顶点
        uint vBase = (c.vertexOffset + i0) * 3;
        float3 p0 = decodeVertexPosition(u_Vertices[vBase], u_Params.bboxMin, u_Params.bboxMax);
        float3 n0 = decodeVertexNormal(u_Vertices[vBase + 1]);
        float2 uv0 = decodeVertexUV(u_Vertices[vBase + 2]);

        vBase = (c.vertexOffset + i1) * 3;
        float3 p1 = decodeVertexPosition(u_Vertices[vBase], u_Params.bboxMin, u_Params.bboxMax);
        float3 n1 = decodeVertexNormal(u_Vertices[vBase + 1]);
        float2 uv1 = decodeVertexUV(u_Vertices[vBase + 2]);

        vBase = (c.vertexOffset + i2) * 3;
        float3 p2 = decodeVertexPosition(u_Vertices[vBase], u_Params.bboxMin, u_Params.bboxMax);
        float3 n2 = decodeVertexNormal(u_Vertices[vBase + 1]);
        float2 uv2 = decodeVertexUV(u_Vertices[vBase + 2]);

        // 投影到屏幕
        float4 clip0 = mul(u_Params.viewProj, float4(p0, 1.0));
        float4 clip1 = mul(u_Params.viewProj, float4(p1, 1.0));
        float4 clip2 = mul(u_Params.viewProj, float4(p2, 1.0));

        // 背面剔除
        float2 edge0 = clip1.xy / clip1.w - clip0.xy / clip0.w;
        float2 edge1 = clip2.xy / clip2.w - clip0.xy / clip0.w;
        if (edge0.x * edge1.y - edge0.y * edge1.x <= 0.0) continue;

        // 屏幕空间 bounding box
        float2 ndc0 = clip0.xy / clip0.w;
        float2 ndc1 = clip1.xy / clip1.w;
        float2 ndc2 = clip2.xy / clip2.w;
        int2 bboxMin = int2(min(min(ndc0, ndc1), ndc2) * 0.5 + 0.5) * u_Params.screenSize;
        int2 bboxMax = int2(max(max(ndc0, ndc1), ndc2) * 0.5 + 0.5) * u_Params.screenSize;

        // 逐像素遍历 + barycentric 插值 + depth test
        for (int py = bboxMin.y; py <= bboxMax.y; py++) {
            for (int px = bboxMin.x; px <= bboxMax.x; px++) {
                if (px < 0 || px >= int(u_Params.screenSize.x) ||
                    py < 0 || py >= int(u_Params.screenSize.y)) continue;

                float2 pixel = float2(float(px) + 0.5, float(py) + 0.5);
                float2 ndc = (pixel / u_Params.screenSize) * 2.0 - 1.0;

                // Barycentric 坐标计算
                float2 v0 = ndc - ndc0;
                float2 v1 = ndc - ndc1;
                float2 v2 = ndc - ndc2;
                float area = edge0.x * edge1.y - edge0.y * edge1.x;
                float w0 = (v1.x * v2.y - v1.y * v2.x) / area;
                float w1 = (v2.x * v0.y - v2.y * v0.x) / area;
                float w2 = 1.0 - w0 - w1;

                if (w0 < 0.0 || w1 < 0.0 || w2 < 0.0) continue;

                // 深度插值
                float depth = w0 * clip0.z + w1 * clip1.z + w2 * clip2.z;
                float depthNDC = depth / (w0 * clip0.w + w1 * clip1.w + w2 * clip2.w);

                // Atomic depth test (interlock)
                float prevDepth;
                InterlockedMin(u_GBufferDepth[uint2(px, py)], asuint(depthNDC), prevDepth);
                if (asfloat(prevDepth) <= depthNDC) continue;  // 被遮挡

                // 写入 GBuffer
                float3 worldPos = w0 * p0 + w1 * p1 + w2 * p2;
                float3 normal   = normalize(w0 * n0 + w1 * n1 + w2 * n2);
                float2 uv       = w0 * uv0 + w1 * uv1 + w2 * uv2;

                u_GBufferA[uint2(px, py)]       = float4(1.0, 1.0, 1.0, 1.0); // placeholder albedo
                u_GBufferB[uint2(px, py)]       = float4(normal * 0.5 + 0.5, 0.5); // roughness=0.5
                u_GBufferC[uint2(px, py)]       = float4(0.0, 0.0, 0.0, 1.0); // emissive+AO
                u_GBufferWorldPos[uint2(px, py)] = float4(worldPos, 1.0);
                u_GBufferDepth[uint2(px, py)]    = depthNDC;
            }
        }
    }
}
```

- [ ] **Step 1: 编写 NaniteShared.slang (解码函数)**

写入 `NaniteShared.slang`。

- [ ] **Step 2: 编写 SoftRaster Compute Shader**

写入 `Nanite_SoftRaster.comp`。包含三角形遍历 + barycentric 插值 + atomic depth write。

- [ ] **Step 3: 编写 C++ SoftRaster 调度代码**

写入 `NaniteSoftRaster.h/cpp`。绑定 GBuffer MRT 为 UAV。

- [ ] **Step 4: 编译验证**

```bash
slangc Engine/Shader/Shaders/Nanite/Nanite_SoftRaster.comp -target spirv -entry main -stage compute -I Engine/Shader/Shaders/ -o build/Engine/Shader/Shaders/Nanite_SoftRaster.comp.spv
cmake --build build --target HugEngineRender --config Debug
```

---

### Task 9: DeferredPipeline 集成

**Files:**
- Modify: `Engine/Render/Pipeline/DeferredPipeline.h` (新增 Nanite 成员)
- Modify: `Engine/Render/Pipeline/DeferredPipeline.cpp` (新增 Nanite 路径)
- Modify: `Engine/Render/CMakeLists.txt` (新增所有源文件)
- Modify: `Engine/Shader/CMakeLists.txt` (新增 Nanite shader)

**Interfaces:**
- Consumes: All Task 5-8 APIs
- Produces: 完整 Nanite GBuffer 渲染路径

在 DeferredPipeline::BuildFrameGraph 中新增 Nanite 路径：

```
if (m_UseNanite)  // 新增 Nanite GBuffer 模式
    Nanite_InstanceCull → Nanite_ClusterCull → Nanite_SoftRaster → Lighting
else
    原有 GPU_Cull → GB_Clear → Lighting
```

- [ ] **Step 1: 扩展 DeferredPipeline**

添加 `m_UseNanite` 标志、`NaniteCulling` / `NaniteSoftRaster` 成员。

- [ ] **Step 2: BuildFrameGraph 新增 Nanite GBuffer Pass**

在 GBuffer Pass 位置插入 Nanite 剔除+光栅化 Pass。

- [ ] **Step 3: 更新两个 CMakeLists.txt**

添加所有新技术 .cpp/.h/.comp 文件。

- [ ] **Step 4: 全量编译 + 运行验证**

```bash
cmake --build build --target 04.Deferred --config Debug
./build/bin/Debug/04.Deferred.exe
```

预期: Nanite 模式下的 Sponza 渲染与标准 GBuffer 渲染画面一致。

---

### Task 10: 集成测试与回归

- [ ] **Step 1: 预处理测试**

```bash
# 测试小网格
python Tools/NanitePreprocess/NanitePreprocess.py --input Content/gltf/Cube/Cube.gltf --output Cube.nanite
# 验证 .nanite 文件正确性
python -c "verify_nanite('Cube.nanite')"
```

- [ ] **Step 2: CPU 剔除对比测试**

在 C++ 端创建 100 个实例，CPU 端模拟视锥剔除，与 GPU Compute 结果对比。

- [ ] **Step 3: 软光栅化正确性测试**

渲染单三角形 cube.nanite，对比标准 GBuffer 渲染的像素级差异。

- [ ] **Step 4: Sponza 全量测试**

```bash
python Tools/NanitePreprocess/NanitePreprocess.py --input Content/gltf/Sponza/glTF/Sponza.gltf --output Sponza.nanite
# 在应用中加载 Sponza.nanite，比较 Nanite GBuffer vs 标准 GBuffer
```

---

## 完成标准

| 里程碑 | 核心交付 | 验证标准 |
|--------|----------|----------|
| N1 | Python 预处理工具链 | .nanite 文件正确生成，LOD 层级 > 0 |
| N2 | GPU 上传 + 两阶段剔除 | 可见 cluster 与 CPU 剔除一致 |
| N3 | 软光栅化 GBuffer | Sponza Nanite 路径与标准 GBuffer 像素级一致 |
