// ============================================================
// Nanite/NaniteTypes.slang — Nanite 模块的 C++/Slang **共享二进制契约与量化公式**（仅供 include）
//
// 【这不是 shader 入口】本文件没有 `[numthreads]` / `[shader(...)]`，也**不登记**进
//   `Engine/Shader/CMakeLists.txt` 的 COMP_SLANG / VERT_SLANG / FRAG_SLANG / RT_SLANG /
//   MESH_SLANG 任何一份编译列表 —— 单独给它一个 slangc 入口是没有意义的。
//   **任务 18 的软光栅**（`Nanite_SoftRaster.comp.slang`）应当这样引入它：
//       #include "NaniteTypes.slang"     // 与本文件同目录
//   并把本文件加进 `CMakeLists.txt` 的 `SLANG_INCLUDES`（"公共共享头"列表），否则增量构建
//   不会因为本文件改动而重编那些 include 它的 shader。
//   【本任务（任务 10）为什么先不加进 SLANG_INCLUDES】此刻还没有任何 shader include 它，
//   加进去会让**每个** shader 都多一条对本文件的依赖（任何改动触发全量重编）。等任务 18
//   真正接入时再加，是更小的改动面。
//
// 【改这里必须同步三处】本文件是 `Engine/Render/Nanite/NaniteTypes.h`（**C++ 权威实现**）
//   的镜像：字段顺序/类型/偏移（std430 / StructuredBuffer 视角）、位域、量化常量与公式
//   必须**逐字一致**。任何一处改动都要同步：
//      ① Engine/Render/Nanite/NaniteTypes.h
//      ② 本文件
//      ③ 设计《Nanite设计与实现.md》§8（.nanite 文件格式）
//   C++ 侧的 `static_assert` 钉住布局；公式与位域靠两侧互写的指针注释（就是本段）+ 单测。
//
// 【"三处一致"的达成程度（如实说明）】任务 10 做到的是"**同一套常量与公式**落在 C++ 与
//   Slang 两侧，并用单测把 C++ 侧的量化误差/段布局/DAG 衔接钉住"。真正的
//   **pack / upload / shader 三处一致**要到：
//      · 任务 12（GPU 上传 + 读回）：GPU 缓冲字节数与 `NanitePackedAsset::bytes` 逐字节一致；
//      · 任务 18（软光栅 shader 解码）：用本文件的解码函数还原几何，与 CPU 侧解码结果一致。
//   在那之前，本文件只保证"公式同源"。
//
// 【本文件的解码口径（§14.19 硬约束①）】位置是**簇内局部**量化值：世界坐标 =
//   簇记录的 `boundsCenterRadius.xyz`（各自簇的球心）+ `(raw - quantBias)/1022 × meshMaxExtent`。
//   `meshMaxExtent` = 文件头 `bboxMax - bboxMin` 的最大轴长（**整网格**尺度，不是每簇各自的
//   尺度）—— 由调用方（任务 18 的 push constant / uniform）传进来。**绝不能用"每簇自己的
//   尺度"**，否则共享内容的簇会解出错误的坐标。
//
// 【校验】本文件在任务 10 里用临时 harness（一个只 include 本文件 + 空 main 的 .slang）
//   通过 `slangc -target spirv` 编译验证，见实施记录；它保证本文件至少是**语法正确、
//   类型自洽**的 Slang。
// ============================================================

#pragma once

// ============================================================
// ── 1. 文件级常量（与 C++ 的 `kNanite*` 一一对应）──
// ============================================================

// 魔数 "NANITE01" 的 8 个字节（低 4 字节 / 高 4 字节，按小端拼读）：CPU 侧校验用，shader 不需要
//   "NANITE01" = 4E 41 4E 49 54 45 30 31 ⇒ 低 u32 = 0x494E414E、高 u32 = 0x31304554
static const uint NANITE_FILE_MAGIC_LO = 0x494E414E;   // 'N','A','N','I'（小端）
static const uint NANITE_FILE_MAGIC_HI = 0x31304554;   // 'T','E','0','1'（小端）
static const uint NANITE_FILE_VERSION  = 1;

// 段对齐与各段步长（.nanite 的文件布局；**段偏移不落盘**，由头部计数推导）
static const uint NANITE_FILE_ALIGNMENT           = 16;
static const uint NANITE_FILE_HEADER_BYTES        = 96;
static const uint NANITE_CLUSTER_RECORD_BYTES     = 64;
static const uint NANITE_VERTEX_RECORD_BYTES      = 16;
static const uint NANITE_INDEX_BYTES_PER_TRIANGLE = 8;
static const uint NANITE_MATERIAL_RECORD_BYTES    = 8;
static const uint NANITE_LOD_OFFSET_BYTES         = 4;
static const uint NANITE_INDICES_PER_TRIANGLE     = 3;

