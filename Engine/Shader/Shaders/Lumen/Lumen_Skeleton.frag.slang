// Lumen_Skeleton.frag.slang — 骨架阶段的占位输出
//
// 【用途】验证「Lumen 作为 GI 源」的接入链路：帧图 pass → 输出纹理 → Lighting 采样 → 分层合成，
// 以及白炉标度判据（value = 1）。它**不是** Lumen 的算法实现，真实内容由步骤 8~25 依次替换：
//   SurfaceCache_Capture.comp → SDF_MeshBuild/GlobalInject/RayMarch → ScreenProbeGather.comp
// 在那之前，这个常量输出让「pass 是否注册、输出是否被合成、白炉读数是否 1.0」三件事
// 可以独立验证，而不是等到算法写完才发现接线错了。
//
// 【为什么要 PSO 而不是只靠 clear】帧图的 `BeginOffscreenPass` 会用**当前绑定的管线**推导
// RenderPass；若沿用上一个 pass（如 GBuffer 的 9 附件）的管线，framebuffer 的附件数与
// RenderPass 不符 → 验证层报 VUID-VkFramebufferCreateInfo-attachmentCount-00876，实测直接
// 在驱动里访问违例崩溃。所以骨架阶段也必须有一条自己的单附件全屏 pass。

struct VSOutput {
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

// 输出常量值：白炉 = (1, alpha=1)（源自身的标度读数）；常态 = (0, alpha=-1)。
// 【alpha = -1 的含义】沿用框架既有的有效性约定（`alpha < 0` ⇒ 本条无数据，合成端 skip）：
// 骨架阶段 Lumen 还没有真实内容，若让它以"有效的零值"参与归一化，会平白稀释同通道其它源
// （`num/den` 里 den 变大）——画面变暗却说不出原因。标成无效后，开/关 Lumen 的画面完全一致，
// 而归一化的有效性契约本身正是《Lumen设计与实现》§10 的 11.3 要做的框架级工作。
[[vk::push_constant]] cbuffer SkeletonPC {
    float2 u_ValueAlpha;   // x = 输出值, y = alpha（<0 表示无数据）
};

float4 fragmentMain(VSOutput input) : SV_Target0 {
    return float4(u_ValueAlpha.x, u_ValueAlpha.x, u_ValueAlpha.x, u_ValueAlpha.y);
}
