// FullscreenVS.slang — 全屏三角形 pass 的**共享接口结构体**
//
// 【只共享接口，不共享顶点逻辑】引擎里有 14 个"无顶点缓冲、用 SV_VertexID 生成全屏三角形"
// 的顶点着色器，实测它们的函数体归并为 **13 种不同实现**：三角顶点表的写法、UV 翻转方式、
// 输出深度 z（0 或 1）、是否做世界方向反投影，各 pass 按自己的**目标 UV 朝向与深度约定**选择，
// 并非重复代码 —— **不要为了"看起来统一"去合并它们**（本仓库历史上多次出现 y 翻转类缺陷）。
//
// 真正重复的是**接口结构体**：同一形状在 37 个文件里各声明一份（A 族 30 处、B 族 7 处），
// 且有多处把 position 命名成 `p`。VS 与 FS 各自手抄一份接口是"漂移型 bug"的温床
//（改了一边忘了另一边 ⇒ 编译期查不出、运行期才炸），因此把接口收敛到这里：
// 引同一个头，接口**由构造保证一致**。
//
// 用法：
//   #include "FullscreenVS.slang"
//   VSOutput vertexMain(uint vid : SV_VertexID) { ... }        // A 族（uv@0）
//   VSOutputWorldDir vertexMain(uint vid : SV_VertexID) { ... } // B 族（worldDir@0）
//
// 【顶点表（可选，语义等价于各 pass 内联的那张表）】
//   覆盖率优先的全屏三角形顶点：(-1,-1) / (3,-1) / (-1,3)。
#ifndef HUGENGINE_FULLSCREEN_VS_SLANG
#define HUGENGINE_FULLSCREEN_VS_SLANG

// A 族（30 处）：position + uv@location(0)。全屏后处理/AA/SSAO(B)/SSGI/SSR/Denoise 等用。
struct VSOutput {
    float4 position : SV_Position;
    [[vk::location(0)]] float2 uv;
};

// B 族（7 处）：position + worldDir@location(0)。IBL 辐照度/预滤波、Skybox、PhysicalSky 用。
struct VSOutputWorldDir {
    float4 position : SV_Position;
    [[vk::location(0)]] float3 worldDir;   // 世界空间采样方向
};

// 全屏三角形顶点表：(-1,-1) → (3,-1) → (-1,3)，覆盖整个 NDC（含 [-1,1]²）
static const float2 kFullscreenTriangleVerts[3] = {
    float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0),
};
float2 FullscreenTriangleVertex(uint vid) { return kFullscreenTriangleVerts[vid]; }

#endif // HUGENGINE_FULLSCREEN_VS_SLANG