// 文件头字段偏移（CPU 侧资产加载用；shader 只需要由 bbox 派生的 `meshMaxExtent` 标量）
static const uint NANITE_HEADER_OFFSET_VERSION      = 8;
static const uint NANITE_HEADER_OFFSET_CLUSTERCOUNT = 12;
static const uint NANITE_HEADER_OFFSET_VERTEXCOUNT  = 16;
static const uint NANITE_HEADER_OFFSET_INDEXCOUNT   = 20;
static const uint NANITE_HEADER_OFFSET_MATERIALCOUNT = 24;
static const uint NANITE_HEADER_OFFSET_LODLEVELCOUNT = 28;
static const uint NANITE_HEADER_OFFSET_FLAGS        = 32;
static const uint NANITE_HEADER_OFFSET_BBOX_MIN     = 36;   // float[3]
static const uint NANITE_HEADER_OFFSET_BBOX_MAX     = 48;   // float[3]
static const uint NANITE_HEADER_OFFSET_MAXLODERROR  = 60;
static const uint NANITE_HEADER_FLAG_HAS_DAG        = 1;    // flags 的 bit0

// 每簇硬上限（§4.1 / §8.4）：≤64 三角形、≤128 顶点
static const uint NANITE_MAX_CLUSTER_TRIANGLES = 64;
static const uint NANITE_MAX_CLUSTER_VERTICES  = 128;

// cone 的"无锥"哨兵：`coneAxisAngle.w == -1` ⇒ 该簇恒不被锥剔除（此时 xyz 允许为 0 向量）
static const float NANITE_CONE_NO_CULL_COS = -1.0;

// ============================================================
// ── 2. 量化常量与位域（**必须与 C++ 侧逐字一致**）──
// ============================================================

// R10G10B10A2 的 10 位域：x[9:0] y[19:10] z[29:20] w[31:30]
static const uint NANITE_VERTEX_QUANT_BITS = 10;
static const uint NANITE_VERTEX_QUANT_MASK = 0x3FF;

// 位置的**有符号**量化偏置（§8.4 定稿）：有符号量 = (raw & 0x3FF) - BIAS
static const int NANITE_VERTEX_QUANT_BIAS = 512;
static const int NANITE_VERTEX_QUANT_MIN  = -512;   // raw = 0    ⇒ -512
static const int NANITE_VERTEX_QUANT_MAX  = 511;    // raw = 1023 ⇒ +511

// 【任务 10 的基准裁决】位置量化 = 盒中心 + 吃满 10 位：
//     signed = round((v - origin) / range × 1022)      // origin = 簇 AABB 中心
//     raw    = clamp(signed, -512, 511) + 512
//     v'     = origin + (raw - 512) / 1022 × range
// 乘数 1022（= 2 × 511）是关键：把 [origin - range/2, origin + range/2] 映射到 [-511, 511]，
// 于是**整个 10 位有符号量程都被用到**（任务 7 的 bboxMin 口径只用上半段 [0,511]，等效 ~9 位）。
// 不 clamp 的依据：簇是网格的子集 ⇒ |v - origin| ≤ 簇半轴长 ≤ range/2 = meshMaxExtent/2。
static const float NANITE_VERTEX_QUANT_FULL_SCALE = 1022.0;
static const float NANITE_POSITION_ERROR_BOUND    = 1.0 / 2044.0;   // 每轴往返误差上界（× meshMaxExtent）

// 法线：八面体编码，每分量 10 位，落在 `packedNormal` 的 x/y 域（z 与 w 保留写 0）。
// 位值按 **UNORM**（0…1023）解释，不用 R10G10B10A2_SNORM 的采样语义（手写位域，不走纹理格式）。
static const uint  NANITE_NORMAL_OCTAHEDRAL_BITS = 10;
static const float NANITE_NORMAL_ANGLE_ERROR_DEGREES = 0.5;   // C++ 单测的验收阈值（实测最坏 < 0.2°）

// UV：R16G16_UNORM（u 低 16 位、v 高 16 位）⇒ 每分量 16 位，步长 1/65535，往返误差 ≤ 1/131070
static const uint  NANITE_UV_QUANT_MAX = 0xFFFF;
static const float NANITE_UV_ERROR_BOUND = 1.0 / 65535.0;

