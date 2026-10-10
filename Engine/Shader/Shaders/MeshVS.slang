// MeshVS.slang — 网格（非全屏）pass 的 VS↔FS 接口结构体（唯一出处）
//
// 【为什么收敛】与 `FullscreenVS.slang` 同一动机：同一形状此前在 VS 与 FS 里**各抄一份**
// （GBuffer/PBR/RSM_Generate 三对，共 6 处），改一边忘一边 ⇒ **编译期查不出、运行期才炸**。
//
// 【为什么不合并成一个结构体】三者只在 location 3 上不同，而这个差异是语义性的：
//   · GBuffer：location 3 = 上一帧裁剪空间位置（velocity 用）
//   · 前向 PBR：location 3 = objectIndex，且必须 **nointerpolation**（FS 按物体索引取材质）
//   · RSM 生成：只需要世界坐标与法线，location 2/3 一律不用
// 强行统一会让 RSM 白传数据、让 GBuffer 与 PBR 的 location 3 互相错位。
#ifndef HUGENGINE_MESH_VS_SLANG
#define HUGENGINE_MESH_VS_SLANG

// GBuffer pass：worldPos@0 + worldNormal@1 + uv@2 + prevClipPos@3
struct GBufferVSOutput {
    float4 position : SV_Position;
    [[vk::location(0)]] float3 worldPos;
    [[vk::location(1)]] float3 worldNormal;
    [[vk::location(2)]] float2 uv;
    [[vk::location(3)]] float4 prevClipPos;   // 上一帧裁剪空间位置（含上帧抖动）→ velocity 计算
};

// PBR（前向与延迟共用）：worldPos@0 + worldNormal@1 + uv@2 + objectIndex@3
// 此前 VS 里叫 `VSOutput`、FS 里叫 `FSInput`（同一形状两个名字）⇒ 现在统一为这个名字。
struct PBRVSOutput {
    float4 position : SV_Position;
    [[vk::location(0)]] float3 worldPos;
    [[vk::location(1)]] float3 worldNormal;
    [[vk::location(2)]] float2 uv;
    [[vk::location(3)]] nointerpolation uint objectIndex;   // 传递到 FS（按物体取材质）
};

// RSM 生成（VPL 光栅化）：worldPos@0 + worldNormal@1
struct RSMVSOutput {
    float4 position : SV_Position;
    [[vk::location(0)]] float3 worldPos;
    [[vk::location(1)]] float3 worldNormal;
};

// 仅输出裁剪空间位置（深度类 pass：阴影 / 贴花投影 / 调试三角形）。
// 此前这 3 个文件各自声明同名同形的 `struct VSOutput { float4 position : SV_Position; }`。
struct PositionOnlyVSOutput {
    float4 position : SV_Position;
};

#endif // HUGENGINE_MESH_VS_SLANG
