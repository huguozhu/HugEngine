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

// 哨兵：第 1 趟之前被模块清成 `0xFFFFFFFF`（"没有几何"）
static const uint kSoftRasterNoGeometryKey = 0xFFFFFFFFu;

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
    uint width = 0u, height = 0u;
    u_DepthKey.GetDimensions(width, height);
    if (pixel.x >= width || pixel.y >= height) { output.depth = 1.0; return output; }
    uint key = u_DepthKey[pixel.y * width + pixel.x];
    if (key == kSoftRasterNoGeometryKey) {
        output.depth = 1.0;          // 没有几何 ⇒ 远平面（天空）
    } else {
        output.depth = asfloat(key & 0xFFFFFF00u);   // 高 24 位 = NDC 深度位模式
    }
    return output;
}