// 三角形索引：3×u16 打包进 u32[2]（8B/三角形），i0/i1/i2 是**簇内局部**顶点下标 [0,127]
static const uint NANITE_INDEX_MAX_U16 = 0xFFFF;

// ============================================================
// ── 3. 结构体（std430 / StructuredBuffer 视角；与 C++ 的 POD 二进制同构）──
// ============================================================

// 量化顶点（16B）：偏移 0 / 4 / 8 / 12。
// 【解码口径】`packedPosition` 是**相对本簇 `NaniteCluster.boundsCenterRadius.xyz`** 的局部偏移。
struct NaniteVertex {
    uint packedPosition;   // R10G10B10A2：x/y/z 各 10 位有符号（+ quantBias），w = 1
    uint packedNormal;     // 八面体 10+10 位（x/y 域），z/w 保留 0
    uint packedUV;         // R16G16_UNORM：u[15:0] v[31:16]
    int  quantBias;        // 量化偏置（默认 +512；从记录里读 ⇒ 解码自包含）
};

// cone 轴角（16B）：xyz = 单位锥轴、w = cos(锥半角)；w == -1 是"无锥"哨兵
struct NaniteConeAxisAngle {
    float3 axis;
    float  cosHalfAngle;
};

// 簇记录（64B）：偏移 0 / 16 / 32 / 36 / 40 / 44 / 48 / 52 / 56 / 60
struct NaniteCluster {
    float4 boundsCenterRadius;   // xyz = 包围球心（**也是位置量化的原点**）, w = 半径
    float4 coneAxisAngle;        // 见 NaniteConeAxisAngle（摊平成一个 float4）
    uint   triangleOffset;       // 三角形下标（× 8B = 索引段字节偏移）
    uint   triangleCount;        // 本簇三角形数（≤ 64）
    uint   vertexOffset;         // 顶点下标（× 16B = 顶点段字节偏移；**共享内容的下标**）
    uint   materialID;           // bindless 材质 ID（材质段下标；任务 12/19 填）
    float  maxParentLODError;    // 切到父级 LOD 的误差阈值（绝对单位）
    uint   childClusterOffset;   // 子节点起始索引（0 = 叶子）
    uint   childCount;           // 子节点数
    uint   _pad;                 // 对齐填充（写 0）
};

// 打包三角形（8B）：lo = i0 | (i1 << 16)、hi = i2（高 16 位保留 0）
struct NanitePackedTriangle {
    uint lo;
    uint hi;
};

// 材质记录（8B）：bindless 纹理 ID 对（0 = 该槽未绑定）
struct NaniteMaterialRecord {
    uint albedoTexture;
    uint normalTexture;
};

// ============================================================
// ── 4. 位域打包/解包（与 C++ 的 `NanitePackR10G10B10A2` / `NaniteUnpackR10G10B10A2` 一致）──
// ============================================================

// R10G10B10A2 打包：x/y/z 各 10 位、w 2 位（超出位宽的位被丢弃）
uint nanitePackR10G10B10A2(uint x, uint y, uint z, uint w) {
    return (x & NANITE_VERTEX_QUANT_MASK)
         | ((y & NANITE_VERTEX_QUANT_MASK) << 10)
         | ((z & NANITE_VERTEX_QUANT_MASK) << 20)
         | ((w & 0x3) << 30);
}

// R10G10B10A2 解包：channel 0/1/2 = x/y/z（10 位），3 = w（2 位）
uint naniteUnpackR10G10B10A2(uint packed, uint channel) {
    return (channel == 3) ? ((packed >> 30) & 0x3)
                          : ((packed >> (channel * 10)) & NANITE_VERTEX_QUANT_MASK);
}

// 把有符号量化值夹到 10 位合法范围（编码端的越界保护）
uint naniteClampRaw10(int raw) {
    if (raw < 0) return 0;
    if (raw > (int)NANITE_VERTEX_QUANT_MASK) return NANITE_VERTEX_QUANT_MASK;
    return (uint)raw;
}

// ============================================================
// ── 5. 位置：簇内局部量化 + 簇心 + 网格最大范围（任务 10 定稿）──
// ============================================================

