// ============================================================
// Nanite_DepthResolve.frag.slang — 把软光栅的**深度键**还原成 NDC 深度写进既有深度附件
//
// 【数据流】`Nanite_SoftRasterDepth.comp.slang` 第 1 趟用 `InterlockedMin` 把每个像素的
//   `key = (asuint(ndcZ) & 0xFFFFFF00) | 三角形下标` 写进 R32_UINT 存储图像；本通道按
//   **帧缓冲像素**索引它，取高 24 位还原 `ndcZ`（浮点位模式与无符号整数同序，`asfloat` 即可），
//   用 `SV_Depth` 输出。哨兵 `0xFFFFFFFF`（该像素没有几何）⇒ 写 1.0（远平面），
//   于是 GBuffer 深度与"模块是唯一几何写入者"的语义一致：没有几何的像素是天空。
//
// 【为什么 SV_Depth 是合法的落点】§14.5 的 A1 裁决原文："深度继续走既有附件路径（或片元
//   `SV_Depth`）"。GPU 上 `D32_SFLOAT` 作为**存储图像**在 NVIDIA 上支持、同机 AMD 核显不支持，
//   而本机的 GBuffer 深度纹理也没有建 `UnorderedAccess`；`SV_Depth` 只要求深度附件，
//   跨厂商，故这是本任务实际采用的路径（运行时还会读一次"该格式能不能做存储图像"并在读数里
//   标出，见 `NaniteRaster` 的 `depthStorageImageSupported`）。
//
// 【§14.3 依赖禁令】只依赖既有 GBuffer 契约（D32 深度附件）与模块自己的深度键纹理。
// ============================================================

[[vk::binding(0, 0)]] StructuredBuffer<uint> u_DepthKey;
// 【P0 修复（§14.30）】读数：本通道**真正写入非远平面深度**的像素数（原子计数，真实 GPU 读回）。
//   全屏片元对每个像素只访问一次 ⇒ 它是**去重后的像素数**，不是第 2 趟的 `pixels_written`
//   （那是"通过等值复检的簇×三角形×像素写次数"）。可核对的不变式：本项 ≤ `pixels_written`
//   ≤ `covered_px`。过去 C++ 侧把 `depth_written` 硬编码成 1，正是这个恒真读数掩盖了
//   "深度一列都没写进去"的 bug。
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> u_Stats;

// 【P0 修复（§14.30）】屏幕尺寸由 CPU 的 push constant 传入。
//   **不要**改回 `u_DepthKey.GetDimensions(w, h)`：结构化缓冲的 `GetDimensions` 返回
//   "元素个数 + 1"（`width = 宽×高`、`height = 1`），当二维尺寸用会让 `pixel.y >= height`
//   对第 0 行以外的所有像素成立 ⇒ 深度解析只写第 0 行，其余全被写成远平面 1.0。
[[vk::push_constant]] cbuffer NaniteDepthResolveParams {
    uint2 screenSize;   // 偏移 0：帧缓冲宽高（像素）
};

// 哨兵：第 1 趟之前被模块清成 `0xFFFFFFFF`（"没有几何"）
static const uint kSoftRasterNoGeometryKey = 0xFFFFFFFFu;
// 读数槽位：与 `Nanite_SoftRasterCommon.slang` 的 `kSoftStatDepthResolvedPixels` 同一个下标
// （本文件不便 include 那个文件 —— 它会引入一整套只有软光栅才需要的绑定）
static const uint kStatsDepthResolvedPixels = 14u;

struct DepthResolvePSInput {
    float4 position : SV_Position;
};

struct DepthResolvePSOutput {
    float depth : SV_Depth;
};

DepthResolvePSOutput fragmentMain(DepthResolvePSInput input) {
    DepthResolvePSOutput output;
    // SV_Position 已经是**帧缓冲像素坐标**（y 向下，与深度键纹理的行列一致）
    uint2 pixel = uint2((uint)input.position.x, (uint)input.position.y);
    // 防御：push constant 未传（宽高为 0）时按"没有几何"处理，不越界读
    if (screenSize.x == 0u || screenSize.y == 0u
        || pixel.x >= screenSize.x || pixel.y >= screenSize.y) {
        output.depth = 1.0;
        return output;
    }
    uint key = u_DepthKey[pixel.y * screenSize.x + pixel.x];
    if (key == kSoftRasterNoGeometryKey) {
        output.depth = 1.0;          // 没有几何 ⇒ 远平面（天空）
    } else {
        output.depth = asfloat(key & 0xFFFFFF00u);   // 高 24 位 = NDC 深度位模式
        // 【P0 修复】真实读数：只有真的写入了非远平面深度的像素才计数
        InterlockedAdd(u_Stats[kStatsDepthResolvedPixels], 1u);
    }
    return output;
}
