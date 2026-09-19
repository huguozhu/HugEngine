// ============================================================
// Nanite_TestWrite.comp.slang — §14.8 任务 4 的 "UAV 写入" 自证通道
//
// 【目标】证明 A1 路线的最小闭环成立：compute 用 `RWTexture2D<float4>` 直接写**既有
//   GBuffer albedo**（`GB_A`，RGBA16F），并且**同一帧**的 Lighting 读到的就是写后的内容。
//   它是任务 4 的证据工具，不是最终软光栅：任务 18 的软光栅会用同一套 UAV 通路写
//   albedo/normal/worldPos 等通道，本文件届时被替换。
//
// 【图案】8×8 棋盘，两种常数颜色（都能被 f16 精确表示，dump 统计可逐位核对）：
//     深色格 = (0.25, 0.25, 0.25, 0.00)
//     浅色格 = (0.75, 0.50, 0.25, 0.00)
//   全图被本通道写满 ⇒ albedo 转储的统计指纹是
//     min=0.0 max=0.75 rgb_mean=(0.5000, 0.3750, 0.2500)
//   其中 B 通道恒为 0.25（min=mean=max），是最不易与真实场景混淆的一条。
//   alpha = metallic = 0（保持漫反射响应，不把整幅画面变成金属）。
//
// 【为什么覆盖整图而不是局部矩形】整图写入让 dump 统计直接等于图案的解析值
//   （不需要知道屏幕坐标/裁切边界），同时让下游 Lighting 的差异最大化 ⇒
//   "Lighting 是否读到写入后的内容" 变成一条可量化的判据而不是目测。
//
// 【与 C++ 侧的契约】唯一的绑定是 set=0 / binding=0 的存储图像；
//   分辨率由 `GetDimensions` 取纹理自身尺寸，因此 C++ 侧无需传 push constant。
//   描述符类型 = `rhi::DescriptorType::StorageImage`（NaniteRaster::EnsureTestWriteResources）。
//
// 【§14.3 依赖禁令】本文件只依赖 GBuffer 契约（RGBA16F albedo），不引用 GI/Lumen/GPUCulling。
// ============================================================

[[vk::binding(0, 0)]] RWTexture2D<float4> u_Albedo;   // GBuffer A：albedo.rgb + metallic.a（RGBA16F）

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    // 取纹理自身尺寸（与 GPU 上真正那张 GBuffer 完全一致，不依赖 CPU 侧缓存的视口尺寸）
    uint w = 0, h = 0;
    u_Albedo.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;

    // 每 8×8 纹素翻转一次 ⇒ 整图只有两个常数
    uint checker = ((id.x >> 3) + (id.y >> 3)) & 1u;

    // 深色格 / 浅色格（f16 可精确表示）
    float4 c0 = float4(0.25, 0.25, 0.25, 0.0);
    float4 c1 = float4(0.75, 0.50, 0.25, 0.0);
    u_Albedo[id.xy] = (checker == 0u) ? c0 : c1;
}