// 单轴编码：raw = clamp(round((v - origin)/range × 1022), -512, 511) + bias
// 【注意】Slang/HLSL 的 `round()` 在 .5 处通常是"四舍六入五取偶"，而 C++ 的 `std::lround`
//   是"远离零"。编码路径由 CPU 独占（任务 10 的 `PackNaniteClusters`），**解码路径不受影响**
//   （纯算术），所以这条差异不影响三处一致；若将来要在 shader 里编码，必须把取整显式写成
//   `sign(x) * floor(abs(x) + 0.5)` 之类与 C++ 对齐的写法。
uint naniteQuantizePositionAxis(float v, float origin, float range, int bias) {
    if (!(range > 0.0)) return naniteClampRaw10(bias);   // 退化轴：取 origin
    float scaled = (v - origin) / range * NANITE_VERTEX_QUANT_FULL_SCALE;
    if (isnan(scaled)) return naniteClampRaw10(bias);
    float clamped = clamp(scaled, (float)NANITE_VERTEX_QUANT_MIN, (float)NANITE_VERTEX_QUANT_MAX);
    return naniteClampRaw10((int)round(clamped) + bias);
}

// 单轴解码：v = origin + ((raw & 0x3FF) - bias) / 1022 × range
//   `origin` / `range` 必须与编码端同一个簇的球心与同一个网格最大范围（§14.19 硬约束①）
float naniteDecodePositionAxis(uint raw, float origin, float range, int bias) {
    int signedValue = (int)(raw & NANITE_VERTEX_QUANT_MASK) - bias;
    if (!(range > 0.0)) return origin;
    return origin + (float)signedValue / NANITE_VERTEX_QUANT_FULL_SCALE * range;
}

// 三轴位置解码：`packed` = NaniteVertex.packedPosition、`clusterCenter` = 该簇球心、
// `meshMaxExtent` = max(bboxMax - bboxMin)（**整网格**尺度）
float3 naniteDecodePosition(uint packed, int quantBias, float3 clusterCenter, float meshMaxExtent) {
    return float3(
        naniteDecodePositionAxis((packed >> 0)  & NANITE_VERTEX_QUANT_MASK, clusterCenter.x, meshMaxExtent, quantBias),
        naniteDecodePositionAxis((packed >> 10) & NANITE_VERTEX_QUANT_MASK, clusterCenter.y, meshMaxExtent, quantBias),
        naniteDecodePositionAxis((packed >> 20) & NANITE_VERTEX_QUANT_MASK, clusterCenter.z, meshMaxExtent, quantBias));
}

// 便捷重载：直接从顶点 + 簇记录解码
float3 naniteDecodeVertexPosition(NaniteVertex vertex, NaniteCluster cluster, float meshMaxExtent) {
    return naniteDecodePosition(vertex.packedPosition, vertex.quantBias,
                                cluster.boundsCenterRadius.xyz, meshMaxExtent);
}

// ============================================================
// ── 6. 法线：八面体（octahedral）10+10 位 ──
// ============================================================

// [-1,1] → 10 位 UNORM（0…1023）；NaN/越界夹到边界（-1 ⇒ 0、+1 ⇒ 1023）
uint naniteQuantizeUNorm10(float value) {
    if (isnan(value)) return 0;
    float scaled = (value + 1.0) * 0.5 * (float)NANITE_VERTEX_QUANT_MASK;
    return naniteClampRaw10((int)round(clamp(scaled, 0.0, (float)NANITE_VERTEX_QUANT_MASK)));
}

// 10 位 UNORM → [-1,1]
float naniteDequantizeUNorm10(uint raw) {
    return (float)(raw & NANITE_VERTEX_QUANT_MASK) / (float)NANITE_VERTEX_QUANT_MASK * 2.0 - 1.0;
}

// 单位法线 → 八面体 2D 坐标（每分量 [-1,1]）
// 【符号约定】折叠时用 `x >= 0 ? +1 : -1`（不是 `sign(x)`：Slang 的 sign(0)=0 会把坐标清零），
//   与 C++ 的 `NaniteEncodeOctahedral` 完全一致。零向量/NaN ⇒ (0,0)（解码回来是 +Z）。
float2 naniteEncodeOctahedral(float3 n) {
    float lengthSquared = dot(n, n);
    if (!(lengthSquared > 0.0)) return float2(0.0, 0.0);
    float3 unitNormal = n * rsqrt(lengthSquared);
    float l1 = abs(unitNormal.x) + abs(unitNormal.y) + abs(unitNormal.z);
    float x = unitNormal.x / l1;
    float y = unitNormal.y / l1;
    if (unitNormal.z < 0.0) {
        // 下半球折叠：n.xy = (1 - |n.yx|) * signNotZero(n.xy)（abs 的分量是**交换**的）
        float signX = (x >= 0.0) ? 1.0 : -1.0;
        float signY = (y >= 0.0) ? 1.0 : -1.0;
        float foldedX = (1.0 - abs(y)) * signX;
        float foldedY = (1.0 - abs(x)) * signY;
        x = foldedX;
        y = foldedY;
    }
    return float2(x, y);
}

// 八面体 2D 坐标 → 单位法线（含反向折叠 + 归一化）
float3 naniteDecodeOctahedral(float2 encoded) {
    float x = encoded.x;
    float y = encoded.y;
    float z = 1.0 - abs(x) - abs(y);
    if (z < 0.0) {
        float signX = (x >= 0.0) ? 1.0 : -1.0;
        float signY = (y >= 0.0) ? 1.0 : -1.0;
        float unfoldedX = (1.0 - abs(y)) * signX;
        float unfoldedY = (1.0 - abs(x)) * signY;
        x = unfoldedX;
        y = unfoldedY;
    }
    float3 decoded = float3(x, y, z);
    float lengthSquared = dot(decoded, decoded);
    if (!(lengthSquared > 0.0)) return float3(0.0, 0.0, 1.0);
    return decoded * rsqrt(lengthSquared);
}

// 法线 → packedNormal（八面体 10+10 位进 x/y 域；z、w 恒 0）
uint nanitePackNormal(float3 n) {
    float2 encoded = naniteEncodeOctahedral(n);
    return nanitePackR10G10B10A2(naniteQuantizeUNorm10(encoded.x), naniteQuantizeUNorm10(encoded.y), 0, 0);
}

// packedNormal → 单位法线
float3 naniteDecodeNormal(uint packedNormal) {
    return naniteDecodeOctahedral(float2(
        naniteDequantizeUNorm10(naniteUnpackR10G10B10A2(packedNormal, 0)),
        naniteDequantizeUNorm10(naniteUnpackR10G10B10A2(packedNormal, 1))));
}

// ============================================================
// ── 7. UV：unorm16（R16G16_UNORM）──
// ============================================================

// [0,1] → unorm16；NaN 取 0、越界 clamp（unorm16 表示不了平铺/负 UV）
uint naniteQuantizeUV(float value) {
    if (isnan(value)) return 0;
    return (uint)round(clamp(value, 0.0, 1.0) * (float)NANITE_UV_QUANT_MAX);
}

// unorm16 → [0,1]
float naniteDecodeUVComponent(uint raw) {
    return (float)(raw & NANITE_UV_QUANT_MAX) / (float)NANITE_UV_QUANT_MAX;
}

// packedUV → float2（u 低 16 位、v 高 16 位）
float2 naniteDecodeUV(uint packedUV) {
    return float2(naniteDecodeUVComponent(packedUV & 0xFFFF),
                  naniteDecodeUVComponent((packedUV >> 16) & 0xFFFF));
}

// ============================================================
// ── 8. 三角形索引与材质（8B 记录）──
// ============================================================

// 打包三角形 → 簇内局部顶点下标（合法区间 [0, 127]；全局顶点下标 = vertexOffset + local）
uint3 naniteDecodeTriangle(NanitePackedTriangle triangle) {
    return uint3(triangle.lo & NANITE_INDEX_MAX_U16,
                 (triangle.lo >> 16) & NANITE_INDEX_MAX_U16,
                 triangle.hi & NANITE_INDEX_MAX_U16);
}

// 簇内局部顶点下标是否落在"每簇 ≤128 顶点"的约束内（合法区间 [0, 127]）
bool naniteIsValidClusterLocalVertexIndex(uint localIndex) {
    return localIndex < NANITE_MAX_CLUSTER_VERTICES;
}

// 材质记录 → 两个 bindless 纹理 ID（word 0 = albedo、1 = normal）
uint2 naniteDecodeMaterial(NaniteMaterialRecord material) {
    return uint2(material.albedoTexture, material.normalTexture);
}

// cone 轴角 → 剔除判据：`dot(viewDirection, axis) >= cosHalfAngle` 时该簇可被剔除。
// 哨兵（cosHalfAngle == -1）恒返回 false（不剔除），与任务 8 的落盘口径一致。
bool naniteConeCulls(float3 axis, float cosHalfAngle, float3 directionToCluster) {
    if (cosHalfAngle == NANITE_CONE_NO_CULL_COS) return false;
    return dot(normalize(directionToCluster), axis) >= cosHalfAngle;
}
